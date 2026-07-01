#include <SFML/Graphics.hpp>
#include "jke/SimulationEngine.hpp"
#include "jke/terrain/TerrainType.hpp"
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>

// ── Layout constants ──────────────────────────────────────────────────────────
constexpr int TILE_PX      = 6;
constexpr int MAP_TILES    = 257;                        // 2^8+1 diamond-square
constexpr int MAP_FULL_PX  = MAP_TILES * TILE_PX;       // 1542 — full map texture
constexpr int VIEWPORT_PX  = 774;                        // visible area on screen (129 tiles)
constexpr int MAP_OX       = 18;
constexpr int WIN_H        = 860;
constexpr int MAP_OY       = 62;
constexpr int PANEL_X      = MAP_OX + VIEWPORT_PX + 22;
constexpr int PANEL_W      = 386;
constexpr int WIN_W        = PANEL_X + PANEL_W + 18;
constexpr int MINIMAP_PX   = 160;                        // minimap display size in panel

// ── Colors ────────────────────────────────────────────────────────────────────
const sf::Color TERRAIN_COL[8] = {
    { 26,  58,  92, 255}, // Ocean
    { 74, 144, 217, 255}, // Coast
    {168, 213, 162, 255}, // Plain
    { 45, 122,  58, 255}, // Forest
    {160, 133, 106, 255}, // Hill
    {136, 136, 136, 255}, // Mountain
    { 91, 155, 213, 255}, // River
    { 44, 120, 115, 255}, // Lake
};

const sf::Color KINGDOM_COL[16] = {
    {220,  60,  60}, {60,  110, 220}, {60,  185,  60}, {220, 165,  30},
    {155,  60, 205}, {30,  185, 185}, {225, 125,  50}, { 90,  90, 140},
    {205,  55, 135}, {55,  205, 205}, {135, 185,  55}, {205, 105,  55},
    {105, 145, 165}, {205, 185,  55}, {145,  85,  55}, {105,  55, 185},
};

const sf::Color BG_COLOR     {14,  17,  24};
const sf::Color TOP_COLOR    {21,  25,  35};
const sf::Color PANEL_COLOR  {25,  30,  40};
const sf::Color SURFACE_COLOR{32,  38,  50};
const sf::Color TEXT_COLOR   {226, 230, 238};
const sf::Color DIM_COLOR    {132, 142, 158};
const sf::Color MUTED_COLOR  {87,  98,  116};
const sf::Color BORDER_COLOR {55,  65,  82};
const sf::Color GOLD_COLOR   {220, 184,  82};

// ── Helpers ───────────────────────────────────────────────────────────────────
static sf::Color blend(sf::Color a, sf::Color b, float t) {
    return {
        static_cast<uint8_t>(a.r + (b.r - a.r) * t),
        static_cast<uint8_t>(a.g + (b.g - a.g) * t),
        static_cast<uint8_t>(a.b + (b.b - a.b) * t),
    };
}

static sf::Color shade(sf::Color c, float amount) {
    auto apply = [&](uint8_t channel) -> uint8_t {
        float v = static_cast<float>(channel);
        if (amount >= 0.0f) {
            v += (255.0f - v) * amount;
        } else {
            v *= (1.0f + amount);
        }
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
    };
    return {apply(c.r), apply(c.g), apply(c.b), c.a};
}

static sf::Color kingdomColor(jke::KingdomID kid) {
    if (kid == jke::NO_KINGDOM) return sf::Color{40, 40, 50};
    return KINGDOM_COL[(kid - 1) % 16];
}

static bool isWaterTerrain(jke::TerrainType terrain) {
    return terrain == jke::TerrainType::Ocean ||
           terrain == jke::TerrainType::Coast ||
           terrain == jke::TerrainType::Lake;
}

static std::string compactNumber(uint64_t value) {
    std::ostringstream out;
    if (value >= 1000000) {
        out << std::fixed << std::setprecision(1) << (static_cast<double>(value) / 1000000.0) << "M";
    } else if (value >= 10000) {
        out << std::fixed << std::setprecision(1) << (static_cast<double>(value) / 1000.0) << "k";
    } else {
        out << value;
    }
    return out.str();
}

static std::string ellipsize(std::string s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);
    return s.substr(0, maxLen - 3) + "...";
}

static const char* terrainEffectText(jke::TerrainType terrain) {
    switch (terrain) {
        case jke::TerrainType::Ocean:    return "Blocked";
        case jke::TerrainType::Coast:    return "Slow, no claim";
        case jke::TerrainType::Plain:    return "Food, cavalry";
        case jke::TerrainType::Forest:   return "Wood, cover";
        case jke::TerrainType::Hill:     return "Stone, defense";
        case jke::TerrainType::Mountain: return "Iron, strong defense";
        case jke::TerrainType::River:    return "Food, border";
        case jke::TerrainType::Lake:     return "Blocked";
    }
    return "";
}

// ── Font loading ──────────────────────────────────────────────────────────────
static sf::Font loadFont() {
    sf::Font f;
    for (const auto& path : {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
    }) {
        if (f.openFromFile(path)) return f;
    }
    std::cerr << "Warning: no system font found — text will be blank.\n";
    return f;
}

// ── Viewer class ──────────────────────────────────────────────────────────────
class Viewer {
public:
    Viewer(jke::SimulationConfig cfg)
        : window_(sf::VideoMode({WIN_W, WIN_H}), "JojiKingdomEngine",
                  sf::Style::Close | sf::Style::Titlebar)
        , engine_(cfg)
        , font_(loadFont())
        , mapTexture_(sf::Vector2u(MAP_FULL_PX, MAP_FULL_PX))
        , mapSprite_(mapTexture_)
        , minimapTex_(sf::Vector2u(MAP_TILES, MAP_TILES))
        , minimapSprite_(minimapTex_)
    {
        window_.setFramerateLimit(60);

        // Map texture starts at world origin (0,0)
        mapSprite_.setPosition({0.f, 0.f});

        // Map view: shows VIEWPORT_PX × VIEWPORT_PX world units
        // centered initially at middle of the full map
        camCenter_ = {MAP_FULL_PX / 2.f, MAP_FULL_PX / 2.f};
        mapView_.setSize({static_cast<float>(VIEWPORT_PX), static_cast<float>(VIEWPORT_PX)});
        mapView_.setCenter(camCenter_);
        mapView_.setViewport(sf::FloatRect(
            {static_cast<float>(MAP_OX) / WIN_W, static_cast<float>(MAP_OY) / WIN_H},
            {static_cast<float>(VIEWPORT_PX) / WIN_W, static_cast<float>(VIEWPORT_PX) / WIN_H}
        ));

        engine_.initializeWorld_pub();
        rebuildMapImage();
    }

    void run() {
        sf::Clock clock;
        float accumulator = 0.f;
        camClock_.restart();

        while (window_.isOpen()) {
            handleEvents();

            // Camera pan with WASD (speed scales with zoom so it feels consistent)
            float dt = camClock_.restart().asSeconds();
            float panSpeed = 500.f / zoom_;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W)) camCenter_.y -= panSpeed * dt;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::S)) camCenter_.y += panSpeed * dt;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A)) camCenter_.x -= panSpeed * dt;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::D)) camCenter_.x += panSpeed * dt;
            clampCamera();

            if (!paused_ && !engine_.isOver()) {
                accumulator += clock.restart().asSeconds();
                float interval = 1.f / static_cast<float>(speed_);
                bool advanced = false;
                while (accumulator >= interval) {
                    accumulator -= interval;
                    engine_.step();
                    advanced = true;
                    if (engine_.isOver()) break;
                }
                if (advanced && shouldRefreshMapImage()) rebuildMapImage();
            } else {
                clock.restart();
            }

            render();
        }
    }

