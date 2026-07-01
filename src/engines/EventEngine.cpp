#include "jke/SimulationEngine.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

void SimulationEngine::updateSeasonEffects() {
    namespace C = constants;

    // Detect season change
    Season cur  = currentSeason();
    Season prev = static_cast<Season>(((currentTurn_ - 1) / C::TURNS_PER_SEASON) % 4);
    if (currentTurn_ > 0 && cur != prev) {
        HistoryEvent ev;
        ev.type        = EventType::SeasonChanged;
        ev.turn        = currentTurn_;
        switch (cur) {
            case Season::Spring:
                ev.description = "Spring campaign season has begun: armies prepare invasions.";
                break;
            case Season::Summer:
                ev.description = "Summer battle season has begun: field battles become decisive.";
                break;
            case Season::Autumn:
                ev.description = "Autumn siege season has begun: strongholds come under pressure.";
                break;
            case Season::Winter:
                ev.description = "Winter quarters have begun: armies rest, resupply, and defend.";
                break;
        }
        ev.context["season"] = static_cast<float>(static_cast<uint8_t>(cur));
        eventBus_.emit(std::move(ev));
        eventBus_.flush();
    }

    SeasonModifiers mods = seasonMods(cur);

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        // Morale drift from season
        k.morale = std::clamp(k.morale + mods.moraleDelta, 0.0f, 1.0f);
        // Winter: apply food penalty now (EconomyEngine multiplied already by bonus;
        // we subtract a winter food tax per city to simulate poor harvests)
        if (cur == Season::Winter && !k.cities.empty()) {
            float coldTax = k.cities.size() * 3.0f;
            k.treasury.food = std::max(0.0f, k.treasury.food - coldTax);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  山賊・略奪者
// ─────────────────────────────────────────────────────────────────────────────
void SimulationEngine::updateBandits() {
    namespace C = constants;

    // Spawn new bandit group
    if (currentTurn_ % C::BANDIT_SPAWN_INTERVAL == 0 &&
        static_cast<int>(bandits_.size()) < C::MAX_BANDIT_GROUPS) {

        // Pick a random unclaimed, non-ocean tile
        std::vector<TileID> candidates;
        for (const auto& t : worldMap_.tiles()) {
            if (t.owner == NO_KINGDOM &&
                t.terrain != TerrainType::Ocean &&
                t.terrain != TerrainType::Lake  &&
                t.city == NO_CITY) {
                candidates.push_back(t.id);
            }
        }
        if (!candidates.empty()) {
            TileID spawnTile = candidates[rng_.nextInt(0, static_cast<int>(candidates.size()) - 1)];
            BanditGroup bg;
            bg.id       = nextBanditID_++;
            bg.tile     = spawnTile;
            bg.strength = rng_.nextInt(120, 350);
            bg.moveCd   = 0;
            bg.raidCd   = 0;
            bandits_.push_back(bg);
        }
    }

    // Update each bandit group
    std::vector<BanditGroup> survivors;
    for (auto& bg : bandits_) {
        if (bg.strength == 0) continue;

        // Check if a kingdom army is on the same tile — fight & destroy bandit
        if (worldMap_.at(bg.tile).army != NO_ARMY) {
            ArmyID aid = worldMap_.at(bg.tile).army;
            auto ait = armies_.find(aid);
            if (ait != armies_.end()) {
                Army& army = ait->second;
                // Bandits deal some casualties
                uint32_t dmg = std::min(static_cast<uint32_t>(bg.strength / 4), army.totalSoldiers() / 10);
                for (auto& u : army.units) {
                    uint32_t ud = std::min(u.soldiers, dmg / static_cast<uint32_t>(army.units.size() + 1));
                    u.soldiers -= ud;
                }
            }
            bg.strength = 0;  // bandits destroyed
            continue;
        }

        // Move toward nearest city
        if (bg.moveCd > 0) { --bg.moveCd; survivors.push_back(bg); continue; }

        CityID nearest = NO_CITY;
        float bestDist = 1e9f;
        auto bgPos = worldMap_.at(bg.tile).position;
        for (const auto& [cid, city] : cities_) {
            if (city.isRuined) continue;
            auto cp = city.position;
            float d = std::hypot(float(bgPos.x - cp.x), float(bgPos.y - cp.y));
            if (d < bestDist) { bestDist = d; nearest = cid; }
        }

        if (nearest == NO_CITY) { survivors.push_back(bg); continue; }

        const auto& target = cities_.at(nearest);
        if (bestDist <= 1.5f) {
            // Raid the city
            if (bg.raidCd == 0) {
                KingdomID owner = target.owner;
                if (owner != NO_KINGDOM && kingdoms_.count(owner) && kingdoms_.at(owner).isAlive) {
                    kingdoms_.at(owner).treasury.gold = std::max(0.0f,
                        kingdoms_.at(owner).treasury.gold - C::BANDIT_RAID_GOLD);
                    kingdoms_.at(owner).treasury.food = std::max(0.0f,
                        kingdoms_.at(owner).treasury.food - C::BANDIT_RAID_FOOD);
                    kingdoms_.at(owner).morale = std::max(0.0f,
                        kingdoms_.at(owner).morale - 0.02f);

                    HistoryEvent ev;
                    ev.type           = EventType::BanditRaid;
                    ev.turn           = currentTurn_;
                    ev.primaryKingdom = owner;
                    ev.relatedCity    = nearest;
                    ev.description    = "Bandits raided " + cities_.at(nearest).name +
                                        " in " + kingdoms_.at(owner).name + "!";
                    eventBus_.emit(std::move(ev));
                    eventBus_.flush();
                }
                bg.raidCd = 8;
                // After raiding, bandits lose some strength (garrison resistance)
                bg.strength = static_cast<uint32_t>(bg.strength * 0.85f);
            } else {
                --bg.raidCd;
            }
        } else {
            // Move one tile toward target
            TileID bnbs[4]; int bnCount = worldMap_.neighbors4(bg.tile, bnbs);
            TileID bestTile = bg.tile;
            float bestD2 = bestDist;
            for (int bni = 0; bni < bnCount; ++bni) {
                TileID nid = bnbs[bni];
                const auto& nt = worldMap_.at(nid);
                if (nt.terrain == TerrainType::Ocean || nt.terrain == TerrainType::Lake) continue;
                float dx = float(nt.position.x - target.position.x);
                float dy = float(nt.position.y - target.position.y);
                float d2 = dx*dx + dy*dy;
                if (d2 < bestD2) { bestD2 = d2; bestTile = nid; }
            }
            bg.tile   = bestTile;
            bg.moveCd = 3;  // move every 4 turns
        }

        if (bg.strength > 0) survivors.push_back(bg);
    }
    bandits_ = std::move(survivors);
}

// ─────────────────────────────────────────────────────────────────────────────
//  疫病の伝播
// ─────────────────────────────────────────────────────────────────────────────
void SimulationEngine::updatePlaguePropagation() {
    // Existing plague: apply effects and countdown
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.plagueTurns <= 0) continue;

        // Plague kills population each turn
        for (CityID cid : k.cities) {
            if (!cities_.count(cid)) continue;
            City& c = cities_.at(cid);
            uint32_t loss = static_cast<uint32_t>(c.population * 0.012f);
            c.population  = c.population > loss ? c.population - loss : std::max(1u, c.population / 2);
            c.happiness   = std::max(0.0f, c.happiness - 0.02f);
        }
        k.morale    = std::max(0.0f, k.morale    - 0.008f);
        k.stability = std::max(0.0f, k.stability - 0.004f);
        k.plagueTurns--;

        if (k.plagueTurns == 0) {
            k.plagueImmune = true;  // survived — immune for a while
        }
    }

    // Reset immunity after 60 turns
    for (auto& [kid, k] : kingdoms_) {
        if (k.plagueImmune && k.plagueTurns <= -60) k.plagueImmune = false;
        if (k.plagueTurns < 0) k.plagueTurns--;
    }

    // Spread: plagued kingdoms infect neighbors through shared borders
    std::vector<KingdomID> toInfect;
    for (const auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.plagueTurns <= 0) continue;

        // Check all neighbors of this kingdom's cities
        for (CityID cid : k.cities) {
            if (!cities_.count(cid)) continue;
            auto pos = cities_.at(cid).position;

            for (auto& [nkid, nk] : kingdoms_) {
                if (!nk.isAlive || nkid == kid || nk.plagueTurns > 0 || nk.plagueImmune) continue;
                // Check if any of nk's cities are close
                for (CityID ncid : nk.cities) {
                    if (!cities_.count(ncid)) continue;
                    auto np = cities_.at(ncid).position;
                    float d = std::hypot(float(pos.x - np.x), float(pos.y - np.y));
                    if (d < 22.0f && rng_.chance(0.06f)) {
                        toInfect.push_back(nkid);
                    }
                }
            }
        }
    }

    for (KingdomID kid : toInfect) {
        if (!kingdoms_.count(kid)) continue;
        auto& k = kingdoms_.at(kid);
        if (k.plagueTurns <= 0 && !k.plagueImmune) {
            k.plagueTurns = rng_.nextInt(15, 30);

            HistoryEvent ev;
            ev.type           = EventType::PlagueSpread;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = "Plague has spread to " + k.name + "!";
            eventBus_.emit(std::move(ev));
        }
    }
    eventBus_.flush();
}

