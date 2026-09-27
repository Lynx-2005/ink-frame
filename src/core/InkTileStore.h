#pragma once
// Sparse copy-on-write tile store with memento undo.
// Production features: per-tile COW (strokes don't copy whole image),
// tile-level versioning for renderer invalidation, extent tracking,
// memento = list of (pos, oldTileOrNull) for O(dirty-tiles) undo.
//
// Batching contract (the stroke hot path): pixel loops must go through
// updateTilesInRect, which takes the store lock ONCE, detaches each touched
// tile ONCE (with memento capture), runs the caller loop on raw tile memory,
// and bumps the version ONCE per tile. Per-pixel locking/scanning is what
// made 1080p projection take seconds.
#include "core/InkTile.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ink {

struct Rect {
    int x{0}, y{0}, w{0}, h{0};
    bool isEmpty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

inline Rect intersectRect(const Rect &a, const Rect &b) {
    int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

inline Rect uniteRect(const Rect &a, const Rect &b) {
    if (a.isEmpty()) return b;
    if (b.isEmpty()) return a;
    int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

struct TileMemento {
    struct Entry { TilePos pos; TileShared oldTile; }; // null oldTile = tile was created
    std::vector<Entry> entries;
    void clear() { entries.clear(); seen_.clear(); }
    bool empty() const { return entries.empty(); }
    // First-touch guard: a stroke touching T tiles used to scan the whole
    // entry list per tile (O(T^2) for long strokes). The set makes it O(1).
    bool noted(const TilePos &p) const { return seen_.find(p) != seen_.end(); }
    void record(const TilePos &p, TileShared oldTile) {
        if (seen_.insert(p).second) entries.push_back({p, std::move(oldTile)});
    }
private:
    std::unordered_set<TilePos, TilePosHash> seen_;
};

class TileStore {
public:
    TileStore() = default;
    TileStore(const TileStore &) = delete;
    TileStore &operator=(const TileStore &) = delete;

    // Read pixel (transparent black outside stored tiles)
    PixelRGBA8 pixel(int x, int y) const;
    // Ensure writable tile (COW detach) and record into memento if provided
    TileData &writableTile(int tx, int ty, TileMemento *memento);
    // Read-only tile or nullptr
    TileShared tile(int tx, int ty) const;

    // Batched rect update: fn(tile, tileOriginX, tileOriginY, clipRect).
    // One lock, one detach + one markDirty per touched tile.
    // fn must not call back into this store.
    template <typename Fn>
    Rect updateTilesInRect(const Rect &r, TileMemento *memento, Fn &&fn) {
        if (r.isEmpty()) return {};
        std::lock_guard<std::mutex> lk(mutex_);
        const int tx0 = floorDiv(r.x, kTileSize), ty0 = floorDiv(r.y, kTileSize);
        const int tx1 = floorDiv(r.x + r.w - 1, kTileSize),
                  ty1 = floorDiv(r.y + r.h - 1, kTileSize);
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                TilePos p{tx, ty};
                TileShared t = detachLocked(p, memento);
                const int ox = tx * kTileSize, oy = ty * kTileSize;
                Rect clip{std::max(r.x, ox), std::max(r.y, oy),
                          std::min(r.x + r.w, ox + kTileSize) - std::max(r.x, ox),
                          std::min(r.y + r.h, oy + kTileSize) - std::max(r.y, oy)};
                fn(*t, ox, oy, clip);
                t->markDirty();
            }
        }
        return r;
    }

    void clear();
    // Drop tiles overlapping r (used before clipped re-projection).
    void clearRect(const Rect &r);
    bool empty() const;
    size_t tileCount() const;
    Rect extent() const; // bounding box of non-transparent tiles, empty if none

    // Memento apply (undo): restore old tiles
    void restore(const TileMemento &m);

    // Direct tile install (redo of a captured after-image). Null erases.
    void setTile(TilePos pos, TileShared tile);

    // For renderer: collect tiles intersecting rect
    void tilesInRect(const Rect &r, std::vector<std::pair<TilePos, TileShared>> &out) const;
    // Small-span snapshot for the blit hot path: fills `out` (capacity 4)
    // with the stored tiles intersecting `r` under a SINGLE lock, with zero
    // heap allocation. Returns the count, or -1 when the span exceeds 2x2
    // tiles (caller falls back to tilesInRect). Blit dirty rects, dots and
    // thumbnails are almost always <= 2x2 tiles, so the common case never
    // builds an unordered_map per layer per frame.
    int snapshotSmall(const Rect &r,
                      std::pair<TilePos, TileShared> (&out)[4]) const;

private:
    // Lock-free core of writableTile; caller must hold mutex_.
    TileShared detachLocked(const TilePos &p, TileMemento *m);
    TileShared detach(const TilePos &p, TileMemento *m);
    mutable std::mutex mutex_;
    std::unordered_map<TilePos, TileShared, TilePosHash> tiles_;
};

} // namespace ink
