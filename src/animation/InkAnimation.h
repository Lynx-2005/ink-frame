#pragma once
// Onion skin + frame cache + playback. Production-grade:
// - OnionSkinConfig: prev/next counts, opacity falloff, tint (red/green like Procreate)
// - FrameCache: background threadpool renders projections, LRU + generation ids
//   so scrubbing invalidates stale jobs (Krita KisAnimationFrameCache concept)
// - PlaybackEngine: fixed-timestep accumulator, drop-frame, loop/ping-pong
#include "core/InkImage.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ink {

struct OnionSkinConfig {
    int prevCount{2};
    int nextCount{1};
    float maxOpacity{0.35f};
    bool tintEnabled{true};
    // The ONLY two onion colours: previous frames red, next frames blue.
    // Ghosts are tinted by coverage alone (never by the ink's own value), so
    // every ghost reads as exactly one of these two.
    PixelRGBA8 prevTint{255, 60, 60, 255}; // red
    PixelRGBA8 nextTint{60, 160, 255, 255}; // blue
    bool enabled{true};
};

// Tinted overlay of one cached frame onto canvas projection.
// Tinting is coverage-only: the ghost's hue is EXACTLY `tint` for every ink
// value (a black line and a white line ghost identically). Pass
// tinted=false to composite the frame in its own colours instead.
// If clip is non-null, only that region is touched (incremental path).
void compositeOnionSkin(PaintDevice &canvas, const PaintDevice &frame,
                        PixelRGBA8 tint, float opacity, const Rect *clip = nullptr,
                        bool tinted = true);

class FrameCache {
public:
    struct Entry {
        int frame{-1};
        uint64_t generation{0};
        std::shared_ptr<PaintDevice> image; // projected
        bool complete{false};
    };
    using RenderFn = std::function<std::shared_ptr<PaintDevice>(int frame)>;

    explicit FrameCache(size_t maxFrames = 64, size_t threads = 2);
    ~FrameCache();

    void setRenderFn(RenderFn fn) { renderFn_ = std::move(fn); }
    void requestRange(int currentFrame, int prefetchAhead = 12);
    std::shared_ptr<PaintDevice> get(int frame); // may be nullptr if not ready
    void invalidate(int fromFrame = 0); // bump generation, drop >= fromFrame
    void invalidateAll() { invalidate(0); }
    size_t cachedCount() const;
    void waitFor(int frame, int timeoutMs = 2000);

private:
    void workerLoop();
    RenderFn renderFn_;
    size_t maxFrames_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<int> pending_;
    std::map<int, Entry> cache_;
    std::atomic<uint64_t> generation_{1};
    std::atomic<bool> stop_{false};
};

enum class LoopMode { Once, Loop, PingPong };

class PlaybackEngine {
public:
    PlaybackEngine(int fps = 24) : fps_(fps) {}
    void play() { playing_ = true; }
    void pause() { playing_ = false; }
    void stop() { playing_ = false; currentFloat_ = (float)start_; }
    bool isPlaying() const { return playing_; }
    void setRange(int s, int e) { start_ = s; end_ = e; }
    void setFps(int f) { if (f>=1&&f<=120) fps_ = f; }
    void setLoop(LoopMode m) { loop_ = m; }
    LoopMode loop() const { return loop_; }
    int fps() const { return fps_; }
    // Advance by dt seconds; returns current integer frame + whether frame changed
    std::pair<int,bool> advance(double dtSec);
    int currentFrame() const { return (int)std::round(currentFloat_); }
    void seek(int f) { currentFloat_ = (float)std::clamp(f, start_, end_); }
private:
    int fps_{24}, start_{0}, end_{120};
    float currentFloat_{0};
    bool playing_{false};
    LoopMode loop_{LoopMode::Loop};
    int pingDir_{1};
};

} // namespace ink
