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

// ── Layout constants (fixed) ──────────────────────────────────────────────────
constexpr int TILE_PX      = 6;
constexpr int MAP_TILES    = 257;                        // 2^8+1 diamond-square
constexpr int MAP_FULL_PX  = MAP_TILES * TILE_PX;       // 1542 — full map texture
constexpr int MAP_OX       = 18;
constexpr int MAP_OY       = 62;
constexpr int MINIMAP_PX   = 190;
constexpr int PANEL_W_MIN  = 380;                        // minimum panel width

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

const sf::Color KINGDOM_COL[20] = {
    {220,  50,  50},  //  1 Red
    { 50,  95, 215},  //  2 Royal Blue
    {215, 120,  25},  //  3 Orange
    {140,  30, 215},  //  4 Violet
    { 40, 185,  60},  //  5 Green
    {215,  30, 130},  //  6 Hot Pink
    {215, 195,  25},  //  7 Yellow
    { 25, 160, 215},  //  8 Sky Blue
    {180, 215,  30},  //  9 Chartreuse
    {130,  20,  40},  // 10 Maroon      (Crimsonより暗く赤と明確に区別)
    { 30, 215, 120},  // 11 Spring Green (TurquoiseをB=120に変更→Cyanと明確に区別)
    { 75,  25, 195},  // 12 Indigo
    {215,  55,  95},  // 13 Rose        (FuchsiaをB=95に変更→Hot Pinkと区別)
    {175,  90,  30},  // 14 Sienna      (Lime廃止→暖色系で緑系過多を解消)
    {200, 145,  25},  // 15 Amber
    {200,  30, 200},  // 16 Magenta
    { 25,  45, 155},  // 17 Navy        (Emerald廃止→暗い青で地形と被らない)
    { 25, 205, 205},  // 18 Cyan
    {150,  80, 215},  // 19 Lavender
    {215,  95,  60},  // 20 Coral
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
    return KINGDOM_COL[(kid - 1) % 20];
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
        : window_(sf::VideoMode::getDesktopMode(), "JojiKingdomEngine",
                  sf::Style::Default)
        , config_(cfg)
        , engine_(config_)
        , font_(loadFont())
        , mapTexture_(sf::Vector2u(MAP_FULL_PX, MAP_FULL_PX))
        , mapSprite_(mapTexture_)
        , minimapTex_(sf::Vector2u(MAP_TILES, MAP_TILES))
        , minimapSprite_(minimapTex_)
    {
        window_.setFramerateLimit(60);
        mapSprite_.setPosition({0.f, 0.f});

        computeLayout();

        camCenter_ = {MAP_FULL_PX / 2.f, MAP_FULL_PX / 2.f};
        mapView_.setSize({static_cast<float>(viewportW_), static_cast<float>(viewportH_)});
        mapView_.setCenter(camCenter_);
        updateMapViewport();

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
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift) ||
                sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift)) {
                panSpeed *= 2.4f;
            }
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
    // ── Dynamic layout (recomputed on resize) ────────────────────────────────
    int winW_       = 1638;
    int winH_       = 1000;
    int viewportW_  = 1120;
    int viewportH_  = 900;
    int panelX_     = 940;
    int panelW_     = 380;

    void computeLayout() {
        auto size  = window_.getSize();
        winW_      = static_cast<int>(size.x);
        winH_      = static_cast<int>(size.y);
        panelW_    = std::clamp(winW_ / 4, PANEL_W_MIN, 560);
        panelX_    = winW_ - panelW_ - 18;
        viewportW_ = std::max(360, panelX_ - MAP_OX - 18);
        viewportH_ = std::max(360, winH_ - MAP_OY - 20);
    }

    void updateMapViewport() {
        mapView_.setSize({static_cast<float>(viewportW_), static_cast<float>(viewportH_)});
        mapView_.setViewport(sf::FloatRect(
            {static_cast<float>(MAP_OX) / winW_, static_cast<float>(MAP_OY) / winH_},
            {static_cast<float>(viewportW_) / winW_, static_cast<float>(viewportH_) / winH_}
        ));
        clampCamera();
    }

    sf::RenderWindow  window_;
    jke::SimulationConfig config_;
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
    bool              draggingMap_ = false;
    bool              leftMouseDown_ = false;
    bool              leftDragMap_ = false;
    sf::Vector2i      leftPressPixel_ = {0, 0};
    sf::Vector2i      dragLastPixel_ = {0, 0};

    float viewWidth() const { return  viewportW_ / zoom_; }
    float viewHeight() const { return viewportH_ / zoom_; }

    bool isMapPixel(sf::Vector2i pixel) const {
        return pixel.x >= MAP_OX && pixel.y >= MAP_OY &&
               pixel.x < MAP_OX + viewportW_ &&
               pixel.y < MAP_OY + viewportH_;
    }

    void selectTileAt(sf::Vector2i pixel) {
        sf::Vector2f world = window_.mapPixelToCoords(pixel, mapView_);
        int tx = static_cast<int>(world.x / TILE_PX);
        int ty = static_cast<int>(world.y / TILE_PX);
        if (tx >= 0 && ty >= 0 && tx < MAP_TILES && ty < MAP_TILES) {
            jke::TileID tid = static_cast<jke::TileID>(ty * MAP_TILES + tx);
            selectedTile_ = (selectedTile_ == tid) ? jke::NO_TILE : tid;
        } else {
            selectedTile_ = jke::NO_TILE;
        }
    }

    void clampCamera() {
        const float halfW = viewWidth() * 0.5f;
        const float halfH = viewHeight() * 0.5f;
        const float mapF = static_cast<float>(MAP_FULL_PX);
        if (halfW * 2.f >= mapF) {
            camCenter_.x = mapF * 0.5f;
        } else {
            camCenter_.x = std::clamp(camCenter_.x, halfW, mapF - halfW);
        }
        if (halfH * 2.f >= mapF) {
            camCenter_.y = mapF * 0.5f;
        } else {
            camCenter_.y = std::clamp(camCenter_.y, halfH, mapF - halfH);
        }
        mapView_.setCenter(camCenter_);
    }

    void applyZoom(float delta, sf::Vector2i mousePixels) {
        constexpr float STEP   = 1.18f;
        // Minimum zoom: never show more than the full map
        const float MIN_Z = std::min(static_cast<float>(viewportW_),
                                     static_cast<float>(viewportH_)) /
                            static_cast<float>(MAP_FULL_PX);
        constexpr float MAX_Z  = 8.0f;
        // Use pow so trackpad micro-deltas give small steps, wheel clicks give full STEP
        float factor = std::pow(STEP, delta);
        float newZoom = std::clamp(zoom_ * factor, MIN_Z, MAX_Z);
        if (newZoom == zoom_) return;

        // World point under cursor — should stay fixed while zooming
        sf::Vector2f worldBefore = window_.mapPixelToCoords(mousePixels, mapView_);
        zoom_ = newZoom;
        mapView_.setSize({viewWidth(), viewHeight()});

        // Re-derive world point and offset camera so it stays put
        sf::Vector2f worldAfter = window_.mapPixelToCoords(mousePixels, mapView_);
        camCenter_ += worldBefore - worldAfter;
        clampCamera();
    }

    void resetCamera() {
        zoom_ = 1.0f;
        mapView_.setSize({viewWidth(), viewHeight()});
        camCenter_ = {MAP_FULL_PX / 2.f, MAP_FULL_PX / 2.f};
        clampCamera();
    }

    void showFullMap() {
        const float minZoom = std::min(static_cast<float>(viewportW_),
                                       static_cast<float>(viewportH_)) /
                              static_cast<float>(MAP_FULL_PX);
        zoom_ = minZoom;
        mapView_.setSize({viewWidth(), viewHeight()});
        camCenter_ = {MAP_FULL_PX / 2.f, MAP_FULL_PX / 2.f};
        clampCamera();
    }

    void resetSimulation() {
        paused_ = true;
        engine_.reset();
        selectedTile_ = jke::NO_TILE;
        sortedKingdoms_.clear();
        sortedKingdomsTurn_ = ~0u;
        tileArmy_.clear();
        districtOwner_.clear();
        districtCity_.clear();
        previousDistrictOwner_.clear();
        districtFlashTurns_.clear();
        haveDistrictHistory_ = false;
        districtSignature_ = 0;
        minimapTurn_ = ~0u;
        lastMapImageTurn_ = ~0u;
        terrainBuilt_ = false;
        terrain_.clear();
        pixels_.clear();
        minimapPixels_.clear();
        logScrollOffset_ = 0;
        resetCamera();
        rebuildMapImage();
        camClock_.restart();
    }

    void adjustInitialKingdoms(int delta) {
        const uint32_t next = std::clamp<int>(
            static_cast<int>(config_.initialKingdoms) + delta, 4,
            jke::constants::NUM_KINGDOMS);
        if (next == config_.initialKingdoms) return;
        config_.initialKingdoms = next;
        engine_.setInitialKingdoms(config_.initialKingdoms);
        resetSimulation();
    }

    void centerOnSelected() {
        if (selectedTile_ == jke::NO_TILE ||
            selectedTile_ >= static_cast<jke::TileID>(engine_.worldMap().tileCount())) {
            return;
        }
        const auto& tile = engine_.worldMap().at(selectedTile_);
        camCenter_ = {
            static_cast<float>(tile.position.x * TILE_PX) + TILE_PX * 0.5f,
            static_cast<float>(tile.position.y * TILE_PX) + TILE_PX * 0.5f
        };
        clampCamera();
    }

    // Minimap
    sf::Texture          minimapTex_;
    sf::Sprite           minimapSprite_;
    std::vector<uint8_t> minimapPixels_;

    bool  paused_ = false;
    int   speed_  = 2;   // turns per second
    int   logScrollOffset_ = 0;   // chronicle scroll position (lines from top)

    // Mouse selection
    jke::TileID     selectedTile_     = jke::NO_TILE;
    sf::FloatRect   minimapScreenRect_;   // filled by drawPanel, used by mouse handler
    sf::FloatRect   pauseButtonRect_;
    sf::FloatRect   resetButtonRect_;
    sf::FloatRect   fitButtonRect_;
    sf::FloatRect   homeButtonRect_;
    sf::FloatRect   kingdomMinusRect_;
    sf::FloatRect   kingdomPlusRect_;

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
            } else if (const auto* resized = event->getIf<sf::Event::Resized>()) {
                (void)resized;
                window_.setView(sf::View(sf::FloatRect(
                    {0.f, 0.f},
                    {static_cast<float>(window_.getSize().x),
                     static_cast<float>(window_.getSize().y)})));
                computeLayout();
                updateMapViewport();
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
                        if (engine_.isOver())
                            logScrollOffset_ = std::max(0, logScrollOffset_ - 3);
                        else
                            speed_ = std::min(240, speed_ + (speed_ >= 20 ? 10 : 1));
                        break;
                    case sf::Keyboard::Key::Down:
                        if (engine_.isOver())
                            logScrollOffset_ += 3;
                        else
                            speed_ = std::max(1, speed_ - (speed_ > 20 ? 10 : 1));
                        break;
                    case sf::Keyboard::Key::Right:
                        if (paused_) { engine_.step(); rebuildMapImage(); }
                        break;
                    case sf::Keyboard::Key::R:
                        resetSimulation();
                        break;
                    case sf::Keyboard::Key::LBracket:
                    case sf::Keyboard::Key::Hyphen:
                        adjustInitialKingdoms(-1);
                        break;
                    case sf::Keyboard::Key::RBracket:
                    case sf::Keyboard::Key::Equal:
                        adjustInitialKingdoms(1);
                        break;
                    case sf::Keyboard::Key::Home:
                        resetCamera();
                        break;
                    case sf::Keyboard::Key::F:
                        showFullMap();
                        break;
                    case sf::Keyboard::Key::C:
                        centerOnSelected();
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
                    if (engine_.isOver()) {
                        logScrollOffset_ -= static_cast<int>(scroll->delta);
                        logScrollOffset_ = std::max(0, logScrollOffset_);
                    } else {
                        applyZoom(scroll->delta, scroll->position);
                    }
                }
            } else if (const auto* mm = event->getIf<sf::Event::MouseMoved>()) {
                if (leftMouseDown_ && !leftDragMap_) {
                    int dx = mm->position.x - leftPressPixel_.x;
                    int dy = mm->position.y - leftPressPixel_.y;
                    if (dx * dx + dy * dy >= 16) {
                        leftDragMap_ = true;
                    }
                }
                if (draggingMap_ || leftDragMap_) {
                    sf::Vector2f before = window_.mapPixelToCoords(dragLastPixel_, mapView_);
                    sf::Vector2f after = window_.mapPixelToCoords(mm->position, mapView_);
                    camCenter_ += before - after;
                    dragLastPixel_ = mm->position;
                    clampCamera();
                }
            } else if (const auto* mb = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (mb->button == sf::Mouse::Button::Right ||
                    mb->button == sf::Mouse::Button::Middle) {
                    draggingMap_ = true;
                    dragLastPixel_ = mb->position;
                }
                if (mb->button == sf::Mouse::Button::Left) {
                    sf::Vector2f mpos{static_cast<float>(mb->position.x),
                                      static_cast<float>(mb->position.y)};

                    if (pauseButtonRect_.contains(mpos)) {
                        paused_ = !paused_;
                    } else if (resetButtonRect_.contains(mpos)) {
                        resetSimulation();
                    } else if (fitButtonRect_.contains(mpos)) {
                        showFullMap();
                    } else if (homeButtonRect_.contains(mpos)) {
                        resetCamera();
                    } else if (kingdomMinusRect_.contains(mpos)) {
                        adjustInitialKingdoms(-1);
                    } else if (kingdomPlusRect_.contains(mpos)) {
                        adjustInitialKingdoms(1);
                    }
                    // Minimap click → jump camera
                    else if (minimapScreenRect_.contains(mpos)) {
                        float fx = (mpos.x - minimapScreenRect_.position.x) / minimapScreenRect_.size.x;
                        float fy = (mpos.y - minimapScreenRect_.position.y) / minimapScreenRect_.size.y;
                        camCenter_ = {fx * MAP_FULL_PX, fy * MAP_FULL_PX};
                        clampCamera();
                    }
                    // Map click selects on release; drag pans.
                    else if (isMapPixel(mb->position)) {
                        leftMouseDown_ = true;
                        leftDragMap_ = false;
                        leftPressPixel_ = mb->position;
                        dragLastPixel_ = mb->position;
                    }
                }
            } else if (const auto* mr = event->getIf<sf::Event::MouseButtonReleased>()) {
                if (mr->button == sf::Mouse::Button::Left) {
                    if (leftMouseDown_ && !leftDragMap_ && isMapPixel(mr->position)) {
                        selectTileAt(mr->position);
                    }
                    leftMouseDown_ = false;
                    leftDragMap_ = false;
                }
                if (mr->button == sf::Mouse::Button::Right ||
                    mr->button == sf::Mouse::Button::Middle) {
                    draggingMap_ = false;
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
            if (city.owner != jke::NO_KINGDOM) {
                auto kit = kingdoms.find(city.owner);
                if (kit == kingdoms.end() || !kit->second.isAlive) continue;
            }
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
        if (city.owner == jke::NO_KINGDOM) {
            switch (city.cityType) {
                case jke::CityType::Fortress:     return 17.0f;
                case jke::CityType::TradeHub:     return 16.0f;
                case jke::CityType::Port:         return 15.0f;
                case jke::CityType::Mining:       return 14.0f;
                case jke::CityType::Agricultural: return 13.0f;
                case jke::CityType::Generic:      return 12.0f;
            }
        }
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
                    idx < static_cast<int>(districtOwner_.size()) ? districtOwner_[idx] : jke::NO_KINGDOM;
                jke::CityID visualDistrict =
                    idx < static_cast<int>(districtCity_.size()) ? districtCity_[idx] : jke::NO_CITY;
                const bool districtControlled = tileInsideDistrictControl(t, visualDistrict);
                jke::KingdomID tintOwner = districtControlled ? visualOwner : jke::NO_KINGDOM;
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
                        : jke::NO_KINGDOM;
                    return no != tintOwner;
                }();
                bool kingdomBorderTop = (tintOwner != jke::NO_KINGDOM && ty > 0) && [&] {
                    const int nidx = (ty - 1) * MAP_TILES + tx;
                    const jke::Tile& nt = tiles[nidx];
                    const jke::CityID nd = districtCity_[nidx];
                    const jke::KingdomID no = tileInsideDistrictControl(nt, nd)
                        ? districtOwner_[nidx]
                        : jke::NO_KINGDOM;
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
                        const bool neutralDistrict =
                            districtControlled &&
                            visualDistrict != jke::NO_CITY &&
                            cities.count(visualDistrict) &&
                            cities.at(visualDistrict).owner == jke::NO_KINGDOM;
                        if (neutralDistrict) {
                            c = blend(c, sf::Color{168, 136, 86}, 0.58f);
                        }

                        // Neutral outpost tile — bright cream tint, clearly visible on any terrain
                        if (hasCity && tintOwner == jke::NO_KINGDOM &&
                            cities.count(t.city) &&
                            cities.at(t.city).owner == jke::NO_KINGDOM) {
                            c = blend(c, sf::Color{230, 204, 132}, 0.92f);
                        }

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
                            if (neutralDistrict) c = sf::Color{78, 62, 40};
                            else c = shade(c, -0.44f);
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
                                : jke::NO_KINGDOM;
                        if (owner != jke::NO_KINGDOM) {
                            c = blend(c, kingdomColor(owner), districtControlled ? 0.38f : 0.24f);
                        } else if (districtControlled &&
                                   district != jke::NO_CITY &&
                                   cities.count(district) &&
                                   cities.at(district).owner == jke::NO_KINGDOM) {
                            c = blend(c, sf::Color{168, 136, 86}, 0.62f);
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

    std::string fitText(std::string str, unsigned size, float maxWidth) const {
        if (maxWidth <= 0.0f) return "";
        sf::Text txt(font_, str, size);
        if (txt.getLocalBounds().size.x <= maxWidth) return str;

        const std::string suffix = "...";
        while (str.size() > suffix.size()) {
            str.pop_back();
            txt.setString(str + suffix);
            if (txt.getLocalBounds().size.x <= maxWidth) return str + suffix;
        }
        return suffix;
    }

    void drawTextFit(const std::string& str, float x, float y, float maxWidth,
                     unsigned size = 13, sf::Color col = TEXT_COLOR) {
        drawText(fitText(str, size, maxWidth), x, y, size, col);
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

        const float GW = static_cast<float>(panelW_ - 32);
        constexpr float GH = 72.f;
        float gx = px;
        float gy = py + 4.f;

        // Don't draw if it would collide with the terrain legend
        if (gy + GH + 40.f > winH_ - 132.f) return;

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
        float by = MAP_OY + viewportH_ - 156.f;
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

            auto roleColor = [](jke::ArmyRole r) -> sf::Color {
                switch (r) {
                    case jke::ArmyRole::Vanguard:    return sf::Color{255, 140,  60};
                    case jke::ArmyRole::Flanker:     return sf::Color{255, 210,  80};
                    case jke::ArmyRole::Siege:       return sf::Color{220,  80,  80};
                    case jke::ArmyRole::Attack:      return sf::Color{220, 110,  90};
                    case jke::ArmyRole::Garrison:    return sf::Color{ 80, 180, 255};
                    case jke::ArmyRole::SupplyGuard: return sf::Color{100, 220, 120};
                    case jke::ArmyRole::Defense:     return sf::Color{120, 160, 220};
                    default:                         return sf::Color{140, 148, 162};
                }
            };

            std::ostringstream as;
            as << "Army: " << army.totalSoldiers() << " soldiers";
            drawText(as.str(), bx + 6.f, y, 11, ac);
            y += 15.f;

            // Role (colored) + supply %
            sf::Color rc = roleColor(army.role);
            std::string roleLine = std::string(jke::armyRoleName(army.role));
            roleLine += "   Supply " + std::to_string(static_cast<int>(army.supplyLevel * 100)) + "%";
            drawText(roleLine, bx + 6.f, y, 10, rc);
            y += 13.f;

            // Supply bar
            {
                float barW = static_cast<float>(panelW_ - 18);
                float filled = barW * std::clamp(army.supplyLevel, 0.0f, 1.0f);
                sf::Color barCol = army.supplyLevel >= 0.7f ? sf::Color{80, 200, 100}
                                 : army.supplyLevel >= 0.4f ? sf::Color{220, 180, 60}
                                                            : sf::Color{220, 70, 70};
                sf::RectangleShape bg({barW, 4.f});
                bg.setPosition({bx + 6.f, y});
                bg.setFillColor(sf::Color{40, 46, 60});
                window_.draw(bg);
                if (filled > 0.f) {
                    sf::RectangleShape bar({filled, 4.f});
                    bar.setPosition({bx + 6.f, y});
                    bar.setFillColor(barCol);
                    window_.draw(bar);
                }
                y += 8.f;
            }

            // Target city or tile
            if (army.targetTile != jke::NO_TILE &&
                army.targetTile < static_cast<jke::TileID>(engine_.worldMap().tileCount())) {
                const auto& tgt = engine_.worldMap().at(army.targetTile);
                std::string targetStr = "Target: ";
                if (tgt.city != jke::NO_CITY && engine_.cities().count(tgt.city)) {
                    targetStr += engine_.cities().at(tgt.city).name;
                } else {
                    targetStr += "(" + std::to_string(tgt.position.x) +
                                 "," + std::to_string(tgt.position.y) + ")";
                }
                if (army.currentTile != jke::NO_TILE &&
                    army.currentTile < static_cast<jke::TileID>(engine_.worldMap().tileCount())) {
                    const auto& ap = engine_.worldMap().at(army.currentTile).position;
                    int dist = static_cast<int>(
                        std::hypot(float(tgt.position.x - ap.x),
                                   float(tgt.position.y - ap.y)));
                    targetStr += " (" + std::to_string(dist) + "t)";
                }
                drawText(targetStr, bx + 6.f, y, 10, DIM_COLOR);
                y += 13.f;
            }

            if (army.hasCommander()) {
                std::string state = army.commander.wounded ? " (wounded)" : "";
                drawText("General: " + ellipsize(army.commander.name, 24) + state,
                         bx + 6.f, y, 10, army.commander.fame >= 0.45f ? GOLD_COLOR : DIM_COLOR);
                y += 13.f;
            } else {
                drawText("General: none", bx + 6.f, y, 10, MUTED_COLOR);
                y += 13.f;
            }
            if (army.hasStrategist()) {
                std::string state = army.strategist.wounded ? " (wounded)" : "";
                drawText("Strategist: " + ellipsize(army.strategist.name, 22) + state,
                         bx + 6.f, y, 10, army.strategist.fame >= 0.45f ? GOLD_COLOR : DIM_COLOR);
            }
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
            if (city.tile == jke::NO_TILE) continue;
            const bool neutral = city.owner == jke::NO_KINGDOM;
            if (!neutral) {
                auto kit = kingdoms.find(city.owner);
                if (kit == kingdoms.end() || !kit->second.isAlive) continue;
            }

            sf::Vector2f pos = tileCenter(city.tile);
            sf::Color owner = neutral ? sf::Color{210, 198, 142} : kingdomColor(city.owner);

            if (neutral || city.isCapital || city.cityType != jke::CityType::Generic) {
                float influenceRadius = neutral ? 28.f : 32.f;
                if (city.isCapital) influenceRadius = 74.f;
                else if (city.cityType == jke::CityType::Fortress) influenceRadius = 52.f;
                else if (city.cityType == jke::CityType::TradeHub) influenceRadius = 46.f;
                else if (city.cityType == jke::CityType::Port) influenceRadius = 42.f;
                else if (city.cityType == jke::CityType::Mining) influenceRadius = 38.f;
                if (neutral && city.cityType == jke::CityType::Generic) influenceRadius = 24.f;

                sf::Color ringColor = city.isCapital
                    ? GOLD_COLOR
                    : blend(owner, sf::Color::White, 0.36f);
                ringColor.a = city.isCapital ? 86 : (neutral ? 44 : 54);

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

            float radius = city.isCapital ? 10.f : (neutral ? 4.8f : 6.f);
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
            ring.setOutlineThickness(city.isCapital ? 2.4f : (neutral ? 1.2f : 1.6f));
            window_.draw(ring);

            const float coreRadius = city.isCapital ? 3.6f : (neutral ? 2.0f : 2.6f);
            sf::CircleShape core(coreRadius);
            core.setOrigin({coreRadius, coreRadius});
            core.setPosition(pos);
            core.setFillColor(city.isCapital ? GOLD_COLOR :
                              neutral ? sf::Color{230, 218, 165} : sf::Color{238, 242, 247});
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
            case jke::ArmyRole::Vanguard:
            case jke::ArmyRole::Flanker:
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
            case jke::ArmyRole::Garrison:
            case jke::ArmyRole::SupplyGuard:
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
        if (army.role == jke::ArmyRole::Defense ||
            army.role == jke::ArmyRole::Garrison ||
            army.role == jke::ArmyRole::SupplyGuard) spread = radius * 1.55f;
        if (army.role == jke::ArmyRole::Siege) spread = radius * 1.35f;
        if (army.role == jke::ArmyRole::Reserve) spread = radius * 1.25f;
        if (army.role == jke::ArmyRole::Flanker) spread = radius * 2.15f;

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
            (army.role == jke::ArmyRole::Attack ||
             army.role == jke::ArmyRole::Vanguard) ? -1.5708f :
            army.role == jke::ArmyRole::Flanker ? -0.35f :
            (army.role == jke::ArmyRole::Defense ||
             army.role == jke::ArmyRole::Garrison ||
             army.role == jke::ArmyRole::SupplyGuard) ? 3.14159f :
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

            if (army.role == jke::ArmyRole::Attack ||
                army.role == jke::ArmyRole::Vanguard ||
                army.role == jke::ArmyRole::Flanker) {
                sf::ConvexShape wedge(3);
                const float r = std::max(2.6f, radius * 0.36f);
                wedge.setPoint(0, {p.x, p.y - r});
                wedge.setPoint(1, {p.x + r * 0.82f, p.y + r * 0.72f});
                wedge.setPoint(2, {p.x - r * 0.82f, p.y + r * 0.72f});
                wedge.setFillColor(pipColor);
                wedge.setOutlineColor(pipOutline);
                wedge.setOutlineThickness(0.8f);
                window_.draw(wedge);
            } else if (army.role == jke::ArmyRole::Defense ||
                       army.role == jke::ArmyRole::Garrison ||
                       army.role == jke::ArmyRole::SupplyGuard) {
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
            if (army.role == jke::ArmyRole::Defense ||
                army.role == jke::ArmyRole::Garrison ||
                army.role == jke::ArmyRole::SupplyGuard) roleOutline = sf::Color{80, 170, 235};
            if (army.role == jke::ArmyRole::Siege) roleOutline = GOLD_COLOR;
            if (army.role == jke::ArmyRole::Vanguard) roleOutline = sf::Color{255, 90, 70};
            if (army.role == jke::ArmyRole::Flanker) roleOutline = sf::Color{196, 110, 255};
            if (invasion) roleOutline = sf::Color{238, 72, 72};
            if (army.hasNotableLeader()) roleOutline = sf::Color{245, 220, 90};
            drawArmyFormation(army, pos, radius, baseBodyColor, roleOutline, invasion);
            drawArmyRoleMarker(army, pos, radius, baseBodyColor, roleOutline);
            if (army.hasNotableLeader()) {
                sf::CircleShape halo(radius + 5.f);
                halo.setOrigin({radius + 5.f, radius + 5.f});
                halo.setPosition(pos);
                halo.setFillColor(sf::Color::Transparent);
                halo.setOutlineColor(sf::Color{245, 220, 90, 150});
                halo.setOutlineThickness(1.8f);
                window_.draw(halo);
            }

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
        drawRect(x, y + 18.f, panelW_ - 32.f, 1.f, BORDER_COLOR);
    }

    void drawMetric(float x, float y, float w, const std::string& label,
                    const std::string& value, sf::Color accent) {
        drawRect(x, y, w, 54.f, SURFACE_COLOR, BORDER_COLOR, 1.f);
        drawRect(x, y, 4.f, 54.f, accent);
        drawText(label, x + 12.f, y + 8.f, 10, DIM_COLOR);
        drawText(value, x + 12.f, y + 25.f, 19, TEXT_COLOR);
    }

    void drawButton(const sf::FloatRect& rect, const std::string& label, sf::Color accent) {
        drawRect(rect.position.x, rect.position.y, rect.size.x, rect.size.y,
                 sf::Color{30, 36, 48}, BORDER_COLOR, 1.f);
        drawRect(rect.position.x, rect.position.y, 3.f, rect.size.y, accent);
        drawText(label, rect.position.x + 10.f, rect.position.y + 6.f, 11, TEXT_COLOR);
    }

    std::string situationText(const jke::Kingdom& k) const {
        int attack = 0;
        int defense = 0;
        int siege = 0;
        int reserve = 0;
        int vanguard = 0;
        int flanker = 0;
        int guard = 0;
        jke::CityID targetCity = jke::NO_CITY;

        for (jke::ArmyID aid : k.armies) {
            auto ait = engine_.armies().find(aid);
            if (ait == engine_.armies().end()) continue;
            const auto& army = ait->second;
            if (army.role == jke::ArmyRole::Attack) ++attack;
            if (army.role == jke::ArmyRole::Defense) ++defense;
            if (army.role == jke::ArmyRole::Siege) ++siege;
            if (army.role == jke::ArmyRole::Reserve) ++reserve;
            if (army.role == jke::ArmyRole::Vanguard) ++vanguard;
            if (army.role == jke::ArmyRole::Flanker) ++flanker;
            if (army.role == jke::ArmyRole::Garrison ||
                army.role == jke::ArmyRole::SupplyGuard) ++guard;

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
        roles << "  V" << vanguard << " F" << flanker
              << " A" << attack << " D" << defense + guard
              << " S" << siege << " R" << reserve;
        return ellipsize(status, 54) + roles.str();
    }

    // ── Render ────────────────────────────────────────────────────────────────
    void render() {
        window_.clear(BG_COLOR);

        // ── Default view: UI chrome ──────────────────────────────────────────
        drawRect(0.f, 0.f, static_cast<float>(winW_), 44.f, TOP_COLOR);
        drawText("JojiKingdomEngine", 18.f, 12.f, 18, GOLD_COLOR);
        drawText("continental simulation", 190.f, 15.f, 12, DIM_COLOR);
        pauseButtonRect_ = sf::FloatRect({static_cast<float>(panelX_), 9.f}, {62.f, 26.f});
        resetButtonRect_ = sf::FloatRect({static_cast<float>(panelX_ + 70), 9.f}, {62.f, 26.f});
        fitButtonRect_   = sf::FloatRect({static_cast<float>(panelX_ + 140), 9.f}, {50.f, 26.f});
        homeButtonRect_  = sf::FloatRect({static_cast<float>(panelX_ + 198), 9.f}, {58.f, 26.f});
        kingdomMinusRect_ = sf::FloatRect({static_cast<float>(panelX_ + 266), 9.f}, {28.f, 26.f});
        kingdomPlusRect_  = sf::FloatRect({static_cast<float>(panelX_ + 302), 9.f}, {28.f, 26.f});
        drawButton(pauseButtonRect_, paused_ ? "Run" : "Pause",
                   paused_ ? sf::Color{86, 172, 126} : GOLD_COLOR);
        drawButton(resetButtonRect_, "Reset", sf::Color{220, 90, 90});
        drawButton(fitButtonRect_, "Fit", sf::Color{82, 142, 214});
        drawButton(homeButtonRect_, "Home", sf::Color{160, 150, 220});
        drawButton(kingdomMinusRect_, "-", sf::Color{120, 130, 150});
        drawButton(kingdomPlusRect_, "+", sf::Color{86, 172, 126});
        drawText("K " + std::to_string(config_.initialKingdoms),
                 static_cast<float>(panelX_ + 338), 15.f, 11, TEXT_COLOR);

        // Map area background and frame (screen coords)
        drawRect(static_cast<float>(MAP_OX - 5), static_cast<float>(MAP_OY - 5),
                 static_cast<float>(viewportW_ + 10), static_cast<float>(viewportH_ + 10),
                 sf::Color{8, 10, 14});
        drawRect(static_cast<float>(MAP_OX - 2), static_cast<float>(MAP_OY - 2),
                 static_cast<float>(viewportW_ + 4), static_cast<float>(viewportH_ + 4),
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

        sf::RectangleShape border({static_cast<float>(viewportW_ + 2), static_cast<float>(viewportH_ + 2)});
        border.setPosition({static_cast<float>(MAP_OX) - 1.f, static_cast<float>(MAP_OY) - 1.f});
        border.setFillColor(sf::Color::Transparent);
        border.setOutlineColor(sf::Color{96, 110, 132});
        border.setOutlineThickness(1.f);
        window_.draw(border);

        drawRect(static_cast<float>(panelX_), 62.f, static_cast<float>(panelW_),
                 static_cast<float>(winH_ - 82), PANEL_COLOR, BORDER_COLOR, 1.f);

        drawPanel();
        window_.display();
    }

    void drawPanel() {
        const float px = panelX_ + 16.f;
        float py = 78.f;
        const float contentW = panelW_ - 32.f;

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

        // ── Victory / Chronicle screen ───────────────────────────────────────
        if (engine_.isOver() && alive == 1 && lastAlive != jke::NO_KINGDOM) {
            const auto& winner = engine_.kingdoms().at(lastAlive);
            sf::Color wCol = KINGDOM_COL[static_cast<int>(lastAlive) % 16];

            // Full-panel dark backdrop
            drawRect(px - 16.f, py - 16.f,
                     static_cast<float>(panelW_),
                     static_cast<float>(winH_ - 60),
                     sf::Color{8, 10, 14, 230});

            float cy = py + 14.f;

            // ── Winner header ─────────────────────────────────────────────────
            drawText("CONTINENT UNIFIED", px + 10.f, cy, 18, GOLD_COLOR);
            cy += 30.f;
            drawRect(px, cy, contentW, 2.f, GOLD_COLOR);
            cy += 8.f;
            drawTextFit(winner.name, px + 8.f, cy, contentW - 16.f, 22, wCol);
            cy += 34.f;

            // Stat row
            std::string statLine =
                "Turn " + std::to_string(turn) +
                "  \xb7  Pop " + compactNumber(winner.totalPopulation) +
                "  \xb7  Cities " + std::to_string(winner.cities.size()) +
                "  \xb7  Armies " + std::to_string(winner.armies.size());
            drawTextFit(statLine, px + 8.f, cy, contentW - 16.f, 10, DIM_COLOR);
            cy += 20.f;

            drawRect(px, cy, contentW, 1.f, BORDER_COLOR);
            cy += 10.f;

            // ── Chronicle header ──────────────────────────────────────────────
            drawText("CHRONICLE", px + 8.f, cy, 11,
                     sf::Color{160, 160, 180});
            drawText("\xe2\x86\x91\xe2\x86\x93 scroll  R reset  Q quit",
                     px + contentW - 130.f, cy + 2.f, 9, MUTED_COLOR);
            cy += 22.f;

            // ── Filter significant events ─────────────────────────────────────
            const auto& all = engine_.timeline().all();
            std::vector<const jke::HistoryEvent*> log;
            log.reserve(all.size());
            for (const auto& ev : all) {
                switch (ev.type) {
                    case jke::EventType::WarDeclared:
                    case jke::EventType::PeaceSigned:
                    case jke::EventType::AllianceFormed:
                    case jke::EventType::AllianceBroken:
                    case jke::EventType::BattleFought:
                    case jke::EventType::CityConquered:
                    case jke::EventType::CapitalCaptured:
                    case jke::EventType::RebellionStarted:
                    case jke::EventType::RebellionSuppressed:
                    case jke::EventType::CivilWar:
                    case jke::EventType::KingdomCollapsed:
                    case jke::EventType::KingdomAnnexed:
                    case jke::EventType::ContinentUnified:
                    case jke::EventType::TechResearched:
                    case jke::EventType::VassalFormed:
                    case jke::EventType::VassalLiberated:
                    case jke::EventType::WorldEventPositive:
                    case jke::EventType::WorldEventNegative:
                    case jke::EventType::PlagueSpread:
                    case jke::EventType::TributaryEstablished:
                        log.push_back(&ev);
                        break;
                    default:
                        break;
                }
            }

            // ── Scrollable list ───────────────────────────────────────────────
            const float logTop    = cy;
            const float logBottom = static_cast<float>(winH_) - 72.f;
            const float lineH     = 27.f;
            const int   visLines  = std::max(1, static_cast<int>((logBottom - logTop) / lineH));
            const int   maxScroll = std::max(0, static_cast<int>(log.size()) - visLines);
            logScrollOffset_ = std::clamp(logScrollOffset_, 0, maxScroll);

            for (int i = logScrollOffset_;
                 i < static_cast<int>(log.size()) && cy + lineH <= logBottom;
                 ++i)
            {
                const auto& ev = *log[i];

                // Row background (alternating)
                sf::Color rowBg = (i % 2 == 0) ? sf::Color{18, 22, 32} : sf::Color{22, 27, 38};
                drawRect(px, cy, contentW, lineH - 2.f, rowBg);

                // Turn tag
                std::string turnTag = "T" + std::to_string(ev.turn);
                drawText(turnTag, px + 4.f, cy + 7.f, 8, sf::Color{90, 100, 120});

                // Event color by type
                sf::Color ec = TEXT_COLOR;
                if      (ev.type == jke::EventType::KingdomCollapsed)  ec = {220, 80,  80};
                else if (ev.type == jke::EventType::CapitalCaptured)   ec = {220, 110, 60};
                else if (ev.type == jke::EventType::WarDeclared)       ec = {220, 160, 60};
                else if (ev.type == jke::EventType::BattleFought)      ec = {200, 180, 100};
                else if (ev.type == jke::EventType::AllianceFormed)    ec = {80,  180, 220};
                else if (ev.type == jke::EventType::AllianceBroken)    ec = {120, 140, 180};
                else if (ev.type == jke::EventType::ContinentUnified)  ec = GOLD_COLOR;
                else if (ev.type == jke::EventType::CivilWar)          ec = {200, 80,  200};
                else if (ev.type == jke::EventType::RebellionStarted)  ec = {180, 100, 200};
                else if (ev.type == jke::EventType::PlagueSpread)      ec = {100, 200, 140};
                else if (ev.type == jke::EventType::WorldEventPositive)ec = {100, 210, 120};
                else if (ev.type == jke::EventType::WorldEventNegative)ec = {210, 100, 100};
                else if (ev.type == jke::EventType::KingdomAnnexed)    ec = {180, 100,  60};
                else if (ev.type == jke::EventType::TechResearched)    ec = {100, 160, 220};
                else if (ev.type == jke::EventType::VassalFormed)      ec = {140, 180, 140};
                else if (ev.type == jke::EventType::PeaceSigned)       ec = {120, 200, 160};

                drawTextFit(ev.description,
                            px + 38.f, cy + 6.f,
                            contentW - 46.f, 10, ec);
                cy += lineH;
            }

            // Scroll progress indicator
            if (maxScroll > 0) {
                float barH   = logBottom - logTop;
                float thumbH = std::max(16.f, barH * visLines / static_cast<float>(log.size()));
                float thumbY = logTop + (barH - thumbH) *
                               static_cast<float>(logScrollOffset_) / static_cast<float>(maxScroll);
                drawRect(px + contentW + 4.f, logTop, 4.f, barH, sf::Color{30, 36, 50});
                drawRect(px + contentW + 4.f, thumbY, 4.f, thumbH, sf::Color{100, 110, 140});
            }

            // Bottom hint bar
            drawRect(px - 16.f,
                     static_cast<float>(winH_) - 66.f,
                     static_cast<float>(panelW_), 52.f,
                     sf::Color{10, 12, 18, 220});
            std::string countLabel = std::to_string(logScrollOffset_ + 1) + "-" +
                std::to_string(std::min(logScrollOffset_ + visLines,
                                        static_cast<int>(log.size()))) +
                " / " + std::to_string(log.size()) + " events";
            drawText(countLabel, px + 8.f, static_cast<float>(winH_) - 56.f, 9, MUTED_COLOR);
            drawText("R reset  |  Q quit",
                     px + 8.f, static_cast<float>(winH_) - 42.f, 10, DIM_COLOR);
            return;
        }

        const float metricGap = 14.f;
        const float metricW = (contentW - metricGap * 2.f) / 3.f;
        drawMetric(px, py, metricW, "TURN", std::to_string(turn),
                   paused_ ? GOLD_COLOR : sf::Color{86, 172, 126});
        drawMetric(px + metricW + metricGap, py, metricW, "KINGDOMS", std::to_string(alive),
                   sf::Color{82, 142, 214});
        drawMetric(px + (metricW + metricGap) * 2.f, py, metricW, "POP", compactNumber(totalPop),
                   sf::Color{198, 120, 82});
        py += 68.f;

        drawRect(px, py, contentW, 34.f, sf::Color{20, 24, 32}, BORDER_COLOR, 1.f);
        drawText(paused_ ? "Paused" : "Running", px + 12.f, py + 9.f, 13,
                 paused_ ? GOLD_COLOR : sf::Color{118, 206, 148});
        drawText("Speed " + std::to_string(speed_) + " t/s", px + 104.f, py + 9.f, 12, DIM_COLOR);
        drawText("Armies " + std::to_string(totalArmies), px + 196.f, py + 9.f, 12, DIM_COLOR);
        drawTextFit("Start " + std::to_string(config_.initialKingdoms) + "  [-/+]",
                    px + 284.f, py + 10.f, contentW - 292.f, 9, MUTED_COLOR);
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
            drawTextFit("Year " + std::to_string(year) + "  |  Bandits: " +
                        std::to_string(engine_.bandits().size()) + hordeStr,
                        px + 90.f, py + 5.f, contentW - 96.f, 10,
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
            float visWorldW = viewWidth();
            float visWorldH = viewHeight();
            float vpW = visWorldW / MAP_FULL_PX * MINIMAP_PX;
            float vpH = visWorldH / MAP_FULL_PX * MINIMAP_PX;
            float vpL = (camCenter_.x - visWorldW * 0.5f) / MAP_FULL_PX * MINIMAP_PX + px;
            float vpT = (camCenter_.y - visWorldH * 0.5f) / MAP_FULL_PX * MINIMAP_PX + py;
            sf::RectangleShape vpRect({vpW, vpH});
            vpRect.setPosition({vpL, vpT});
            vpRect.setFillColor(sf::Color::Transparent);
            vpRect.setOutlineColor(sf::Color{255, 255, 255, 220});
            vpRect.setOutlineThickness(1.f);
            window_.draw(vpRect);

            py += MINIMAP_PX + 4.f;
            drawTextFit("WASD/drag pan  |  scroll zoom  |  C selected  |  1-9 capitals",
                        px, py, contentW, 9, MUTED_COLOR);
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
                    if (warCount >= 3) break;

                    const auto& ka = kingdoms.at(lo);
                    const auto& kb = kingdoms.at(hi);
                    std::string label = ellipsize(ka.name, 20) + " vs " + ellipsize(kb.name, 20);

                    drawRect(px, py, contentW, 21.f, sf::Color{40, 18, 18});
                    drawRect(px, py, 4.f, 21.f, sf::Color{200, 60, 60});
                    drawRect(px + 4.f, py, contentW / 2.f - 2.f, 21.f,
                             sf::Color{kingdomColor(lo).r, kingdomColor(lo).g,
                                       kingdomColor(lo).b, 40});
                    drawTextFit(label, px + 10.f, py + 4.f, contentW - 68.f, 10, sf::Color{230, 130, 130});

                    std::string turns = "T+" + std::to_string(rel.turnsAtWar);
                    drawText(turns, px + contentW - 48.f, py + 4.f, 10, sf::Color{160, 80, 80});
                    py += 24.f;
                    ++warCount;
                }
                if (warCount >= 3) break;
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
        for (size_t si = 0; si < sortedKingdoms_.size() && shownSituation < 2; ++si) {
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
            drawTextFit(situationText(*k), px + 11.f, py + 5.f, contentW - 130.f, 10, TEXT_COLOR);
            drawTextFit(std::string(jke::nationalPolicyName(k->policy)),
                        px + contentW - 112.f, py + 5.f, 106.f, 10, policyCol);
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
            line << rank << ". " << k->name;

            drawTextFit(line.str(), px + 12.f, py + 4.f, contentW - 260.f, 12,
                        k->isAlive ? TEXT_COLOR : DIM_COLOR);

            if (k->isAlive) {
                std::ostringstream info;
                info << "P " << compactNumber(k->totalPopulation)
                     << "  C" << k->cities.size()
                     << "  A" << k->armies.size();
                drawTextFit(info.str(), px + contentW - 250.f, py + 5.f, 190.f, 11, DIM_COLOR);

                // Ruler name + traits
                if (k->hasRuler) {
                    std::string rulerLine = k->ruler.name;
                    rulerLine += " (" + std::to_string(k->ruler.age) + ")";
                    drawTextFit(rulerLine, px + 12.f, py + 19.f, 210.f, 9, sf::Color{180,160,110});
                    // Traits
                    std::string traitLine;
                    for (size_t ti = 0; ti < k->ruler.traits.size(); ++ti) {
                        if (ti > 0) traitLine += " / ";
                        traitLine += std::string(jke::traitName(k->ruler.traits[ti]));
                    }
                    drawTextFit(traitLine, px + 232.f, py + 19.f, contentW - 288.f, 9, sf::Color{140,150,170});
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
                drawTextFit("FALLEN", px + contentW - 76.f, py + 5.f, 72.f, 11, sf::Color{190, 86, 86});
            }

            py += 40.f;
            rank++;
            if (rank > 4) break;
        }

        // Recent events
        py += 8.f;
        drawSectionTitle("RECENT EVENTS", px, py);
        py += 30.f;

        const auto& all = engine_.timeline().all();
        int start = std::max(0, static_cast<int>(all.size()) - 4);
        for (int i = static_cast<int>(all.size()) - 1;
             i >= start && py < winH_ - 40; --i) {
            const auto& ev = all[i];
            std::string desc = ev.description;
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
            drawTextFit(desc, px + 44.f, py + 5.f, contentW - 52.f, 11, ec);
            py += 28.f;
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
        else if (a == "--kingdoms" && i+1 < argc) {
            cfg.initialKingdoms =
                std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(argv[++i])), 4u,
                                     static_cast<uint32_t>(jke::constants::NUM_KINGDOMS));
        }
    }

    std::cout << "JojiKingdomEngine Viewer\n";
    std::cout << "Seed: " << cfg.seed << "  MaxTurns: ";
    if (cfg.maxTurns == 0) std::cout << "unlimited";
    else std::cout << cfg.maxTurns;
    std::cout << "  Kingdoms: " << cfg.initialKingdoms << "\n";
    std::cout << "Controls: Space=pause  Up/Down=speed  Right=step  [-]/[+]=kingdoms  Q=quit\n\n";

    Viewer viewer(cfg);
    viewer.run();
    return 0;
}
