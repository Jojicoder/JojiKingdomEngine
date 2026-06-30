#include "jke/SimulationEngine.hpp"
#include "jke/history/HistoryEvent.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct InitialKingdomStats {
    int cities = 0;
    int armies = 0;
    float capitalCenterDistance = 0.0f;
};

struct RunResult {
    uint64_t seed = 0;
    uint32_t turns = 0;
    std::string winnerName = "NONE";
    std::string personality = "NONE";
    std::string specialization = "NONE";
    int winnerInitialCities = 0;
    int winnerInitialArmies = 0;
    float winnerInitialCenterDistance = 0.0f;
    int finalCities = 0;
    uint32_t finalPopulation = 0;
    int aliveKingdoms = 0;
    int wars = 0;
    int battles = 0;
    int conquests = 0;
    int rebellions = 0;
    int collapses = 0;
    bool unified = false;
};

std::string csvEscape(std::string_view value) {
    std::string out;
    bool quote = false;
    for (char ch : value) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
            quote = true;
        }
        if (ch == '"') out.push_back('"');
        out.push_back(ch);
    }
    if (!quote) return out;
    return "\"" + out + "\"";
}

void printHelp(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --seeds <N>        Number of seeds to run (default: 50)\n"
        << "  --start-seed <N>   First seed (default: 1)\n"
        << "  --turns <N>        Max turns per run (default: 2000)\n"
        << "  --output <path>    CSV output path (default: balance.csv)\n"
        << "  --quiet            Only write CSV, no aggregate summary\n"
        << "  --help             Show this help\n";
}

