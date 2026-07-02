#include "jke/generators/NationGenerator.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <queue>
#include <cmath>
#include <string>

namespace jke {
namespace {
bool isClaimableTerritory(TerrainType terrain) {
    return terrain != TerrainType::Ocean &&
           terrain != TerrainType::Coast &&
           terrain != TerrainType::Lake;
}
}

// Kingdom lore — name, personality, specialization, concept
// Idx  Name           Personality     Specialization  Concept
//  0   Valdoria       Diplomatic      Economy         大陸の盟主気取り。貿易と外交で覇権を握る古い名家
//  1   Ironspire      Expansionist    Military        鉄の塔の国。鉄の軍事力で帝国を広げる上昇志向の野心家
//  2   Aethermoor     Economic        Technology      霧の湿原に生まれた研究国家。不思議な技術を売り歩く
//  3   Sunhaven       Diplomatic      Agriculture     豊かな農業国。誰とでも仲良くしたい温和な民
//  4   Blackthorn     Aggressive      Military        茨の国。暗く閉鎖的で、侵略によって領土を広げる
//  5   Goldenveil     Opportunistic   Trade           ベールの裏で動く商人国家。利があればどこにでも売る
//  6   Stonegate      Defensive       Defense         石の門の国。難攻不落の要塞都市で守り一辺倒
//  7   Rivermark      Diplomatic      Agriculture     川沿いの肥沃な国。農業と外交で穏やかに栄える
//  8   Ashford        Opportunistic   Economy         かつて栄えた国の残滓。廃墟から這い上がろうとする抜け目ない民
//  9   Crystalholm    Economic        Technology      水晶の島国。精密技術と知識を独占し高値で売る
// 10   Embervast      Aggressive      Military        燃え広がる炎のように膨張する軍事大国
// 11   Dawnreach      Expansionist    Agriculture     夜明けの先へ。肥沃な土地を求めて版図を広げ続ける
// 12   Thornwall      Defensive       Defense         茨の壁に囲まれた孤立主義国家。外を拒み内を守る
// 13   Silvershard    Economic        Trade           銀の破片のように鋭い商才。資源取引で静かに富む
// 14   Highcrest      Expansionist    Military        高台の覇者。見下ろす視点で領土拡張を続ける誇り高い王国
// 15   Mistwood       Opportunistic   Agriculture     霧の森に潜む。機を見てこっそり隣国の食料を奪う
// 16   Duskfell       Opportunistic   Military        黄昏の荒野。滅びかけた国が生き残るために機を突く日和見主義
// 17   Crownsreach    Expansionist    Economy         王冠への渇望。経済力で覇権を取りにくる野心家
// 18   Ironhollow     Defensive       Defense         かつての軍事大国の残骸。鉄の空洞の中で守りに徹す
// 19   Embermarch     Aggressive      Military        国境の炎。侵略の前線基地として常に戦い続ける辺境国
static const std::string KINGDOM_NAMES[] = {
    "Valdoria", "Ironspire", "Aethermoor", "Sunhaven",
    "Blackthorn", "Goldenveil", "Stonegate", "Rivermark",
    "Ashford", "Crystalholm", "Embervast", "Dawnreach",
    "Thornwall", "Silvershard", "Highcrest", "Mistwood",
    "Duskfell", "Crownsreach", "Ironhollow", "Embermarch"
};

NationGenerator::NationGenerator(Random& rng) : rng_(rng) {}

void NationGenerator::generate(GeneratedWorld& world, int kingdomCount) {
    kingdomCount = std::clamp(kingdomCount, 4, constants::NUM_KINGDOMS);

    // Curated index order for small kingdom counts.
    // Guarantees at least 1 Aggressive and 1 Expansionist for active early wars,
    // while keeping personality variety (no two identical personalities in first 4 slots).
    //
    // Index → Name        → Personality    → Specialization
    //  1    Ironspire     Expansionist     Military
    //  4    Blackthorn    Aggressive       Military
    //  5    Goldenveil    Opportunistic    Trade
    //  6    Stonegate     Defensive        Defense
    //  2    Aethermoor    Economic         Technology
    //  0    Valdoria      Diplomatic       Economy
    //  3    Sunhaven      Diplomatic       Agriculture
    //  9    Crystalholm   Economic         Technology
    //  8    Ashford       Opportunistic    Economy
    // 20 kingdoms: sequential 0-19
    static const int curatedOrder[] = {
        1, 4, 5, 6,        // slots 0-3  (4 kingdoms)
        2,                 // slot  4    (5 kingdoms)
        0,                 // slot  5    (6 kingdoms)
        3,                 // slot  6    (7 kingdoms)
        9,                 // slot  7    (8 kingdoms)
        // 9+ kingdoms: sequential from here
        7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
    };
    // For 9+ kingdoms, fall back to 0-N sequential order so all named kingdoms appear.
    auto kingdomIndex = [&](int slot) -> int {
        if (kingdomCount <= 8) return curatedOrder[slot];
        return slot;
    };

    auto sites = pickCapitalSites(world.worldMap, kingdomCount);

    for (int i = 0; i < kingdomCount && i < static_cast<int>(sites.size()); ++i) {
        const int idx = kingdomIndex(i);
        KingdomID kid = world.nextKingdomID++;
        Kingdom k;
        k.id              = kid;
        k.name            = generateKingdomName(idx);
        k.personality     = assignPersonality(idx);
        k.specialization  = assignSpecialization(idx);
        k.foundedTurn     = 0;

        // Starting resources — balanced with small random variation
        k.treasury.food  = constants::START_FOOD  * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.gold  = constants::START_GOLD  * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.wood  = constants::START_WOOD  * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.stone = constants::START_STONE * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.iron  = constants::START_IRON  * rng_.nextFloat(0.9f, 1.1f);

        k.stability   = rng_.nextFloat(0.85f, 1.0f);
        k.morale      = rng_.nextFloat(0.85f, 1.0f);
        k.legitimacy  = 1.0f;

        // aggression初期値をパーソナリティで設定（ランダムでなく意図的な値）
        switch (k.personality) {
            case KingdomPersonality::Aggressive:    k.aggression = 0.85f; break; // 好戦的
            case KingdomPersonality::Expansionist:  k.aggression = 0.70f; break; // 積極的だが計算する
            case KingdomPersonality::Opportunistic: k.aggression = 0.60f; break; // 機を見て動く
            case KingdomPersonality::Defensive:     k.aggression = 0.30f; break; // 守り主体
            case KingdomPersonality::Economic:      k.aggression = 0.40f; break; // 戦争より取引
            case KingdomPersonality::Diplomatic:    k.aggression = 0.20f; break; // 争いを避ける
        }

        applySpecializationBonuses(k);

        // Assign starting technologies by specialization
        switch (k.specialization) {
            case KingdomSpecialization::Military:
                k.researchedTechs = {7, 10};  // Iron Working, Shield Wall
                break;
            case KingdomSpecialization::Economy:
                k.researchedTechs = {13, 14}; // Taxation, Trade Routes
                break;
            case KingdomSpecialization::Agriculture:
                k.researchedTechs = {1, 3};   // Crop Rotation, Selective Breeding
                break;
            case KingdomSpecialization::Technology:
                k.researchedTechs = {11, 16}; // Writing, Mining
                break;
            case KingdomSpecialization::Trade:
                k.researchedTechs = {14, 4};  // Trade Routes, Roads
                break;
            case KingdomSpecialization::Defense:
                k.researchedTechs = {19, 4};  // Fortification, Roads
                break;
        }

        // Store capital tile reference (city will be built by CityGenerator)
        world.worldMap.at(sites[i]).owner = kid;

        world.kingdoms[kid] = std::move(k);
    }

    // Assign territories via BFS Voronoi expansion from capital sites
    assignTerritories(world);
}

std::vector<TileID> NationGenerator::pickCapitalSites(const WorldMap& map, int count) const {
    // Score each land tile — prefer high fertility, not on border, not ocean/mountain
    std::vector<std::pair<float, TileID>> candidates;

    for (const auto& tile : map.tiles()) {
        if (tile.terrain == TerrainType::Ocean   ||
            tile.terrain == TerrainType::Mountain ||
            tile.terrain == TerrainType::Lake     ||
            tile.terrain == TerrainType::Coast)
            continue;

        // Prefer tiles away from edges
        int margin = 8;
        auto [x, y] = tile.position;
        if (x < margin || y < margin ||
            x >= map.width() - margin ||
            y >= map.height() - margin) continue;

        float score = tile.fertility + tile.elevation * 0.5f;
        if (tile.hasRiver) score += 0.3f;
        score += rng_.nextFloat(0.0f, 0.2f);
        candidates.push_back({score, tile.id});
    }

    // Sort descending by score
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // Pick sites with wide separation, especially for low kingdom counts.
    const float minSep = count <= 4 ? 78.0f :
                         count <= 8 ? 56.0f :
                         count <= 12 ? 42.0f : 24.0f;
    std::vector<TileID> chosen;
    chosen.reserve(count);

    for (auto& [score, tid] : candidates) {
        if (static_cast<int>(chosen.size()) >= count) break;
        auto [cx, cy] = map.at(tid).position;

        bool tooClose = false;
        for (TileID prev : chosen) {
            auto [px, py] = map.at(prev).position;
            float d = std::hypot(float(cx - px), float(cy - py));
            if (d < minSep) { tooClose = true; break; }
        }
        if (!tooClose) chosen.push_back(tid);
    }

    // If not enough sites found, relax constraints
    if (static_cast<int>(chosen.size()) < count) {
        for (auto& [score, tid] : candidates) {
            if (static_cast<int>(chosen.size()) >= count) break;
            if (std::find(chosen.begin(), chosen.end(), tid) == chosen.end())
                chosen.push_back(tid);
        }
    }
    return chosen;
}

void NationGenerator::assignTerritories(GeneratedWorld& world) const {
    WorldMap& map = world.worldMap;
    const int kingdomCount = static_cast<int>(world.kingdoms.size());
    const float maxClaimCost = kingdomCount <= 4 ? 20.0f :
                               kingdomCount <= 8 ? 17.0f :
                               kingdomCount <= 12 ? 14.0f : 11.0f;

    // Multi-source BFS from each kingdom's capital tile. Initial kingdoms claim
    // only one compact home region; the rest of the map starts neutral.
    struct Node { TileID tile; float cost; KingdomID kingdom; };
    auto cmp = [](const Node& a, const Node& b){ return a.cost > b.cost; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
    std::vector<float> dist(map.tileCount(), 1e9f);

    for (const auto& [kid, k] : world.kingdoms) {
        // Find the tile owned by this kingdom (capital site)
        for (const auto& tile : map.tiles()) {
            if (tile.owner == kid) {
                pq.push({tile.id, 0.0f, kid});
                dist[tile.id] = 0.0f;
                break;
            }
        }
    }

    while (!pq.empty()) {
        auto [tid, cost, kid] = pq.top(); pq.pop();
        if (cost > dist[tid]) continue;
        if (cost > maxClaimCost) continue;

        Tile& t = map.at(tid);
        if (!isClaimableTerritory(t.terrain)) continue;

        t.owner = kid;

        for (TileID nid : map.neighbors4v(tid)) {
            Tile& nb = map.at(nid);
            if (!isClaimableTerritory(nb.terrain)) continue;

            float moveCost = terrainMoveCost(nb.terrain) * terrainBorderStrength(nb.terrain);
            float newCost  = cost + moveCost;
            if (newCost > maxClaimCost) continue;
            if (newCost < dist[nid]) {
                dist[nid] = newCost;
                pq.push({nid, newCost, kid});
            }
        }
    }

    for (auto& tile : map.tiles()) {
        if (!isClaimableTerritory(tile.terrain)) {
            tile.owner = NO_KINGDOM;
        }
    }
}

void NationGenerator::applySpecializationBonuses(Kingdom& k) const {
    switch (k.specialization) {
        case KingdomSpecialization::Military:
            k.treasury.iron  *= 1.3f;
            k.combatBonus     = 1.15f;
            k.treasury.gold  *= 0.85f;
            break;
        case KingdomSpecialization::Economy:
            k.treasury.gold  *= 1.4f;
            k.goldBonus       = 1.2f;
            k.treasury.iron  *= 0.8f;
            break;
        case KingdomSpecialization::Agriculture:
            k.treasury.food  *= 1.4f;
            k.foodBonus       = 1.2f;
            k.treasury.stone *= 0.85f;
            break;
        case KingdomSpecialization::Technology:
            k.treasury.gold  *= 1.1f;
            k.treasury.iron  *= 1.1f;
            break;
        case KingdomSpecialization::Trade:
            k.treasury.gold  *= 1.3f;
            k.goldBonus       = 1.15f;
            k.treasury.iron  *= 0.85f;
            break;
        case KingdomSpecialization::Defense:
            k.defenseBonus    = 1.2f;
            k.treasury.stone *= 1.4f;
            k.treasury.gold  *= 0.85f;
            break;
    }
}

KingdomPersonality NationGenerator::assignPersonality(int index) const {
    // Fixed per kingdom name — intentional character design
    static const KingdomPersonality personalities[] = {
        KingdomPersonality::Diplomatic,    // 0  Valdoria
        KingdomPersonality::Expansionist,  // 1  Ironspire
        KingdomPersonality::Economic,      // 2  Aethermoor
        KingdomPersonality::Diplomatic,    // 3  Sunhaven
        KingdomPersonality::Aggressive,    // 4  Blackthorn
        KingdomPersonality::Opportunistic, // 5  Goldenveil
        KingdomPersonality::Defensive,     // 6  Stonegate
        KingdomPersonality::Diplomatic,    // 7  Rivermark
        KingdomPersonality::Opportunistic, // 8  Ashford
        KingdomPersonality::Economic,      // 9  Crystalholm
        KingdomPersonality::Aggressive,    // 10 Embervast
        KingdomPersonality::Expansionist,  // 11 Dawnreach
        KingdomPersonality::Defensive,     // 12 Thornwall
        KingdomPersonality::Economic,      // 13 Silvershard
        KingdomPersonality::Expansionist,  // 14 Highcrest
        KingdomPersonality::Opportunistic, // 15 Mistwood
        KingdomPersonality::Opportunistic, // 16 Duskfell
        KingdomPersonality::Expansionist,  // 17 Crownsreach
        KingdomPersonality::Defensive,     // 18 Ironhollow
        KingdomPersonality::Aggressive,    // 19 Embermarch
    };
    return personalities[index % 20];
}

KingdomSpecialization NationGenerator::assignSpecialization(int index) const {
    // Fixed per kingdom name — intentional character design
    static const KingdomSpecialization specs[] = {
        KingdomSpecialization::Economy,     // 0  Valdoria
        KingdomSpecialization::Military,    // 1  Ironspire
        KingdomSpecialization::Technology,  // 2  Aethermoor
        KingdomSpecialization::Agriculture, // 3  Sunhaven
        KingdomSpecialization::Military,    // 4  Blackthorn
        KingdomSpecialization::Trade,       // 5  Goldenveil
        KingdomSpecialization::Defense,     // 6  Stonegate
        KingdomSpecialization::Agriculture, // 7  Rivermark
        KingdomSpecialization::Economy,     // 8  Ashford
        KingdomSpecialization::Technology,  // 9  Crystalholm
        KingdomSpecialization::Military,    // 10 Embervast
        KingdomSpecialization::Agriculture, // 11 Dawnreach
        KingdomSpecialization::Defense,     // 12 Thornwall
        KingdomSpecialization::Trade,       // 13 Silvershard
        KingdomSpecialization::Military,    // 14 Highcrest
        KingdomSpecialization::Agriculture, // 15 Mistwood
        KingdomSpecialization::Military,    // 16 Duskfell
        KingdomSpecialization::Economy,     // 17 Crownsreach
        KingdomSpecialization::Defense,     // 18 Ironhollow
        KingdomSpecialization::Military,    // 19 Embermarch
    };
    return specs[index % 20];
}

std::string NationGenerator::generateKingdomName(int index) const {
    static constexpr int namedCount =
        static_cast<int>(sizeof(KINGDOM_NAMES) / sizeof(KINGDOM_NAMES[0]));
    if (index >= 0 && index < namedCount) {
        return KINGDOM_NAMES[index];
    }
    return KINGDOM_NAMES[namedCount - 1];
}

} // namespace jke
