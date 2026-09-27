// Extreme stress / scaling-law probe for InkFrame (HEADLESS, OOM-SAFE).
//
// This file MEASURES ONLY. Timings are REPORTED, never pass/fail budgets.
// Correctness is spot-checked with INK_CHECK; every [extreme] line is data.
//
// What it pushes (the exact lag reports in the request):
//   E1  timeline at 1000+ tracks x dozens of keys  (100k+ cells metadata)
//   E2  graph/curve editing at 5k-10k bezier keys   (bulk build, handle drags,
//       dense redraw sampling, segment walk, playhead follow)
//   E3  ParamBlock x 1000 layers x 18 params        (per-frame eval cost that
//       project() pays per animated layer per frame)
//   E4  projection with 1000 tracks + cameras       (0 / 1 global / 100 layer /
//       FULL: 1000 layer cameras -- the "camera in each layer" worst case --
//       plus projectRect small-dirty fast path vs camera fallback)
//   E5  cell stacks: depth sweep + breadth sweep    ("layers in each cell")
//   E6  strokes: raw dab throughput with 1M/10M/100M extrapolation, brush-op
//       strokes, device-allocation storm ("millions of strokes" extrapolation)
//   E7  structural key ops at scale                 (move/shift/mirror)
//   E8  memory model: per-unit costs + impossibility arithmetic for the literal
//       request (100k cells x 1000 layers of real pixels), RSS deltas
//
// OOM SAFETY (this box: ~5.7 GiB, ~3.7 GiB free at authoring):
//   - Canvases are SMALL (480x270 max, ~0.5 MB/frame). Pixel fills are sparse
//     24x24 dots (1 tile each), never full-canvas fills at 1000 layers.
//   - Dab storms reuse ONE device confined to 256x256 (<=25 tiles, ~400 KB).
//   - Device-allocation storm caps at 10k live devices (~160 MB worst case,
//     freed immediately; RSS delta is reported).
//   - The literal request (100k pixel cells x 1000 layers, millions of live
//     strokes) is NOT allocated: E8 shows by arithmetic it is terabytes of
//     RAM on ANY tile engine. We measure per-unit costs so the ceiling can be
//     computed without OOMing the box.
//   - Full extremes run only with INKFRAME_EXTREME_FULL=1; default `ctest`
//     runs the quick tier (target < ~3 min on 8 cores).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <vector>

#include "animation/InkAnimation.h"
#include "animation/InkKeyframe.h"
#include "animation/InkTimelineModel.h"
#include "brush/InkBrushTip.h"
#include "brush/InkPaintOp.h"
#include "core/InkCell.h"
#include "core/InkImage.h"
#include "core/InkPaintDevice.h"
#include "test_check.h"

using namespace ink;
using Clock = std::chrono::steady_clock;

namespace {

double secs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}
long rssMB() {
    struct rusage u {};
    if (getrusage(RUSAGE_SELF, &u) != 0) return -1;
    return (long)(u.ru_maxrss / 1024); // Linux ru_maxrss is KiB
}
bool extremeFull() {
    const char *e = std::getenv("INKFRAME_EXTREME_FULL");
    return e && std::string(e) == "1";
}

} // namespace

