#include "jke/SimulationEngine.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

void SimulationEngine::recordHistorySnapshot() {
    for (const auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        KingdomSnapshot snap;
        snap.pop    = k.totalPopulation;
        snap.gold   = k.treasury.gold;
        snap.armies = static_cast<int>(k.armies.size());
        snap.cities = static_cast<int>(k.cities.size());
        history_[kid].emplace_back(currentTurn_, snap);

        // Keep only the last 300 snapshots per kingdom
        auto& vec = history_[kid];
        if (vec.size() > 300) vec.erase(vec.begin());
    }
}

// ── Ruler / Dynasty system ────────────────────────────────────────────────────

static const char* RULER_FIRST[] = {
    "Aldric","Bram","Cedric","Doran","Edric","Faeron","Gareth","Halvard",
    "Idris","Jareth","Kael","Lorcan","Maekar","Neron","Osric","Prynn",
    "Quillon","Rhaest","Soren","Taran","Ulric","Vael","Wulfric","Xander",
    "Ysera","Zoran","Aelindra","Brynn","Cassian","Dwyn",
};
static const char* RULER_DYNASTY[] = {
    "Ironwood","Greymantle","Thornwall","Dawnbreak","Emberveil","Stormcrest",
    "Coldwater","Blackthorn","Ashvale","Rivenmoor","Goldenspire","Silentkeep",
    "Wraithborn","Oakenshield","Frostmere","Scalden","Alderpeak","Veldthorn",
};
static const char* HEIR_FIRST[] = {
    "Aryn","Berit","Calan","Dael","Eiran","Fynn","Gwen","Hael",
    "Irys","Joren","Kira","Lysen","Mira","Nael","Owyn","Pyrra",
};

Ruler SimulationEngine::generateRuler(TurnNumber reignStart, bool isHeir) {
    Ruler r;
    int ni = static_cast<int>(rng_.nextInt(0, 29));
    int di = static_cast<int>(rng_.nextInt(0, 17));
    r.name        = std::string(RULER_FIRST[ni]) + " " + RULER_DYNASTY[di];
    r.dynastyName = RULER_DYNASTY[di];
    r.age         = isHeir ? static_cast<uint8_t>(rng_.nextInt(16, 28))
                           : static_cast<uint8_t>(rng_.nextInt(25, 50));
    r.reignStart  = reignStart;

    // Assign 1-3 random traits (no duplicates, no contradictory pairs)
    std::vector<RulerTrait> pool = {
        RulerTrait::Brave, RulerTrait::Cowardly, RulerTrait::Greedy,
        RulerTrait::Generous, RulerTrait::Wise, RulerTrait::Foolish,
        RulerTrait::Ambitious, RulerTrait::Diplomatic, RulerTrait::Cruel, RulerTrait::Pious
    };
    int numTraits = static_cast<int>(rng_.nextInt(1, 3));
    for (int i = 0; i < numTraits; ++i) {
        if (pool.empty()) break;
        int idx = static_cast<int>(rng_.nextInt(0, static_cast<uint32_t>(pool.size() - 1)));
        RulerTrait picked = pool[idx];
        pool.erase(pool.begin() + idx);
        // Remove contradictory trait
        RulerTrait opposite = picked;
        switch (picked) {
            case RulerTrait::Brave:      opposite = RulerTrait::Cowardly;   break;
            case RulerTrait::Cowardly:   opposite = RulerTrait::Brave;      break;
            case RulerTrait::Greedy:     opposite = RulerTrait::Generous;   break;
            case RulerTrait::Generous:   opposite = RulerTrait::Greedy;     break;
            case RulerTrait::Wise:       opposite = RulerTrait::Foolish;    break;
            case RulerTrait::Foolish:    opposite = RulerTrait::Wise;       break;
            case RulerTrait::Diplomatic: opposite = RulerTrait::Cruel;      break;
            case RulerTrait::Cruel:      opposite = RulerTrait::Diplomatic; break;
            default: break;
        }
        pool.erase(std::remove(pool.begin(), pool.end(), opposite), pool.end());
        r.traits.push_back(picked);
    }

    // 60% chance of having an heir
    // Precompute one-time multipliers from traits
    for (RulerTrait t : r.traits) {
        switch (t) {
            case RulerTrait::Brave:      r.combatMult *= 1.22f; break;  // +22% combat
            case RulerTrait::Cowardly:   r.combatMult *= 0.80f; break;  // -20% combat
            case RulerTrait::Greedy:     r.goldMult   *= 1.28f; break;  // +28% gold
            case RulerTrait::Generous:   r.goldMult   *= 0.88f; break;  // -12% gold
            case RulerTrait::Cruel:      r.combatMult *= 1.10f; break;  // +10% combat
            case RulerTrait::Diplomatic: r.goldMult   *= 1.12f; break;  // +12% gold (trade)
            default: break;
        }
    }

    r.hasHeir = rng_.chance(0.60f);
    if (r.hasHeir) {
        int hi = static_cast<int>(rng_.nextInt(0, 15));
        r.heirName = std::string(HEIR_FIRST[hi]) + " " + r.dynastyName;
        r.heirAge  = static_cast<uint8_t>(rng_.nextInt(8, 22));
    }
    return r;
}

