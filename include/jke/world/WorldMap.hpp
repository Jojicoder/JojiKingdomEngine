#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include "jke/world/Tile.hpp"

namespace jke {

struct River {
    std::vector<TileID> path;
    float width = 1.0f;
};

class WorldMap {
public:
    WorldMap(int width, int height);

    int width()  const noexcept { return width_; }
    int height() const noexcept { return height_; }
    int tileCount() const noexcept { return static_cast<int>(tiles_.size()); }

    Tile&       at(TileID id)       { return tiles_[id]; }
    const Tile& at(TileID id) const { return tiles_[id]; }

    Tile&       at(int x, int y)       { return tiles_[y * width_ + x]; }
    const Tile& at(int x, int y) const { return tiles_[y * width_ + x]; }

    TileID      idOf(int x, int y) const noexcept { return static_cast<TileID>(y * width_ + x); }
    Coordinate  coordOf(TileID id) const noexcept {
        return { static_cast<int32_t>(id % width_), static_cast<int32_t>(id / width_) };
    }

    bool inBounds(int x, int y) const noexcept {
        return x >= 0 && x < width_ && y >= 0 && y < height_;
    }

    // Zero-allocation neighbors: returns count, fills ids[] (max 4)
    int neighbors4(TileID id, TileID ids[4]) const noexcept;
    // Legacy vector form — only used in generators / init paths
    std::vector<TileID> neighbors4v(TileID id) const;
    std::vector<TileID> neighbors8(TileID id) const;
    std::vector<TileID> tilesOwnedBy(KingdomID k) const;
    std::vector<TileID> landTiles() const;

    std::vector<Tile>&       tiles()       { return tiles_; }
    const std::vector<Tile>& tiles() const { return tiles_; }

    std::vector<River>& rivers() { return rivers_; }
    const std::vector<River>& rivers() const { return rivers_; }

private:
    int               width_;
    int               height_;
    std::vector<Tile> tiles_;
    std::vector<River> rivers_;
};

} // namespace jke
