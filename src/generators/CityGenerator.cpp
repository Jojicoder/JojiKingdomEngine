#include "jke/generators/CityGenerator.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <unordered_map>

namespace jke {

CityGenerator::CityGenerator(Random& rng) : rng_(rng) {}

void CityGenerator::generate(GeneratedWorld& world) {
    for (auto& [kid, kingdom] : world.kingdoms) {
        // Find the tile marked as owned by this kingdom near center of territory
        // For starting state: place capital on one of the owned tiles
        TileID capitalTile = NO_TILE;
        float  bestScore   = -1.0f;

        for (const auto& tile : world.worldMap.tiles()) {
            if (tile.owner != kid) continue;
            if (tile.terrain == TerrainType::Ocean   ||
                tile.terrain == TerrainType::Mountain ||
                tile.terrain == TerrainType::Lake) continue;

            float score = tile.fertility + tile.resourceRichness * 0.5f;
            if (tile.hasRiver) score += 0.3f;
            if (score > bestScore) {
                bestScore   = score;
                capitalTile = tile.id;
            }
        }

        if (capitalTile == NO_TILE) continue;

        City capital = buildCapital(world, kingdom, capitalTile);
        CityID cid   = world.nextCityID++;
        capital.id   = cid;

        world.worldMap.at(capitalTile).city = cid;

        kingdom.capitalCity = cid;
        kingdom.cities.push_back(cid);
        kingdom.totalPopulation = capital.population;

        world.cities[cid] = std::move(capital);

        struct Candidate {
            TileID tile  = NO_TILE;
            float  score = 0.0f;
            bool   mountain = false;
        };
        std::vector<Candidate> candidates;
        auto capPos = world.worldMap.at(capitalTile).position;

        for (const auto& tile : world.worldMap.tiles()) {
            if (tile.owner != kid || tile.city != NO_CITY) continue;
            if (tile.terrain == TerrainType::Ocean ||
                tile.terrain == TerrainType::Lake) continue;

            float dist = std::hypot(float(tile.position.x - capPos.x),
                                    float(tile.position.y - capPos.y));
            if (dist < 12.0f) continue;

            bool isMountain = (tile.terrain == TerrainType::Mountain);
            bool isCoast    = (tile.terrain == TerrainType::Coast);

            float score = tile.fertility * 1.2f + tile.resourceRichness * 0.8f;
            if (tile.hasRiver)  score += 0.8f;
            if (tile.terrain == TerrainType::Hill) score += 0.5f;
            if (isMountain)     score += 0.3f;  // mountain strongholds are valuable
            if (isCoast)        score += 0.6f;  // coastal ports are valuable
            // Distance weight scales with territory size: fewer kingdoms → larger
            // territories → cities must spread further to cover them meaningfully.
            //   ≤6 kingdoms: max +7.0 at 70 tiles — spread dominates
            //  ≤12 kingdoms: max +3.3 at 55 tiles — moderate spread
            //   20 kingdoms: max +1.2 at 40 tiles — unchanged original behaviour
            const int totalKingdoms = static_cast<int>(world.kingdoms.size());
            const float distWeight = (totalKingdoms <= 6) ? 0.10f
                                   : (totalKingdoms <= 12) ? 0.06f
                                   : 0.03f;
            const float distCap    = (totalKingdoms <= 6) ? 70.0f
                                   : (totalKingdoms <= 12) ? 55.0f
                                   : 40.0f;
            score += std::min(dist, distCap) * distWeight;
            candidates.push_back({tile.id, score, isMountain});
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.score > b.score;
                  });

        // Denser maps: more small strongpoints make supply/frontline warfare legible.
        const int targetSettlements = rng_.nextInt(5, 8);
        const float minSpacing = 9.0f;  // min distance between any two cities

        int settlements = 0;
        for (const Candidate& c : candidates) {
            if (settlements >= targetSettlements) break;

            bool tooClose = false;
            auto pos = world.worldMap.at(c.tile).position;
            for (CityID existing : kingdom.cities) {
                if (!world.cities.count(existing)) continue;
                auto ep = world.cities.at(existing).position;
                float dist = std::hypot(float(pos.x - ep.x), float(pos.y - ep.y));
                if (dist < minSpacing) { tooClose = true; break; }
            }
            // Also check other kingdoms' cities to avoid overlap on edges
            for (const auto& [okid, ok] : world.kingdoms) {
                if (okid == kid) continue;
                for (CityID existing : ok.cities) {
                    if (!world.cities.count(existing)) continue;
                    auto ep = world.cities.at(existing).position;
                    float dist = std::hypot(float(pos.x - ep.x), float(pos.y - ep.y));
                    if (dist < 6.0f) { tooClose = true; break; }
                }
                if (tooClose) break;
            }
            if (tooClose) continue;

            // Multiple regional strongpoints, plus mountain pass forts.
            const bool stronghold = (settlements < 2) || c.mountain;
            City city = buildSettlement(world, kingdom, c.tile, stronghold);
            CityID sid = world.nextCityID++;
            city.id = sid;

            world.worldMap.at(c.tile).city = sid;
            kingdom.cities.push_back(sid);
            kingdom.totalPopulation += city.population;
            world.cities[sid] = std::move(city);
            ++settlements;
        }
    }

