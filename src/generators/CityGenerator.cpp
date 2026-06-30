#include "jke/generators/CityGenerator.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

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
            score += std::min(dist, 40.0f) * 0.03f;
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

    // Second pass: neutral strategic outposts (river crossings, mountain passes)
    // These start unclaimed and become early-game conquest targets
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
        if (tile.owner != NO_KINGDOM) continue;   // must be unclaimed
        if (tile.city  != NO_CITY)   continue;
        if (tile.terrain == TerrainType::Ocean ||
            tile.terrain == TerrainType::Lake)   continue;

        float score = 0.0f;
        // Value: river crossings, coasts, and mountain passes
        if (tile.hasRiver || tile.terrain == TerrainType::River) score += 2.0f;
        if (tile.terrain == TerrainType::Coast)    score += 1.5f;
        if (tile.terrain == TerrainType::Mountain) score += 1.2f;
        if (tile.terrain == TerrainType::Hill)     score += 0.8f;
        if (tile.fertility > 1.5f)                 score += 0.5f;
        if (score < 1.0f) continue;

        // Must be at least 12 tiles from any existing city
        bool tooClose = false;
        for (const auto& [cid, city] : world.cities) {
            float d = std::hypot(float(tile.position.x - city.position.x),
                                 float(tile.position.y - city.position.y));
            if (d < 12.0f) { tooClose = true; break; }
        }
        if (tooClose) continue;

        sites.push_back({tile.id, score});
    }

    std::sort(sites.begin(), sites.end(),
              [](const OutpostSite& a, const OutpostSite& b){ return a.score > b.score; });

    // Place more neutral outposts, spaced enough to create local objectives.
    int placed = 0;
    std::vector<Coordinate> placedPos;
    for (const auto& s : sites) {
        if (placed >= 36) break;
        auto pos = world.worldMap.at(s.tile).position;

        bool tooClose = false;
        for (const auto& p : placedPos) {
            if (std::hypot(float(pos.x - p.x), float(pos.y - p.y)) < 14.0f) {
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
        outpost.name          = generateOutpostName(t.terrain);
        outpost.population    = rng_.nextInt(120, 420);
        outpost.baseProduction.food  = 5.0f  * t.fertility;
        outpost.baseProduction.gold  = 2.5f;
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
    if (isCapital) return kingdomName + " City";
    static const std::string suffixes[] = {
        "ford", "haven", "wick", "gate", "keep",
        "hold", "burg", "vale", "moor", "fell"
    };
    return kingdomName.substr(0, 4) + suffixes[rng_.nextInt(0, 9)];
}

std::string CityGenerator::generateOutpostName(TerrainType terrain) const {
    static const std::string mountain[] = {
        "Stonepass", "Ironpeak", "Greyspire", "Rimwatch", "Frostcrag",
        "Duskwall", "Shadowcleft", "Ashpeak", "Thornspire", "Coldgap"
    };
    static const std::string coast[] = {
        "Saltwatch", "Ironharbor", "Seagate", "Wavebreak", "Tidemark",
        "Drifthaven", "Gullrock", "Brinecroft", "Foamhold", "Stormcove"
    };
    static const std::string river[] = {
        "Fordkeep", "Bridgewatch", "Crossgate", "Millpoint", "Rivermark",
        "Floodstop", "Shallows", "Weirdford", "Rushgate", "Forkhold"
    };
    static const std::string generic[] = {
        "Outpost", "Waypost", "Redoubt", "Bulwark", "Bastion",
        "Rampart", "Stockade", "Citadel", "Watchtower", "Garrison"
    };
    switch (terrain) {
        case TerrainType::Mountain: return mountain[rng_.nextInt(0, 9)];
        case TerrainType::Hill:     return mountain[rng_.nextInt(0, 9)];
        case TerrainType::Coast:    return coast[rng_.nextInt(0, 9)];
        case TerrainType::River:    return river[rng_.nextInt(0, 9)];
        default:                    return generic[rng_.nextInt(0, 9)];
    }
}

} // namespace jke