private:
    sf::RenderWindow  window_;
    jke::SimulationEngine engine_;
    sf::Font          font_;

    // Map rendering
    sf::Texture          mapTexture_;
    sf::Sprite           mapSprite_;
    std::vector<uint8_t> pixels_;

    // Scrollable camera + zoom
    sf::View          mapView_;
    sf::Vector2f      camCenter_;
    sf::Clock         camClock_;
    float             zoom_       = 1.0f;   // 1.0 = 129 tiles visible

    float viewHalf() const { return (VIEWPORT_PX / zoom_) * 0.5f; }
    float viewSize() const { return  VIEWPORT_PX / zoom_; }

    void clampCamera() {
        float h    = viewHalf();
        float mapF = static_cast<float>(MAP_FULL_PX);
        if (h * 2.f >= mapF) {
            // View is larger than the map — lock to center
            camCenter_ = {mapF * 0.5f, mapF * 0.5f};
        } else {
            camCenter_.x = std::clamp(camCenter_.x, h, mapF - h);
            camCenter_.y = std::clamp(camCenter_.y, h, mapF - h);
        }
        mapView_.setCenter(camCenter_);
    }

    void applyZoom(float delta, sf::Vector2i mousePixels) {
        constexpr float STEP   = 1.18f;
        // Minimum zoom: never show more than the full map
        const float MIN_Z = static_cast<float>(VIEWPORT_PX) / static_cast<float>(MAP_FULL_PX);
        constexpr float MAX_Z  = 8.0f;
        // Use pow so trackpad micro-deltas give small steps, wheel clicks give full STEP
        float factor = std::pow(STEP, delta);
        float newZoom = std::clamp(zoom_ * factor, MIN_Z, MAX_Z);
        if (newZoom == zoom_) return;

        // World point under cursor — should stay fixed while zooming
        sf::Vector2f worldBefore = window_.mapPixelToCoords(mousePixels, mapView_);
        zoom_ = newZoom;
        mapView_.setSize({viewSize(), viewSize()});

        // Re-derive world point and offset camera so it stays put
        sf::Vector2f worldAfter = window_.mapPixelToCoords(mousePixels, mapView_);
        camCenter_ += worldBefore - worldAfter;
        clampCamera();
    }

    // Minimap
    sf::Texture          minimapTex_;
    sf::Sprite           minimapSprite_;
    std::vector<uint8_t> minimapPixels_;

    bool  paused_ = false;
    int   speed_  = 2;   // turns per second

    // Mouse selection
    jke::TileID     selectedTile_     = jke::NO_TILE;
    sf::FloatRect   minimapScreenRect_;   // filled by drawPanel, used by mouse handler

    // Cached kingdom sort order — rebuilt once per turn, not every frame
    std::vector<const jke::Kingdom*> sortedKingdoms_;
    jke::TurnNumber                  sortedKingdomsTurn_ = ~0u;

    // tileArmy map reused across turns (clear() instead of reallocating)
    struct ArmyMark { jke::KingdomID owner; bool invasion; uint32_t soldiers; };
    std::unordered_map<jke::TileID, ArmyMark> tileArmy_;

    // Visual districts: each land tile is assigned to its nearest living city.
    // Territory tint uses the district city's owner, so a city conquest recolors
    // the surrounding district instead of every marched-through tile.
    std::vector<jke::KingdomID> districtOwner_;
    std::vector<jke::CityID>    districtCity_;
    std::vector<jke::KingdomID> previousDistrictOwner_;
    std::vector<uint8_t>        districtFlashTurns_;
    bool                        haveDistrictHistory_ = false;
    uint64_t                    districtSignature_ = 0;

    // Minimap only rebuilt when turn advances
    jke::TurnNumber minimapTurn_ = ~0u;
    jke::TurnNumber lastMapImageTurn_ = ~0u;

    // ── Event handling ────────────────────────────────────────────────────────
    void handleEvents() {
        while (const auto event = window_.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window_.close();
            } else if (const auto* key = event->getIf<sf::Event::KeyPressed>()) {
                switch (key->code) {
                    case sf::Keyboard::Key::Escape:
                    case sf::Keyboard::Key::Q:
                        window_.close();
                        break;
                    case sf::Keyboard::Key::Space:
                        paused_ = !paused_;
                        break;
                    case sf::Keyboard::Key::Up:
                        speed_ = std::min(240, speed_ + (speed_ >= 20 ? 10 : 1));
                        break;
                    case sf::Keyboard::Key::Down:
                        speed_ = std::max(1, speed_ - (speed_ > 20 ? 10 : 1));
                        break;
                    case sf::Keyboard::Key::Right:
                        if (paused_) { engine_.step(); rebuildMapImage(); }
                        break;
                    // Number keys 1-9: jump camera to Nth alive kingdom's capital
                    case sf::Keyboard::Key::Num1: jumpToKingdom(0); break;
                    case sf::Keyboard::Key::Num2: jumpToKingdom(1); break;
                    case sf::Keyboard::Key::Num3: jumpToKingdom(2); break;
                    case sf::Keyboard::Key::Num4: jumpToKingdom(3); break;
                    case sf::Keyboard::Key::Num5: jumpToKingdom(4); break;
                    case sf::Keyboard::Key::Num6: jumpToKingdom(5); break;
                    case sf::Keyboard::Key::Num7: jumpToKingdom(6); break;
                    case sf::Keyboard::Key::Num8: jumpToKingdom(7); break;
                    case sf::Keyboard::Key::Num9: jumpToKingdom(8); break;
                    default: break;
                }
            } else if (const auto* scroll = event->getIf<sf::Event::MouseWheelScrolled>()) {
                if (scroll->wheel == sf::Mouse::Wheel::Vertical) {
                    applyZoom(scroll->delta, scroll->position);
                }
            } else if (const auto* mb = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (mb->button == sf::Mouse::Button::Left) {
                    sf::Vector2f mpos{static_cast<float>(mb->position.x),
                                      static_cast<float>(mb->position.y)};

                    // Minimap click → jump camera
                    if (minimapScreenRect_.contains(mpos)) {
                        float fx = (mpos.x - minimapScreenRect_.position.x) / minimapScreenRect_.size.x;
                        float fy = (mpos.y - minimapScreenRect_.position.y) / minimapScreenRect_.size.y;
                        camCenter_ = {fx * MAP_FULL_PX, fy * MAP_FULL_PX};
                        clampCamera();
                    }
                    // Map click → select tile
                    else {
                        sf::Vector2f world = window_.mapPixelToCoords(mb->position, mapView_);
                        int tx = static_cast<int>(world.x / TILE_PX);
                        int ty = static_cast<int>(world.y / TILE_PX);
                        if (tx >= 0 && ty >= 0 && tx < MAP_TILES && ty < MAP_TILES) {
                            jke::TileID tid = static_cast<jke::TileID>(ty * MAP_TILES + tx);
                            selectedTile_ = (selectedTile_ == tid) ? jke::NO_TILE : tid;
                        } else {
                            selectedTile_ = jke::NO_TILE;
                        }
                    }
                }
            }
        }
    }

    void jumpToKingdom(int idx) {
        int count = 0;
        for (const auto& [kid, k] : engine_.kingdoms()) {
            if (!k.isAlive) continue;
            if (count++ == idx) {
                if (k.capitalCity == jke::NO_CITY || !engine_.cities().count(k.capitalCity)) return;
                const auto& city = engine_.cities().at(k.capitalCity);
                if (city.tile == jke::NO_TILE) return;
                const auto& tile = engine_.worldMap().at(city.tile);
                float wx = static_cast<float>(tile.position.x * TILE_PX) + TILE_PX * 0.5f;
                float wy = static_cast<float>(tile.position.y * TILE_PX) + TILE_PX * 0.5f;
                camCenter_ = {wx, wy};
                clampCamera();
                return;
            }
        }
    }

    // ── Map pixel buffer ──────────────────────────────────────────────────────
    // terrain_ holds the immutable per-pixel terrain color (built once at init)
    std::vector<uint8_t> terrain_;
    bool                 terrainBuilt_ = false;

    // Pure terrain color without any kingdom ownership tint
    static sf::Color terrainBaseColor(const jke::Tile& t) {
        sf::Color base = TERRAIN_COL[static_cast<int>(t.terrain)];
        if (t.terrain == jke::TerrainType::Ocean) {
            float shallow = std::clamp(t.elevation / 0.20f, 0.0f, 1.0f);
            base = blend(sf::Color{12, 38, 72}, TERRAIN_COL[0], shallow);
        } else if (t.terrain == jke::TerrainType::Coast) {
            base = blend(TERRAIN_COL[1], sf::Color{142, 206, 228}, 0.30f);
        } else if (t.terrain == jke::TerrainType::Mountain) {
            base = shade(base, std::clamp((t.elevation - 0.70f) * 0.65f, 0.0f, 0.22f));
        } else if (t.terrain == jke::TerrainType::Hill) {
            base = shade(base, std::clamp((t.elevation - 0.55f) * 0.35f, 0.0f, 0.12f));
        } else if (t.terrain != jke::TerrainType::Lake && t.terrain != jke::TerrainType::River) {
            base = shade(base, (t.elevation - 0.42f) * 0.18f);
        }
        return base;
    }

    void buildTerrainLayer() {
        const auto& tiles = engine_.worldMap().tiles();
        terrain_.resize(MAP_FULL_PX * MAP_FULL_PX * 4, 255);
        for (int ty = 0; ty < MAP_TILES; ++ty) {
            for (int tx = 0; tx < MAP_TILES; ++tx) {
                const jke::Tile& t = tiles[ty * MAP_TILES + tx];
                sf::Color base = terrainBaseColor(t);  // no kingdom tint — ownership applied per turn
                bool coastline = false;
                if (!isWaterTerrain(t.terrain)) {
                    if (tx > 0) coastline |= isWaterTerrain(tiles[ty*MAP_TILES+(tx-1)].terrain);
                    if (ty > 0) coastline |= isWaterTerrain(tiles[(ty-1)*MAP_TILES+tx].terrain);
                }
                for (int dy = 0; dy < TILE_PX; ++dy) {
                    for (int dx = 0; dx < TILE_PX; ++dx) {
                        sf::Color c = coastline && (dx==0||dy==0) ? sf::Color{34,54,56} : base;
                        switch (t.terrain) {
                            case jke::TerrainType::Forest:
                                if (((tx*13+ty*7+dx*3+dy*5)%11)<3) c=shade(c,-0.18f); break;
                            case jke::TerrainType::Hill:
                                if (((tx+ty+dx+dy)%7)==0) c=shade(c,0.12f); break;
                            case jke::TerrainType::Mountain:
                                if (dx==dy||dx+dy==TILE_PX-1) c=shade(c,0.20f);
                                else if (((tx*5+ty*9+dx+dy)%5)==0) c=shade(c,-0.16f); break;
                            case jke::TerrainType::River:
                                if (dx==TILE_PX/2||dy==TILE_PX/2) {
                                    c=blend(c,sf::Color{150,220,245},0.35f);
                                }
                                break;
                            case jke::TerrainType::Plain:
                                if (((tx*5+ty*3+dx+dy)%19)==0) c=shade(c,0.08f); break;
                            case jke::TerrainType::Coast:
                                if (dx==1||dy==1||dx==TILE_PX-2||dy==TILE_PX-2) {
                                    c=blend(c,sf::Color{190,220,205},0.18f);
                                }
                                break;
                            default: break;
                        }
                        int pi = ((ty*TILE_PX+dy)*MAP_FULL_PX+(tx*TILE_PX+dx))*4;
                        terrain_[pi]=c.r; terrain_[pi+1]=c.g; terrain_[pi+2]=c.b; terrain_[pi+3]=255;
                    }
                }
            }
        }
        terrainBuilt_ = true;
    }

    void rebuildDistrictLayer() {
        const auto& tiles = engine_.worldMap().tiles();
        const auto& cities = engine_.cities();
        const auto& kingdoms = engine_.kingdoms();

        uint64_t signature = 1469598103934665603ull;
        auto mix = [&](uint64_t v) {
            signature ^= v;
            signature *= 1099511628211ull;
        };
        mix(static_cast<uint64_t>(cities.size()));
        for (const auto& [cid, city] : cities) {
            mix(static_cast<uint64_t>(cid));
            mix(static_cast<uint64_t>(city.owner + 0x9e3779b9u));
            mix(static_cast<uint64_t>(city.isRuined));
            mix(static_cast<uint64_t>(city.isCapital));
        }
        for (const auto& [kid, kingdom] : kingdoms) {
            mix(static_cast<uint64_t>(kid));
            mix(static_cast<uint64_t>(kingdom.isAlive));
        }

        if (districtFlashTurns_.size() != tiles.size()) {
            districtFlashTurns_.assign(tiles.size(), 0);
            previousDistrictOwner_.clear();
            haveDistrictHistory_ = false;
        }

        if (signature == districtSignature_ &&
            districtOwner_.size() == tiles.size() &&
            districtCity_.size() == tiles.size()) {
            for (auto& flash : districtFlashTurns_) {
                if (flash > 0) --flash;
            }
            return;
        }

        districtOwner_.assign(tiles.size(), jke::NO_KINGDOM);
        districtCity_.assign(tiles.size(), jke::NO_CITY);

        std::vector<const jke::City*> anchors;
        anchors.reserve(cities.size());
        for (const auto& [cid, city] : cities) {
            if (city.isRuined || city.tile == jke::NO_TILE) continue;
            auto kit = kingdoms.find(city.owner);
            if (kit == kingdoms.end() || !kit->second.isAlive) continue;
            anchors.push_back(&city);
        }
        if (anchors.empty()) return;

        for (const jke::Tile& tile : tiles) {
            if (isWaterTerrain(tile.terrain)) continue;

            const jke::City* best = nullptr;
            float bestScore = 1.0e18f;
            for (const jke::City* city : anchors) {
                const float dx = static_cast<float>(tile.position.x - city->position.x);
                const float dy = static_cast<float>(tile.position.y - city->position.y);
                float score = dx * dx + dy * dy;

                if (city->isCapital) score *= 0.82f;
                if (city->cityType == jke::CityType::Fortress) score *= 0.90f;
                if (city->cityType == jke::CityType::TradeHub) score *= 0.92f;

                if (score < bestScore) {
                    bestScore = score;
                    best = city;
                }
            }

            if (best) {
                districtOwner_[tile.id] = best->owner;
                districtCity_[tile.id] = best->id;
            }
        }

        if (haveDistrictHistory_ && previousDistrictOwner_.size() == districtOwner_.size()) {
            for (size_t i = 0; i < districtOwner_.size(); ++i) {
                const jke::KingdomID before = previousDistrictOwner_[i];
                const jke::KingdomID after = districtOwner_[i];
                const jke::CityID district = districtCity_[i];
                const bool districtWasJustConquered =
                    district != jke::NO_CITY &&
                    cities.count(district) &&
                    cities.at(district).lastConquered > 0 &&
                    engine_.currentTurn() >= cities.at(district).lastConquered &&
                    engine_.currentTurn() - cities.at(district).lastConquered <= 1;
                if (before != after &&
                    before != jke::NO_KINGDOM &&
                    after != jke::NO_KINGDOM &&
                    districtWasJustConquered) {
                    districtFlashTurns_[i] = 9;
                } else if (districtFlashTurns_[i] > 0) {
                    --districtFlashTurns_[i];
                }
            }
        } else {
            haveDistrictHistory_ = true;
        }
        previousDistrictOwner_ = districtOwner_;
        districtSignature_ = signature;
    }

    static float districtControlRadiusTiles(const jke::City& city) {
        if (city.isCapital) return 20.0f;
        switch (city.cityType) {
            case jke::CityType::Fortress:     return 15.0f;
            case jke::CityType::TradeHub:     return 14.0f;
            case jke::CityType::Port:         return 13.0f;
            case jke::CityType::Mining:       return 11.0f;
            case jke::CityType::Agricultural: return 11.0f;
            case jke::CityType::Generic:      return 9.0f;
        }
        return 9.0f;
    }

    bool tileInsideDistrictControl(const jke::Tile& tile, jke::CityID district) const {
        if (district == jke::NO_CITY) return false;
        const auto& cities = engine_.cities();
        auto it = cities.find(district);
        if (it == cities.end()) return false;
        const jke::City& city = it->second;
        const float dx = static_cast<float>(tile.position.x - city.position.x);
        const float dy = static_cast<float>(tile.position.y - city.position.y);
        const float radius = districtControlRadiusTiles(city);
        return dx * dx + dy * dy <= radius * radius;
    }

    void rebuildMapImage() {
        if (!terrainBuilt_) buildTerrainLayer();
        lastMapImageTurn_ = engine_.currentTurn();

        const auto& tiles    = engine_.worldMap().tiles();
        const auto& cities   = engine_.cities();
        const auto& armies   = engine_.armies();
        const auto& kingdoms = engine_.kingdoms();

        // Build tile → army info (reuse member map to avoid per-turn allocation)
        tileArmy_.clear();
        for (const auto& [aid, army] : armies) {
            auto kit = kingdoms.find(army.owner);
            if (kit == kingdoms.end() || !kit->second.isAlive) continue;
            if (army.currentTile >= static_cast<jke::TileID>(tiles.size())) continue;
            const jke::Tile& at = tiles[army.currentTile];
            bool inv = (at.owner != army.owner && at.owner != jke::NO_KINGDOM);
            uint32_t total = 0;
            for (const auto& u : army.units) total += u.soldiers;
            tileArmy_[army.currentTile] = {army.owner, inv, total};
        }
        rebuildDistrictLayer();

        // Start from immutable terrain layer, then paint dynamic data on top
        pixels_ = terrain_;

        for (int ty = 0; ty < MAP_TILES; ++ty) {
            for (int tx = 0; tx < MAP_TILES; ++tx) {
                int idx = ty * MAP_TILES + tx;
                const jke::Tile& t = tiles[idx];

                bool hasCity   = t.city != jke::NO_CITY;
                bool isCapital = hasCity && cities.count(t.city) &&
                                 cities.at(t.city).isCapital;
                jke::KingdomID visualOwner =
                    idx < static_cast<int>(districtOwner_.size()) ? districtOwner_[idx] : t.owner;
                jke::CityID visualDistrict =
                    idx < static_cast<int>(districtCity_.size()) ? districtCity_[idx] : jke::NO_CITY;
                const bool districtControlled = tileInsideDistrictControl(t, visualDistrict);
                jke::KingdomID tintOwner = districtControlled ? visualOwner : t.owner;
                bool currentCapitalDistrict =
                    visualDistrict != jke::NO_CITY &&
                    cities.count(visualDistrict) &&
                    cities.at(visualDistrict).isCapital;

                auto armyIt = tileArmy_.find(t.id);
                bool hasArmy  = (armyIt != tileArmy_.end());
                bool invasion = hasArmy && armyIt->second.invasion;
                jke::KingdomID armyOwner    = hasArmy ? armyIt->second.owner   : jke::NO_KINGDOM;
                uint32_t       armySoldiers = hasArmy ? armyIt->second.soldiers : 0;

                // Precompute border conditions once per tile (not per pixel)
                bool kingdomBorderLeft = (tintOwner != jke::NO_KINGDOM && tx > 0) && [&] {
                    const int nidx = ty * MAP_TILES + (tx - 1);
                    const jke::Tile& nt = tiles[nidx];
                    const jke::CityID nd = districtCity_[nidx];
                    const jke::KingdomID no = tileInsideDistrictControl(nt, nd)
                        ? districtOwner_[nidx]
                        : nt.owner;
                    return no != tintOwner;
                }();
                bool kingdomBorderTop = (tintOwner != jke::NO_KINGDOM && ty > 0) && [&] {
                    const int nidx = (ty - 1) * MAP_TILES + tx;
                    const jke::Tile& nt = tiles[nidx];
                    const jke::CityID nd = districtCity_[nidx];
                    const jke::KingdomID no = tileInsideDistrictControl(nt, nd)
                        ? districtOwner_[nidx]
                        : nt.owner;
                    return no != tintOwner;
                }();
                bool districtBorderLeft = false;
                if (visualDistrict != jke::NO_CITY && tx > 0 &&
                    districtCity_[ty * MAP_TILES + (tx-1)] != visualDistrict) {
                    const int nidx = ty * MAP_TILES + (tx - 1);
                    districtBorderLeft = districtControlled ||
                        tileInsideDistrictControl(tiles[nidx], districtCity_[nidx]);
                }
                bool districtBorderTop = false;
                if (visualDistrict != jke::NO_CITY && ty > 0 &&
                    districtCity_[(ty-1) * MAP_TILES + tx] != visualDistrict) {
                    const int nidx = (ty - 1) * MAP_TILES + tx;
                    districtBorderTop = districtControlled ||
                        tileInsideDistrictControl(tiles[nidx], districtCity_[nidx]);
                }
                bool leftCapitalDistrict = false;
                bool topCapitalDistrict = false;
                if (tx > 0) {
                    jke::CityID leftDistrict = districtCity_[ty * MAP_TILES + (tx-1)];
                    leftCapitalDistrict = leftDistrict != jke::NO_CITY &&
                                          cities.count(leftDistrict) &&
                                          cities.at(leftDistrict).isCapital;
                }
                if (ty > 0) {
                    jke::CityID topDistrict = districtCity_[(ty-1) * MAP_TILES + tx];
                    topCapitalDistrict = topDistrict != jke::NO_CITY &&
                                         cities.count(topDistrict) &&
                                         cities.at(topDistrict).isCapital;
                }
                bool capitalBorderLeft = districtBorderLeft &&
                                         (currentCapitalDistrict || leftCapitalDistrict);
                bool capitalBorderTop = districtBorderTop &&
                                        (currentCapitalDistrict || topCapitalDistrict);

                for (int dy = 0; dy < TILE_PX; ++dy) {
                    for (int dx = 0; dx < TILE_PX; ++dx) {
                        int px = tx * TILE_PX + dx;
                        int py = ty * TILE_PX + dy;

                        bool kingdomBorder = ((dx <= 1) && kingdomBorderLeft) ||
                                             ((dy <= 1) && kingdomBorderTop);
                        bool capitalDistrictBorder = !kingdomBorder &&
                            (((dx <= 2) && capitalBorderLeft) ||
                             ((dy <= 2) && capitalBorderTop));
                        bool districtBorder = !kingdomBorder &&
                            !capitalDistrictBorder &&
                            (((dx <= 1) && districtBorderLeft) ||
                             ((dy <= 1) && districtBorderTop));

                        // Start from pure terrain pixel, then apply district ownership tint
                        int pi = (py * MAP_FULL_PX + px) * 4;
                        sf::Color c{pixels_[pi], pixels_[pi+1], pixels_[pi+2], 255};
                        if (tintOwner != jke::NO_KINGDOM)
                            c = blend(c, kingdomColor(tintOwner), districtControlled ? 0.38f : 0.24f);

                        if (idx < static_cast<int>(districtFlashTurns_.size()) &&
                            districtControlled &&
                            districtFlashTurns_[idx] > 0 &&
                            ((districtFlashTurns_[idx] + engine_.currentTurn()) % 4) < 2) {
                            c = blend(c, sf::Color{255, 238, 170}, 0.46f);
                        }

                        // Kingdom border and inner district border
                        if (kingdomBorder) {
                            c = sf::Color{10, 10, 10};
                        } else if (capitalDistrictBorder) {
                            c = blend(sf::Color{20, 16, 8}, GOLD_COLOR, 0.62f);
                        } else if (districtBorder) {
                            c = shade(c, -0.44f);
                        }

                        // Invasion red tint
                        if (invasion) c = blend(c, sf::Color{200, 40, 40}, 0.35f);

                        // ── City cross (center of tile) ───────────────────────
                        if (hasCity) {
                            int cx6 = TILE_PX / 2; // 3
                            bool onCross = isCapital
                                ? (dx >= cx6-1 && dx <= cx6 && dy >= 1 && dy <= TILE_PX-2) ||
                                  (dy >= cx6-1 && dy <= cx6 && dx >= 1 && dx <= TILE_PX-2)
                                : (dx == cx6-1 && dy >= 1 && dy <= TILE_PX-2) ||
                                  (dy == cx6-1 && dx >= 1 && dx <= TILE_PX-2);
                            bool onShadow = !onCross && (isCapital
                                ? ((dx+1 >= cx6-1 && dx+1 <= cx6 && dy+1 >= 1 && dy+1 <= TILE_PX-2) ||
                                   (dy+1 >= cx6-1 && dy+1 <= cx6 && dx+1 >= 1 && dx+1 <= TILE_PX-2))
                                : ((dx+1 == cx6-1 && dy+1 >= 1 && dy+1 <= TILE_PX-2) ||
                                   (dy+1 == cx6-1 && dx+1 >= 1 && dx+1 <= TILE_PX-2)));
                            if (onCross)
                                c = isCapital ? sf::Color{255, 215, 0}
                                              : sf::Color{255, 255, 255};
                            else if (onShadow)
                                c = sf::Color{20, 20, 20};
                        }

                        // ── Army marker: size scales with soldier count ────────
                        // Small  (<500):  2×2 dot  at centre
                        // Medium (500-2k): 4×4 square
                        // Large  (2k+):   full tile (6×6)
                        if (hasArmy) {
                            sf::Color armyCol = invasion
                                ? sf::Color{210, 45, 45}
                                : kingdomColor(armyOwner);

                            bool inMarker = false;
                            bool onRim    = false;
                            if (armySoldiers >= 2000) {
                                // Large: fill tile
                                inMarker = true;
                                onRim = (dx==0||dx==5||dy==0||dy==5);
                            } else if (armySoldiers >= 500) {
                                // Medium: 4×4 centred (dx 1-4, dy 1-4)
                                inMarker = (dx>=1&&dx<=4&&dy>=1&&dy<=4);
                                onRim = inMarker&&(dx==1||dx==4||dy==1||dy==4);
                            } else {
                                // Small: 2×2 dot (dx 2-3, dy 2-3)
                                inMarker = (dx>=2&&dx<=3&&dy>=2&&dy<=3);
                            }

                            if (onRim) {
                                c = sf::Color{10, 10, 10};
                            } else if (inMarker) {
                                c = blend(c, armyCol, 0.85f);
                            }
                        }

                        int wpi = (py * MAP_FULL_PX + px) * 4;
                        pixels_[wpi+0] = c.r;
                        pixels_[wpi+1] = c.g;
                        pixels_[wpi+2] = c.b;
                        pixels_[wpi+3] = 255;
                    }
                }
            }
        }
        mapTexture_.update(pixels_.data());

        // Rebuild minimap only when turn advances (territory rarely changes mid-frame)
        if (engine_.currentTurn() != minimapTurn_) {
            minimapPixels_.assign(MAP_TILES * MAP_TILES * 4, 0);
            for (int ty = 0; ty < MAP_TILES; ++ty) {
                for (int tx = 0; tx < MAP_TILES; ++tx) {
                    int idx = ty * MAP_TILES + tx;
                    const jke::Tile& t = tiles[ty * MAP_TILES + tx];
                    sf::Color c = terrainBaseColor(t);
                    if (!isWaterTerrain(t.terrain)) {
                        const jke::CityID district =
                            idx < static_cast<int>(districtCity_.size()) ? districtCity_[idx] : jke::NO_CITY;
                        const bool districtControlled = tileInsideDistrictControl(t, district);
                        const jke::KingdomID owner =
                            districtControlled && idx < static_cast<int>(districtOwner_.size())
                                ? districtOwner_[idx]
                                : t.owner;
                        if (owner != jke::NO_KINGDOM) {
                            c = blend(c, kingdomColor(owner), districtControlled ? 0.38f : 0.24f);
                        }
                    }
                    int mi = (ty * MAP_TILES + tx) * 4;
                    minimapPixels_[mi+0] = c.r;
                    minimapPixels_[mi+1] = c.g;
                    minimapPixels_[mi+2] = c.b;
                    minimapPixels_[mi+3] = 255;
                }
            }
            minimapTex_.update(minimapPixels_.data());
            minimapTurn_ = engine_.currentTurn();
        }
    }

    bool shouldRefreshMapImage() const {
        if (lastMapImageTurn_ == ~0u) return true;
        const jke::TurnNumber elapsed = engine_.currentTurn() - lastMapImageTurn_;
        if (speed_ <= 20) return elapsed >= 1;
        if (speed_ <= 80) return elapsed >= 2;
        if (speed_ <= 160) return elapsed >= 4;
        return elapsed >= 8;
    }

    // ── Text helper ───────────────────────────────────────────────────────────
    void drawText(const std::string& str, float x, float y,
                  unsigned size = 13, sf::Color col = TEXT_COLOR) {
        sf::Text txt(font_, str, size);
        txt.setFillColor(col);
        txt.setPosition({x, y});
        window_.draw(txt);
    }

    void drawRect(float x, float y, float w, float h, sf::Color fill,
                  sf::Color outline = sf::Color::Transparent,
                  float outlineThickness = 0.f) {
        sf::RectangleShape rect({w, h});
        rect.setPosition({x, y});
        rect.setFillColor(fill);
        rect.setOutlineColor(outline);
        rect.setOutlineThickness(outlineThickness);
        window_.draw(rect);
    }

    sf::Vector2f tileCenter(jke::TileID tileID) const {
        const auto& tile = engine_.worldMap().at(tileID);
        // Returns world-space coordinates (map view is active when these are used)
        return {
            static_cast<float>(tile.position.x * TILE_PX) + TILE_PX * 0.5f,
            static_cast<float>(tile.position.y * TILE_PX) + TILE_PX * 0.5f
        };
    }

    void drawThickLine(sf::Vector2f a, sf::Vector2f b, sf::Color color, float thickness) {
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.f) return;

        sf::RectangleShape line({length, thickness});
        line.setOrigin({0.f, thickness * 0.5f});
        line.setPosition(a);
        line.setRotation(sf::degrees(std::atan2(dy, dx) * 180.f / 3.14159265f));
        line.setFillColor(color);
        window_.draw(line);
    }

    void drawMapOverlays() {
        drawMovementOverlays();
        drawStrategicPointOverlays();
        drawCityOverlays();
        drawArmyOverlays();
        drawBanditOverlays();
        drawFleetOverlays();
        drawSelectionHighlight();
    }

    void drawBanditOverlays() {
        for (const auto& bg : engine_.bandits()) {
            if (bg.tile >= static_cast<jke::TileID>(engine_.worldMap().tileCount())) continue;
            const auto& tile = engine_.worldMap().at(bg.tile);
            float wx = static_cast<float>(tile.position.x * TILE_PX) + TILE_PX * 0.5f;
            float wy = static_cast<float>(tile.position.y * TILE_PX) + TILE_PX * 0.5f;
            // Red diamond marker
            sf::ConvexShape diamond(4);
            float r = std::max(3.f, std::min(6.f, static_cast<float>(TILE_PX) * zoom_));
            diamond.setPoint(0, {wx,     wy - r});
            diamond.setPoint(1, {wx + r, wy    });
            diamond.setPoint(2, {wx,     wy + r});
            diamond.setPoint(3, {wx - r, wy    });
            diamond.setFillColor(sf::Color{220, 50, 50, 210});
            diamond.setOutlineColor(sf::Color{255, 120, 120, 180});
            diamond.setOutlineThickness(1.f);
            window_.draw(diamond);
        }
    }

    void drawFleetOverlays() {
        const auto& fleets = engine_.fleets();
        if (fleets.empty()) return;
        const auto& kingdoms = engine_.kingdoms();
        for (const auto& [fid, fl] : fleets) {
            if (fl.tile >= static_cast<jke::TileID>(engine_.worldMap().tileCount())) continue;
            const auto& tile = engine_.worldMap().at(fl.tile);
            float wx = static_cast<float>(tile.position.x * TILE_PX) + TILE_PX * 0.5f;
            float wy = static_cast<float>(tile.position.y * TILE_PX) + TILE_PX * 0.5f;
            float r  = std::max(3.f, std::min(7.f, static_cast<float>(TILE_PX) * zoom_));

            // Hull shape — small pentagon (ship silhouette)
            sf::ConvexShape ship(5);
            ship.setPoint(0, {wx,       wy - r});       // bow
            ship.setPoint(1, {wx + r,   wy - r * 0.3f});
            ship.setPoint(2, {wx + r * 0.7f, wy + r});  // stern
            ship.setPoint(3, {wx - r * 0.7f, wy + r});
            ship.setPoint(4, {wx - r,   wy - r * 0.3f});

            sf::Color col{100, 180, 255, 200};
            if (kingdoms.count(fl.owner)) {
                int idx = static_cast<int>(fl.owner) % 16;
                col = KINGDOM_COL[idx];
                col.a = 210;
            }
            ship.setFillColor(col);
            ship.setOutlineColor(sf::Color{255, 255, 255, 160});
            ship.setOutlineThickness(1.f);
            window_.draw(ship);

            // Cargo indicator (yellow dot if carrying an army)
            if (fl.cargo != jke::NO_ARMY) {
                sf::CircleShape dot(r * 0.35f);
                dot.setOrigin({r * 0.35f, r * 0.35f});
                dot.setPosition({wx, wy});
                dot.setFillColor(sf::Color{255, 220, 60, 230});
                window_.draw(dot);
            }
        }
    }

    void drawPopulationGraph(float px, float py) {
        const auto& hist = engine_.history();
        if (hist.empty()) return;

        constexpr float GW = 340.f;
        constexpr float GH = 72.f;
        float gx = px;
        float gy = py + 4.f;

        // Don't draw if it would collide with the terrain legend
        if (gy + GH + 40.f > WIN_H - 132.f) return;

        drawSectionTitle("POPULATION TREND", gx, gy);
        gy += 24.f;
        drawRect(gx, gy, GW, GH, sf::Color{10, 13, 20, 200}, BORDER_COLOR, 1.f);

        // Find global max population across all kingdoms
        uint32_t maxPop = 1;
        for (const auto& [kid, snaps] : hist) {
            for (const auto& [t, s] : snaps) {
                if (s.pop > maxPop) maxPop = s.pop;
            }
        }

        jke::TurnNumber maxTurn = engine_.currentTurn();
        jke::TurnNumber minTurn = maxTurn > 200 ? maxTurn - 200 : 0;

        // Draw one line per alive kingdom
        for (const auto& [kid, snaps] : hist) {
            if (!engine_.kingdoms().count(kid)) continue;
            if (!engine_.kingdoms().at(kid).isAlive) continue;
            if (snaps.empty()) continue;

            sf::Color lc = kingdomColor(kid);
            lc.a = 200;

            std::vector<sf::Vertex> verts;
            for (const auto& [t, s] : snaps) {
                if (t < minTurn) continue;
                float fx = (static_cast<float>(t - minTurn) /
                            static_cast<float>(std::max(1u, maxTurn - minTurn))) * (GW - 4.f);
                float fy = (1.0f - static_cast<float>(s.pop) / static_cast<float>(maxPop)) * (GH - 4.f);
                verts.emplace_back(sf::Vector2f{gx + 2.f + fx, gy + 2.f + fy}, lc);
            }
            if (verts.size() >= 2) {
                window_.draw(verts.data(), verts.size(), sf::PrimitiveType::LineStrip);
            }
        }

        // Grid lines
        for (int i = 1; i < 4; ++i) {
            float ly2 = gy + GH * i / 4.f;
            sf::Vertex hline[] = {
                {{gx + 1.f, ly2}, sf::Color{255, 255, 255, 20}},
                {{gx + GW - 1.f, ly2}, sf::Color{255, 255, 255, 20}}
            };
            window_.draw(hline, 2, sf::PrimitiveType::Lines);
        }
        drawText("Pop max " + compactNumber(maxPop), gx + 4.f, gy + GH + 2.f, 9, MUTED_COLOR);
    }

    void drawTileInspector() {
        const auto& tile     = engine_.worldMap().at(selectedTile_);
        const auto& kingdoms = engine_.kingdoms();
        const auto& cities   = engine_.cities();
        const auto& armies   = engine_.armies();

        // Position: bottom-left of map viewport
        float bx = MAP_OX + 6.f;
        float by = MAP_OY + VIEWPORT_PX - 156.f;
        float bw = 230.f;
        float bh = 148.f;

        drawRect(bx, by, bw, bh, sf::Color{10, 13, 20, 215}, BORDER_COLOR, 1.f);
        drawRect(bx, by, bw, 18.f, sf::Color{22, 28, 42, 240});

        // Terrain
        float y = by + 3.f;
        std::string terrName = std::string(terrainEffectText(tile.terrain));
        drawText("TILE (" + std::to_string(tile.position.x) + "," +
                 std::to_string(tile.position.y) + ")", bx + 6.f, y, 9, GOLD_COLOR);
        y += 18.f;

        static const char* TNAME[] = {"Ocean","Coast","Plain","Forest","Hill","Mountain","River","Lake"};
        int tidx = static_cast<int>(tile.terrain);
        drawText(tidx >= 0 && tidx < 8 ? TNAME[tidx] : "?", bx + 6.f, y, 11, TEXT_COLOR);
        drawText(terrName, bx + 80.f, y, 10, DIM_COLOR);
        y += 16.f;

        // Owner
        if (tile.owner != jke::NO_KINGDOM && kingdoms.count(tile.owner)) {
            sf::Color oc = kingdomColor(tile.owner);
            drawText("Owner: " + kingdoms.at(tile.owner).name, bx + 6.f, y, 11, oc);
        } else {
            drawText("Owner: unclaimed", bx + 6.f, y, 11, MUTED_COLOR);
        }
        y += 16.f;

        if (selectedTile_ < districtCity_.size() &&
            districtCity_[selectedTile_] != jke::NO_CITY &&
            cities.count(districtCity_[selectedTile_])) {
            const auto& district = cities.at(districtCity_[selectedTile_]);
            std::string ownerName = kingdoms.count(district.owner)
                ? kingdoms.at(district.owner).name
                : "unknown";
            drawText("District: " + ellipsize(district.name, 13) +
                     " / " + ellipsize(ownerName, 11),
                     bx + 6.f, y, 10, kingdomColor(district.owner));
        } else {
            drawText("District: wilderness", bx + 6.f, y, 10, MUTED_COLOR);
        }
        y += 14.f;

        if (tile.strategicPoint != jke::StrategicPointType::None) {
            sf::Color pointColor = sf::Color{180, 210, 235};
            switch (tile.strategicPoint) {
                case jke::StrategicPointType::MountainPass: pointColor = sf::Color{190, 150, 95}; break;
                case jke::StrategicPointType::Bridge:       pointColor = sf::Color{110, 180, 230}; break;
                case jke::StrategicPointType::RiverFord:    pointColor = sf::Color{105, 205, 210}; break;
                case jke::StrategicPointType::HarborSite:   pointColor = sf::Color{70, 150, 245};  break;
                case jke::StrategicPointType::SupplyDepot:  pointColor = sf::Color{220, 195, 90};  break;
                case jke::StrategicPointType::None: break;
            }
            drawText("Point: " + std::string(jke::strategicPointName(tile.strategicPoint)),
                     bx + 6.f, y, 10, pointColor);
            y += 13.f;
            drawText("Value " + std::to_string(static_cast<int>(tile.strategicValue)),
                     bx + 6.f, y, 10, DIM_COLOR);
            y += 13.f;
        }

        // City
        if (tile.city != jke::NO_CITY && cities.count(tile.city)) {
            const auto& city = cities.at(tile.city);
            sf::Color cc = city.isCapital ? GOLD_COLOR : TEXT_COLOR;
            std::string cLabel = (city.isCapital ? "★ " : "  ") + city.name;
            cLabel += " [" + std::string(jke::cityTypeName(city.cityType)) + "]";
            drawText(cLabel, bx + 4.f, y, 11, cc);
            y += 15.f;
            std::ostringstream cs;
            cs << "Pop " << compactNumber(city.population)
               << "  Fort " << static_cast<int>(city.fortification * 100) << "%"
               << "  Hap " << static_cast<int>(city.happiness * 100) << "%";
            drawText(cs.str(), bx + 6.f, y, 10, DIM_COLOR);
            y += 14.f;
            // Culture assimilation bar
            if (city.cultureOwner != city.owner && city.cultureOwner != jke::NO_KINGDOM) {
                int pct = static_cast<int>(city.cultureAssimilation * 100);
                sf::Color cultureCol = (pct < 40) ? sf::Color{220,80,80} :
                                       (pct < 80) ? sf::Color{220,180,60} :
                                                    sf::Color{80,200,120};
                drawText("Culture: " + std::to_string(pct) + "% assimilated",
                         bx + 6.f, y, 10, cultureCol);
                y += 13.f;
            }
        }

        // Army
        if (tile.army != jke::NO_ARMY && armies.count(tile.army)) {
            const auto& army = armies.at(tile.army);
            sf::Color ac = kingdomColor(army.owner);
            std::ostringstream as;
            as << "Army: " << army.totalSoldiers() << " soldiers";
            drawText(as.str(), bx + 6.f, y, 11, ac);
            y += 15.f;
            std::ostringstream as2;
            as2 << "Supply " << static_cast<int>(army.supplyLevel * 100) << "%"
                << "  Role: " << jke::armyRoleName(army.role);
            drawText(as2.str(), bx + 6.f, y, 10, DIM_COLOR);
        }
    }

    void drawSelectionHighlight() {
        if (selectedTile_ == jke::NO_TILE) return;
        if (selectedTile_ >= static_cast<jke::TileID>(engine_.worldMap().tileCount())) return;
        const auto& tile = engine_.worldMap().at(selectedTile_);
        float wx = static_cast<float>(tile.position.x * TILE_PX);
        float wy = static_cast<float>(tile.position.y * TILE_PX);
        // Animated cyan border around the selected tile
        sf::RectangleShape sel({static_cast<float>(TILE_PX), static_cast<float>(TILE_PX)});
        sel.setPosition({wx, wy});
        sel.setFillColor(sf::Color::Transparent);
        sel.setOutlineColor(sf::Color{0, 220, 220, 230});
        sel.setOutlineThickness(2.f);
        window_.draw(sel);
    }

    void drawMovementOverlays() {
        const auto& kingdoms = engine_.kingdoms();
        const auto tileCount = static_cast<jke::TileID>(engine_.worldMap().tileCount());

        for (const auto& [aid, army] : engine_.armies()) {
            auto kit = kingdoms.find(army.owner);
            if (kit == kingdoms.end() || !kit->second.isAlive) continue;
            if (army.currentTile == jke::NO_TILE || army.targetTile == jke::NO_TILE) continue;
            if (army.currentTile >= tileCount || army.targetTile >= tileCount) continue;
            if (army.currentTile == army.targetTile) continue;

            sf::Color color = kingdomColor(army.owner);
            color.a = 118;

            sf::Vector2f from = tileCenter(army.currentTile);
            sf::Vector2f to = tileCenter(army.targetTile);
            drawThickLine(from, to, sf::Color{0, 0, 0, 120}, 4.f);
            drawThickLine(from, to, color, 2.f);

            sf::CircleShape endpoint(3.f);
            endpoint.setOrigin({3.f, 3.f});
            endpoint.setPosition(to);
            endpoint.setFillColor(sf::Color{230, 236, 244, 170});
            endpoint.setOutlineColor(sf::Color{0, 0, 0, 180});
            endpoint.setOutlineThickness(1.f);
            window_.draw(endpoint);
        }
    }

    void drawStrategicPointOverlays() {
        const auto& kingdoms = engine_.kingdoms();
        for (const jke::Tile& tile : engine_.worldMap().tiles()) {
            if (tile.strategicPoint == jke::StrategicPointType::None ||
                tile.city != jke::NO_CITY) {
                continue;
            }

            sf::Vector2f pos = tileCenter(tile.id);
            sf::Color base = sf::Color{200, 210, 220};
            switch (tile.strategicPoint) {
                case jke::StrategicPointType::MountainPass: base = sf::Color{190, 140, 80};  break;
                case jke::StrategicPointType::Bridge:       base = sf::Color{120, 190, 240}; break;
                case jke::StrategicPointType::RiverFord:    base = sf::Color{115, 220, 215}; break;
                case jke::StrategicPointType::HarborSite:   base = sf::Color{70, 150, 245};  break;
                case jke::StrategicPointType::SupplyDepot:  base = sf::Color{230, 200, 90};  break;
                case jke::StrategicPointType::None: break;
            }
            if (tile.owner != jke::NO_KINGDOM && kingdoms.count(tile.owner)) {
                base = blend(base, kingdomColor(tile.owner), 0.34f);
            }

            const float r = tile.strategicPoint == jke::StrategicPointType::SupplyDepot ? 4.8f : 4.2f;
            sf::CircleShape glow(r + 2.2f, 4);
            glow.setOrigin({r + 2.2f, r + 2.2f});
            glow.setPosition(pos);
            glow.setRotation(sf::degrees(45.f));
            glow.setFillColor(sf::Color{0, 0, 0, 125});
            window_.draw(glow);

            sf::CircleShape marker(r, 4);
            marker.setOrigin({r, r});
            marker.setPosition(pos);
            marker.setRotation(sf::degrees(45.f));
            marker.setFillColor(base);
            marker.setOutlineColor(sf::Color{12, 14, 18, 210});
            marker.setOutlineThickness(1.2f);
            window_.draw(marker);
        }
    }

    void drawCityOverlays() {
        const auto& kingdoms = engine_.kingdoms();
        for (const auto& [cid, city] : engine_.cities()) {
            auto kit = kingdoms.find(city.owner);
            if (kit == kingdoms.end() || !kit->second.isAlive || city.tile == jke::NO_TILE) continue;

            sf::Vector2f pos = tileCenter(city.tile);
            sf::Color owner = kingdomColor(city.owner);

            if (city.isCapital || city.cityType != jke::CityType::Generic) {
                float influenceRadius = 32.f;
                if (city.isCapital) influenceRadius = 74.f;
                else if (city.cityType == jke::CityType::Fortress) influenceRadius = 52.f;
                else if (city.cityType == jke::CityType::TradeHub) influenceRadius = 46.f;
                else if (city.cityType == jke::CityType::Port) influenceRadius = 42.f;
                else if (city.cityType == jke::CityType::Mining) influenceRadius = 38.f;

                sf::Color ringColor = city.isCapital
                    ? GOLD_COLOR
                    : blend(owner, sf::Color::White, 0.36f);
                ringColor.a = city.isCapital ? 86 : 54;

                sf::CircleShape influence(influenceRadius);
                influence.setOrigin({influenceRadius, influenceRadius});
                influence.setPosition(pos);
                influence.setFillColor(sf::Color::Transparent);
                influence.setOutlineColor(ringColor);
                influence.setOutlineThickness(city.isCapital ? 2.2f : 1.2f);
                window_.draw(influence);

                if (city.isCapital) {
                    sf::CircleShape inner(influenceRadius * 0.62f);
                    inner.setOrigin({influenceRadius * 0.62f, influenceRadius * 0.62f});
                    inner.setPosition(pos);
                    inner.setFillColor(sf::Color::Transparent);
                    inner.setOutlineColor(sf::Color{255, 226, 120, 48});
                    inner.setOutlineThickness(1.2f);
                    window_.draw(inner);
                }
            }

            float radius = city.isCapital ? 10.f : 6.f;
            sf::CircleShape shadow(radius + 2.f);
            shadow.setOrigin({radius + 2.f, radius + 2.f});
            shadow.setPosition(pos);
            shadow.setFillColor(sf::Color{0, 0, 0, 145});
            window_.draw(shadow);

            sf::CircleShape ring(radius);
            ring.setOrigin({radius, radius});
            ring.setPosition(pos);
            ring.setFillColor(sf::Color{12, 14, 18, 170});
            ring.setOutlineColor(city.isCapital ? GOLD_COLOR : blend(owner, sf::Color::White, 0.32f));
            ring.setOutlineThickness(city.isCapital ? 2.4f : 1.6f);
            window_.draw(ring);

            sf::CircleShape core(city.isCapital ? 3.6f : 2.6f);
            float coreRadius = city.isCapital ? 3.6f : 2.6f;
            core.setOrigin({coreRadius, coreRadius});
            core.setPosition(pos);
            core.setFillColor(city.isCapital ? GOLD_COLOR : sf::Color{238, 242, 247});
            window_.draw(core);

            // City type badge (small letter beside non-capital cities)
            if (!city.isCapital && city.cityType != jke::CityType::Generic) {
                sf::Color typeCol;
                switch (city.cityType) {
                    case jke::CityType::Port:         typeCol = sf::Color{60, 160, 240}; break;
                    case jke::CityType::Fortress:     typeCol = sf::Color{160, 90, 40};  break;
                    case jke::CityType::Agricultural: typeCol = sf::Color{80, 200, 80};  break;
                    case jke::CityType::Mining:       typeCol = sf::Color{180, 140, 60}; break;
                    case jke::CityType::TradeHub:     typeCol = GOLD_COLOR;              break;
                    default:                          typeCol = DIM_COLOR;               break;
                }
                // Show first character of type name
                std::string badge(1, std::string(jke::cityTypeName(city.cityType))[0]);
                drawText(badge, pos.x + 5.f, pos.y - 7.f, 9, typeCol);
            }

            if (city.isCapital) {
                std::string label = ellipsize(city.name, 14);
                // Clamp in world space so label stays on-map
                float lx = std::min(pos.x + 12.f, static_cast<float>(MAP_FULL_PX - 86));
                float ly = std::clamp(pos.y - 8.f, 4.f, static_cast<float>(MAP_FULL_PX - 18));
                drawText(label, lx + 1.f, ly + 1.f, 10, sf::Color{0, 0, 0, 210});
                drawText(label, lx, ly, 10, TEXT_COLOR);
            }
        }
    }

    void drawArmyRoleMarker(const jke::Army& army,
                            sf::Vector2f pos,
                            float radius,
                            sf::Color bodyColor,
                            sf::Color outlineColor) {
        const float outline = (army.role == jke::ArmyRole::Siege) ? 2.4f : 2.0f;

        switch (army.role) {
            case jke::ArmyRole::Attack: {
                sf::ConvexShape blade(4);
                blade.setPoint(0, {pos.x, pos.y - radius * 1.45f});
                blade.setPoint(1, {pos.x + radius * 0.42f, pos.y - radius * 0.08f});
                blade.setPoint(2, {pos.x, pos.y + radius * 0.66f});
                blade.setPoint(3, {pos.x - radius * 0.42f, pos.y - radius * 0.08f});
                blade.setFillColor(blend(bodyColor, sf::Color::White, 0.14f));
                blade.setOutlineColor(outlineColor);
                blade.setOutlineThickness(outline);
                window_.draw(blade);

                sf::RectangleShape guard({radius * 1.45f, 2.6f});
                guard.setOrigin({radius * 0.725f, 1.3f});
                guard.setPosition({pos.x, pos.y + radius * 0.55f});
                guard.setFillColor(sf::Color{236, 240, 246, 230});
                guard.setOutlineColor(sf::Color{8, 10, 14, 220});
                guard.setOutlineThickness(1.f);
                window_.draw(guard);

                sf::RectangleShape grip({3.f, radius * 0.72f});
                grip.setOrigin({1.5f, 0.f});
                grip.setPosition({pos.x, pos.y + radius * 0.62f});
                grip.setFillColor(sf::Color{16, 18, 24, 235});
                window_.draw(grip);
                break;
            }
            case jke::ArmyRole::Defense: {
                sf::ConvexShape shield(6);
                shield.setPoint(0, {pos.x - radius * 0.85f, pos.y - radius * 0.9f});
                shield.setPoint(1, {pos.x + radius * 0.85f, pos.y - radius * 0.9f});
                shield.setPoint(2, {pos.x + radius * 0.72f, pos.y + radius * 0.22f});
                shield.setPoint(3, {pos.x + radius * 0.25f, pos.y + radius * 0.92f});
                shield.setPoint(4, {pos.x, pos.y + radius * 1.18f});
                shield.setPoint(5, {pos.x - radius * 0.72f, pos.y + radius * 0.22f});
                shield.setFillColor(bodyColor);
                shield.setOutlineColor(outlineColor);
                shield.setOutlineThickness(outline);
                window_.draw(shield);

                drawThickLine({pos.x, pos.y - radius * 0.62f},
                              {pos.x, pos.y + radius * 0.72f},
                              sf::Color{235, 244, 255, 210}, 2.f);
                break;
            }
            case jke::ArmyRole::Siege: {
                sf::RectangleShape tower({radius * 1.42f, radius * 1.72f});
                tower.setOrigin({radius * 0.71f, radius * 0.86f});
                tower.setPosition(pos);
                tower.setFillColor(bodyColor);
                tower.setOutlineColor(outlineColor);
                tower.setOutlineThickness(outline);
                window_.draw(tower);

                const float bw = radius * 0.34f;
                for (int i = -1; i <= 1; ++i) {
                    sf::RectangleShape battlement({bw, radius * 0.28f});
                    battlement.setOrigin({bw * 0.5f, radius * 0.14f});
                    battlement.setPosition({pos.x + i * bw * 1.35f, pos.y - radius * 1.03f});
                    battlement.setFillColor(blend(bodyColor, sf::Color::White, 0.18f));
                    battlement.setOutlineColor(sf::Color{8, 10, 14, 210});
                    battlement.setOutlineThickness(0.8f);
                    window_.draw(battlement);
                }

                drawThickLine({pos.x - radius * 0.38f, pos.y - radius * 0.34f},
                              {pos.x + radius * 0.38f, pos.y - radius * 0.34f},
                              sf::Color{18, 20, 26, 220}, 2.f);
                drawThickLine({pos.x - radius * 0.38f, pos.y + radius * 0.22f},
                              {pos.x + radius * 0.38f, pos.y + radius * 0.22f},
                              sf::Color{18, 20, 26, 220}, 2.f);
                break;
            }
            case jke::ArmyRole::Reserve:
            default: {
                sf::RectangleShape stem({2.4f, radius * 1.9f});
                stem.setOrigin({1.2f, radius * 0.95f});
                stem.setPosition({pos.x - radius * 0.35f, pos.y});
                stem.setFillColor(sf::Color{12, 14, 18, 235});
                window_.draw(stem);

                sf::ConvexShape flag(4);
                flag.setPoint(0, {pos.x - radius * 0.18f, pos.y - radius * 0.92f});
                flag.setPoint(1, {pos.x + radius * 1.05f, pos.y - radius * 0.70f});
                flag.setPoint(2, {pos.x + radius * 0.82f, pos.y - radius * 0.08f});
                flag.setPoint(3, {pos.x - radius * 0.18f, pos.y - radius * 0.25f});
                flag.setFillColor(bodyColor);
                flag.setOutlineColor(outlineColor);
                flag.setOutlineThickness(outline);
                window_.draw(flag);

                sf::CircleShape base(radius * 0.44f);
                base.setOrigin({radius * 0.44f, radius * 0.44f});
                base.setPosition({pos.x - radius * 0.35f, pos.y + radius * 0.78f});
                base.setFillColor(blend(bodyColor, sf::Color::White, 0.16f));
                base.setOutlineColor(outlineColor);
                base.setOutlineThickness(1.2f);
                window_.draw(base);
                break;
            }
        }
    }

    void drawArmyFormation(const jke::Army& army,
                           sf::Vector2f pos,
                           float radius,
                           sf::Color bodyColor,
                           sf::Color outlineColor,
                           bool invasion) {
        const uint32_t soldiers = army.totalSoldiers();
        int detachments = 2;
        if (soldiers >= 900) detachments = 3;
        if (soldiers >= 1800) detachments = 4;
        if (soldiers >= 3000) detachments = 5;

        float spread = radius * 1.85f;
        if (army.role == jke::ArmyRole::Defense) spread = radius * 1.55f;
        if (army.role == jke::ArmyRole::Siege) spread = radius * 1.35f;
        if (army.role == jke::ArmyRole::Reserve) spread = radius * 1.25f;

        sf::Color zoneColor = bodyColor;
        zoneColor.a = invasion ? 58 : 38;
        sf::CircleShape zone(spread + radius * 0.65f);
        zone.setOrigin({spread + radius * 0.65f, spread + radius * 0.65f});
        zone.setPosition(pos);
        zone.setFillColor(zoneColor);
        zone.setOutlineColor(sf::Color{255, 255, 255,
            static_cast<uint8_t>(invasion ? 72 : 38)});
        zone.setOutlineThickness(0.8f);
        window_.draw(zone);

        const float startAngle =
            army.role == jke::ArmyRole::Attack ? -1.5708f :
            army.role == jke::ArmyRole::Defense ? 3.14159f :
            army.role == jke::ArmyRole::Siege ? -0.7854f : 2.3562f;

        for (int i = 0; i < detachments; ++i) {
            const float angle = startAngle +
                (static_cast<float>(i) - (detachments - 1) * 0.5f) * 0.62f;
            const float lane = spread * (0.72f + 0.10f * static_cast<float>(i % 2));
            sf::Vector2f p{
                pos.x + std::cos(angle) * lane,
                pos.y + std::sin(angle) * lane
            };

            sf::Color pipColor = blend(bodyColor, sf::Color::White, 0.16f);
            pipColor.a = 218;
            sf::Color pipOutline = outlineColor;
            pipOutline.a = 230;

            if (army.role == jke::ArmyRole::Attack) {
                sf::ConvexShape wedge(3);
                const float r = std::max(2.6f, radius * 0.36f);
                wedge.setPoint(0, {p.x, p.y - r});
                wedge.setPoint(1, {p.x + r * 0.82f, p.y + r * 0.72f});
                wedge.setPoint(2, {p.x - r * 0.82f, p.y + r * 0.72f});
                wedge.setFillColor(pipColor);
                wedge.setOutlineColor(pipOutline);
                wedge.setOutlineThickness(0.8f);
                window_.draw(wedge);
            } else if (army.role == jke::ArmyRole::Defense) {
                sf::RectangleShape block({radius * 0.58f, radius * 0.58f});
                block.setOrigin({radius * 0.29f, radius * 0.29f});
                block.setPosition(p);
                block.setFillColor(pipColor);
                block.setOutlineColor(pipOutline);
                block.setOutlineThickness(0.8f);
                window_.draw(block);
            } else if (army.role == jke::ArmyRole::Siege) {
                sf::RectangleShape engine({radius * 0.62f, radius * 0.44f});
                engine.setOrigin({radius * 0.31f, radius * 0.22f});
                engine.setPosition(p);
                engine.setFillColor(pipColor);
                engine.setOutlineColor(pipOutline);
                engine.setOutlineThickness(0.8f);
                window_.draw(engine);
            } else {
                sf::CircleShape dot(std::max(2.2f, radius * 0.26f));
                const float r = dot.getRadius();
                dot.setOrigin({r, r});
                dot.setPosition(p);
                dot.setFillColor(pipColor);
                dot.setOutlineColor(pipOutline);
                dot.setOutlineThickness(0.8f);
                window_.draw(dot);
            }
        }
    }

    void drawArmyOverlays() {
        const auto& kingdoms = engine_.kingdoms();
        const auto tileCount = static_cast<jke::TileID>(engine_.worldMap().tileCount());
        std::unordered_map<jke::TileID, int> stackIndex;

        for (const auto& [aid, army] : engine_.armies()) {
            auto kit = kingdoms.find(army.owner);
            if (kit == kingdoms.end() || !kit->second.isAlive) continue;
            if (army.currentTile == jke::NO_TILE || army.currentTile >= tileCount) continue;

            const auto& tile = engine_.worldMap().at(army.currentTile);
            const bool invasion = tile.owner != army.owner && tile.owner != jke::NO_KINGDOM;
            const uint32_t soldiers = army.totalSoldiers();

            int stack = stackIndex[army.currentTile]++;
            const float stackAngle = static_cast<float>(stack) * 2.39996f;
            const float stackRing = stack == 0 ? 0.f :
                10.f + 5.f * std::floor(std::sqrt(static_cast<float>(stack)));
            float offsetX = std::cos(stackAngle) * stackRing;
            float offsetY = std::sin(stackAngle) * stackRing;
            sf::Vector2f pos = tileCenter(army.currentTile) + sf::Vector2f{offsetX, offsetY};

            float radius = 6.f;
            if (soldiers >= 3500) radius = 10.f;
            else if (soldiers >= 1800) radius = 8.5f;
            else if (soldiers >= 700) radius = 7.f;

            sf::CircleShape shadow(radius + 3.f);
            shadow.setOrigin({radius + 3.f, radius + 3.f});
            shadow.setPosition({pos.x + 1.f, pos.y + 2.f});
            shadow.setFillColor(sf::Color{0, 0, 0, 165});
            window_.draw(shadow);

            // Supply-tinted body: orange at <50%, red at <25%
            sf::Color baseBodyColor = kingdomColor(army.owner);
            const float sup = army.supplyLevel;
            if (sup < 0.25f) {
                // Critical — blend strongly toward red
                baseBodyColor = blend(sf::Color{210, 40, 40}, baseBodyColor, 0.35f);
            } else if (sup < 0.50f) {
                // Warning — blend toward orange
                baseBodyColor = blend(sf::Color{220, 140, 20}, baseBodyColor, 0.40f);
            }

            sf::Color roleOutline = sf::Color{10, 12, 16};
            if (army.role == jke::ArmyRole::Defense) roleOutline = sf::Color{80, 170, 235};
            if (army.role == jke::ArmyRole::Siege) roleOutline = GOLD_COLOR;
            if (invasion) roleOutline = sf::Color{238, 72, 72};
            drawArmyFormation(army, pos, radius, baseBodyColor, roleOutline, invasion);
            drawArmyRoleMarker(army, pos, radius, baseBodyColor, roleOutline);

            // Supply bar below the army marker
            {
                const float barW  = radius * 2.2f;
                const float barH  = 3.f;
                const float barX  = pos.x - barW * 0.5f;
                const float barY  = pos.y + radius + 3.f;

                // Background track
                sf::RectangleShape track({barW, barH});
                track.setPosition({barX, barY});
                track.setFillColor(sf::Color{20, 20, 20, 200});
                window_.draw(track);

                // Filled portion
                sf::Color barColor = sup >= 0.5f
                    ? sf::Color{60, 200, 80}          // green
                    : sup >= 0.25f
                        ? sf::Color{220, 160, 30}     // orange
                        : sf::Color{210, 50, 50};     // red

                sf::RectangleShape fill({barW * std::clamp(sup, 0.0f, 1.0f), barH});
                fill.setPosition({barX, barY});
                fill.setFillColor(barColor);
                window_.draw(fill);
            }
        }
    }

    void drawSectionTitle(const std::string& title, float x, float y) {
        drawText(title, x, y, 12, GOLD_COLOR);
        drawRect(x, y + 18.f, PANEL_W - 32.f, 1.f, BORDER_COLOR);
    }

    void drawMetric(float x, float y, float w, const std::string& label,
                    const std::string& value, sf::Color accent) {
        drawRect(x, y, w, 54.f, SURFACE_COLOR, BORDER_COLOR, 1.f);
        drawRect(x, y, 4.f, 54.f, accent);
        drawText(label, x + 12.f, y + 8.f, 10, DIM_COLOR);
        drawText(value, x + 12.f, y + 25.f, 19, TEXT_COLOR);
    }

    std::string situationText(const jke::Kingdom& k) const {
        int attack = 0;
        int defense = 0;
        int siege = 0;
        int reserve = 0;
        jke::CityID targetCity = jke::NO_CITY;

        for (jke::ArmyID aid : k.armies) {
            auto ait = engine_.armies().find(aid);
            if (ait == engine_.armies().end()) continue;
            const auto& army = ait->second;
            if (army.role == jke::ArmyRole::Attack) ++attack;
            if (army.role == jke::ArmyRole::Defense) ++defense;
            if (army.role == jke::ArmyRole::Siege) ++siege;
            if (army.role == jke::ArmyRole::Reserve) ++reserve;

            if (army.targetTile != jke::NO_TILE &&
                army.targetTile < static_cast<jke::TileID>(engine_.worldMap().tileCount())) {
                const auto& tile = engine_.worldMap().at(army.targetTile);
                if (tile.city != jke::NO_CITY) targetCity = tile.city;
            }
        }
        if (k.currentWarGoal.active()) {
            for (jke::CityID cid : k.currentWarGoal.targetCities) {
                if (engine_.cities().count(cid) &&
                    engine_.cities().at(cid).owner != k.id) {
                    targetCity = cid;
                    break;
                }
            }
        }

        std::string status;
        if (!k.aiReason.empty()) {
            status = k.aiReason;
        } else if (k.currentWarGoal.active() &&
            engine_.kingdoms().count(k.currentWarGoal.enemy)) {
            const auto& enemy = engine_.kingdoms().at(k.currentWarGoal.enemy);
            if (targetCity != jke::NO_CITY && engine_.cities().count(targetCity)) {
                status = k.name + " targets " + engine_.cities().at(targetCity).name;
            } else {
                status = k.name + " pursues " +
                         std::string(jke::warGoalTypeName(k.currentWarGoal.type)) +
                         " vs " + enemy.name;
            }
        } else if (k.strategyPlan == jke::StrategyPlan::RevengeWar &&
            k.strategicTarget != jke::NO_KINGDOM &&
            engine_.kingdoms().count(k.strategicTarget)) {
            status = k.name + " seeks revenge on " +
                     engine_.kingdoms().at(k.strategicTarget).name;
        } else if (k.strategyPlan == jke::StrategyPlan::OpportunisticRaid &&
                   k.strategicTarget != jke::NO_KINGDOM &&
                   engine_.kingdoms().count(k.strategicTarget)) {
            status = k.name + " raids weak " +
                     engine_.kingdoms().at(k.strategicTarget).name;
        } else if ((k.policy == jke::NationalPolicy::Invading ||
                    k.policy == jke::NationalPolicy::FinalWar) &&
            targetCity != jke::NO_CITY && engine_.cities().count(targetCity)) {
            const auto& city = engine_.cities().at(targetCity);
            std::string targetOwner = engine_.kingdoms().count(city.owner)
                ? engine_.kingdoms().at(city.owner).name
                : "enemy";
            status = k.name + " advances on " + targetOwner;
        } else if (k.policy == jke::NationalPolicy::Defending) {
            status = k.name + " is defending its cities";
        } else if (k.policy == jke::NationalPolicy::CoalitionBuilding) {
            status = k.name + " is building alliances";
        } else if (k.policy == jke::NationalPolicy::FinalWar) {
            status = k.name + " is fighting the final war";
        } else {
            status = k.name + " is rebuilding";
        }

        std::ostringstream roles;
        roles << "  A" << attack << " D" << defense << " S" << siege << " R" << reserve;
        return ellipsize(status, 48) + roles.str();
    }

    // ── Render ────────────────────────────────────────────────────────────────
    void render() {
        window_.clear(BG_COLOR);

        // ── Default view: UI chrome ──────────────────────────────────────────
        drawRect(0.f, 0.f, static_cast<float>(WIN_W), 44.f, TOP_COLOR);
        drawText("JojiKingdomEngine", 18.f, 12.f, 18, GOLD_COLOR);
        drawText("continental simulation", 190.f, 15.f, 12, DIM_COLOR);

        // Map area background and frame (screen coords)
        drawRect(static_cast<float>(MAP_OX - 5), static_cast<float>(MAP_OY - 5),
                 static_cast<float>(VIEWPORT_PX + 10), static_cast<float>(VIEWPORT_PX + 10),
                 sf::Color{8, 10, 14});
        drawRect(static_cast<float>(MAP_OX - 2), static_cast<float>(MAP_OY - 2),
                 static_cast<float>(VIEWPORT_PX + 4), static_cast<float>(VIEWPORT_PX + 4),
                 SURFACE_COLOR, BORDER_COLOR, 1.f);

        // ── Map view: scrollable world ───────────────────────────────────────
        window_.setView(mapView_);
        window_.draw(mapSprite_);
        drawMapOverlays();

        // ── Back to default view: border + panel ─────────────────────────────
        window_.setView(window_.getDefaultView());

        // Draw tile inspector overlay (screen space, bottom-left of map area)
        if (selectedTile_ != jke::NO_TILE &&
            selectedTile_ < static_cast<jke::TileID>(engine_.worldMap().tileCount())) {
            drawTileInspector();
        }

        sf::RectangleShape border({VIEWPORT_PX + 2.f, VIEWPORT_PX + 2.f});
        border.setPosition({static_cast<float>(MAP_OX) - 1.f, static_cast<float>(MAP_OY) - 1.f});
        border.setFillColor(sf::Color::Transparent);
        border.setOutlineColor(sf::Color{96, 110, 132});
        border.setOutlineThickness(1.f);
        window_.draw(border);

        drawRect(static_cast<float>(PANEL_X), 62.f, static_cast<float>(PANEL_W),
                 static_cast<float>(WIN_H - 82), PANEL_COLOR, BORDER_COLOR, 1.f);

        drawPanel();
        window_.display();
    }

    void drawPanel() {
        const float px = PANEL_X + 16.f;
        float py = 78.f;
        const float contentW = PANEL_W - 32.f;

        auto turn = engine_.currentTurn();

        int alive = 0;
        uint64_t totalPop = 0;
        int totalArmies = 0;
        jke::KingdomID lastAlive = jke::NO_KINGDOM;
        for (const auto& [kid, k] : engine_.kingdoms()) {
            if (k.isAlive) {
                alive++;
                totalPop += k.totalPopulation;
                totalArmies += static_cast<int>(k.armies.size());
                lastAlive = kid;
            }
        }

        // ── Victory screen ───────────────────────────────────────────────────
        if (engine_.isOver() && alive == 1 && lastAlive != jke::NO_KINGDOM) {
            const auto& winner = engine_.kingdoms().at(lastAlive);
            int colorIdx = static_cast<int>(lastAlive) % 16;
            sf::Color wCol = KINGDOM_COL[colorIdx];

            drawRect(px - 16.f, py - 16.f, static_cast<float>(PANEL_W),
                     static_cast<float>(WIN_H - 60), sf::Color{8, 10, 14, 230});

            float cy = py + 60.f;
            drawText("CONTINENT UNIFIED", px + 20.f, cy, 22, GOLD_COLOR);
            cy += 42.f;
            drawRect(px, cy, contentW, 3.f, GOLD_COLOR);
            cy += 18.f;
            drawText(winner.name, px + 10.f, cy, 26, wCol);
            cy += 44.f;
            drawText("Turn " + std::to_string(turn), px + 10.f, cy, 14, DIM_COLOR);
            cy += 22.f;
            drawText("Population: " + compactNumber(winner.totalPopulation),
                     px + 10.f, cy, 13, TEXT_COLOR);
            cy += 20.f;
            drawText("Cities: " + std::to_string(winner.cities.size()),
                     px + 10.f, cy, 13, TEXT_COLOR);
            cy += 20.f;
            drawText("Armies: " + std::to_string(winner.armies.size()),
                     px + 10.f, cy, 13, TEXT_COLOR);
            cy += 30.f;
            drawText("Press Q to quit", px + 10.f, cy, 11, MUTED_COLOR);
            return;
        }

        drawMetric(px, py, 108.f, "TURN", std::to_string(turn),
                   paused_ ? GOLD_COLOR : sf::Color{86, 172, 126});
        drawMetric(px + 123.f, py, 108.f, "KINGDOMS", std::to_string(alive),
                   sf::Color{82, 142, 214});
        drawMetric(px + 246.f, py, 108.f, "POP", compactNumber(totalPop),
                   sf::Color{198, 120, 82});
        py += 68.f;

        drawRect(px, py, contentW, 34.f, sf::Color{20, 24, 32}, BORDER_COLOR, 1.f);
        drawText(paused_ ? "Paused" : "Running", px + 12.f, py + 9.f, 13,
                 paused_ ? GOLD_COLOR : sf::Color{118, 206, 148});
        drawText("Speed " + std::to_string(speed_) + " t/s", px + 104.f, py + 9.f, 12, DIM_COLOR);
        drawText("Armies " + std::to_string(totalArmies), px + 196.f, py + 9.f, 12, DIM_COLOR);
        drawText("Space/Arrows/WASD/Q", px + 266.f, py + 10.f, 9, MUTED_COLOR);
        py += 28.f;

        // Season indicator
        {
            jke::Season s = engine_.currentSeason();
            static const sf::Color SEASON_COL[] = {
                sf::Color{120, 220, 120},   // Spring - green
                sf::Color{240, 200, 60},    // Summer - gold
                sf::Color{220, 140, 50},    // Autumn - orange
                sf::Color{150, 200, 240},   // Winter - blue
            };
            int si = static_cast<int>(s);
            sf::Color sc = SEASON_COL[si];
            drawRect(px, py, contentW, 22.f, sf::Color{18, 22, 32, 200});
            drawText(std::string(jke::seasonName(s)), px + 6.f, py + 5.f, 11, sc);
            int year = engine_.currentTurn() / 80 + 1;
            std::string hordeStr = engine_.nomadHorde().active
                ? "  |  HORDE!" : "";
            drawText("Year " + std::to_string(year) + "  |  Bandits: " +
                     std::to_string(engine_.bandits().size()) + hordeStr,
                     px + 90.f, py + 5.f, 10,
                     engine_.nomadHorde().active ? sf::Color{255,80,60} : DIM_COLOR);
            py += 22.f;
        }
        py += 4.f;

        // ── Minimap ──────────────────────────────────────────────────────────
        {
            float scale = static_cast<float>(MINIMAP_PX) / MAP_TILES;
            minimapSprite_.setScale({scale, scale});
            minimapSprite_.setPosition({px, py});
            minimapScreenRect_ = sf::FloatRect({px, py}, {MINIMAP_PX, MINIMAP_PX});
            window_.draw(minimapSprite_);

            // Viewport indicator rectangle (size scales with zoom)
            float visWorld = viewSize();  // world units currently visible
            float vpW = visWorld / MAP_FULL_PX * MINIMAP_PX;
            float vpH = vpW;
            float vpL = (camCenter_.x - visWorld * 0.5f) / MAP_FULL_PX * MINIMAP_PX + px;
            float vpT = (camCenter_.y - visWorld * 0.5f) / MAP_FULL_PX * MINIMAP_PX + py;
            sf::RectangleShape vpRect({vpW, vpH});
            vpRect.setPosition({vpL, vpT});
            vpRect.setFillColor(sf::Color::Transparent);
            vpRect.setOutlineColor(sf::Color{255, 255, 255, 220});
            vpRect.setOutlineThickness(1.f);
            window_.draw(vpRect);

            py += MINIMAP_PX + 4.f;
            drawText("WASD pan  |  scroll = zoom  |  " + std::to_string(MAP_TILES) + "x" +
                     std::to_string(MAP_TILES) + " map", px, py, 9, MUTED_COLOR);
            py += 18.f;
        }

        // ── Active Wars ──────────────────────────────────────────────────────
        drawSectionTitle("ACTIVE WARS", px, py);
        py += 29.f;

        {
            const auto& kingdoms  = engine_.kingdoms();
            const auto& relations = engine_.relations();
            int warCount = 0;
            for (const auto& [lo, row] : relations) {
                for (const auto& [hi, rel] : row) {
                    if (rel.state != jke::RelationState::War) continue;
                    if (!kingdoms.count(lo) || !kingdoms.at(lo).isAlive) continue;
                    if (!kingdoms.count(hi) || !kingdoms.at(hi).isAlive) continue;
                    if (warCount >= 5) break;

                    const auto& ka = kingdoms.at(lo);
                    const auto& kb = kingdoms.at(hi);
                    std::string label = ellipsize(ka.name, 12) + " vs " + ellipsize(kb.name, 12);

                    drawRect(px, py, contentW, 21.f, sf::Color{40, 18, 18});
                    drawRect(px, py, 4.f, 21.f, sf::Color{200, 60, 60});
                    drawRect(px + 4.f, py, contentW / 2.f - 2.f, 21.f,
                             sf::Color{kingdomColor(lo).r, kingdomColor(lo).g,
                                       kingdomColor(lo).b, 40});
                    drawText(label, px + 10.f, py + 4.f, 10, sf::Color{230, 130, 130});

                    std::string turns = "T+" + std::to_string(rel.turnsAtWar);
                    drawText(turns, px + contentW - 48.f, py + 4.f, 10, sf::Color{160, 80, 80});
                    py += 24.f;
                    ++warCount;
                }
                if (warCount >= 5) break;
            }
            if (warCount == 0) {
                drawText("No active wars", px + 8.f, py, 10, sf::Color{80, 100, 80});
                py += 20.f;
            }
        }
        py += 8.f;

        // ── Situation ────────────────────────────────────────────────────────
        drawSectionTitle("SITUATION", px, py);
        py += 29.f;

        // sortedKingdoms_ is already sorted alive-first by population (cached per turn)
        size_t shownSituation = 0;
        for (size_t si = 0; si < sortedKingdoms_.size() && shownSituation < 4; ++si) {
            const auto* k = sortedKingdoms_[si];
            if (!k->isAlive) break;
            // Policy colour: red=war/finalWar, blue=defend, gold=coalition, green=rebuild
            sf::Color policyCol = sf::Color{80, 100, 80};
            if (k->policy == jke::NationalPolicy::FinalWar)         policyCol = sf::Color{220, 60,  60};
            else if (k->policy == jke::NationalPolicy::Invading)    policyCol = sf::Color{210, 110, 50};
            else if (k->policy == jke::NationalPolicy::Defending)   policyCol = sf::Color{70,  140, 210};
            else if (k->policy == jke::NationalPolicy::CoalitionBuilding) policyCol = GOLD_COLOR;

            drawRect(px, py, contentW, 24.f, sf::Color{22, 27, 36});
            drawRect(px, py, 4.f, 24.f, kingdomColor(k->id));
            drawText(situationText(*k), px + 11.f, py + 5.f, 10, TEXT_COLOR);
            drawText(std::string(jke::nationalPolicyName(k->policy)),
                     px + 256.f, py + 5.f, 10, policyCol);
            py += 27.f;
            ++shownSituation;
        }
        py += 8.f;

        // Kingdom leaderboard
        drawSectionTitle("KINGDOM STANDINGS", px, py);
        py += 29.f;

        // Sort kingdoms by population — cached per turn
        if (engine_.currentTurn() != sortedKingdomsTurn_) {
            sortedKingdoms_.clear();
            for (const auto& [kid, k] : engine_.kingdoms())
                sortedKingdoms_.push_back(&k);
            std::sort(sortedKingdoms_.begin(), sortedKingdoms_.end(),
                      [](const jke::Kingdom* a, const jke::Kingdom* b){
                          if (a->isAlive != b->isAlive) return a->isAlive > b->isAlive;
                          return a->totalPopulation > b->totalPopulation;
                      });
            sortedKingdomsTurn_ = engine_.currentTurn();
        }
        const auto& sorted = sortedKingdoms_;

        int rank = 1;
        for (const auto* k : sorted) {
            sf::Color kc = kingdomColor(k->id);
            if (!k->isAlive) kc = {70, 70, 80};

            drawRect(px, py, contentW, 32.f, rank % 2 == 0 ? sf::Color{28, 33, 43}
                                                           : sf::Color{23, 28, 37});
            drawRect(px, py, 4.f, 32.f, kc);

            std::ostringstream line;
            line << rank << ". " << ellipsize(k->name, 18);

            drawText(line.str(), px + 12.f, py + 4.f, 12, k->isAlive ? TEXT_COLOR : DIM_COLOR);

            if (k->isAlive) {
                std::ostringstream info;
                info << "P " << compactNumber(k->totalPopulation)
                     << "  C" << k->cities.size()
                     << "  A" << k->armies.size();
                drawText(info.str(), px + 156.f, py + 5.f, 11, DIM_COLOR);

                // Ruler name + traits
                if (k->hasRuler) {
                    std::string rulerLine = ellipsize(k->ruler.name, 20);
                    rulerLine += " (" + std::to_string(k->ruler.age) + ")";
                    drawText(rulerLine, px + 12.f, py + 19.f, 9, sf::Color{180,160,110});
                    // Traits
                    std::string traitLine;
                    for (size_t ti = 0; ti < k->ruler.traits.size(); ++ti) {
                        if (ti > 0) traitLine += " · ";
                        traitLine += std::string(jke::traitName(k->ruler.traits[ti]));
                    }
                    drawText(traitLine, px + 140.f, py + 19.f, 9, sf::Color{140,150,170});
                }

                // War weariness bar
                if (k->warWeariness > 0.05f) {
                    const float barW = 40.f;
                    const float barX = px + contentW - barW - 4.f;
                    const float barY = py + 6.f;
                    drawRect(barX, barY, barW, 5.f, sf::Color{30, 20, 20, 200});
                    sf::Color wCol = k->warWeariness > 0.65f
                        ? sf::Color{220, 50, 50}
                        : sf::Color{180, 100, 40};
                    drawRect(barX, barY, barW * std::clamp(k->warWeariness, 0.f, 1.f), 5.f, wCol);
                    drawText("W", barX - 10.f, barY - 1.f, 8, sf::Color{160, 80, 80});
                }
            } else {
                drawText("FALLEN", px + 278.f, py + 5.f, 11, sf::Color{190, 86, 86});
            }

            py += 40.f;
            rank++;
            if (rank > 8) break;
        }

        // Recent events
        py += 12.f;
        drawSectionTitle("RECENT EVENTS", px, py);
        py += 30.f;

        const auto& all = engine_.timeline().all();
        int start = std::max(0, static_cast<int>(all.size()) - 8);
        for (int i = static_cast<int>(all.size()) - 1;
             i >= start && py < WIN_H - 115; --i) {
            const auto& ev = all[i];
            std::string desc = ellipsize(ev.description, 48);
            sf::Color ec = DIM_COLOR;
            if (ev.type == jke::EventType::KingdomCollapsed) ec = {220, 80, 80};
            if (ev.type == jke::EventType::WarDeclared)      ec = {220, 140, 60};
            if (ev.type == jke::EventType::AllianceFormed)   ec = {80, 180, 220};
            if (ev.type == jke::EventType::ContinentUnified) ec = {220, 200, 60};
            if (ev.type == jke::EventType::CivilWar)              ec = {200, 80, 200};
            if (ev.type == jke::EventType::WorldEventPositive)    ec = {100, 210, 120};
            if (ev.type == jke::EventType::WorldEventNegative)    ec = {210, 100, 100};

            drawRect(px, py, contentW, 25.f, sf::Color{22, 27, 36});
            drawText(std::to_string(ev.turn), px + 9.f, py + 6.f, 10, MUTED_COLOR);
            drawText(desc, px + 44.f, py + 5.f, 11, ec);
            py += 28.f;
        }

        // ── Population Graph ─────────────────────────────────────────────────
        drawPopulationGraph(px, py);

        // Legend (bottom of panel)
        float ly = WIN_H - 132.f;
        drawSectionTitle("TERRAIN", px, ly);
        ly += 30.f;
        const char* terrainNames[] = {"Ocean","Coast","Plain","Forest","Hill","Mtn","River","Lake"};
        float lx = px;
        for (int i = 0; i < 8; ++i) {
            drawRect(lx, ly + 1.f, 11.f, 11.f, TERRAIN_COL[i], sf::Color{8, 10, 14}, 1.f);
            drawText(terrainNames[i], lx + 12.f, ly, 10, DIM_COLOR);
            lx += 52.f;
            if (lx > PANEL_X + PANEL_W - 60) { lx = px; ly += 15.f; }
        }

        ly += 21.f;
        const jke::TerrainType terrainOrder[] = {
            jke::TerrainType::Plain, jke::TerrainType::Forest,
            jke::TerrainType::Hill, jke::TerrainType::Mountain,
            jke::TerrainType::River, jke::TerrainType::Coast
        };
        for (int i = 0; i < 6; ++i) {
            float rowY = ly + static_cast<float>(i / 2) * 15.f;
            float colX = px + static_cast<float>(i % 2) * 176.f;
            auto terrain = terrainOrder[i];
            drawRect(colX, rowY + 2.f, 8.f, 8.f, TERRAIN_COL[static_cast<int>(terrain)]);
            drawText(std::string(terrainNames[static_cast<int>(terrain)]) + ": " +
                     terrainEffectText(terrain),
                     colX + 12.f, rowY, 9, MUTED_COLOR);
        }
    }
};

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    jke::SimulationConfig cfg;
    cfg.verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--seed"  && i+1 < argc) cfg.seed     = std::stoull(argv[++i]);
        else if (a == "--turns" && i+1 < argc) cfg.maxTurns = std::stoul(argv[++i]);
    }

    std::cout << "JojiKingdomEngine Viewer\n";
    std::cout << "Seed: " << cfg.seed << "  MaxTurns: ";
    if (cfg.maxTurns == 0) std::cout << "unlimited";
    else std::cout << cfg.maxTurns;
    std::cout << "\n";
    std::cout << "Controls: Space=pause  Up/Down=speed  Right=step  Q=quit\n\n";

    Viewer viewer(cfg);
    viewer.run();
    return 0;
}