    // Second pass: neutral strategic outposts (river crossings, mountain passes).
    // Keep city density close to the 20-kingdom baseline even when fewer kingdoms start.
    placeNeutralOutposts(world);
}

void CityGenerator::placeNeutralOutposts(GeneratedWorld& world) {
    // Scan for strategic tiles: river+coast intersections, mountain passes
    struct OutpostSite {
        TileID tile  = NO_TILE;
        float  score = 0.0f;
    };
    std::vector<OutpostSite> sites;

    for (const auto& tile : world.worldMap.tiles()) {
        if (tile.owner != NO_KINGDOM) continue;
        if (tile.city  != NO_CITY)   continue;
        if (tile.terrain == TerrainType::Ocean ||
            tile.terrain == TerrainType::Lake)   continue;

        // Large random jitter ensures outposts spread across the whole map.
        // Terrain bonus is a tiebreaker, not a dominant factor.
        float score = rng_.nextFloat(0.0f, 10.0f);
        if (tile.hasRiver || tile.terrain == TerrainType::River) score += 2.0f;
        if (tile.terrain == TerrainType::Coast)    score += 1.5f;
        if (tile.terrain == TerrainType::Mountain) score += 1.2f;
        if (tile.terrain == TerrainType::Hill)     score += 0.8f;
        if (tile.terrain == TerrainType::Forest)   score += 0.6f;
        if (tile.fertility > 1.5f)                 score += 0.5f;
        else if (tile.fertility > 0.8f)            score += 0.2f;

        // Must be at least 12 tiles from any existing city
        bool tooClose = false;
        for (const auto& [cid, city] : world.cities) {
            float d = std::hypot(float(tile.position.x - city.position.x),
                                 float(tile.position.y - city.position.y));
            if (d < 9.0f) { tooClose = true; break; }
        }
        if (tooClose) continue;

        sites.push_back({tile.id, score});
    }

    std::sort(sites.begin(), sites.end(),
              [](const OutpostSite& a, const OutpostSite& b){ return a.score > b.score; });

    // Keep the total number of conquerable city/outpost anchors stable across
    // different starting kingdom counts.
    const int baselineTotalAnchors = constants::NUM_KINGDOMS * 8 + 200;
    const int targetOutposts = std::max(
        0, baselineTotalAnchors - static_cast<int>(world.cities.size()));

    // Place neutral outposts, spaced enough to create local objectives.
    int placed = 0;
    std::unordered_map<int, int> terrainCounter; // per-terrain name index
    std::vector<Coordinate> placedPos;
    for (const auto& s : sites) {
        if (placed >= targetOutposts) break;
        auto pos = world.worldMap.at(s.tile).position;

        bool tooClose = false;
        for (const auto& p : placedPos) {
            if (std::hypot(float(pos.x - p.x), float(pos.y - p.y)) < 8.0f) {
                tooClose = true; break;
            }
        }
        if (tooClose) continue;

        const Tile& t = world.worldMap.at(s.tile);
        City outpost;
        outpost.owner         = NO_KINGDOM;
        outpost.originalOwner = NO_KINGDOM;
        outpost.tile          = s.tile;
        outpost.position      = pos;
        outpost.isCapital     = false;
        outpost.foundedTurn   = 0;
        const int terrainKey  = static_cast<int>(t.terrain);
        outpost.name          = generateOutpostName(t.terrain, terrainCounter[terrainKey]++);

        outpost.population    = rng_.nextInt(120, 420);
        outpost.baseProduction.food  = 5.0f  * t.fertility;
        outpost.baseProduction.gold  = 2.5f;
        outpost.baseProduction.wood  = (t.terrain == TerrainType::Forest) ? 8.0f : 2.0f;
        outpost.baseProduction.iron  = t.resourceRichness * 3.0f;
        outpost.baseProduction.stone = (t.terrain == TerrainType::Mountain ||
                                        t.terrain == TerrainType::Hill) ? 7.0f : 2.0f;
        outpost.happiness      = 0.50f;
        outpost.fortification  = 0.45f + rng_.nextFloat(0.0f, 0.30f);
        outpost.buildings.push_back({BuildingType::Walls,    1, 1.0f});
        outpost.buildings.push_back({BuildingType::Barracks, 1, 1.0f});
        assignCityType(outpost, t, world, s.tile);

        CityID cid = world.nextCityID++;
        outpost.id = cid;
        world.worldMap.at(s.tile).owner = NO_KINGDOM;
        world.worldMap.at(s.tile).city = cid;
        world.cities[cid] = std::move(outpost);
        placedPos.push_back(pos);
        ++placed;
    }
}