void SimulationEngine::assignInitialRulers() {
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        k.ruler    = generateRuler(0);
        k.hasRuler = true;
    }
}

void SimulationEngine::updateRulerAging() {
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || !k.hasRuler) continue;
        if (k.ruler.age < 255) k.ruler.age++;
        if (k.ruler.hasHeir && k.ruler.heirAge < 255) k.ruler.heirAge++;
        // Heir comes of age at 16 — publicly announced
        if (k.ruler.hasHeir && k.ruler.heirAge == 16) {
            HistoryEvent ev;
            ev.type           = EventType::WorldEventPositive;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = k.ruler.heirName + " of " + k.name +
                " has come of age and is declared heir to the throne.";
            eventBus_.emit(std::move(ev));
        }
    }
}

void SimulationEngine::applyRulerTraits() {
    // combatMult / goldMult are stored on Ruler and applied in BattleEngine /
    // EconomyEngine directly — NOT here — to avoid per-turn accumulation bug.
    // Only per-turn drift effects (stability, legitimacy, aggression) go here.
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || !k.hasRuler) continue;
        for (RulerTrait t : k.ruler.traits) {
            switch (t) {
                case RulerTrait::Greedy:
                    k.stability  = std::max(0.0f, k.stability  - 0.002f); break;
                case RulerTrait::Generous:
                    k.stability  = std::min(1.0f, k.stability  + 0.002f); break;
                case RulerTrait::Wise:
                    k.stability  = std::min(1.0f, k.stability  + 0.003f);
                    k.researchProgress += 0.05f; break;
                case RulerTrait::Foolish:
                    k.stability  = std::max(0.0f, k.stability  - 0.003f); break;
                case RulerTrait::Ambitious:
                    k.aggression = std::min(1.0f, k.aggression + 0.003f); break;
                case RulerTrait::Pious:
                    k.legitimacy = std::min(1.0f, k.legitimacy + 0.003f); break;
                default: break;
            }
        }
    }
}

// ── Nomad Horde ───────────────────────────────────────────────────────────────

