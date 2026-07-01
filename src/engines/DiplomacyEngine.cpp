#include "jke/engines/DiplomacyEngine.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

void DiplomacyEngine::update(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    RelationMap& relations,
    std::vector<TreatyProposal>& pendingProposals,
    std::unordered_map<CityID, City>& cities,
    EventBus& bus,
    TurnNumber turn)
{
    processPendingProposals(pendingProposals, relations, kingdoms, cities, bus, turn);
    updateTrustValues(relations, kingdoms);

    // Increment turnsAtWar for all warring pairs
    for (auto& [ka, row] : relations) {
        for (auto& [kb, rel] : row) {
            if (rel.state == RelationState::War) rel.turnsAtWar++;
        }
    }
}

void DiplomacyEngine::submitProposal(TreatyProposal proposal,
                                      std::vector<TreatyProposal>& pending) {
    pending.push_back(std::move(proposal));
}

DiplomaticRelation& DiplomacyEngine::getOrCreate(
    RelationMap& rel, KingdomID a, KingdomID b)
{
    if (a > b) std::swap(a, b); // canonical order
    auto& row = rel[a];
    if (!row.count(b)) {
        DiplomaticRelation r;
        r.kingdomA = a;
        r.kingdomB = b;
        row[b]     = r;
    }
    return row[b];
}

const DiplomaticRelation* DiplomacyEngine::get(
    const RelationMap& rel, KingdomID a, KingdomID b) const
{
    if (a > b) std::swap(a, b);
    auto it = rel.find(a);
    if (it == rel.end()) return nullptr;
    auto it2 = it->second.find(b);
    if (it2 == it->second.end()) return nullptr;
    return &it2->second;
}

bool DiplomacyEngine::areAtWar(const RelationMap& rel, KingdomID a, KingdomID b) const {
    const DiplomaticRelation* r = get(rel, a, b);
    return r && r->state == RelationState::War;
}

bool DiplomacyEngine::areAllied(const RelationMap& rel, KingdomID a, KingdomID b) const {
    const DiplomaticRelation* r = get(rel, a, b);
    return r && r->state == RelationState::Alliance;
}