City CityGenerator::buildCapital(GeneratedWorld& world, Kingdom& kingdom,
                                  TileID tile) const {
    City city;
    city.owner         = kingdom.id;
    city.originalOwner = kingdom.id;
    city.tile          = tile;
    city.position      = world.worldMap.at(tile).position;
    city.isCapital     = true;
    city.foundedTurn   = 0;
    city.name          = generateCityName(kingdom.name, true);

    // Starting population by specialization
    switch (kingdom.specialization) {
        case KingdomSpecialization::Agriculture:
            city.population = 4000; break;
        case KingdomSpecialization::Economy:
        case KingdomSpecialization::Trade:
            city.population = 3500; break;
        default:
            city.population = 3000; break;
    }

    // Base production from tile
    const Tile& t = world.worldMap.at(tile);
    city.baseProduction.food  = 30.0f * t.fertility;
    city.baseProduction.gold  = 15.0f;
    city.baseProduction.wood  = (t.terrain == TerrainType::Forest) ? 20.0f : 8.0f;
    city.baseProduction.stone = (t.terrain == TerrainType::Hill || t.terrain == TerrainType::Mountain) ? 18.0f : 6.0f;
    city.baseProduction.iron  = t.resourceRichness * 8.0f;

    city.happiness     = 0.80f;
    city.fortification = 0.45f;  // capitals start well-defended

    // Assign city type from terrain + surroundings, then boost production
    assignCityType(city, t, world, tile);
    addStartingBuildings(city, kingdom.specialization);
    return city;
}

City CityGenerator::buildSettlement(GeneratedWorld& world, Kingdom& kingdom,
                                    TileID tile, bool stronghold) const {
    City city;
    city.owner         = kingdom.id;
    city.originalOwner = kingdom.id;
    city.tile          = tile;
    city.position      = world.worldMap.at(tile).position;
    city.isCapital     = false;
    city.foundedTurn   = 0;
    city.name          = generateCityName(kingdom.name, false);

    const Tile& t = world.worldMap.at(tile);
    city.population = stronghold ? rng_.nextInt(700, 1150)
                                 : rng_.nextInt(320, 720);

    city.baseProduction.food  = (stronghold ? 10.0f : 12.0f) * t.fertility;
    city.baseProduction.gold  = stronghold ? 5.0f : 3.5f;
    city.baseProduction.wood  = (t.terrain == TerrainType::Forest) ? 8.0f : 3.0f;
    city.baseProduction.stone = (t.terrain == TerrainType::Hill) ? 10.0f : 3.0f;
    city.baseProduction.iron  = t.resourceRichness * (stronghold ? 4.5f : 3.0f);

    city.happiness = 0.72f;
    city.fortification = stronghold ? 0.48f : 0.12f;

    // Assign city type, then apply type bonuses
    assignCityType(city, t, world, tile);

    if (stronghold) {
        city.buildings.push_back({BuildingType::Walls, 1, 1.0f});
        city.buildings.push_back({BuildingType::Barracks, 1, 1.0f});
    } else if (t.terrain == TerrainType::Forest) {
        city.buildings.push_back({BuildingType::LumberMill, 1, 1.0f});
    } else if (t.terrain == TerrainType::Hill) {
        city.buildings.push_back({BuildingType::Quarry, 1, 1.0f});
    } else {
        city.buildings.push_back({BuildingType::Farm, 1, 1.0f});
    }

    return city;
}

