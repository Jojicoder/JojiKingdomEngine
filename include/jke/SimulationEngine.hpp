#pragma once
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jke/core/Random.hpp"
#include "jke/core/Constants.hpp"
#include "jke/core/EventBus.hpp"
#include "jke/world/WorldMap.hpp"
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"
#include "jke/technology/TechTree.hpp"
#include "jke/diplomacy/DiplomaticRelation.hpp"
#include "jke/rebellion/Rebellion.hpp"
#include "jke/history/Timeline.hpp"
#include "jke/ai/AIStrategy.hpp"
#include "jke/output/SnapshotSerializer.hpp"
#include "jke/engines/EconomyEngine.hpp"
#include "jke/engines/TechnologyEngine.hpp"
#include "jke/engines/MilitaryEngine.hpp"
#include "jke/engines/BattleEngine.hpp"
#include "jke/engines/DiplomacyEngine.hpp"
#include "jke/engines/RebellionEngine.hpp"
#include "jke/engines/HistoryEngine.hpp"
#include "jke/world/Season.hpp"
#include "jke/world/BanditGroup.hpp"
#include "jke/world/NomadHorde.hpp"
#include "jke/navy/Fleet.hpp"

namespace jke {

struct SimulationConfig {
    uint64_t    seed          = 42;
    uint32_t    maxTurns      = 2000;
    std::string outputDir     = "output";
    bool        verbose       = false;
};

class SimulationEngine {
public:
    explicit SimulationEngine(SimulationConfig config);
    ~SimulationEngine() = default;

    void setSerializer(std::unique_ptr<SnapshotSerializer> s);

    // Run all turns until maxTurns or continent unified
    void run();

    // Run exactly one turn; returns false when simulation is over
    bool step();

    // Public init for viewer (run() calls this internally too)
    void initializeWorld_pub() { initializeWorld(); }

    bool isOver() const { return simulationOver_; }

    // Read-only access to state (for viewer / testing)
    const std::unordered_map<KingdomID, Kingdom>& kingdoms() const { return kingdoms_; }
    const std::unordered_map<CityID, City>&        cities()   const { return cities_; }
    const std::unordered_map<ArmyID, Army>&        armies()   const { return armies_; }
    const WorldMap&    worldMap()   const { return worldMap_; }
    const Timeline&    timeline()   const { return timeline_; }
    const RelationMap&              relations()   const { return relations_; }
    TurnNumber                      currentTurn() const { return currentTurn_; }
    Season                          currentSeason() const {
        return static_cast<Season>((currentTurn_ / constants::TURNS_PER_SEASON) % 4);
    }
    const std::vector<BanditGroup>& bandits()     const { return bandits_; }
    const std::unordered_map<FleetID, Fleet>& fleets() const { return fleets_; }
    const NomadHorde& nomadHorde() const { return horde_; }

    // History snapshots for graph (population, gold, army size keyed by turn)
    struct KingdomSnapshot { uint32_t pop; float gold; int armies; int cities; };
    const std::unordered_map<KingdomID, std::vector<std::pair<TurnNumber,KingdomSnapshot>>>&
        history() const { return history_; }

private:
    SimulationConfig config_;
    Random           rng_;
    TurnNumber       currentTurn_ = 0;
    bool             simulationOver_ = false;

    // ── Entity collections (SimulationEngine owns all state) ──────────────────
    WorldMap                                 worldMap_;
    std::unordered_map<KingdomID, Kingdom>   kingdoms_;
    std::unordered_map<CityID, City>         cities_;
    std::unordered_map<ArmyID, Army>         armies_;
    TechTree                                 techTree_;
    RelationMap                              relations_;
    std::vector<TreatyProposal>              pendingProposals_;
    std::vector<Rebellion>                   rebellions_;
    std::vector<CivilWar>                    civilWars_;
    Timeline                                 timeline_;

    // ── ID counters ───────────────────────────────────────────────────────────
    KingdomID nextKingdomID_ = 1;
    CityID    nextCityID_    = 1;
    ArmyID    nextArmyID_    = 1;
    UnitID    nextUnitID_    = 1;

    // ── Engines ───────────────────────────────────────────────────────────────
    EventBus         eventBus_;
    EconomyEngine    economyEngine_;
    TechnologyEngine techEngine_;
    MilitaryEngine   militaryEngine_;
    BattleEngine     battleEngine_;
    DiplomacyEngine  diplomacyEngine_;
    RebellionEngine  rebellionEngine_;
    HistoryEngine    historyEngine_;

    // ── AI strategies (one per personality) ──────────────────────────────────
    std::unordered_map<KingdomID, std::unique_ptr<AIStrategy>> aiStrategies_;

    // ── Output ────────────────────────────────────────────────────────────────
    std::unique_ptr<SnapshotSerializer> serializer_;
    std::vector<Tile>                   prevTileState_;

    // ── Season / Bandit / Disease / History graph ────────────────────────────
    std::vector<BanditGroup>            bandits_;
    uint32_t                            nextBanditID_ = 1;
    std::unordered_map<KingdomID,
        std::vector<std::pair<TurnNumber,KingdomSnapshot>>> history_;

    // ── Navy ─────────────────────────────────────────────────────────────────
    std::unordered_map<FleetID, Fleet>  fleets_;
    FleetID                             nextFleetID_ = 1;

    // ── Nomad horde ───────────────────────────────────────────────────────────
    NomadHorde  horde_;
    bool        hordeSpawned_ = false;

    // ── Strategic posture cache ───────────────────────────────────────────────
    // Kingdoms whose tiles are being invaded this turn (rebuilt each turn in O(n_armies))
    std::unordered_set<KingdomID>              invaded_;
    // weakTarget per kingdom, refreshed every 5 turns
    std::unordered_map<KingdomID, KingdomID>   weakTargetCache_;
    TurnNumber                                  weakTargetCacheTurn_ = ~0u;

    // ── Internal methods ─────────────────────────────────────────────────────
    void initializeWorld();
    void runTurn();
    void runAIPhase();
    void executeDecision(const AIDecision& decision);
    void updateStrategicPostures();
    void assignArmyRoles();
    void enforceSupplyRetreats();
    void enforceAllianceObligations();
    bool checkDominationVictory();
    void collectSnapshot();
    SimulationSnapshot buildSnapshot() const;
    void printTurnSummary() const;

    void updateSeasonEffects();
    void updateBandits();
    void updatePlaguePropagation();
    void runAICityBuilding();
    void recordHistorySnapshot();
    bool canFoundCity(const Kingdom& k, TileID tile) const;

    // Culture
    void assignCultureGroups();
    void updateCultureAssimilation();

    // Ruler / dynasty
    void assignInitialRulers();
    void updateRulerAging();
    void applyRulerTraits();
    Ruler generateRuler(TurnNumber reignStart, bool isHeir = false);

    // Nomad horde
    void updateNomadHorde();

    // Navy
    void updateNavy();
    FleetID buildFleet(KingdomID owner, CityID portCity);
    bool    isCoastalTile(TileID tile) const;
    TileID  findAdjacentCoast(TileID tile) const;
};

} // namespace jke