void SimulationEngine::assignCultureGroups() {
    // Divide the map into 8 cultural regions based on capital position
    // Use quadrant + half to get 8 zones
    constexpr int GROUPS = 8;
    constexpr float MAP_SIZE = 257.0f;
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.capitalCity == NO_CITY) continue;
        if (!cities_.count(k.capitalCity)) continue;
        const auto& cap = cities_.at(k.capitalCity);
        float nx = static_cast<float>(cap.position.x) / MAP_SIZE; // 0-1
        float ny = static_cast<float>(cap.position.y) / MAP_SIZE;
        int qx = (nx < 0.5f) ? 0 : 1;
        int qy = (ny < 0.5f) ? 0 : 1;
        int half = (nx < 0.25f || (nx >= 0.5f && nx < 0.75f)) ? 0 : 1;
        k.cultureGroup = static_cast<uint8_t>((qy * 2 + qx) * 2 + half) % GROUPS;
    }
    // Propagate culture group to each kingdom's owned cities
    for (auto& [cid, c] : cities_) {
        if (c.owner != NO_KINGDOM && kingdoms_.count(c.owner)) {
            c.cultureOwner        = c.owner;
            c.cultureAssimilation = 1.0f;
        }
    }
}

void SimulationEngine::updateCultureAssimilation() {
    for (auto& [cid, c] : cities_) {
        if (c.owner == NO_KINGDOM) continue;
        if (c.cultureOwner == c.owner) {
            // Native culture: maintain full assimilation
            c.cultureAssimilation = 1.0f;
            continue;
        }
        // Foreign culture: tick toward assimilation
        // Opportunistic kingdoms: fast-talking administrators integrate cities quickly
        float assimRate = 0.02f;
        auto kit = kingdoms_.find(c.owner);
        if (kit != kingdoms_.end() && kit->second.isAlive &&
            kit->second.personality == KingdomPersonality::Opportunistic)
            assimRate = 0.038f;  // ~2× faster integration
        c.cultureAssimilation = std::min(1.0f, c.cultureAssimilation + assimRate);
        if (c.cultureAssimilation >= 1.0f) {
            c.cultureOwner = c.owner; // fully assimilated
        }
    }
}

// ── Navy ─────────────────────────────────────────────────────────────────────

bool SimulationEngine::isCoastalTile(TileID tile) const {
    if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return false;
    auto t = worldMap_.at(tile).terrain;
    return t == TerrainType::Coast || t == TerrainType::Ocean;
}

TileID SimulationEngine::findAdjacentCoast(TileID tile) const {
    if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return NO_TILE;
    const int W = static_cast<int>(worldMap_.width());
    const int x = static_cast<int>(tile) % W;
    const int y = static_cast<int>(tile) / W;
    const int dx[] = {0, 0, -1, 1};
    const int dy[] = {-1, 1, 0, 0};
    for (int i = 0; i < 4; ++i) {
        int nx = x + dx[i], ny = y + dy[i];
        if (nx < 0 || ny < 0 || nx >= W || ny >= static_cast<int>(worldMap_.height())) continue;
        TileID adj = static_cast<TileID>(ny * W + nx);
        if (isCoastalTile(adj)) return adj;
    }
    return NO_TILE;
}

FleetID SimulationEngine::buildFleet(KingdomID owner, CityID portCity) {
    if (!cities_.count(portCity)) return NO_FLEET;
    const auto& city = cities_.at(portCity);
    TileID coastTile = findAdjacentCoast(city.tile);
    if (coastTile == NO_TILE) {
        coastTile = city.tile; // fallback: place at city tile
    }
    Fleet f;
    f.id       = nextFleetID_++;
    f.owner    = owner;
    f.tile     = coastTile;
    f.homePort = portCity;
    f.inPort   = true;
    f.hull     = 1000;
    fleets_.emplace(f.id, f);
    return f.id;
}