void CityGenerator::addStartingBuildings(City& city, KingdomSpecialization spec) const {
    // Every kingdom starts with a Farm and Market
    city.buildings.push_back({BuildingType::Farm,   1, 1.0f});
    city.buildings.push_back({BuildingType::Market, 1, 1.0f});

    // Specialization-specific starting buildings
    switch (spec) {
        case KingdomSpecialization::Military:
            city.buildings.push_back({BuildingType::Barracks, 1, 1.0f});
            city.buildings.push_back({BuildingType::Walls,    1, 1.0f});
            break;
        case KingdomSpecialization::Economy:
            city.buildings.push_back({BuildingType::Market, 1, 1.0f}); // second market
            city.buildings.push_back({BuildingType::Workshop, 1, 1.0f});
            break;
        case KingdomSpecialization::Agriculture:
            city.buildings.push_back({BuildingType::Farm,    1, 1.0f}); // second farm
            city.buildings.push_back({BuildingType::Granary, 1, 1.0f});
            break;
        case KingdomSpecialization::Technology:
            city.buildings.push_back({BuildingType::Library, 1, 1.0f});
            city.buildings.push_back({BuildingType::Workshop, 1, 1.0f});
            break;
        case KingdomSpecialization::Trade:
            city.buildings.push_back({BuildingType::Market, 1, 1.0f});
            break;
        case KingdomSpecialization::Defense:
            city.buildings.push_back({BuildingType::Walls,   1, 1.0f});
            city.buildings.push_back({BuildingType::Fortress, 1, 1.0f});
            city.fortification = 0.35f;
            break;
    }
}

void CityGenerator::assignCityType(City& city, const Tile& t,
                                    const GeneratedWorld& world, TileID tile) const {
    // Check neighbours for coastal proximity
    bool nearCoast = (t.terrain == TerrainType::Coast);
    if (!nearCoast) {
        for (TileID nb : world.worldMap.neighbors4v(tile)) {
            if (world.worldMap.at(nb).terrain == TerrainType::Coast ||
                world.worldMap.at(nb).terrain == TerrainType::Ocean) {
                nearCoast = true;
                break;
            }
        }
    }

    const bool highResource = t.resourceRichness > 0.6f;
    const bool onRiver      = t.hasRiver ||
                              t.terrain == TerrainType::River;
    const bool hillOrMtn    = t.terrain == TerrainType::Hill ||
                              t.terrain == TerrainType::Mountain;
    const bool fertile      = t.fertility > 0.7f || onRiver;

    // Precedence: Fortress > Port > Mining > Agricultural > TradeHub > Generic
    if (hillOrMtn && city.fortification >= 0.35f) {
        city.cityType        = CityType::Fortress;
        city.fortification   = std::min(1.0f, city.fortification + 0.20f);
        city.baseProduction.stone *= 1.5f;
        city.baseProduction.iron  *= 1.3f;
        city.buildings.push_back({BuildingType::Walls, 1, 1.0f});
    } else if (nearCoast) {
        city.cityType = CityType::Port;
        city.baseProduction.gold  *= 1.6f;
        city.baseProduction.food  *= 0.85f;
        city.buildings.push_back({BuildingType::Market, 1, 1.0f});
    } else if (highResource) {
        city.cityType = CityType::Mining;
        city.baseProduction.iron  *= 2.0f;
        city.baseProduction.stone *= 1.8f;
        city.buildings.push_back({BuildingType::IronMine, 1, 1.0f});
    } else if (fertile) {
        city.cityType = CityType::Agricultural;
        city.baseProduction.food  *= 1.8f;
        city.population = static_cast<uint32_t>(city.population * 1.15f);
        city.buildings.push_back({BuildingType::Granary, 1, 1.0f});
    } else if (city.isCapital) {
        city.cityType = CityType::TradeHub;
        city.baseProduction.gold *= 1.4f;
        city.happiness = std::min(1.0f, city.happiness + 0.05f);
    }
    // else: Generic — no change
}