RunResult runOne(uint64_t seed, uint32_t turns) {
    jke::SimulationConfig config;
    config.seed = seed;
    config.maxTurns = turns;
    config.verbose = false;

    jke::SimulationEngine engine(config);
    engine.initializeWorld_pub();

    std::unordered_map<jke::KingdomID, InitialKingdomStats> initial;
    const auto& initialMap = engine.worldMap();
    const float centerX = static_cast<float>(initialMap.width() - 1) * 0.5f;
    const float centerY = static_cast<float>(initialMap.height() - 1) * 0.5f;

    for (const auto& [kid, kingdom] : engine.kingdoms()) {
        InitialKingdomStats stats;
        stats.cities = static_cast<int>(kingdom.cities.size());
        stats.armies = static_cast<int>(kingdom.armies.size());
        if (kingdom.capitalCity != jke::NO_CITY && engine.cities().count(kingdom.capitalCity)) {
            const auto pos = engine.cities().at(kingdom.capitalCity).position;
            stats.capitalCenterDistance =
                std::hypot(static_cast<float>(pos.x) - centerX,
                           static_cast<float>(pos.y) - centerY);
        }
        initial[kid] = stats;
    }

    while (engine.step()) {}

    RunResult result;
    result.seed = seed;
    result.turns = engine.currentTurn();

    for (const auto& event : engine.timeline().all()) {
        switch (event.type) {
            case jke::EventType::WarDeclared:      ++result.wars; break;
            case jke::EventType::BattleFought:     ++result.battles; break;
            case jke::EventType::CityConquered:    ++result.conquests; break;
            case jke::EventType::RebellionStarted: ++result.rebellions; break;
            case jke::EventType::KingdomCollapsed: ++result.collapses; break;
            case jke::EventType::ContinentUnified: result.unified = true; break;
            default: break;
        }
    }

    const jke::Kingdom* winner = nullptr;
    for (const auto& [kid, kingdom] : engine.kingdoms()) {
        if (!kingdom.isAlive) continue;
        ++result.aliveKingdoms;
        if (winner == nullptr ||
            kingdom.cities.size() > winner->cities.size() ||
            (kingdom.cities.size() == winner->cities.size() &&
             kingdom.totalPopulation > winner->totalPopulation)) {
            winner = &kingdom;
        }
    }

    if (winner != nullptr) {
        result.winnerName = winner->name;
        result.personality = std::string(jke::personalityName(winner->personality));
        result.specialization = std::string(jke::specializationName(winner->specialization));
        result.finalCities = static_cast<int>(winner->cities.size());
        result.finalPopulation = winner->totalPopulation;
        if (initial.count(winner->id)) {
            result.winnerInitialCities = initial.at(winner->id).cities;
            result.winnerInitialArmies = initial.at(winner->id).armies;
            result.winnerInitialCenterDistance = initial.at(winner->id).capitalCenterDistance;
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv) {
    uint64_t startSeed = 1;
    uint32_t seedCount = 50;
    uint32_t turns = 2000;
    std::filesystem::path output = "balance.csv";
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp(argv[0]);
            return 0;
        } else if (arg == "--seeds" && i + 1 < argc) {
            seedCount = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--start-seed" && i + 1 < argc) {
            startSeed = std::stoull(argv[++i]);
        } else if (arg == "--turns" && i + 1 < argc) {
            turns = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printHelp(argv[0]);
            return 1;
        }
    }

    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path());
    }

    std::ofstream csv(output);
    if (!csv) {
        std::cerr << "Failed to open output: " << output << "\n";
        return 1;
    }

    csv << "seed,turns,winner,personality,specialization,"
        << "winner_initial_cities,winner_initial_armies,winner_initial_center_distance,"
        << "final_cities,final_population,alive_kingdoms,unified,"
        << "wars,battles,conquests,rebellions,collapses\n";

    std::map<std::string, int> winsByPersonality;
    std::map<std::string, int> winsBySpecialization;
    int unifiedRuns = 0;
    uint64_t totalTurns = 0;
    uint64_t totalRebellions = 0;
    std::vector<RunResult> results;
    results.reserve(seedCount);

    for (uint32_t i = 0; i < seedCount; ++i) {
        const uint64_t seed = startSeed + i;
        RunResult result = runOne(seed, turns);
        results.push_back(result);

        winsByPersonality[result.personality]++;
        winsBySpecialization[result.specialization]++;
        unifiedRuns += result.unified ? 1 : 0;
        totalTurns += result.turns;
        totalRebellions += result.rebellions;

        csv << result.seed << ','
            << result.turns << ','
            << csvEscape(result.winnerName) << ','
            << csvEscape(result.personality) << ','
            << csvEscape(result.specialization) << ','
            << result.winnerInitialCities << ','
            << result.winnerInitialArmies << ','
            << result.winnerInitialCenterDistance << ','
            << result.finalCities << ','
            << result.finalPopulation << ','
            << result.aliveKingdoms << ','
            << (result.unified ? 1 : 0) << ','
            << result.wars << ','
            << result.battles << ','
            << result.conquests << ','
            << result.rebellions << ','
            << result.collapses << '\n';

        if (!quiet) {
            std::cout << "seed " << seed << ": "
                      << result.winnerName << " / " << result.personality
                      << " / turn " << result.turns << "\n";
        }
    }

    if (!quiet) {
        std::cout << "\n=== Balance Summary ===\n";
        std::cout << "Runs: " << seedCount << "\n";
        std::cout << "Unified: " << unifiedRuns << "/" << seedCount << "\n";
        std::cout << "Avg turns: " << (seedCount ? totalTurns / seedCount : 0) << "\n";
        std::cout << "Avg rebellions: " << (seedCount ? totalRebellions / seedCount : 0) << "\n";

        std::cout << "\nWins by personality:\n";
        for (const auto& [personality, wins] : winsByPersonality) {
            std::cout << "  " << personality << ": " << wins << "\n";
        }

        std::cout << "\nWins by specialization:\n";
        for (const auto& [specialization, wins] : winsBySpecialization) {
            std::cout << "  " << specialization << ": " << wins << "\n";
        }

        std::cout << "\nCSV: " << output << "\n";
    }

    return 0;
}
