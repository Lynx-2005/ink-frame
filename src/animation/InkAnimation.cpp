#include "animation/InkAnimation.h"
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace ink {

void compositeOnionSkin(PaintDevice &canvas, const PaintDevice &frame,
                        PixelRGBA8 tint, float opacity, const Rect *clip, bool tinted) {
    Rect e = frame.extent();
    if (e.isEmpty() || opacity <= 0.0f) return;
    if (clip) {
        e = intersectRect(e, *clip);
        if (e.isEmpty()) return;
    }
    // Snapshot ghost tiles once (single lock), then write canvas tiles
    // batched per tile (single lock) — the old per-pixel pixel()/setPixel()
    // pair took two mutexes per pixel.
    std::vector<std::pair<TilePos, TileShared>> ghost;
    frame.store().tilesInRect(e, ghost);
    std::unordered_map<TilePos, const TileData *, TilePosHash> lookup;
    lookup.reserve(ghost.size() * 2 + 1);
    for (auto &g : ghost) lookup.emplace(g.first, g.second.get());
    canvas.store().updateTilesInRect(
        e, nullptr, [&](TileData &t, int ox, int oy, const Rect &c) {
            for (int y = c.y; y < c.y + c.h; ++y) {
                PixelRGBA8 *row = &t.px[(y - oy) * kTileSize + (c.x - ox)];
                for (int x = c.x; x < c.x + c.w; ++x, ++row) {
                    auto it = lookup.find(TilePos{floorDiv(x, kTileSize), floorDiv(y, kTileSize)});
                    if (it == lookup.end()) continue;
                    const TileData *gt = it->second;
                    PixelRGBA8 p = gt->px[modPos(y, kTileSize) * kTileSize + modPos(x, kTileSize)];
                    if (p.a == 0) continue;
                    // Coverage-only tint: the hue is EXACTLY `tint` whatever the
                    // ink value was. Modulating by luminance (the old path) made
                    // a black line ghost black and a white line ghost bright, so
                    // thick opaque strokes stopped reading as red/blue.
                    const PixelRGBA8 tt = tinted ? PixelRGBA8{tint.r, tint.g, tint.b, p.a} : p;
                    *row = PaintDevice::compositeOver(*row, tt, opacity);
                }
            }
        });
}

FrameCache::FrameCache(size_t maxFrames, size_t threads) : maxFrames_(maxFrames) {
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back(&FrameCache::workerLoop, this);
}
FrameCache::~FrameCache() {
    stop_ = true; cv_.notify_all();
    for (auto &t : workers_) if (t.joinable()) t.join();
}
void FrameCache::requestRange(int cur, int ahead) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (int f = cur; f <= cur + ahead; ++f) {
        if (cache_.count(f)) continue;
        // bound queue
        if (pending_.size() > maxFrames_ + 16) break;
        pending_.push(f);
    }
    cv_.notify_all();
}
std::shared_ptr<PaintDevice> FrameCache::get(int frame) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = cache_.find(frame);
    if (it == cache_.end() || !it->second.complete) return nullptr;
    return it->second.image;
}
void FrameCache::invalidate(int fromFrame) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++generation_;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->first >= fromFrame) it = cache_.erase(it);
        else ++it;
    }
    // clear pending >= fromFrame
    std::queue<int> keep;
    while (!pending_.empty()) { int f = pending_.front(); pending_.pop(); if (f < fromFrame) keep.push(f); }
    pending_ = std::move(keep);
}
size_t FrameCache::cachedCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return cache_.size();
}
void FrameCache::waitFor(int frame, int timeoutMs) {
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        { std::lock_guard<std::mutex> lk(mutex_);
          auto it = cache_.find(frame);
          if (it != cache_.end() && it->second.complete) return; }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-t0).count() > timeoutMs) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
void FrameCache::workerLoop() {
    while (!stop_) {
        int frame = -1;
        uint64_t gen = 0;
        { std::unique_lock<std::mutex> lk(mutex_);
          cv_.wait(lk, [&]{ return stop_ || !pending_.empty(); });
          if (stop_) return;
          frame = pending_.front(); pending_.pop();
          gen = generation_.load();
          if (cache_.count(frame)) continue;
          cache_[frame] = Entry{frame, gen, nullptr, false};
        }
        std::shared_ptr<PaintDevice> img;
        if (renderFn_) img = renderFn_(frame);
        else img = std::make_shared<PaintDevice>();
        { std::lock_guard<std::mutex> lk(mutex_);
          if (gen != generation_.load()) { cache_.erase(frame); continue; }
          auto it = cache_.find(frame);
          if (it != cache_.end()) { it->second.image = img; it->second.complete = true; }
          // LRU evict oldest (map ordered by frame; evict farthest from nothing — simple size cap)
          while (cache_.size() > maxFrames_) cache_.erase(cache_.begin());
        }
    }
}

std::pair<int,bool> PlaybackEngine::advance(double dt) {
    if (!playing_) return {currentFrame(), false};
    float prev = currentFloat_;
    currentFloat_ += float(dt * fps_) * pingDir_;
    bool changed = (int)std::round(prev) != (int)std::round(currentFloat_);
    if (pingDir_ > 0 && currentFloat_ >= end_) {
        if (loop_ == LoopMode::Once) { currentFloat_ = (float)end_; playing_ = false; }
        else if (loop_ == LoopMode::Loop) currentFloat_ = (float)start_ + (currentFloat_-end_);
        else { currentFloat_ = (float)end_; pingDir_ = -1; }
    } else if (pingDir_ < 0 && currentFloat_ <= start_) {
        currentFloat_ = (float)start_; pingDir_ = 1;
    }
    return {currentFrame(), changed};
}

} // namespace ink