std::string CityGenerator::generateCityName(const std::string& kingdomName,
                                              bool isCapital) const {
    // Per-kingdom naming style — lore-driven city names
    // Capital uses the kingdom name itself; settlements use themed prefix+suffix
    struct KingdomNaming {
        std::array<const char*, 5> prefixes;
        std::array<const char*, 5> suffixes;
    };
    static const std::unordered_map<std::string, KingdomNaming> table = {
        // Noble trade empire
        {"Valdoria",    {{"Golden","Noble","Crown","Grand","High"},     {"court","hall","vale","reach","shire"}}},
        // Iron military empire expanding outward
        {"Ironspire",   {{"Steel","Forge","Iron","Anvil","Smelter"},    {"hold","keep","bastion","gate","burg"}}},
        // Mystical research nation on foggy moors
        {"Aethermoor",  {{"Aether","Mist","Veil","Arcane","Ether"},     {"spire","haven","moor","reach","sanctum"}}},
        // Warm agricultural nation, peaceful
        {"Sunhaven",    {{"Sun","Gold","Bright","Dawn","Warm"},         {"field","haven","meadow","lea","glow"}}},
        // Dark conquering thorn kingdom
        {"Blackthorn",  {{"Dark","Grim","Shadow","Iron","Blood"},       {"thorn","fell","hollow","pit","scar"}}},
        // Merchant nation trading everything
        {"Goldenveil",  {{"Gold","Silk","Silver","Amber","Veil"},       {"veil","port","market","cove","crossing"}}},
        // Fortress nation, stone walls everywhere
        {"Stonegate",   {{"Stone","Grey","Iron","Rock","Flint"},        {"gate","wall","ward","keep","pass"}}},
        // Peaceful river farming nation
        {"Rivermark",   {{"River","Mill","Ford","Bridge","Tide"},       {"ford","bridge","bank","weir","crossing"}}},
        // Opportunistic survivors of a fallen kingdom
        {"Ashford",     {{"Ash","Ember","Cinder","Dust","Grey"},        {"ford","hollow","ruin","wick","remnant"}}},
        // Precision technology island nation
        {"Crystalholm", {{"Crystal","Glass","Prism","Gem","Quartz"},    {"holm","spire","peak","point","shard"}}},
        // Vast fire military empire
        {"Embervast",   {{"Ember","Flame","Blaze","Char","Singe"},      {"vast","forge","march","hearth","kiln"}}},
        // Expansionist agricultural frontier nation
        {"Dawnreach",   {{"Dawn","First","New","Far","East"},           {"reach","land","plain","step","hold"}}},
        // Isolationist thorn fortress nation
        {"Thornwall",   {{"Thorn","Spike","Briar","Barb","Hook"},       {"wall","keep","ward","hedge","barrier"}}},
        // Silver trade nation with sharp business sense
        {"Silvershard", {{"Silver","Bright","Clear","White","Moon"},    {"shard","port","croft","bay","quay"}}},
        // Proud highland expansionist kingdom
        {"Highcrest",   {{"High","Peak","Crown","Summit","Ridge"},      {"crest","mount","ridge","top","heights"}}},
        // Fog forest opportunist nation
        {"Mistwood",    {{"Mist","Fog","Shadow","Shroud","Grey"},       {"wood","grove","hollow","copse","thicket"}}},
        // Twilight survivor kingdom, grasping at any opportunity
        {"Duskfell",    {{"Dusk","Twilight","Grey","Fading","Last"},    {"fell","heath","moor","end","watch"}}},
        // Ambitious crown-hungry economic empire
        {"Crownsreach", {{"Crown","Royal","Grand","Imperial","Sovereign"},{"reach","hall","court","palace","seat"}}},
        // Hollow remnant of an ancient iron military power
        {"Ironhollow",  {{"Iron","Rust","Old","Ancient","Hollow"},      {"hollow","ruin","hold","depths","vault"}}},
        // Aggressive border nation, always at war
        {"Embermarch",  {{"Ember","Scar","Ash","Char","Burnt"},         {"march","post","watch","outpost","front"}}},
    };

    if (isCapital) return kingdomName;

    auto it = table.find(kingdomName);
    if (it == table.end()) {
        static const std::string fallback[] = {"ford","haven","wick","gate","keep","hold","burg","vale","moor","fell"};
        return kingdomName.substr(0, 4) + fallback[rng_.nextInt(0, 9)];
    }
    const auto& n = it->second;
    return std::string(n.prefixes[rng_.nextInt(0, 4)]) + n.suffixes[rng_.nextInt(0, 4)];
}

