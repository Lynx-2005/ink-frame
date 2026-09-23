#pragma once
// Clean-room tile: fixed 64x64 RGBA8, copy-on-write via shared_ptr.
// Inspired by Krita tiles3 concept, zero code reuse.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace ink {

inline constexpr int kTileSize = 64;
inline constexpr int kTilePixels = kTileSize * kTileSize;

struct PixelRGBA8 {
    uint8_t r{0}, g{0}, b{0}, a{0};
};

struct TileData {
    std::array<PixelRGBA8, kTilePixels> px{};
    uint32_t version{0};
    // Transparency flags are computed LAZILY (see ensureFlags): scanning the
    // tile on every pixel write made strokes O(tileSize) per pixel.
    mutable bool fullyTransparent{true};
    mutable bool fullyOpaque{false};
    mutable std::atomic<bool> flagsDirty{true};
    TileData() = default;
    TileData(const TileData &o)
        : px(o.px), version(o.version), fullyTransparent(o.fullyTransparent),
          fullyOpaque(o.fullyOpaque),
          flagsDirty(o.flagsDirty.load(std::memory_order_relaxed)) {}
    TileData &operator=(const TileData &o) {
        if (this != &o) {
            px = o.px;
            version = o.version;
            fullyTransparent = o.fullyTransparent;
            fullyOpaque = o.fullyOpaque;
            flagsDirty.store(o.flagsDirty.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        }
        return *this;
    }
    void markDirty();
    void ensureFlags() const;
};

using TileShared = std::shared_ptr<TileData>;

// Tile coordinates (not pixels)
struct TilePos {
    int tx{0}, ty{0};
    bool operator==(const TilePos &o) const { return tx == o.tx && ty == o.ty; }
};
struct TilePosHash {
    size_t operator()(const TilePos &p) const noexcept {
        return (static_cast<size_t>(p.tx) * 73856093u) ^ (static_cast<size_t>(p.ty) * 19349663u);
    }
};

inline int floorDiv(int a, int b) {
    int q = a / b, r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) --q;
    return q;
}
inline int modPos(int a, int b) {
    int m = a % b;
    return m < 0 ? m + b : m;
}

} // namespace ink
