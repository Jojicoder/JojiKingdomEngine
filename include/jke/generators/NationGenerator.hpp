#pragma once
#include "jke/generators/WorldGenerator.hpp"

namespace jke {

class NationGenerator {
public:
    explicit NationGenerator(Random& rng);
    void generate(GeneratedWorld& world);

private:
    Random& rng_;

    std::vector<TileID> pickCapitalSites(const WorldMap& map, int count) const;
    void assignTerritories(GeneratedWorld& world) const;
    void applySpecializationBonuses(Kingdom& k) const;
    KingdomPersonality assignPersonality(int index) const;
    KingdomSpecialization assignSpecialization(int index) const;
    std::string generateKingdomName(int index) const;
};

} // namespace jke