void DiplomacyEngine::processPendingProposals(
    std::vector<TreatyProposal>& pending,
    RelationMap& relations,
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities,
    EventBus& bus,
    TurnNumber turn)
{
    for (auto& proposal : pending) {
        if (proposal.processed) continue;
        proposal.processed = true;

        if (!kingdoms.count(proposal.proposer) || !kingdoms.count(proposal.recipient)) continue;
        if (!kingdoms.at(proposal.proposer).isAlive) continue;
        if (!kingdoms.at(proposal.recipient).isAlive) continue;

        DiplomaticRelation& rel = getOrCreate(relations, proposal.proposer, proposal.recipient);

        // Recipient AI acceptance logic
        bool accepted = false;
        switch (proposal.proposedState) {
            case RelationState::Peace: {
                // Recipient accepts peace only if they are losing or exhausted.
                // A winning recipient (more cities, less weariness) should refuse.
                const Kingdom& prop = kingdoms.at(proposal.proposer);
                const Kingdom& recv = kingdoms.at(proposal.recipient);

                float propPower = static_cast<float>(prop.cities.size()) *
                                  (1.0f - prop.warWeariness);
                float recvPower = static_cast<float>(recv.cities.size()) *
                                  (1.0f - recv.warWeariness);

                bool recipientIsWinning = recvPower > propPower * 1.25f;
                bool bothExhausted      = prop.warWeariness > 0.55f &&
                                          recv.warWeariness > 0.55f;
                bool longWar            = rel.turnsAtWar > 60;

                if (proposal.accepted) {
                    accepted = true; // forced acceptance (e.g. alliance obligation)
                } else if (recipientIsWinning && !bothExhausted && !longWar) {
                    accepted = false; // winning side refuses
                } else {
                    accepted = rel.turnsAtWar > 25 || recv.warWeariness > 0.50f ||
                               recv.stability < 0.45f;
                }
                break;
            }
            case RelationState::Alliance: {
                // Accept if trust is positive
                accepted = (rel.trust > 0.2f) || proposal.accepted;
                break;
            }
            case RelationState::TradeAgreement: {
                accepted = (rel.trust > -0.2f) || proposal.accepted;
                break;
            }
            case RelationState::War: {
                // War declarations don't need acceptance
                accepted = true;
                break;
            }
            case RelationState::Neutral: {
                // Non-aggression pact proposal (Neutral with goldTransfer = pact duration)
                accepted = (rel.trust > -0.1f && rel.state != RelationState::War);
                break;
            }
            default:
                accepted = proposal.accepted;
                break;
        }

        if (!accepted) continue;

        // Non-aggression pact: stored on Neutral relation
        if (proposal.proposedState == RelationState::Neutral && proposal.goldTransfer > 0) {
            auto& relAB = getOrCreate(relations, proposal.proposer, proposal.recipient);
            relAB.nonAggressionPact  = true;
            relAB.nonAggressionUntil = turn + static_cast<TurnNumber>(proposal.goldTransfer);
            auto& relBA = getOrCreate(relations, proposal.recipient, proposal.proposer);
            relBA.nonAggressionPact  = true;
            relBA.nonAggressionUntil = relAB.nonAggressionUntil;

            HistoryEvent ev;
            ev.type            = EventType::NonAggressionSigned;
            ev.turn            = turn;
            ev.primaryKingdom  = proposal.proposer;
            ev.secondaryKingdom= proposal.recipient;
            ev.description     = kingdoms.at(proposal.proposer).name + " and " +
                                 kingdoms.at(proposal.recipient).name +
                                 " signed a non-aggression pact.";
            bus.emit(std::move(ev));
            continue;  // don't change the relation state
        }

        // Territory cession on peace: determine who is the loser and transfer cities
        if (proposal.proposedState == RelationState::Peace &&
            rel.state == RelationState::War) {
            const Kingdom& prop = kingdoms.at(proposal.proposer);
            const Kingdom& recv = kingdoms.at(proposal.recipient);
            // Loser = lower stability AND higher war weariness
            bool proposerWeaker = (prop.warWeariness >= recv.warWeariness &&
                                   prop.stability   <= recv.stability);
            KingdomID winner = proposerWeaker ? proposal.recipient : proposal.proposer;
            KingdomID loser  = proposerWeaker ? proposal.proposer  : proposal.recipient;
            // Only cede if meaningful difference (avoid mutual exhaustion giving nothing)
            int occupiedClaims = 0;
            for (CityID cid : kingdoms.at(winner).cities) {
                if (!cities.count(cid)) continue;
                const City& city = cities.at(cid);
                if (city.owner == winner && city.originalOwner == loser) ++occupiedClaims;
            }
            const bool decisiveCollapse =
                kingdoms.at(loser).warWeariness > 0.65f ||
                kingdoms.at(loser).stability < 0.42f;
            const bool earnedSettlement = occupiedClaims > 0 || decisiveCollapse;
            if (earnedSettlement &&
                (kingdoms.at(loser).warWeariness > 0.35f ||
                 kingdoms.at(loser).stability < 0.55f)) {
                ceedTerritory(winner, loser, kingdoms, cities, turn);
            }
        }

        RelationState old = rel.state;
        rel.state = proposal.proposedState;
        rel.turnEstablished = turn;
        if (proposal.proposedState != RelationState::War) rel.turnsAtWar = 0;

        // Transfer gold if part of peace deal
        if (proposal.goldTransfer > 0 && proposal.proposedState == RelationState::Peace) {
            auto& payer = kingdoms.at(proposal.proposer);
            auto& recv  = kingdoms.at(proposal.recipient);
            float actual = std::min(proposal.goldTransfer, payer.treasury.gold);
            payer.treasury.gold -= actual;
            recv.treasury.gold  += actual;
        }

        // Trust adjustments
        if (proposal.proposedState == RelationState::Alliance) rel.trust += 0.3f;
        if (proposal.proposedState == RelationState::War)      rel.trust -= 0.5f;
        if (proposal.proposedState == RelationState::Peace)    rel.trust += 0.1f;
        rel.trust = std::clamp(rel.trust, -1.0f, 1.0f);

        // Emit event
        EventType evType;
        std::string desc;
        switch (proposal.proposedState) {
            case RelationState::War:
                evType = EventType::WarDeclared;
                desc = kingdoms.at(proposal.proposer).name + " declared war on " +
                       kingdoms.at(proposal.recipient).name + ".";
                break;
            case RelationState::Peace:
                evType = EventType::PeaceSigned;
                desc = kingdoms.at(proposal.proposer).name + " and " +
                       kingdoms.at(proposal.recipient).name + " signed a peace treaty.";
                break;
            case RelationState::Alliance:
                evType = EventType::AllianceFormed;
                desc = kingdoms.at(proposal.proposer).name + " and " +
                       kingdoms.at(proposal.recipient).name + " formed an alliance.";
                break;
            default:
                evType = EventType::TreatyFormed;
                desc = kingdoms.at(proposal.proposer).name + " made a treaty with " +
                       kingdoms.at(proposal.recipient).name + ".";
                break;
        }

        HistoryEvent ev;
        ev.type              = evType;
        ev.turn              = turn;
        ev.primaryKingdom    = proposal.proposer;
        ev.secondaryKingdom  = proposal.recipient;
        ev.description       = desc;
        bus.emit(std::move(ev));

        (void)old;
    }

    // Remove processed proposals
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
                       [](const TreatyProposal& p){ return p.processed; }),
        pending.end());
}

