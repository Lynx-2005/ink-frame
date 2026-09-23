// Clean-room tile implementation.
#include "core/InkTile.h"

namespace ink {

void TileData::markDirty() {
    ++version;
    flagsDirty.store(true, std::memory_order_relaxed);
}

void TileData::ensureFlags() const {
    if (!flagsDirty.load(std::memory_order_relaxed)) return;
    // Single scan per tile per dirty epoch (called from TileStore::extent
    // under the store lock), instead of once per pixel write.
    bool any = false, all = true;
    for (const auto &p : px) {
        if (p.a != 0) any = true;
        if (p.a != 255) all = false;
        if (any && !all) break;
    }
    fullyTransparent = !any;
    fullyOpaque = all;
    flagsDirty.store(false, std::memory_order_relaxed);
}

} // namespace ink
