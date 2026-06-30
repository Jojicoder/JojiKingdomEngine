#pragma once
#include "jke/generators/WorldGenerator.hpp"

namespace jke {

class CityGenerator {
public:
    explicit CityGenerator(Random& rng);
    void generate(GeneratedWorld& world);

private:
    Random& rng_;

    City buildCapital(GeneratedWorld& world, Kingdom& kingdom, TileID tile) const;
    City buildSettlement(GeneratedWorld& world, Kingdom& kingdom, TileID tile,
                         bool stronghold) const;
    void addStartingBuildings(City& city, KingdomSpecialization spec) const;
    void assignCityType(City& city, const Tile& t,
                        const GeneratedWorld& world, TileID tile) const;
    std::string generateCityName(const std::string& kingdomName, bool isCapital) const;
    std::string generateOutpostName(TerrainType terrain) const;
    void placeNeutralOutposts(GeneratedWorld& world);
};

} // namespace jke
