#include "jke/engines/RebellionEngine.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <string>
#include <unordered_set>

// Helper — create or retrieve a DiplomaticRelation between two kingdoms
static jke::DiplomaticRelation& getOrCreateRel(
    jke::RelationMap& rel, jke::KingdomID a, jke::KingdomID b)
{
    return rel[a][b];
}

namespace jke {

RebellionEngine::RebellionEngine(Random& rng) : rng_(rng) {}

void RebellionEngine::update(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities,
    std::unordered_map<ArmyID, Army>& armies,
    std::vector<Rebellion>& rebellions,
    std::vector<CivilWar>& civilWars,
    RelationMap& relations,
    KingdomID& nextKingdomID,
    ArmyID& nextArmyID,
    UnitID& nextUnitID,
    EventBus& bus,
    TurnNumber turn)
{
    int aliveKingdoms = 0;
    int aliveMajorKingdoms = 0;
    for (const auto& [kid, kingdom] : kingdoms) {
        if (!kingdom.isAlive) continue;
        ++aliveKingdoms;
        if (!kingdom.isRebel) {
            ++aliveMajorKingdoms;
        }
    }
    const bool allowNewRebellions =
        aliveKingdoms > 4 &&
        aliveMajorKingdoms > 4 &&
        turn > 80 &&
        turn < 900;

    std::unordered_set<KingdomID> unstableKingdoms;
    for (const Rebellion& r : rebellions) {
        if (!r.suppressed && !r.escalatedToCivilWar) {
            unstableKingdoms.insert(r.targetKingdom);
        }
    }
    for (const CivilWar& cw : civilWars) {
        if (!cw.resolved) {
            unstableKingdoms.insert(cw.parentKingdom);
        } else if (turn > cw.startedTurn && turn - cw.startedTurn < 120) {
            unstableKingdoms.insert(cw.parentKingdom);
        }
    }

    // Check each city for new rebellion
    if (allowNewRebellions) {
        for (auto& [cid, city] : cities) {
            if (city.isRuined) continue;
            if (city.population < 2000) continue;
            auto it = kingdoms.find(city.owner);
            if (it == kingdoms.end() || !it->second.isAlive) continue;

            const Kingdom& owner = it->second;
            if (owner.isRebel) continue;
            if (unstableKingdoms.count(owner.id)) continue;

            float pressure = computeRebellionPressure(city, owner);
            if (pressure < 0.65f) continue;

            if (rng_.chance(pressure * 0.006f)) {
                // Trigger rebellion
                bool alreadyRebelling = false;
                for (auto& r : rebellions) {
                    if (r.epicenterCity == cid && !r.suppressed) {
                        r.strength = std::min(1.0f, r.strength + 0.15f);
                        alreadyRebelling = true;
                        break;
                    }
                }
                if (!alreadyRebelling) {
                    Rebellion r;
                    r.targetKingdom = city.owner;
                    r.epicenterCity = cid;
                    r.cause         = (owner.starvationTurns > 0)
                                        ? RebellionCause::Famine
                                        : (owner.stability < 0.3f)
                                            ? RebellionCause::LowLegitimacy
                                            : RebellionCause::Oppression;
                    r.strength      = pressure * 0.5f;
                    r.startedTurn   = turn;

                    HistoryEvent ev;
                    ev.type             = EventType::RebellionStarted;
                    ev.turn             = turn;
                    ev.primaryKingdom   = city.owner;
                    ev.relatedCity      = cid;
                    ev.description      = "Rebellion erupted in " + city.name +
                                          " against " + owner.name + ".";
                    bus.emit(std::move(ev));

                    rebellions.push_back(std::move(r));
                }
            }
        }
    }

    // Grow or suppress existing rebellions
    for (auto& r : rebellions) {
        if (r.suppressed || r.escalatedToCivilWar) continue;
        if (!allowNewRebellions) {
            r.suppressed = true;
            continue;
        }

        auto ownerIt = kingdoms.find(r.targetKingdom);
        if (ownerIt == kingdoms.end() || !ownerIt->second.isAlive) {
            r.suppressed = true;
            continue;
        }
        Kingdom& owner = ownerIt->second;
        if (owner.isRebel) {
            r.suppressed = true;
            continue;
        }

        // Stability suppresses rebellions
        if (owner.stability > 0.7f || rng_.chance(0.3f * owner.stability)) {
            suppressRebellion(r, cities, bus, turn);
            owner.stability = std::min(1.0f, owner.stability + 0.05f);
        } else {
            r.strength += 0.025f;
            owner.stability -= 0.015f;
        }

        // Escalate to civil war
        if (r.strength >= 0.95f && !r.escalatedToCivilWar) {
            escalateToCivilWar(r, kingdoms, cities, armies, civilWars, relations,
                               nextKingdomID, nextArmyID, nextUnitID, bus, turn);
        }
    }

    // Remove suppressed rebellions
    rebellions.erase(
        std::remove_if(rebellions.begin(), rebellions.end(),
                       [](const Rebellion& r){ return r.suppressed || r.escalatedToCivilWar; }),
        rebellions.end());

    // Handle ongoing civil wars
    for (auto& cw : civilWars) {
        if (cw.resolved) continue;
        // Civil war resolves when one side has no armies or cities left
        auto& parent = kingdoms.at(cw.parentKingdom);
        auto it = kingdoms.find(cw.rebelFactionID);
        if (it == kingdoms.end() || !it->second.isAlive) {
            cw.resolved = true;
        } else if (!parent.isAlive) {
            cw.resolved = true;
        }
    }
}

float RebellionEngine::computeRebellionPressure(
    const City& city,
    const Kingdom& owner) const
{
    float pressure = 0.0f;

    // Low stability
    if (owner.stability < constants::REBELLION_THRESHOLD)
        pressure += (constants::REBELLION_THRESHOLD - owner.stability) * 2.0f;

    // City unhappiness
    pressure += (1.0f - city.happiness) * 0.30f;

    // Starvation
    if (owner.starvationTurns > 0)
        pressure += owner.starvationTurns * 0.12f;

    // Recently conquered city
    if (city.originalOwner != city.owner)
        pressure += 0.06f;

    // Foreign culture: unassimilated cities resist rule
    if (city.cultureOwner != city.owner && city.cultureAssimilation < 1.0f)
        pressure += (1.0f - city.cultureAssimilation) * 0.10f;

    // Large empire overstretch: >8 cities adds governance burden
    const int empireSize = static_cast<int>(owner.cities.size());
    if (empireSize > 8)
        pressure += std::min(0.20f, (empireSize - 8) * 0.010f);

    // War weariness
    if (owner.warWeariness > 0.5f)
        pressure += (owner.warWeariness - 0.5f) * 0.30f;

    return std::clamp(pressure, 0.0f, 1.0f);
}

void RebellionEngine::escalateToCivilWar(
    Rebellion& rebellion,
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities,
    std::unordered_map<ArmyID, Army>& armies,
    std::vector<CivilWar>& civilWars,
    RelationMap& relations,
    KingdomID& nextKingdomID,
    ArmyID& nextArmyID,
    UnitID& nextUnitID,
    EventBus& bus,
    TurnNumber turn)
{
    Kingdom& parent = kingdoms.at(rebellion.targetKingdom);
    if (parent.isRebel || parent.cities.size() < 3) {
        rebellion.suppressed = true;
        return;
    }

    // Create rebel faction
    KingdomID rebelID = nextKingdomID++;
    Kingdom rebel;
    rebel.id              = rebelID;
    rebel.name            = parent.name + " Rebels";
    rebel.personality     = KingdomPersonality::Aggressive;
    rebel.specialization  = KingdomSpecialization::Military;
    rebel.foundedTurn     = turn;
    rebel.isAlive         = true;
    rebel.isRebel         = true;
    rebel.stability       = 0.7f;
    rebel.morale          = 0.8f;
    rebel.legitimacy      = 0.4f;
    rebel.treasury        = parent.treasury * 0.3f;

    // Transfer epicenter city to rebel
    CityID epicenter = rebellion.epicenterCity;
    auto cit = cities.find(epicenter);
    if (cit == cities.end()) { rebellion.suppressed = true; return; }
    City& city = cit->second;
    if (city.population < 1200) {
        rebellion.suppressed = true;
        return;
    }

    auto& pCities = parent.cities;
    pCities.erase(std::remove(pCities.begin(), pCities.end(), epicenter), pCities.end());

    city.owner = rebelID;
    rebel.cities.push_back(epicenter);
    rebel.capitalCity     = epicenter;
    city.isCapital        = true;

    // Create rebel army
    ArmyID rebelArmyID = nextArmyID++;
    Army rebelArmy;
    rebelArmy.id          = rebelArmyID;
    rebelArmy.owner       = rebelID;
    rebelArmy.currentTile = city.tile;
    rebelArmy.position    = city.position;
    rebelArmy.supplyLevel = 0.8f;

    Unit u;
    u.id       = nextUnitID++;
    u.type     = UnitType::Militia;
    u.soldiers = std::clamp<uint32_t>(city.population / 2, 500, 1500);
    u.training = 0.35f;
    u.equipment= 0.3f;
    u.morale   = 0.9f;
    u.supply   = 0.8f;
    rebelArmy.units.push_back(u);

    rebel.armies.push_back(rebelArmyID);
    rebel.totalPopulation = city.population;
    armies[rebelArmyID]   = std::move(rebelArmy);
    kingdoms[rebelID]     = std::move(rebel);

    // Declare mutual war between rebel faction and parent kingdom
    {
        auto& rel1 = getOrCreateRel(relations, rebellion.targetKingdom, rebelID);
        rel1.state           = RelationState::War;
        rel1.trust           = -1.0f;
        rel1.turnEstablished = turn;
        auto& rel2 = getOrCreateRel(relations, rebelID, rebellion.targetKingdom);
        rel2.state           = RelationState::War;
        rel2.trust           = -1.0f;
        rel2.turnEstablished = turn;
        kingdoms.at(rebelID).revengeTarget = rebellion.targetKingdom;
        kingdoms.at(rebelID).revengeUntil  = turn + 300;
    }

    CivilWar cw;
    cw.parentKingdom   = rebellion.targetKingdom;
    cw.rebelFactionID  = rebelID;
    cw.rebelCities     = {epicenter};
    cw.rebelArmies     = {rebelArmyID};
    cw.startedTurn     = turn;
    civilWars.push_back(std::move(cw));

    rebellion.escalatedToCivilWar = true;
    parent.stability -= 0.2f;

    HistoryEvent ev;
    ev.type              = EventType::CivilWar;
    ev.turn              = turn;
    ev.primaryKingdom    = rebellion.targetKingdom;
    ev.secondaryKingdom  = rebelID;
    ev.relatedCity       = epicenter;
    ev.description       = "Civil war broke out in " + parent.name +
                           "! Rebel faction " + kingdoms.at(rebelID).name + " emerged.";
    bus.emit(std::move(ev));
}

void RebellionEngine::suppressRebellion(
    Rebellion& r,
    std::unordered_map<CityID, City>& cities,
    EventBus& bus,
    TurnNumber turn) const
{
    r.suppressed = true;
    auto it = cities.find(r.epicenterCity);
    if (it != cities.end()) {
        it->second.happiness = std::min(0.8f, it->second.happiness + 0.1f);
    }

    HistoryEvent ev;
    ev.type             = EventType::RebellionSuppressed;
    ev.turn             = turn;
    ev.primaryKingdom   = r.targetKingdom;
    ev.relatedCity      = r.epicenterCity;
    ev.description      = "Rebellion in city was suppressed.";
    bus.emit(std::move(ev));
}

} // namespace jke
