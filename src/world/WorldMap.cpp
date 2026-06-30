#include "jke/world/WorldMap.hpp"
#include <algorithm>

namespace jke {

WorldMap::WorldMap(int width, int height)
    : width_(width), height_(height)
{
    tiles_.resize(static_cast<size_t>(width * height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            TileID id = static_cast<TileID>(y * width + x);
            tiles_[id].id       = id;
            tiles_[id].position = { x, y };
        }
    }
}

int WorldMap::neighbors4(TileID id, TileID ids[4]) const noexcept {
    auto [x, y] = coordOf(id);
    const int dx[] = { 0, 0, -1, 1 };
    const int dy[] = {-1, 1,  0, 0 };
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        int nx = x + dx[i], ny = y + dy[i];
        if (inBounds(nx, ny)) ids[count++] = idOf(nx, ny);
    }
    return count;
}

std::vector<TileID> WorldMap::neighbors4v(TileID id) const {
    TileID ids[4]; int n = neighbors4(id, ids);
    return std::vector<TileID>(ids, ids + n);
}

std::vector<TileID> WorldMap::neighbors8(TileID id) const {
    std::vector<TileID> result;
    result.reserve(8);
    auto [x, y] = coordOf(id);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (inBounds(nx, ny)) result.push_back(idOf(nx, ny));
        }
    return result;
}

std::vector<TileID> WorldMap::tilesOwnedBy(KingdomID k) const {
    std::vector<TileID> result;
    for (const auto& t : tiles_)
        if (t.owner == k) result.push_back(t.id);
    return result;
}

std::vector<TileID> WorldMap::landTiles() const {
    std::vector<TileID> result;
    for (const auto& t : tiles_)
        if (t.terrain != TerrainType::Ocean &&
            t.terrain != TerrainType::Coast &&
            t.terrain != TerrainType::Lake)
            result.push_back(t.id);
    return result;
}

} // namespace jke