std::string CityGenerator::generateOutpostName(TerrainType terrain, int index) const {
    // 20 unique names per category; sequential index prevents duplicates within each run.
    // If more than 20 of one type are ever needed, wrap with a roman-numeral suffix.
    static const std::string mountain[] = {
        "Stonepass",  "Ironpeak",    "Greyspire",   "Rimwatch",   "Frostcrag",
        "Duskwall",   "Shadowcleft", "Ashpeak",      "Thornspire", "Coldgap",
        "Frostgate",  "Icepeak",     "Steelcliff",  "Granitecrest","Bleakhold",
        "Cobaltridge","Stormcrag",   "Windpass",    "Ironwall",   "Dreadspire"
    };
    static const std::string coast[] = {
        "Saltwatch",  "Ironharbor",  "Seagate",     "Wavebreak",  "Tidemark",
        "Drifthaven", "Gullrock",    "Brinecroft",  "Foamhold",   "Stormcove",
        "Shorehold",  "Breakwater",  "Mireport",    "Cliffwatch", "Sandgate",
        "Deepwater",  "Harborwall",  "Mistport",    "Cragcove",   "Swellbreak"
    };
    static const std::string river[] = {
        "Fordkeep",   "Bridgewatch", "Crossgate",   "Millpoint",  "Rushgate",
        "Floodstop",  "Shallows",    "Weirdford",   "Forkhold",   "Tidegate",
        "Riverstop",  "Bankwatch",   "Stoneferry",  "Mudcross",   "Quickford",
        "Longbridge", "Reedgate",    "Swiftpass",   "Brookhold",  "Driftford"
    };
    static const std::string forest[] = {
        "Deepwood",   "Timberhold",  "Greenwatch",  "Mossgate",   "Briarpost",
        "Fernkeep",   "Ashgrove",    "Oakwatch",    "Thornwood",  "Willowfort",
        "Darkbough",  "Pinecrest",   "Elmstop",     "Cedarhold",  "Birchwall",
        "Ivygate",    "Hazelkeep",   "Rootwatch",   "Barkfort",   "Canopypost"
    };
    static const std::string plain[] = {
        "Waypost",    "Redoubt",     "Bulwark",     "Bastion",    "Rampart",
        "Stockade",   "Watchtower",  "Garrison",    "Outpost",    "Lookout",
        "Fieldfort",  "Dusthold",    "Plainwatch",  "Dirtgate",   "Grasskeep",
        "Ridgepost",  "Hillock",     "Knollwatch",  "Meadowfort", "Cropwatch"
    };

    const std::string* pool = plain;
    int poolSize = 20;
    switch (terrain) {
        case TerrainType::Mountain:
        case TerrainType::Hill:   pool = mountain; break;
        case TerrainType::Coast:  pool = coast;    break;
        case TerrainType::River:  pool = river;    break;
        case TerrainType::Forest: pool = forest;   break;
        default:                  pool = plain;    break;
    }
    const int slot = index % poolSize;
    if (index < poolSize) return pool[slot];
    // Wrap suffix for extreme outpost counts (unlikely but safe)
    static const char* suffix[] = {"II","III","IV","V","VI","VII","VIII","IX","X"};
    const int wrap = std::min(index / poolSize - 1, 8);
    return pool[slot] + " " + suffix[wrap];
}

} // namespace jke