void DiplomacyEngine::updateTrustValues(
    RelationMap& relations,
    const std::unordered_map<KingdomID, Kingdom>& kingdoms) const
{
    for (auto& [ka, row] : relations) {
        for (auto& [kb, rel] : row) {
            // Trust drifts toward 0 over time
            if (rel.trust > 0) rel.trust -= 0.005f;
            if (rel.trust < 0) rel.trust += 0.005f;

            // Culture distance caps maximum trust
            // Same culture group: cap 1.0; totally different (7 apart): cap 0.5
            float trustCap = 1.0f;
            auto kitA = kingdoms.find(ka);
            auto kitB = kingdoms.find(kb);
            if (kitA != kingdoms.end() && kitB != kingdoms.end()) {
                int cDist = std::abs(static_cast<int>(kitA->second.cultureGroup) -
                                     static_cast<int>(kitB->second.cultureGroup));
                trustCap = 1.0f - static_cast<float>(cDist) / 14.0f;
            }
            rel.trust = std::clamp(rel.trust, -1.0f, trustCap);
        }
    }
}

void DiplomacyEngine::ceedTerritory(
    KingdomID winner, KingdomID loser,
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities,
    TurnNumber turn)
{
    if (!kingdoms.count(winner) || !kingdoms.count(loser)) return;
    Kingdom& winnerK = kingdoms.at(winner);
    Kingdom& loserK  = kingdoms.at(loser);

    // Find loser's non-capital cities; prefer those close to winner's cities
    auto winnerCityPos = [&]() -> std::pair<float,float> {
        float ax = 0, ay = 0; int n = 0;
        for (CityID cid : winnerK.cities) {
            if (!cities.count(cid)) continue;
            ax += cities.at(cid).position.x;
            ay += cities.at(cid).position.y;
            ++n;
        }
        return n ? std::make_pair(ax/n, ay/n) : std::make_pair(0.f,0.f);
    };

    auto [wx, wy] = winnerCityPos();

    // Sort loser's non-capital cities by proximity to winner's centroid
    std::vector<std::pair<float, CityID>> candidates;
    for (CityID cid : loserK.cities) {
        if (!cities.count(cid)) continue;
        const City& city = cities.at(cid);
        if (city.isCapital) continue;
        float d = std::hypot(city.position.x - wx, city.position.y - wy);
        candidates.emplace_back(d, cid);
    }
    std::sort(candidates.begin(), candidates.end());

    // Transfer 1 city (or 2 if loser is very exhausted and has enough cities)
    int cede = 1;
    if (kingdoms.at(loser).warWeariness > 0.70f &&
        static_cast<int>(loserK.cities.size()) > 3) cede = 2;
    cede = std::min(cede, static_cast<int>(candidates.size()));

    for (int i = 0; i < cede; ++i) {
        CityID cid = candidates[i].second;
        City& city = cities.at(cid);
        city.owner = winner;
        city.happiness = std::max(0.3f, city.happiness - 0.2f); // unrest from transfer

        // Update kingdom city lists
        auto& lv = loserK.cities;
        lv.erase(std::remove(lv.begin(), lv.end(), cid), lv.end());
        winnerK.cities.push_back(cid);
    }

    // Loser remembers who took their cities — triggers revenge war cycle
    if (cede > 0) {
        loserK.revengeTarget = winner;
        loserK.revengeUntil  = turn + 150;  // 150 turns of revenge motivation
    }
}

void DiplomacyEngine::declareWar(
    RelationMap& relations, KingdomID attacker, KingdomID defender,
    EventBus& bus, TurnNumber turn)
{
    DiplomaticRelation& rel = getOrCreate(relations, attacker, defender);

    // Breaking a non-aggression pact: heavy trust penalty
    if (rel.nonAggressionPact && rel.nonAggressionUntil > turn) {
        rel.trust -= 0.4f;   // diplomatic disgrace
        rel.nonAggressionPact = false;
    }

    rel.state           = RelationState::War;
    rel.trust          -= 0.5f;
    rel.trust           = std::clamp(rel.trust, -1.0f, 1.0f);
    rel.turnEstablished = turn;

    (void)bus;
}

} // namespace jke
