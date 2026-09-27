#include "core/InkTileStore.h"

namespace ink {

PixelRGBA8 TileStore::pixel(int x, int y) const {
    std::lock_guard<std::mutex> lk(mutex_);
    TilePos p{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = tiles_.find(p);
    if (it == tiles_.end()) return {};
    const TileData &t = *it->second;
    int lx = modPos(x, kTileSize), ly = modPos(y, kTileSize);
    return t.px[ly * kTileSize + lx];
}

TileShared TileStore::tile(int tx, int ty) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = tiles_.find(TilePos{tx, ty});
    return it == tiles_.end() ? nullptr : it->second;
}

TileShared TileStore::detachLocked(const TilePos &p, TileMemento *m) {
    auto it = tiles_.find(p);
    if (it == tiles_.end()) {
        auto fresh = std::make_shared<TileData>();
        tiles_[p] = fresh;
        if (m) m->record(p, nullptr);
        return fresh;
    }
    if (it->second.use_count() > 1) {
        // Shared (e.g. with undo snapshot or projection cache) -> copy
        TileShared old = it->second;
        auto copy = std::make_shared<TileData>(*old);
        it->second = copy;
        if (m) m->record(p, old); // first touch per stroke only
        return copy;
    }
    if (m && !m->noted(p)) {
        // Exclusive but undoable: snapshot a copy of pre-stroke data
        m->record(p, std::make_shared<TileData>(*it->second));
    }
    return it->second;
}

TileData &TileStore::writableTile(int tx, int ty, TileMemento *memento) {
    std::lock_guard<std::mutex> lk(mutex_);
    TilePos p{tx, ty};
    return *detachLocked(p, memento);
}

TileShared TileStore::detach(const TilePos &p, TileMemento *m) {
    std::lock_guard<std::mutex> lk(mutex_);
    return detachLocked(p, m);
}

void TileStore::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    tiles_.clear();
}

void TileStore::clearRect(const Rect &r) {
    if (r.isEmpty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    int tx0 = floorDiv(r.x, kTileSize), ty0 = floorDiv(r.y, kTileSize);
    int tx1 = floorDiv(r.x + r.w - 1, kTileSize), ty1 = floorDiv(r.y + r.h - 1, kTileSize);
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) tiles_.erase(TilePos{tx, ty});
}

bool TileStore::empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tiles_.empty();
}

size_t TileStore::tileCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tiles_.size();
}

Rect TileStore::extent() const {
    std::lock_guard<std::mutex> lk(mutex_);
    bool first = true;
    int x0=0,y0=0,x1=0,y1=0;
    for (auto &[pos, t] : tiles_) {
        t->ensureFlags(); // lazy: one scan per tile per dirty epoch at most
        if (t->fullyTransparent) continue;
        int bx = pos.tx * kTileSize, by = pos.ty * kTileSize;
        if (first) { x0=bx; y0=by; x1=bx+kTileSize; y1=by+kTileSize; first=false; }
        else {
            x0 = std::min(x0,bx); y0 = std::min(y0,by);
            x1 = std::max(x1,bx+kTileSize); y1 = std::max(y1,by+kTileSize);
        }
    }
    if (first) return {};
    return {x0,y0,x1-x0,y1-y0};
}

void TileStore::restore(const TileMemento &m) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto &e : m.entries) {
        if (!e.oldTile) tiles_.erase(e.pos);
        else tiles_[e.pos] = e.oldTile;
    }
}

void TileStore::setTile(TilePos pos, TileShared tile) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!tile) tiles_.erase(pos);
    else tiles_[pos] = std::move(tile);
}

void TileStore::tilesInRect(const Rect &r, std::vector<std::pair<TilePos, TileShared>> &out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    int tx0 = floorDiv(r.x, kTileSize), ty0 = floorDiv(r.y, kTileSize);
    int tx1 = floorDiv(r.x + r.w - 1, kTileSize), ty1 = floorDiv(r.y + r.h - 1, kTileSize);
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            auto it = tiles_.find(TilePos{tx, ty});
            if (it != tiles_.end()) out.emplace_back(TilePos{tx,ty}, it->second);
        }
}

int TileStore::snapshotSmall(const Rect &r,
                             std::pair<TilePos, TileShared> (&out)[4]) const {
    if (r.isEmpty()) return 0;
    const int tx0 = floorDiv(r.x, kTileSize), ty0 = floorDiv(r.y, kTileSize);
    const int tx1 = floorDiv(r.x + r.w - 1, kTileSize), ty1 = floorDiv(r.y + r.h - 1, kTileSize);
    if (tx1 - tx0 > 1 || ty1 - ty0 > 1) return -1; // wide span: use tilesInRect
    std::lock_guard<std::mutex> lk(mutex_);
    int n = 0;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            auto it = tiles_.find(TilePos{tx, ty});
            if (it != tiles_.end()) out[n++] = {TilePos{tx, ty}, it->second};
        }
    return n;
}

} // namespace ink