void SimulationEngine::updateNavy() {
    constexpr float FLEET_BUILD_GOLD = 200.0f;
    constexpr float FLEET_BUILD_WOOD = 150.0f;
    constexpr float FLEET_UPKEEP     = 4.0f;    // gold per turn
    constexpr int   FLEET_MOVE_CD    = 2;        // turns between moves
    constexpr int   FLEET_MOVE_RANGE = 4;        // coast tiles per move action

    // ── 1. Build fleets at Port cities (AI decision) ─────────────────────────
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        // Count existing fleets
        int ownFleets = 0;
        for (const auto& [fid, fl] : fleets_) if (fl.owner == kid) ownFleets++;

        // Port city kingdoms can build 1 fleet per 2 Port cities
        int portCount = 0;
        for (CityID cid : k.cities) {
            if (cities_.count(cid) && cities_.at(cid).cityType == CityType::Port) portCount++;
        }
        int maxFleets = portCount / 2 + (portCount > 0 ? 1 : 0);
        if (portCount == 0 || ownFleets >= maxFleets) continue;
        if (k.treasury.gold < FLEET_BUILD_GOLD || k.treasury.wood < FLEET_BUILD_WOOD) continue;
        if (!rng_.chance(0.08f)) continue;

        // Pick a Port city without an assigned fleet
        for (CityID cid : k.cities) {
            if (!cities_.count(cid) || cities_.at(cid).cityType != CityType::Port) continue;
            bool hasFleet = false;
            for (const auto& [fid, fl] : fleets_)
                if (fl.homePort == cid) { hasFleet = true; break; }
            if (hasFleet) continue;

            k.treasury.gold -= FLEET_BUILD_GOLD;
            k.treasury.wood -= FLEET_BUILD_WOOD;
            FleetID fid = buildFleet(kid, cid);
            if (fid == NO_FLEET) continue;

            HistoryEvent ev;
            ev.type           = EventType::WorldEventPositive;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = k.name + " launched a fleet from " + cities_.at(cid).name + ".";
            eventBus_.emit(std::move(ev));
            break;
        }
    }

    // ── 2. Upkeep & sink fleets of dead kingdoms ────────────────────────────
    std::vector<FleetID> toSink;
    for (auto& [fid, fl] : fleets_) {
        if (!kingdoms_.count(fl.owner) || !kingdoms_.at(fl.owner).isAlive) {
            toSink.push_back(fid);
            continue;
        }
        kingdoms_.at(fl.owner).treasury.gold -= FLEET_UPKEEP;
    }
    for (FleetID fid : toSink) fleets_.erase(fid);

    // ── 3. Sink fleets whose home port was captured ─────────────────────────
    toSink.clear();
    for (auto& [fid, fl] : fleets_) {
        auto cit = cities_.find(fl.homePort);
        if (fl.homePort == NO_CITY || cit == cities_.end()) continue;
        if (cit->second.owner != fl.owner) toSink.push_back(fid);
    }
    for (FleetID fid : toSink) {
        auto fit = fleets_.find(fid);
        if (fit == fleets_.end()) continue;
        // Disembark cargo if any
        if (fit->second.cargo != NO_ARMY) {
            ArmyID aid = fit->second.cargo;
            auto ait = armies_.find(aid);
            if (ait != armies_.end()) {
                ait->second.currentTile = fit->second.tile;
                ait->second.isBesieging = false;
            }
        }
        fleets_.erase(fit);
    }

    // ── 4. Move fleets (basic AI: find enemy coast, transport army) ──────────
    const int W = static_cast<int>(worldMap_.width());
    for (auto& [fid, fl] : fleets_) {
        if (fl.moveCd > 0) { fl.moveCd--; continue; }
        Kingdom& k = kingdoms_.at(fl.owner);

        // Find the active war enemy
        KingdomID enemy = NO_KINGDOM;
        for (const auto& [eid, ek] : kingdoms_) {
            if (!ek.isAlive || eid == fl.owner) continue;
            KingdomID lo = std::min(fl.owner, eid), hi = std::max(fl.owner, eid);
            if (relations_.count(lo) && relations_.at(lo).count(hi) &&
                relations_.at(lo).at(hi).state == RelationState::War) {
                enemy = eid;
                break;
            }
        }
        if (enemy == NO_KINGDOM) continue;

        // If empty, try to embark a nearby army
        if (fl.cargo == NO_ARMY) {
            for (ArmyID aid : k.armies) {
                auto ait2 = armies_.find(aid);
                if (ait2 == armies_.end()) continue;
                auto& army = ait2->second;
                TileID at = army.currentTile;
                if (at == NO_TILE || at >= static_cast<TileID>(worldMap_.tileCount())) continue;
                // Army must be adjacent to fleet tile
                int ax = static_cast<int>(at) % W, ay = static_cast<int>(at) / W;
                int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
                int dist = std::abs(ax - fx) + std::abs(ay - fy);
                if (dist <= 2 && army.role == ArmyRole::Attack) {
                    fl.cargo = aid;
                    army.currentTile = fl.tile; // army moves onto fleet
                    fl.inPort = false;
                    break;
                }
            }
            if (fl.cargo == NO_ARMY) continue;
        }

        // Navigate toward nearest enemy coast tile
        TileID bestTile = NO_TILE;
        int bestScore = INT_MAX;
        for (const auto& [ecid, ec] : cities_) {
            if (ec.owner != enemy) continue;
            TileID coast = findAdjacentCoast(ec.tile);
            if (coast == NO_TILE) continue;
            int cx = static_cast<int>(coast) % W, cy = static_cast<int>(coast) / W;
            int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
            int d = std::abs(cx - fx) + std::abs(cy - fy);
            if (d < bestScore) { bestScore = d; bestTile = coast; }
        }
        if (bestTile == NO_TILE) continue;

        // Move toward target (up to FLEET_MOVE_RANGE steps along coast/ocean)
        int tx = static_cast<int>(bestTile) % W, ty = static_cast<int>(bestTile) / W;
        int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
        int steps = 0;
        for (; steps < FLEET_MOVE_RANGE && (fx != tx || fy != ty); ++steps) {
            int ndx = (tx > fx) ? 1 : (tx < fx) ? -1 : 0;
            int ndy = (ty > fy) ? 1 : (ty < fy) ? -1 : 0;
            // Prefer coast movement, try horizontal then vertical
            auto tryStep = [&](int ddx, int ddy) -> bool {
                int nx = fx + ddx, ny = fy + ddy;
                if (nx < 0 || ny < 0 || nx >= W || ny >= static_cast<int>(worldMap_.height())) return false;
                TileID nt = static_cast<TileID>(ny * W + nx);
                if (!isCoastalTile(nt)) return false;
                fx = nx; fy = ny;
                fl.tile = nt;
                return true;
            };
            if (!tryStep(ndx, ndy)) {
                if (!tryStep(ndx, 0)) tryStep(0, ndy);
            }
        }

        // Disembark army if adjacent to enemy city
        if (fl.cargo != NO_ARMY) {
            for (const auto& [ecid, ec] : cities_) {
                if (ec.owner != enemy) continue;
                int cx = static_cast<int>(ec.tile) % W, cy = static_cast<int>(ec.tile) / W;
                int dist = std::abs(cx - fx) + std::abs(cy - fy);
                if (dist <= 3) {
                    // Disembark
                    if (armies_.count(fl.cargo)) {
                        armies_.at(fl.cargo).currentTile = fl.tile;
                        armies_.at(fl.cargo).targetTile  = ec.tile;
                        armies_.at(fl.cargo).role        = ArmyRole::Attack;
                    }
                    fl.cargo  = NO_ARMY;
                    fl.inPort = false;

                    HistoryEvent ev;
                    ev.type           = EventType::WorldEventNegative;
                    ev.turn           = currentTurn_;
                    ev.primaryKingdom = fl.owner;
                    ev.description    = k.name + "'s fleet landed troops near " + ec.name + "!";
                    eventBus_.emit(std::move(ev));
                    break;
                }
            }
        }
        fl.moveCd = FLEET_MOVE_CD;
    }
}


} // namespace jke