void SimulationEngine::updateNomadHorde() {
    // Spawn first time at turn 100-200; re-spawn 200 turns after dissolution
    const TurnNumber spawnWindow = (horde_.spawnTurn == 0) ? 100
                                  : horde_.spawnTurn + 200;
    if (!hordeSpawned_ && currentTurn_ >= spawnWindow) {
        TurnNumber spawnAt = spawnWindow + static_cast<TurnNumber>(rng_.nextInt(0, 50));
        if (currentTurn_ >= spawnAt) {
            hordeSpawned_ = true;
            horde_.active    = true;
            // Scale with the largest kingdom's army to always be a credible threat
            uint32_t maxArmy = 0;
            for (const auto& [aid, army] : armies_) maxArmy = std::max(maxArmy, army.totalSoldiers());
            uint32_t baseStr = std::max(6000u, maxArmy * 2u);
            horde_.strength  = baseStr + static_cast<uint32_t>(rng_.nextInt(0, 3000));
            horde_.spawnTurn = currentTurn_;
            horde_.moveCd    = 0;
            horde_.raidCd    = 0;
            horde_.turnsActive = 0;

            // Spawn at a random map edge
            const int W = static_cast<int>(worldMap_.width());
            const int H = static_cast<int>(worldMap_.height());
            int edge = static_cast<int>(rng_.nextInt(0, 3));
            int ex, ey;
            switch (edge) {
                case 0: ex = static_cast<int>(rng_.nextInt(0, W-1)); ey = 0;   break;
                case 1: ex = static_cast<int>(rng_.nextInt(0, W-1)); ey = H-1; break;
                case 2: ex = 0;   ey = static_cast<int>(rng_.nextInt(0, H-1)); break;
                default:ex = W-1; ey = static_cast<int>(rng_.nextInt(0, H-1)); break;
            }
            // Find a land tile near that edge point
            horde_.tile = NO_TILE;
            for (int r2 = 0; r2 < 10 && horde_.tile == NO_TILE; ++r2) {
                for (int dx = -r2; dx <= r2 && horde_.tile == NO_TILE; ++dx) {
                    int nx = std::clamp(ex + dx, 0, W-1);
                    int ny = std::clamp(ey + (r2 - std::abs(dx)), 0, H-1);
                    TileID t = static_cast<TileID>(ny * W + nx);
                    auto tt = worldMap_.at(t).terrain;
                    if (tt != TerrainType::Ocean && tt != TerrainType::Lake) horde_.tile = t;
                }
            }
            if (horde_.tile == NO_TILE) horde_.tile = 0;

            HistoryEvent ev;
            ev.type           = EventType::WorldEventNegative;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = NO_KINGDOM;
            ev.description    = "A great nomadic horde has appeared on the horizon! "
                                "All kingdoms are threatened!";
            eventBus_.emit(std::move(ev));
            return;
        }
    }

    if (!horde_.active) return;

    horde_.turnsActive++;
    // Horde dissolves after 180 turns or when too weak; re-emerges after 200 turns
    if (horde_.turnsActive > 180 || horde_.strength < 300) {
        horde_.active = false;
        horde_.spawnTurn = currentTurn_;  // re-emerge after ~200 turns
        hordeSpawned_ = false;            // allow re-spawn
        HistoryEvent ev;
        ev.type        = EventType::WorldEventPositive;
        ev.turn        = currentTurn_;
        ev.primaryKingdom = NO_KINGDOM;
        ev.description = "The nomadic horde has been driven off. But they may return.";
        eventBus_.emit(std::move(ev));
        return;
    }

    const int W = static_cast<int>(worldMap_.width());

    // ── Movement: pick target (most populated city within range) ────────────
    if (horde_.moveCd > 0) { horde_.moveCd--; }
    else {
        // Find target city
        if (horde_.target == NO_CITY || !cities_.count(horde_.target) ||
            cities_.at(horde_.target).owner == NO_KINGDOM) {
            horde_.target = NO_CITY;
            uint32_t bestPop = 0;
            for (const auto& [cid, c] : cities_) {
                if (c.owner == NO_KINGDOM || !kingdoms_.count(c.owner)) continue;
                if (!kingdoms_.at(c.owner).isAlive) continue;
                if (c.population > bestPop) { bestPop = c.population; horde_.target = cid; }
            }
        }

        if (horde_.target != NO_CITY && cities_.count(horde_.target)) {
            const auto& tc = cities_.at(horde_.target);
            int tx = tc.position.x, ty = tc.position.y;
            int hx = static_cast<int>(horde_.tile) % W;
            int hy = static_cast<int>(horde_.tile) / W;

            // Move 3-5 tiles toward target
            int steps = 3 + static_cast<int>(rng_.nextInt(0, 2));
            for (int s = 0; s < steps; ++s) {
                int ndx = (tx > hx) ? 1 : (tx < hx) ? -1 : 0;
                int ndy = (ty > hy) ? 1 : (ty < hy) ? -1 : 0;
                int nx = hx + (ndx != 0 ? ndx : (rng_.chance(0.5f) ? 1 : -1));
                int ny = hy + (ndy != 0 ? ndy : (rng_.chance(0.5f) ? 1 : -1));
                nx = std::clamp(nx, 0, W - 1);
                ny = std::clamp(ny, 0, static_cast<int>(worldMap_.height()) - 1);
                TileID nt = static_cast<TileID>(ny * W + nx);
                auto tt = worldMap_.at(nt).terrain;
                if (tt != TerrainType::Ocean && tt != TerrainType::Lake) {
                    horde_.tile = nt; hx = nx; hy = ny;
                }
            }
        }
        horde_.moveCd = 2;
    }

    // ── Raid nearby cities ────────────────────────────────────────────────────
    if (horde_.raidCd > 0) { horde_.raidCd--; return; }

    int hx = static_cast<int>(horde_.tile) % W;
    int hy = static_cast<int>(horde_.tile) / W;

    for (auto& [cid, city] : cities_) {
        if (city.owner == NO_KINGDOM || !kingdoms_.count(city.owner)) continue;
        if (!kingdoms_.at(city.owner).isAlive) continue;

        int cx = city.position.x, cy = city.position.y;
        int dist = std::abs(cx - hx) + std::abs(cy - hy);
        if (dist > 6) continue;

        // Check if any army defends the city
        bool defended = false;
        if (city.tile != NO_TILE && city.tile < static_cast<TileID>(worldMap_.tileCount())) {
            ArmyID defender = worldMap_.at(city.tile).army;
            if (defender != NO_ARMY && armies_.count(defender)) {
                const Army& da = armies_.at(defender);
                if (da.owner == city.owner) {
                    // Horde vs army combat
                    float hordeStr  = static_cast<float>(horde_.strength) * 0.8f;
                    float armyStr   = da.combatStrength(worldMap_.at(city.tile).terrain, true);
                    if (hordeStr > armyStr) {
                        horde_.strength -= static_cast<uint32_t>(armyStr * 0.3f);
                        // Destroy defending army
                        for (auto& u : armies_.at(defender).units) u.soldiers = 0;
                    } else {
                        horde_.strength -= static_cast<uint32_t>(hordeStr * 0.5f);
                        defended = true;
                    }
                }
            }
        }
        if (defended) continue;

        // Raid! Loot gold, food, reduce population
        auto& k = kingdoms_.at(city.owner);
        float loot = std::min(k.treasury.gold, 200.0f + rng_.nextFloat(0.f, 200.f));
        k.treasury.gold  = std::max(0.0f, k.treasury.gold  - loot);
        k.treasury.food  = std::max(0.0f, k.treasury.food  - 100.0f);
        city.population  = static_cast<uint32_t>(city.population * 0.85f);
        city.happiness   = std::max(0.0f, city.happiness   - 0.25f);
        city.fortification = std::max(0.0f, city.fortification - 0.10f);
        horde_.strength  -= static_cast<uint32_t>(rng_.nextFloat(100.f, 300.f));

        // Clear target so horde picks a new one
        horde_.target = NO_CITY;

        HistoryEvent ev;
        ev.type           = EventType::WorldEventNegative;
        ev.turn           = currentTurn_;
        ev.primaryKingdom = city.owner;
        ev.description    = "The nomadic horde raided " + city.name + "! Gold stolen: " +
                            std::to_string(static_cast<int>(loot));
        eventBus_.emit(std::move(ev));
        horde_.raidCd = 4;
        break; // one raid per update
    }
}

// ── Culture system ────────────────────────────────────────────────────────────


} // namespace jke
