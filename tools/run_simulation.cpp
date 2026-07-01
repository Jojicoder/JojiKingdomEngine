#include "jke/SimulationEngine.hpp"
#include "jke/output/SnapshotSerializer.hpp"
#include <iostream>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <filesystem>

static void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [options]\n"
              << "Options:\n"
              << "  --seed <N>       Random seed (default: 42)\n"
              << "  --turns <N>      Max simulation turns; 0 = unlimited until unification (default: 2000)\n"
              << "  --output <dir>   Output directory for JSON snapshots (default: output)\n"
              << "  --single         Write single output.json instead of per-turn files\n"
              << "  --no-output      Run simulation without writing JSON snapshots\n"
              << "  --verbose        Print turn summaries to stdout\n"
              << "  --help           Show this help\n";
}

int main(int argc, char** argv) {
    jke::SimulationConfig config;
    bool singleFile = false;
    bool noOutput = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp(argv[0]);
            return 0;
        } else if (arg == "--seed" && i + 1 < argc) {
            config.seed = std::stoull(argv[++i]);
        } else if (arg == "--turns" && i + 1 < argc) {
            config.maxTurns = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--output" && i + 1 < argc) {
            config.outputDir = argv[++i];
        } else if (arg == "--single") {
            singleFile = true;
        } else if (arg == "--no-output") {
            noOutput = true;
        } else if (arg == "--verbose" || arg == "-v") {
            config.verbose = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printHelp(argv[0]);
            return 1;
        }
    }

    std::cout << "JojiKingdomEngine v1.0\n";
    std::cout << "Seed: " << config.seed << "  Turns: ";
    if (config.maxTurns == 0) std::cout << "unlimited";
    else std::cout << config.maxTurns;
    std::cout << "\n\n";

    jke::SimulationEngine engine(config);

    if (noOutput) {
        std::cout << "Output: disabled\n";
    } else if (singleFile) {
        std::string outPath = config.outputDir + "/simulation.json";
        std::filesystem::create_directories(config.outputDir);
        engine.setSerializer(
            std::make_unique<jke::SingleFileJsonSerializer>(outPath));
        std::cout << "Output: " << outPath << "\n";
    } else {
        engine.setSerializer(
            std::make_unique<jke::JsonSnapshotSerializer>(config.outputDir));
        std::cout << "Output directory: " << config.outputDir << "\n";
    }

    std::cout << "\n";
    engine.run();

    // Print final history summary
    const jke::Timeline& tl = engine.timeline();

    std::cout << "\n=== Historical Record ===\n";
    std::cout << "Total recorded events: " << tl.all().size() << "\n\n";

    // Count event types
    int wars      = 0, battles = 0, collapses = 0, conquests = 0, rebellions = 0;
    for (const auto& ev : tl.all()) {
        switch (ev.type) {
            case jke::EventType::WarDeclared:      wars++;       break;
            case jke::EventType::BattleFought:     battles++;    break;
            case jke::EventType::KingdomCollapsed: collapses++;  break;
            case jke::EventType::CityConquered:    conquests++;  break;
            case jke::EventType::RebellionStarted: rebellions++; break;
            default: break;
        }
    }
    std::cout << "Wars declared:      " << wars      << "\n";
    std::cout << "Battles fought:     " << battles   << "\n";
    std::cout << "Cities conquered:   " << conquests << "\n";
    std::cout << "Rebellions:         " << rebellions<< "\n";
    std::cout << "Kingdoms collapsed: " << collapses << "\n\n";

    // Print significant events
    std::cout << "--- Notable Events ---\n";
    for (const auto& ev : tl.all()) {
        if (ev.type == jke::EventType::KingdomCollapsed   ||
            ev.type == jke::EventType::ContinentUnified   ||
            ev.type == jke::EventType::CivilWar           ||
            ev.type == jke::EventType::CapitalCaptured    ||
            ev.type == jke::EventType::AllianceFormed) {
            std::cout << "[Turn " << ev.turn << "] " << ev.description << "\n";
        }
    }

    // Kingdom final standings
    std::cout << "\n--- Final Kingdom Standings ---\n";
    const auto& kingdoms = engine.kingdoms();
    std::vector<const jke::Kingdom*> sorted;
    for (const auto& [kid, k] : kingdoms) sorted.push_back(&k);
    std::sort(sorted.begin(), sorted.end(),
              [](const jke::Kingdom* a, const jke::Kingdom* b){
                  return a->totalPopulation > b->totalPopulation;
              });

    for (const auto* k : sorted) {
        std::cout << (k->isAlive ? "[ALIVE]" : "[FALLEN]") << " "
                  << k->name
                  << " | Pop: " << k->totalPopulation
                  << " | Cities: " << k->cities.size()
                  << " | " << jke::personalityName(k->personality)
                  << "\n";
    }

    std::cout << "\nSimulation complete. Turns run: " << engine.currentTurn() << "\n";
    return 0;
}
