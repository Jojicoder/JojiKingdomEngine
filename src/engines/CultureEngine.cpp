#include "jke/SimulationEngine.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

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
    constexpr float    FLEET_BUILD_GOLD  = 200.0f;
    constexpr float    FLEET_BUILD_WOOD  = 150.0f;
    constexpr float    FLEET_UPKEEP      = 4.0f;
    constexpr int      FLEET_MOVE_CD     = 2;
    constexpr int      FLEET_MOVE_RANGE  = 4;
    constexpr uint32_t FLEET_HULL_COMBAT = 150;  // hull damage per naval combat exchange

    const int W = static_cast<int>(worldMap_.width());
    const int H = static_cast<int>(worldMap_.height());

    auto atWar = [&](KingdomID a, KingdomID b) -> bool {
        KingdomID lo = std::min(a, b), hi = std::max(a, b);
        return relations_.count(lo) && relations_.at(lo).count(hi) &&
               relations_.at(lo).at(hi).state == RelationState::War;
    };

    // BFS coastal pathfinding: returns next coast/ocean tile to step onto from `from`
    // toward `to`. Returns NO_TILE if unreachable within LIMIT nodes.
    auto bfsCoastStep = [&](TileID from, TileID to) -> TileID {
        if (from == to || from == NO_TILE || to == NO_TILE) return from;
        constexpr int LIMIT = 1500;
        const int ddx[] = {0, 0, -1, 1};
        const int ddy[] = {-1, 1, 0, 0};
        std::unordered_map<TileID, TileID> parent;
        std::queue<TileID> q;
        parent[from] = from;
        q.push(from);
        int visited = 0;
        while (!q.empty() && visited < LIMIT) {
            TileID cur = q.front(); q.pop();
            ++visited;
            if (cur == to) {
                TileID step = to;
                while (parent.at(step) != from) step = parent.at(step);
                return step;
            }
            int cx = static_cast<int>(cur) % W, cy = static_cast<int>(cur) / W;
            for (int i = 0; i < 4; ++i) {
                int nx = cx + ddx[i], ny = cy + ddy[i];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                TileID nt = static_cast<TileID>(ny * W + nx);
                if (!isCoastalTile(nt) || parent.count(nt)) continue;
                parent[nt] = cur;
                q.push(nt);
            }
        }
        return NO_TILE;
    };

    // ── 1. Build fleets at Port cities (AI decision) ─────────────────────────
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        int ownFleets = 0;
        for (const auto& [fid, fl] : fleets_) if (fl.owner == kid) ownFleets++;

        int portCount = 0;
        for (CityID cid : k.cities)
            if (cities_.count(cid) && cities_.at(cid).cityType == CityType::Port) portCount++;
        int maxFleets = portCount / 2 + (portCount > 0 ? 1 : 0);
        if (portCount == 0 || ownFleets >= maxFleets) continue;
        if (k.treasury.gold < FLEET_BUILD_GOLD || k.treasury.wood < FLEET_BUILD_WOOD) continue;
        if (!rng_.chance(0.08f)) continue;

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
    {
        std::vector<FleetID> toSink;
        for (auto& [fid, fl] : fleets_) {
            if (!kingdoms_.count(fl.owner) || !kingdoms_.at(fl.owner).isAlive) {
                toSink.push_back(fid);
                continue;
            }
            kingdoms_.at(fl.owner).treasury.gold -= FLEET_UPKEEP;
        }
        for (FleetID fid : toSink) fleets_.erase(fid);
    }

    // ── 3. Sink fleets whose home port was captured ─────────────────────────
    {
        std::vector<FleetID> toSink;
        for (auto& [fid, fl] : fleets_) {
            auto cit = cities_.find(fl.homePort);
            if (fl.homePort == NO_CITY || cit == cities_.end()) continue;
            if (cit->second.owner != fl.owner) toSink.push_back(fid);
        }
        for (FleetID fid : toSink) {
            auto fit = fleets_.find(fid);
            if (fit == fleets_.end()) continue;
            if (fit->second.cargo != NO_ARMY && armies_.count(fit->second.cargo))
                armies_.at(fit->second.cargo).currentTile = fit->second.tile;
            fleets_.erase(fit);
        }
    }

    // ── Naval combat: fleets from warring kingdoms on adjacent tiles exchange fire ──
    {
        std::vector<FleetID> ids;
        ids.reserve(fleets_.size());
        for (const auto& [fid, _] : fleets_) ids.push_back(fid);

        for (size_t i = 0; i < ids.size(); ++i) {
            if (!fleets_.count(ids[i])) continue;
            Fleet& fa = fleets_.at(ids[i]);
            if (fa.hull == 0) continue;
            int ax = static_cast<int>(fa.tile) % W, ay = static_cast<int>(fa.tile) / W;

            for (size_t j = i + 1; j < ids.size(); ++j) {
                if (!fleets_.count(ids[j])) continue;
                Fleet& fb = fleets_.at(ids[j]);
                if (fb.hull == 0) continue;
                if (fa.owner == fb.owner || !atWar(fa.owner, fb.owner)) continue;

                int bx = static_cast<int>(fb.tile) % W, by = static_cast<int>(fb.tile) / W;
                if (std::max(std::abs(ax - bx), std::abs(ay - by)) > 1) continue;

                uint32_t dmgA = FLEET_HULL_COMBAT + rng_.nextInt(0, 50);
                uint32_t dmgB = FLEET_HULL_COMBAT + rng_.nextInt(0, 50);
                fa.hull = (fa.hull > dmgB) ? fa.hull - dmgB : 0;
                fb.hull = (fb.hull > dmgA) ? fb.hull - dmgA : 0;

                if (fb.hull == 0) {
                    HistoryEvent ev;
                    ev.type = EventType::WorldEventNegative; ev.turn = currentTurn_;
                    ev.primaryKingdom = fa.owner;
                    ev.description = kingdoms_.at(fa.owner).name + "'s fleet sank an enemy vessel!";
                    eventBus_.emit(std::move(ev));
                }
                if (fa.hull == 0) {
                    HistoryEvent ev;
                    ev.type = EventType::WorldEventNegative; ev.turn = currentTurn_;
                    ev.primaryKingdom = fb.owner;
                    ev.description = kingdoms_.at(fb.owner).name + "'s fleet sank an enemy vessel!";
                    eventBus_.emit(std::move(ev));
                    break;
                }
            }
        }

        // Remove sunk fleets; disembark cargo onto the tile they sank on
        for (auto it = fleets_.begin(); it != fleets_.end(); ) {
            if (it->second.hull == 0) {
                if (it->second.cargo != NO_ARMY && armies_.count(it->second.cargo))
                    armies_.at(it->second.cargo).currentTile = it->second.tile;
                it = fleets_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Track armies already embarked this turn to prevent double-embarkation
    std::unordered_set<ArmyID> embarkedThisTurn;
    for (const auto& [fid, fl] : fleets_)
        if (fl.cargo != NO_ARMY) embarkedThisTurn.insert(fl.cargo);

    // ── 4. Move fleets ───────────────────────────────────────────────────────
    for (auto& [fid, fl] : fleets_) {
        if (fl.moveCd > 0) { fl.moveCd--; continue; }
        if (!kingdoms_.count(fl.owner)) continue;
        Kingdom& k = kingdoms_.at(fl.owner);

        // Determine war enemy
        KingdomID enemy = NO_KINGDOM;
        for (const auto& [eid, ek] : kingdoms_) {
            if (!ek.isAlive || eid == fl.owner) continue;
            if (atWar(fl.owner, eid)) { enemy = eid; break; }
        }

        // ── Peace-time: patrol between own port coast tiles ───────────────
        if (enemy == NO_KINGDOM) {
            std::vector<TileID> portTiles;
            for (CityID cid : k.cities) {
                if (!cities_.count(cid) || cities_.at(cid).cityType != CityType::Port) continue;
                TileID coast = findAdjacentCoast(cities_.at(cid).tile);
                if (coast != NO_TILE) portTiles.push_back(coast);
            }
            if (portTiles.size() >= 2) {
                size_t slot = (static_cast<size_t>(fid) + static_cast<size_t>(currentTurn_) / 4)
                              % portTiles.size();
                TileID patrol = portTiles[slot];
                if (fl.tile == patrol) patrol = portTiles[(slot + 1) % portTiles.size()];
                TileID step = bfsCoastStep(fl.tile, patrol);
                if (step != NO_TILE && step != fl.tile) fl.tile = step;
            }
            fl.moveCd = FLEET_MOVE_CD;
            continue;
        }

        // Find nearest enemy coastal city (used for both empty and loaded fleets)
        TileID bestCoast = NO_TILE;
        {
            int bestDist = INT_MAX;
            int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
            for (const auto& [ecid, ec] : cities_) {
                if (ec.owner != enemy) continue;
                TileID coast = findAdjacentCoast(ec.tile);
                if (coast == NO_TILE) continue;
                int cx = static_cast<int>(coast) % W, cy = static_cast<int>(coast) / W;
                int d = std::abs(cx - fx) + std::abs(cy - fy);
                if (d < bestDist) { bestDist = d; bestCoast = coast; }
            }
        }

        // ── War-time empty: move toward enemy coast to pick up armies ─────
        if (fl.cargo == NO_ARMY) {
            // Try to embark a nearby attack army (Chebyshev <= 8, no double-embark)
            int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
            for (ArmyID aid : k.armies) {
                if (embarkedThisTurn.count(aid)) continue;
                auto ait = armies_.find(aid);
                if (ait == armies_.end()) continue;
                Army& army = ait->second;
                if (army.currentTile == NO_TILE) continue;
                int ax2 = static_cast<int>(army.currentTile) % W;
                int ay2 = static_cast<int>(army.currentTile) / W;
                int dist = std::max(std::abs(ax2 - fx), std::abs(ay2 - fy));
                if (dist <= 8 &&
                    (army.role == ArmyRole::Attack ||
                     army.role == ArmyRole::Vanguard ||
                     army.role == ArmyRole::Flanker)) {
                    fl.cargo = aid;
                    army.currentTile = fl.tile;
                    fl.inPort = false;
                    embarkedThisTurn.insert(aid);
                    break;
                }
            }
            // If still empty, advance toward enemy coast to be ready next turn
            if (fl.cargo == NO_ARMY && bestCoast != NO_TILE) {
                TileID step = bfsCoastStep(fl.tile, bestCoast);
                if (step != NO_TILE && step != fl.tile) fl.tile = step;
            }
            fl.moveCd = FLEET_MOVE_CD;
            continue;
        }

        if (bestCoast == NO_TILE) { fl.moveCd = FLEET_MOVE_CD; continue; }

        // BFS navigate up to FLEET_MOVE_RANGE steps; disembark when Chebyshev <= 3 to enemy city
        bool disembarked = false;
        for (int s = 0; s < FLEET_MOVE_RANGE && !disembarked; ++s) {
            TileID next = bfsCoastStep(fl.tile, bestCoast);
            if (next == NO_TILE || next == fl.tile) break;
            fl.tile = next;
            if (armies_.count(fl.cargo)) armies_.at(fl.cargo).currentTile = fl.tile;

            int nfx = static_cast<int>(fl.tile) % W, nfy = static_cast<int>(fl.tile) / W;
            for (const auto& [ecid, ec] : cities_) {
                if (ec.owner != enemy) continue;
                int cx = static_cast<int>(ec.tile) % W, cy = static_cast<int>(ec.tile) / W;
                if (std::max(std::abs(cx - nfx), std::abs(cy - nfy)) > 3) continue;
                if (armies_.count(fl.cargo)) {
                    armies_.at(fl.cargo).currentTile = fl.tile;
                    armies_.at(fl.cargo).targetTile  = ec.tile;
                    armies_.at(fl.cargo).role        = ArmyRole::Attack;
                }
                fl.cargo     = NO_ARMY;
                fl.inPort    = false;
                disembarked  = true;
                HistoryEvent ev;
                ev.type           = EventType::WorldEventNegative;
                ev.turn           = currentTurn_;
                ev.primaryKingdom = fl.owner;
                ev.description    = k.name + "'s fleet landed troops near " + ec.name + "!";
                eventBus_.emit(std::move(ev));
                break;
            }
        }
        fl.moveCd = FLEET_MOVE_CD;
    }
}


} // namespace jke