int main() {
    const long rss0 = rssMB();
    const bool FULL = extremeFull();
    std::printf("[extreme] rss_start=%ld MB full=%d\n", rss0, FULL ? 1 : 0);

    // === E1. Timeline: 1000+ tracks x keys (100k+ cells metadata, no pixels) =
    {
        TimelineModel tm;
        tm.setFrameCount(500);
        const int kFolders = FULL ? 20 : 10;
        const int kTracksPer = FULL ? 100 : 100; // 1000 tracks both tiers
        const int kKeysPer = FULL ? 100 : 40;    // 100k / 40k cells
        auto t0 = Clock::now();
        int tracks = 0;
        for (int f = 0; f < kFolders; ++f) {
            std::string fid = tm.addFolder("F" + std::to_string(f));
            for (int t = 0; t < kTracksPer; ++t) {
                std::string id = "X" + std::to_string(f) + "_" + std::to_string(t);
                tm.addTrack(fid, id, "track");
                for (int k = 0; k < kKeysPer; ++k)
                    tm.setKey(id, (k * 7 + t * 3) % 500);
                ++tracks;
            }
        }
        double buildS = secs(t0, Clock::now());
        const long totalCells = (long)tracks * kKeysPer;

        // Full cells() enumeration over ALL tracks (what timeline paint does).
        t0 = Clock::now();
        size_t cellTotal = 0;
        for (const auto &fo : tm.folders())
            for (const auto &tr : fo.tracks) cellTotal += tm.cells(tr.layerId).size();
        double cellsS = secs(t0, Clock::now());

        // Scrub: activeKey over all tracks x 120 frames (playback pass).
        t0 = Clock::now();
        long hits = 0;
        for (int fr = 0; fr < 120; ++fr)
            for (const auto &fo : tm.folders())
                for (const auto &tr : fo.tracks) hits += tm.activeKey(tr.layerId, fr);
        double scrubS = secs(t0, Clock::now());

        // Hover path: cellStartAt x 10k queries on one dense track.
        t0 = Clock::now();
        volatile long hoverSink = 0;
        for (int i = 0; i < 10000; ++i)
            hoverSink += tm.cellStartAt("X0_0", (i * 13) % 500);
        double hoverS = secs(t0, Clock::now());

        // Structural edit: moveKey one key on every 5th track.
        t0 = Clock::now();
        int moved = 0;
        int sampled = 0;
        for (const auto &fo : tm.folders())
            for (const auto &tr : fo.tracks) {
                if ((sampled++ % 5) != 0) continue;
                auto cs = tm.cells(tr.layerId);
                if (!cs.empty()) {
                    int from = cs[0].start;
                    int to = std::min(from + 1, 499);
                    if (tm.moveKey(tr.layerId, from, to)) ++moved;
                }
            }
        double moveS = secs(t0, Clock::now());

        std::printf("[extreme] E1 timeline tracks=%d keys/track~%d cells~%ld "
                    "build=%.3fs enumCells=%zu in %.3fs (%.1f cells/s) "
                    "scrub120f=%.3fs (%.1f lookups/s) hover10k=%.3fs moveKey=%d in %.3fs\n",
                    tracks, kKeysPer, totalCells, buildS, cellTotal, cellsS,
                    cellTotal / std::max(cellsS, 1e-9), scrubS,
                    (tracks * 120.0) / std::max(scrubS, 1e-9), hoverS, moved, moveS);
        (void)hits;
        (void)hoverSink;
        INK_CHECK(tm.visibleRowCount() > tracks);
        INK_CHECK(cellTotal > 0);
    }

    // === E2. Graph editing: 5k-10k bezier keys ================================
    {
        const int K = FULL ? 10000 : 5000;
        // E2a. Raw channel bulk build (no auto-tangent refresh): O(K log K).
        ScalarChannel raw;
        auto t0 = Clock::now();
        for (int i = 0; i < K; ++i)
            raw.setKey(i, float(i) * 1.5f, (i % 3 == 0) ? Interp::Bezier : Interp::Linear);
        double rawBuildS = secs(t0, Clock::now());
        INK_CHECK(raw.keyCount() == K);

        // E2b. ParamBlock bulk build WITH per-insert auto refresh: O(1) per
        // insert since the localized-refresh fix (was O(K): full sweep each).
        // This is the keying/recording path (setKey -> refreshAutoHandlesNear).
        ParamBlock pb;
        t0 = Clock::now();
        const int KB = FULL ? 2000 : 1000;
        for (int i = 0; i < KB; ++i)
            pb.setKey(ParamId::PositionX, i, double(i) * 1.5, Interp::Bezier);
        double pbBuildS = secs(t0, Clock::now());
        std::printf("[extreme] E2a bulk-build rawKeys=%d in %.3fs (%.1f keys/s) | "
                    "paramBlockKeys=%d in %.3fs (%.1f keys/s, O(1) localized auto-refresh each)\n",
                    K, rawBuildS, K / std::max(rawBuildS, 1e-9), KB, pbBuildS,
                    KB / std::max(pbBuildS, 1e-9));

        // E2c. Handle-drag simulation: value edit + full auto refresh per drag.
        // This is the per-drag cost the graph editor pays on every mouse move.
        t0 = Clock::now();
        const int kDrags = 50;
        for (int i = 0; i < kDrags; ++i) {
            int f = (i * 97) % KB;
            pb.setKeyValue(ParamId::PositionX, f, double(f) * 1.5 + 10.0);
        }
        double dragS = secs(t0, Clock::now());
        std::printf("[extreme] E2b drag value-edit+refresh keys=%d drags=%d in %.3fs "
                    "(%.3fs/drag %.1f drags/s)\n",
                    KB, kDrags, dragS, dragS / kDrags, kDrags / std::max(dragS, 1e-9));

        // E2b2. Localized refresh == full-refresh fixed point under RANDOM
        // edits (insert/value/interp/move/remove, random order + collisions).
        // Reference is a raw channel with the mirrored op sequence and ONE
        // full fixed-point refresh at the end (the old per-edit semantics).
        // Stored handles must match BIT-EXACTLY (same formula, same inputs).
        {
            uint32_t rng = 12345u;
            auto lcg = [&]() { return (rng = rng * 1664525u + 1013904223u); };
            ParamBlock inc;
            ScalarChannel rawRef;
            const int kOps = 600, kSpan = 2000;
            for (int i = 0; i < kOps; ++i) {
                const int f = (int)(lcg() % (uint32_t)kSpan);
                const float v = float(lcg() % 100000u) * 0.01f;
                const Interp ip = (lcg() & 1u) ? Interp::Bezier : Interp::Linear;
                const uint32_t op = lcg() % 10u;
                if (op < 5) { // insert (new or overwrite, like keying over a key)
                    inc.setKey(ParamId::Rotation, f, double(v), ip);
                    const bool had = rawRef.hasKey(f);
                    rawRef.setKey(f, v, ip);
                    if (!had) rawRef.setAutoBez(f, true);
                } else if (op < 7) { // value edit (drag)
                    if (!inc.hasKey(ParamId::Rotation, f)) continue;
                    const ScalarKey *k0 = inc.channel(ParamId::Rotation)->keyAt(f);
                    INK_CHECK(k0 != nullptr);
                    inc.setKeyValue(ParamId::Rotation, f, double(v));
                    ScalarKey kk = *rawRef.keyAt(f);
                    rawRef.setKey(f, v, kk.interp);
                    rawRef.setHandles(f, kk.outInfl, kk.outSpeed, kk.inInfl, kk.inSpeed);
                    rawRef.setAutoBez(f, kk.autoBez);
                } else if (op < 8) { // interp flip
                    if (!inc.hasKey(ParamId::Rotation, f)) continue;
                    inc.setInterp(ParamId::Rotation, f, ip);
                    rawRef.setInterp(f, ip);
                } else if (op < 9) { // move (with overwrite, like the block)
                    if (!inc.hasKey(ParamId::Rotation, f)) continue;
                    const int to = (int)(lcg() % (uint32_t)kSpan);
                    if (f == to) continue;
                    inc.moveKey(ParamId::Rotation, f, to);
                    if (rawRef.hasKey(to)) rawRef.removeKey(to);
                    rawRef.moveKey(f, to);
                } else { // remove
                    if (!inc.hasKey(ParamId::Rotation, f)) continue;
                    inc.removeKey(ParamId::Rotation, f);
                    rawRef.removeKey(f);
                }
            }
            // Old semantics fixed point: recompute every due tangent once.
            for (int f : rawRef.keys()) {
                const ScalarKey *k = rawRef.keyAt(f);
                if (k && k->autoBez && k->interp == Interp::Bezier) rawRef.autoHandles(f);
            }
            const ScalarChannel *got = inc.channel(ParamId::Rotation);
            INK_CHECK(got != nullptr);
            INK_CHECK(got->keyCount() == rawRef.keyCount());
            int cmpKeys = 0;
            for (int f : got->keys()) {
                const ScalarKey *a = got->keyAt(f);
                const ScalarKey *b = rawRef.keyAt(f);
                INK_CHECK(b != nullptr);
                INK_CHECK(a->value == b->value && a->interp == b->interp);
                INK_CHECK(a->outInfl == b->outInfl && a->outSpeed == b->outSpeed);
                INK_CHECK(a->inInfl == b->inInfl && a->inSpeed == b->inSpeed);
                INK_CHECK(a->autoBez == b->autoBez);
                ++cmpKeys;
            }
            // Dense eval must agree exactly too (same keys => same curve).
            int lo = got->firstKey(), hi = got->lastKey();
            for (double fr = lo - 2.0; fr <= hi + 2.0; fr += 0.5) {
                double x = got->valueAtF(fr, 0.0), y = rawRef.valueAtF(fr, 0.0);
                INK_CHECK(x == y);
            }
            std::printf("[extreme] E2b2 refresh-equivalence ops=%d keys=%d IDENTICAL\n",
                        kOps, cmpKeys);
        }

        // E2d. Dense redraw sampling: valueAtF over N samples (one wide graph
        // repaint, oversampled; 4K wide ~= 3840 samples per param).
        t0 = Clock::now();
        const int kSamples = FULL ? 100000 : 40000;
        volatile double sink = 0;
        for (int i = 0; i < kSamples; ++i)
            sink += raw.valueAtF(i * (double(K) / kSamples));
        double sampleS = secs(t0, Clock::now());
        std::printf("[extreme] E2c sample keys=%d samples=%d in %.3fs (%.1f eval/s)\n",
                    K, kSamples, sampleS, kSamples / std::max(sampleS, 1e-9));

        // E2e. Segment walk (lane-diamond painter per animated param) +
        // playhead-follow (prev/next/segment per frame x 1000 frames).
        t0 = Clock::now();
        int segs = 0;
        for (int k : raw.keys()) {
            CubicSegment s;
            if (raw.segment(k, &s)) ++segs;
        }
        double segS = secs(t0, Clock::now());
        t0 = Clock::now();
        volatile int followSink = 0;
        for (int f = 0; f < 1000; ++f) {
            int p = raw.prevKey(f);
            int n = raw.nextKey(f);
            followSink += (p >= 0) + (n >= 0);
            CubicSegment s;
            if (p >= 0) raw.segment(p, &s);
        }
        double followS = secs(t0, Clock::now());
        std::printf("[extreme] E2d segments=%d walked in %.3fs | follow1000f in %.3fs\n",
                    segs, segS, followS);
        (void)sink;
        (void)followSink;

        // E2f. Full 18-param block, 200-500 keys/param: identity + matrix eval.
        ParamBlock all;
        const int kKeysEach = FULL ? 500 : 200;
        for (int pi = 0; pi < kParamCount; ++pi)
            for (int k = 0; k < kKeysEach; ++k)
                all.setKey((ParamId)pi, k * 2, paramDesc((ParamId)pi).def,
                           Interp::Bezier, true, false); // raw keys, no refresh
        all.refreshAllAutoHandles(); // one refresh, timed below separately
        t0 = Clock::now();
        all.refreshAllAutoHandles();
        double refreshAllS = secs(t0, Clock::now());
        const Rect canvas{0, 0, 480, 270};
        t0 = Clock::now();
        volatile double msink = 0;
        for (int f = 0; f < 60; ++f) {
            float op = 1.0f;
            Mat3 m = all.matrixFor(canvas, double(f), &op);
            msink += m.m[0] + op;
            if (all.isIdentityAt(double(f))) msink += 1.0;
        }
        double evalS = secs(t0, Clock::now());
        std::printf("[extreme] E2e params=18 keys/param=%d totalKeys=%d "
                    "refreshAll=%.3fs eval60f=%.3fs (%.3fms/frame/layer)\n",
                    kKeysEach, all.keyCount(), refreshAllS, evalS,
                    1000.0 * evalS / 60.0);
        (void)msink;
        INK_CHECK(all.keyCount() == 18 * kKeysEach);
    }

    // === E3. ParamBlock x 1000 layers (the per-frame project() tax) ==========
    {
        const int kLayers = 1000;
        const int kKeys = FULL ? 10 : 4;
        std::vector<ParamBlock> blocks;
        blocks.reserve(kLayers);
        for (int l = 0; l < kLayers; ++l) {
            ParamBlock b;
            // Full default key set (what "Add keyframe" writes: 18 params).
            for (ParamId id : defaultKeySet())
                for (int k = 0; k < kKeys; ++k)
                    b.setKey(id, k * 10, paramDesc(id).def + 0.5 * (k > 0),
                             Interp::Bezier, true, false);
            b.refreshAllAutoHandles();
            blocks.push_back(std::move(b));
        }
        const Rect canvas{0, 0, 480, 270};
        auto t0 = Clock::now();
        volatile double sink = 0;
        const int kFrames = 30;
        for (int f = 0; f < kFrames; ++f)
            for (auto &b : blocks) {
                float op = 1.0f;
                Mat3 m = b.matrixFor(canvas, double(f), &op);
                sink += m.m[0] + op;
                if (b.isIdentityAt(double(f))) sink += 1.0;
            }
        double s = secs(t0, Clock::now());
        long layerFrames = (long)kLayers * kFrames;
        std::printf("[extreme] E3 paramblock layers=%d params=18 keys/param=%d "
                    "frames=%d layerFrames=%ld in %.3fs (%.3fms/layerFrame %.1f layerFrames/s)\n",
                    kLayers, kKeys, kFrames, layerFrames, s,
                    1000.0 * s / layerFrames, layerFrames / std::max(s, 1e-9));
        (void)sink;

        // Rest-pose variant: keys exist but sit exactly at defaults, so every
        // frame IS identity -- measures whether isIdentityAt early-outs
        // (it evaluates all 18 params regardless: the skip is per-frame, not free).
        std::vector<ParamBlock> rest;
        rest.reserve(kLayers);
        for (int l = 0; l < kLayers; ++l) {
            ParamBlock b;
            for (ParamId id : defaultKeySet())
                b.setKey(id, 0, paramDesc(id).def, Interp::Bezier, true, false);
            rest.push_back(std::move(b));
        }
        t0 = Clock::now();
        volatile double rsink = 0;
        for (int f = 0; f < kFrames; ++f)
            for (auto &b : rest) {
                if (b.isIdentityAt(double(f))) rsink += 1.0;
            }
        double rs = secs(t0, Clock::now());
        std::printf("[extreme] E3b rest-pose identity-check layers=%d frames=%d in %.3fs "
                    "(%.3fms/layerFrame)\n",
                    kLayers, kFrames, rs, 1000.0 * rs / layerFrames);
        (void)rsink;
        INK_CHECK(blocks.front().keyCount() == (int)defaultKeySet().size() * kKeys);
    }

    // === E4. Projection: 1000 tracks + cameras ================================
    {
        const int W = 480, H = 270;
        const int kTracks = 1000;
        const int kCellKeys = FULL ? 5 : 3;
        Image img(W, H, 24);
        GroupLayer *g = img.createFolder("G");
        std::vector<PaintLayer *> layers;
        layers.reserve(kTracks);
        for (int t = 0; t < kTracks; ++t) {
            PaintLayer *pl = img.createTrack(g, "T" + std::to_string(t));
            layers.push_back(pl);
            for (int k = 0; k < kCellKeys; ++k) {
                int key = k * 10;
                PaintDevice *d = pl->deviceAt(key, true);
                // Sparse: 24x24 dot ONLY on key 0 of every track (1 tile each)
                // + full key runs on 5 tracks. Pixel memory ~= kTracks tiles.
                if (k == 0 || t < 5) {
                    int px = (t * 37) % (W - 24), py = (t * 53 + k * 11) % (H - 24);
                    TileMemento mm;
                    d->fillRect({px, py, 24, 24},
                                {(uint8_t)(t & 255), 120, 200, 255});
                    (void)mm;
                }
                pl->setSpanEnd(key, kOpenEnd);
            }
        }
        // Animate 50 tracks (2-key PositionX slides).
        for (int t = 0; t < 50; ++t) {
            layers[t]->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Linear,
                                       true, false);
            layers[t]->params().setKey(ParamId::PositionX, 30, 20.0, Interp::Linear,
                                       true, false);
        }
        PaintDevice out;
        auto bench = [&](const char *tag, int frames) {
            auto t0 = Clock::now();
            for (int f = 0; f < frames; ++f) {
                out.clear();
                img.project(f, out);
            }
            double s = secs(t0, Clock::now());
            std::printf("[extreme] E4 project[%s] tracks=%d cells/track=%d frames=%d "
                        "total=%.3fs (%.3fs/frame %.1f fps) tiles=%zu\n",
                        tag, kTracks, kCellKeys, frames, s, s / frames,
                        frames / std::max(s, 1e-9), out.tileCount());
            return s;
        };
        auto benchRect = [&](const char *tag, int n) {
            auto t0 = Clock::now();
            for (int i = 0; i < n; ++i) {
                Rect r{60 + (i % 5) * 8, 40, 64, 64};
                img.projectRect(0, r, out);
            }
            double s = secs(t0, Clock::now());
            std::printf("[extreme] E4 projectRect[%s] x%d (64x64) total=%.3fs (%.3fs each)\n",
                        tag, n, s, s / n);
            return s;
        };

        bench("no-cameras", 5);
        benchRect("no-cameras", 10);
        INK_CHECK(!out.extent().isEmpty());

        // 1 global camera, active (forces full-rebuild incremental path).
        CameraLayer *gc = img.createCamera(g, "GC", CameraScope::Global, 0);
        gc->params().setStaticValue(ParamId::PositionX, 12.0);
        bench("1-global-cam", 5);
        benchRect("1-global-cam-fallback", 10);
        // Park the global camera back at identity so the layer-cam benches
        // below measure the incremental layer-camera path, not the global
        // fallback it would otherwise (correctly) keep forcing.
        gc->params().setStaticValue(ParamId::PositionX, 0.0);

        // 100 layer-scoped cameras, active (one per animated track + more).
        std::vector<CameraLayer *> cams;
        for (int t = 0; t < 100; ++t) {
            CameraLayer *c = img.createCamera(g, "LC" + std::to_string(t),
                                              CameraScope::Layer, layers[t]->id());
            c->params().setStaticValue(ParamId::PositionX, 4.0);
            cams.push_back(c);
        }
        bench("100-layer-cams", 5);
        benchRect("100-layer-cams", 10);

        if (FULL) {
            // The literal "camera in each layer" worst case: 1000 cameras.
            for (int t = 100; t < 1000; ++t) {
                CameraLayer *c = img.createCamera(g, "LC" + std::to_string(t),
                                                  CameraScope::Layer, layers[t]->id());
                c->params().setStaticValue(ParamId::PositionX, 4.0);
                cams.push_back(c);
            }
            bench("1000-layer-cams", 5);
            benchRect("1000-layer-cams", 10);
        } else {
            std::printf("[extreme] E4 1000-layer-cams SKIPPED (needs INKFRAME_EXTREME_FULL=1)\n");
        }
    }

    // === E5. Cell stacks: depth x breadth =====================================
    {
        // Depth sweep: one cell, 1..64 paint layers, each a 32x32 fill.
        Image img(480, 270, 24);
        for (int depth : {1, 4, 16, 64}) {
            Cell cell;
            for (int i = 0; i < depth; ++i) {
                CellLayer *l = cell.addLayer("L" + std::to_string(i), nullptr);
                TileMemento m;
                l->device()->fillRect({8, 8, 32, 32},
                                      {(uint8_t)(i * 4), 100, 200, 255},
                                      CompositeOp::Normal, &m);
            }
            PaintDevice out;
            auto t0 = Clock::now();
            const int kIters = 20;
            for (int i = 0; i < kIters; ++i) {
                out.clear();
                img.compositeCell(cell, out);
            }
            double s = secs(t0, Clock::now());
            std::printf("[extreme] E5a cell-depth=%d composite x%d total=%.3fs (%.3fs each)\n",
                        depth, kIters, s, s / kIters);
        }
        // Breadth sweep: 400 tracks x 5 keys = 2000 cells, 3-layer stacks,
        // thumbnails (cellComposite) over a 200-cell sample.
        const int kTracks = 400, kKeys = 5;
        Image img2(480, 270, 24);
        GroupLayer *g = img2.createFolder("G");
        for (int t = 0; t < kTracks; ++t) {
            PaintLayer *pl = img2.createTrack(g, "T" + std::to_string(t));
            for (int k = 0; k < kKeys; ++k) {
                int key = k * 10;
                Cell *c = pl->cellAt(key, true);
                c->addLayer("a", nullptr);
                c->addLayer("b", nullptr);
                TileMemento m;
                c->activePaintLayer()->device()->fillRect(
                    {(t * 37) % 440, (k * 41) % 230, 24, 24}, {200, 100, 50, 255},
                    CompositeOp::Normal, &m);
                pl->setSpanEnd(key, kOpenEnd);
            }
        }
        PaintDevice out;
        auto t0 = Clock::now();
        int done = 0;
        std::vector<PaintLayer *> pls;
        img2.collectPaintLayers(pls);
        for (auto *pl : pls) {
            for (int k : pl->keys()) {
                if ((done++ % 10) != 0) continue; // 200-cell sample of 2000
                img2.cellComposite(*pl, k, out);
            }
        }
        double s = secs(t0, Clock::now());
        std::printf("[extreme] E5b cells=%d sampled=%d thumbnails total=%.3fs (%.3fs each)\n",
                    kTracks * kKeys, done / 10, s, s / std::max(done / 10, 1));
        INK_CHECK(done > 0);
    }

    // === E6. Strokes: dab throughput + brush strokes + device storm ===========
    {
        AutoBrushTip tip(BrushTipShape{16.0f, 0.7f, 1.0f, 0.08f});
        const auto &mask = tip.mask();
        const int ms = tip.maskSize();
        PaintDevice dev;
        TileMemento m;
        const long kDabs = FULL ? 1000000L : 200000L;
        auto t0 = Clock::now();
        for (long i = 0; i < kDabs; ++i) {
            float x = float(32 + (i * 7) % 224);
            float y = float(32 + (i * 13) % 224);
            dev.applyDab(x, y, mask.data(), ms, {255, 30, 30, 255}, 1.0f,
                         CompositeOp::Normal, (i == 0) ? &m : nullptr);
        }
        double s = secs(t0, Clock::now());
        double dabsPerS = kDabs / std::max(s, 1e-9);
        std::printf("[extreme] E6a dabs=%ld in %.3fs = %.1f dabs/s tiles=%zu | "
                    "extrapolated: 1M=%.1fs 10M=%.1fs 100M=%.1fs\n",
                    kDabs, s, dabsPerS, dev.tileCount(), 1e6 / dabsPerS,
                    1e7 / dabsPerS, 1e8 / dabsPerS);
        INK_CHECK(dev.tileCount() <= 25);

        // Brush-op strokes (interpolator + mask cache + stabilizer path).
        const int kStrokes = FULL ? 2000 : 500;
        const int kEventsPer = 40;
        PaintDevice dev2;
        t0 = Clock::now();
        for (int st = 0; st < kStrokes; ++st) {
            TileMemento mm;
            PixelBrushOp op(AutoBrushTip(BrushTipShape{12.0f, 0.7f, 1.0f, 0.05f}),
                            BrushDynamics{});
            op.beginStroke({&dev2, {20, 120, 220, 255}, CompositeOp::Normal, &mm});
            for (int e = 0; e < kEventsPer; ++e) {
                StrokeEvent ev;
                ev.state.x = float((st * 37 + e * 5) % 220 + 16);
                ev.state.y = float((st * 53 + e * 3) % 220 + 16);
                ev.state.pressure = 0.8f;
                op.addEvent(ev);
            }
            op.endStroke();
        }
        double s2 = secs(t0, Clock::now());
        std::printf("[extreme] E6b brush_strokes=%d events/stroke=%d in %.3fs = %.1f strokes/s "
                    "tiles=%zu | extrapolated 1M strokes=%.1fh\n",
                    kStrokes, kEventsPer, s2, kStrokes / std::max(s2, 1e-9),
                    dev2.tileCount(), 1e6 / (kStrokes / std::max(s2, 1e-9)) / 3600.0);
        INK_CHECK(dev2.tileCount() <= 16);

        // Device-allocation storm: 10k live PaintDevices x 1 tile (the per-cell
        // device cost behind "strokes in each cell"). Freed right after.
        t0 = Clock::now();
        {
            std::vector<std::unique_ptr<PaintDevice>> devs;
            devs.reserve(10000);
            for (int i = 0; i < 10000; ++i) {
                auto d = std::make_unique<PaintDevice>();
                TileMemento mm;
                d->fillRect({0, 0, 24, 24}, {10, 20, 30, 255}, CompositeOp::Normal, &mm);
                devs.push_back(std::move(d));
            }
            double allocS = secs(t0, Clock::now());
            long rssMid = rssMB();
            std::printf("[extreme] E6c devices=10000 x1-tile alloc in %.3fs rss_mid=%ld MB "
                        "(~%.1f KB/device incl. 16KB tile) | extrapolated 100k cells=~%.0f MB\n",
                        allocS, rssMid, (double)(rssMid - rss0) * 1024.0 / 10000.0,
                        (double)(rssMid - rss0) * 10.0);
        }
    }

    // === E7. Structural key ops at scale ======================================
    {
        ScalarChannel ch;
        const int K = 10000;
        for (int i = 0; i < K; ++i) ch.setKey(i, float(i));
        auto t0 = Clock::now();
        int movedN = ch.shiftFrom(1000, 50, 20000);
        double shiftS = secs(t0, Clock::now());
        t0 = Clock::now();
        int mirrored = ch.mirrorRange(0, 5000);
        double mirrorS = secs(t0, Clock::now());
        t0 = Clock::now();
        for (int i = 0; i < 200; ++i) ch.moveKey(200 + i, 15000 + i);
        double moveS = secs(t0, Clock::now());
        std::printf("[extreme] E7 keys=%d shiftFrom=%d in %.3fs mirror=%d in %.3fs "
                    "moveKeyx200 in %.3fs\n",
                    K, movedN, shiftS, mirrored, mirrorS, moveS);
        INK_CHECK(ch.keyCount() > 0);
    }

    // === E8. Memory model + impossibility arithmetic ===========================
    {
        // Per-unit costs measured on THIS build (no allocation storms).
        Cell probe;
        PaintDevice empty;
        std::printf("[extreme] E8 sizeof(Cell)=%zu sizeof(CellLayer)~%zu sizeof(PaintDevice)=%zu "
                    "sizeof(ScalarKey)=%zu sizeof(ParamBlock)=%zu tile=16KB\n",
                    sizeof(Cell), sizeof(CellLayer), sizeof(PaintDevice),
                    sizeof(ScalarKey), sizeof(ParamBlock));
        std::printf("[extreme] E8 math: 1 tile=16KB | 100k 1-tile cells=%.0f MB | "
                    "1000 layers x 100k 1-tile cells=%.1f TB | "
                    "1M dabs confined=~400KB tiles (CPU-bound, see E6a) | "
                    "1000 tracks x 18 params x 10 keys x ~48B/key=~8.6MB key data\n",
                    100000.0 * 16.0 / 1024.0, 1000.0 * 100000.0 * 16.0 / 1024.0 / 1024.0 / 1024.0);
        (void)probe;
        (void)empty;
    }

    const long rss1 = rssMB();
    std::printf("[extreme] rss_start=%ld MB rss_end=%ld MB delta=%ld MB\n", rss0, rss1,
                rss1 - rss0);
    std::printf("test_extreme_stress PASS (full=%d)\n", FULL ? 1 : 0);
    return 0;
}
