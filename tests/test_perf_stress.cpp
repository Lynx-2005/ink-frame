// Extreme stress / performance probe for InkFrame (HEADLESS, OOM-SAFE).
//
// Goal: measure current engine cost for the exact scenarios in the request:
//   1. keyframes at scale (1000+ tracks x many keys, TimelineModel + channels)
//   2. graph/curve editing (bezier eval + handle drags + auto-tangent refresh)
//   3. ParamBlock eval (isIdentityAt/matrixFor per animated layer per frame)
//   4. strokes at scale ("millions of dabs" ~= thousands of strokes; one shared
//      small device so tiles stay bounded and the box can never OOM)
//   5. projection at scale (Image with folders/tracks/cells/cameras, full +
//      incremental projectRect, multi-frame scrub)
//   6. onion skin + FrameCache
//   7. OOM-safety proofs (tile caps, key-drop policy, RSS delta cap)
//
// OOM SAFETY (5.7 GiB box, ~3 GiB free at authoring time):
//   - All canvases are SMALL (320x180 or 480x270). A full 480x270 RGBA frame
//     is ~0.5 MB; even 64 cached frames are ~32 MB.
//   - Stroke tests reuse ONE PaintDevice confined to a 256x256 region
//     (max 16 tiles = 256 KB). Dab count scales CPU only, never memory.
//   - Per-track paint is a 24x24 fill (1 tile, 16 KB). 1000 tracks ~= 16 MB.
//   - TimelineModel-only scale test allocates no pixels at all.
//   - RSS is sampled before/after via getrusage; the suite FAILS if the delta
//     exceeds kMaxRssDeltaMB (leak/OOM-path tripwire, not a perf budget).
//   - Full extremes (1M dabs, 1000-track full projection) run only with
//     INKFRAME_PERF_FULL=1; default `ctest` runs the quick tier (<30 s).
//
// This file MEASURES ONLY. It asserts correctness invariants (no new bugs)
// but treats timings as REPORTED numbers, not pass/fail budgets. Budgets
// come after we see the baseline.
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

// ---- helpers ---------------------------------------------------------------
double secs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}
long rssMB() {
    struct rusage u {};
    if (getrusage(RUSAGE_SELF, &u) != 0) return -1;
    return (long)(u.ru_maxrss / 1024); // Linux ru_maxrss is KiB
}
bool perfFull() {
    const char *e = std::getenv("INKFRAME_PERF_FULL");
    return e && std::string(e) == "1";
}
void memHeader(long rss0) {
    std::printf("[perf] rss_start=%ld MB full=%d\n", rss0, perfFull() ? 1 : 0);
}

} // namespace

int main() {
    const long rss0 = rssMB();
    memHeader(rss0);
    const bool FULL = perfFull();

    // === 1. TimelineModel at 1000+ tracks x keys (no pixels) =================
    {
        TimelineModel tm;
        tm.setFrameCount(240);
        const int kFolders = FULL ? 20 : 10;
        const int kTracksPer = FULL ? 60 : 120; // default: 1200 tracks
        const int kKeysPer = FULL ? 40 : 20;
        auto t0 = Clock::now();
        int tracks = 0;
        for (int f = 0; f < kFolders; ++f) {
            std::string fid = tm.addFolder("F" + std::to_string(f));
            for (int t = 0; t < kTracksPer; ++t) {
                std::string id = "T" + std::to_string(f) + "_" + std::to_string(t);
                tm.addTrack(fid, id, "track");
                for (int k = 0; k < kKeysPer; ++k)
                    tm.setKey(id, (k * 11 + t) % 240);
                ++tracks;
            }
        }
        double buildS = secs(t0, Clock::now());

        // Scrub: activeKey over all tracks x 60 frames (playback pass).
        t0 = Clock::now();
        long hits = 0;
        for (int fr = 0; fr < 60; ++fr)
            for (const auto &fo : tm.folders())
                for (const auto &tr : fo.tracks)
                    hits += tm.activeKey(tr.layerId, fr);
        double scrubS = secs(t0, Clock::now());

        // Cells enumeration (the O(K^2) suspect) over a sample of tracks.
        t0 = Clock::now();
        size_t cellTotal = 0;
        int sampled = 0;
        for (const auto &fo : tm.folders())
            for (const auto &tr : fo.tracks) {
                if ((sampled++ % 6) != 0) continue; // sample 1/6
                cellTotal += tm.cells(tr.layerId).size();
            }
        double cellsS = secs(t0, Clock::now());

        // Structural edit: move one key per sampled track.
        t0 = Clock::now();
        int moved = 0;
        sampled = 0;
        for (const auto &fo : tm.folders())
            for (const auto &tr : fo.tracks) {
                if ((sampled++ % 12) != 0) continue;
                auto cs = tm.cells(tr.layerId);
                if (!cs.empty()) {
                    int from = cs[0].start;
                    int to = std::min(from + 1, 239);
                    if (tm.moveKey(tr.layerId, from, to)) ++moved;
                }
            }
        double moveS = secs(t0, Clock::now());

        std::printf("[perf] timeline tracks=%d keys/track=%d build=%.3fs "
                    "scrub60f=%.3fs (%.1f track-lookups/s) cells(sample)=%zu in %.3fs "
                    "moveKey=%d in %.3fs\n",
                    tracks, kKeysPer, buildS, scrubS,
                    (tracks * 60.0) / std::max(scrubS, 1e-9), cellTotal, cellsS,
                    moved, moveS);
        // Correctness spot-checks (no new bugs).
        INK_CHECK(tm.visibleRowCount() > tracks);
        INK_CHECK(tm.activeKey("T0_0", 239) >= 0);
    }

    // === 2. Scalar/bezier graph editing ======================================
    {
        ScalarChannel ch;
        const int K = FULL ? 2000 : 1000;
        for (int i = 0; i < K; ++i)
            ch.setKey(i * 2, float(i) * 1.5f,
                      (i % 3 == 0) ? Interp::Bezier : Interp::Linear);
        INK_CHECK(ch.keyCount() == K);
        // Curve-editor redraw: sample valueAtF densely (one 1920px graph).
        auto t0 = Clock::now();
        const int kSamples = 10000;
        volatile double sink = 0;
        for (int i = 0; i < kSamples; ++i)
            sink += ch.valueAtF(i * (2.0 * K / kSamples));
        double sampleS = secs(t0, Clock::now());
        std::printf("[perf] bezier keys=%d sampleN=%d in %.3fs (%.1f eval/s)\n",
                    K, kSamples, sampleS, kSamples / std::max(sampleS, 1e-9));

        // Handle drag simulation: 200 handle edits (free drags + auto recompute).
        t0 = Clock::now();
        for (int i = 0; i < 200; ++i) {
            int f = (i * 10) % (K * 2);
            if (!ch.hasKey(f)) continue;
            if (i % 2 == 0)
                ch.setHandles(f, 0.5, 1.0, 0.5, -1.0); // free-handle drag
            else
                ch.autoHandles(f); // snap back to neighbour-derived tangent
        }
        double handlesS = secs(t0, Clock::now());
        (void)sink;
        std::printf("[perf] bezier handle-edits=200 in %.3fs\n", handlesS);

        // Segment walk (what the lane-diamond painter does per animated param).
        t0 = Clock::now();
        int segs = 0;
        for (int k : ch.keys()) {
            CubicSegment s;
            if (ch.segment(k, &s)) ++segs;
        }
        double segS = secs(t0, Clock::now());
        std::printf("[perf] bezier segments=%d walked in %.3fs\n", segs, segS);
        // Collinear sanity: linear keys lerp exactly.
        ScalarChannel lin;
        lin.setKey(0, 0.0f, Interp::Linear);
        lin.setKey(10, 10.0f, Interp::Linear);
        INK_CHECK(std::abs(lin.valueAt(5) - 5.0f) < 1e-4);
    }

    // === 3. ParamBlock eval (per animated layer per frame) ===================
    {
        const int kLayers = FULL ? 400 : 200;
        const int kKeys = 20;
        std::vector<ParamBlock> blocks;
        blocks.reserve(kLayers);
        for (int l = 0; l < kLayers; ++l) {
            ParamBlock b;
            b.setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
            b.setKey(ParamId::PositionX, 30, 40.0 + l, Interp::Bezier);
            b.setKey(ParamId::Rotation, 0, 0.0, Interp::Linear);
            b.setKey(ParamId::Rotation, 30, 15.0, Interp::Linear);
            for (int k = 0; k < kKeys; ++k) {
                b.setKey(ParamId::Opacity, k * 2, 0.5 + 0.01 * k, Interp::Hold);
            }
            blocks.push_back(std::move(b));
        }
        const Rect canvas{0, 0, 480, 270};
        auto t0 = Clock::now();
        volatile double sink = 0;
        const int kFrames = 60;
        for (int f = 0; f < kFrames; ++f)
            for (auto &b : blocks) {
                float op = 1.0f;
                Mat3 m = b.matrixFor(canvas, double(f), &op);
                sink += m.m[0] + op;
                if (b.isIdentityAt(double(f))) sink += 1.0;
            }
        double s = secs(t0, Clock::now());
        long evals = (long)kLayers * kFrames * 2; // matrixFor + isIdentityAt
        std::printf("[perf] paramblock layers=%d frames=%d evals=%ld in %.3fs "
                    "(%.1f eval/s)\n",
                    kLayers, kFrames, evals, s, evals / std::max(s, 1e-9));
        (void)sink;
        ParamBlock empty;
        INK_CHECK(empty.isIdentityAt(0));
    }

    // === 4. Strokes: millions of dabs, bounded tiles =========================
    {
        AutoBrushTip tip(BrushTipShape{16.0f, 0.7f, 1.0f, 0.08f});
        const auto &mask = tip.mask();
        const int ms = tip.maskSize();
        PaintDevice dev;
        TileMemento m;
        const long kDabs = FULL ? 1000000L : 200000L;
        auto t0 = Clock::now();
        // Confine to 256x256 so tile count caps at 16 tiles (256 KB).
        for (long i = 0; i < kDabs; ++i) {
            float x = float(32 + (i * 7) % 224);
            float y = float(32 + (i * 13) % 224);
            dev.applyDab(x, y, mask.data(), ms, {255, 30, 30, 255}, 1.0f,
                         CompositeOp::Normal, (i == 0) ? &m : nullptr);
        }
        double s = secs(t0, Clock::now());
        std::printf("[perf] dabs=%ld in %.3fs = %.1f dabs/s tiles=%zu (~%zu KB)\n",
                    kDabs, s, kDabs / std::max(s, 1e-9), dev.tileCount(),
                    dev.tileCount() * 16);
        // 256x256 area + 17px mask => 5x5 tiles max (24 observed: edge clips).
        INK_CHECK(dev.tileCount() <= 25);
        INK_CHECK(!dev.extent().isEmpty());

        // 1000+ strokes x 200 dabs via PixelBrushOp, memento cleared per
        // stroke so undo memory never accumulates (the OOM guard).
        const int kStrokes = FULL ? 2000 : 1000;
        const int kEventsPer = 40; // ~5 dabs/event => ~200 dabs/stroke
        PaintDevice dev2;
        t0 = Clock::now();
        long totalDabs = 0;
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
            totalDabs += kEventsPer * 5;
            // mm drops here: bounded undo memory per stroke by design.
        }
        double s2 = secs(t0, Clock::now());
        std::printf("[perf] brush_strokes=%d (~%ld dabs) in %.3fs = %.1f strokes/s tiles=%zu\n",
                    kStrokes, totalDabs, s2, kStrokes / std::max(s2, 1e-9),
                    dev2.tileCount());
        INK_CHECK(dev2.tileCount() <= 16);
    }

    // === 5. Projection: folders/tracks/cells/cameras =========================
    {
        const int W = 480, H = 270;
        const int kTracks = FULL ? 1000 : 300;
        const int kCellKeys = 5;
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
                // 24x24 dot at a deterministic tile: 1 tile per painted cell.
                // Paint ONLY key 0 on every track + all keys on 5 tracks, so
                // pixel memory stays ~= (kTracks + 20) tiles, never full-canvas.
                if (k == 0 || t < 5) {
                    int px = (t * 37) % (W - 24), py = (t * 53 + k * 11) % (H - 24);
                    TileMemento mm;
                    d->fillRect({px, py, 24, 24},
                                {(uint8_t)(t & 255), 120, 200, 255});
                    (void)mm;
                }
                pl->setSpanEnd(key, kOpenEnd);
            }
            // Cell-layer depth on a few tracks (stack cost, not pixel cost).
            if (t < 3) {
                Cell *c = pl->cellAt(0, false);
                if (c) {
                    c->addLayer("extra paint", nullptr);
                    c->addGroup("grp", nullptr);
                }
            }
        }
        // Animate 5 tracks + 2 cameras (layer + global scopes).
        for (int t = 0; t < 5; ++t) {
            layers[t]->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Linear);
            layers[t]->params().setKey(ParamId::PositionX, 30, 20.0, Interp::Linear);
        }
        CameraLayer *lc =
            img.createCamera(g, "LC", CameraScope::Layer, layers[0]->id());
        lc->params().setStaticValue(ParamId::PositionX, 4.0);
        CameraLayer *gc = img.createCamera(g, "GC", CameraScope::Global, 0);
        gc->params().setStaticValue(ParamId::PositionY, 0.0); // identity-ish

        PaintDevice out;
        auto t0 = Clock::now();
        const int kFrames = FULL ? 30 : 10;
        for (int f = 0; f < kFrames; ++f) {
            out.clear();
            img.project(f, out);
        }
        double projS = secs(t0, Clock::now());
        std::printf("[perf] project tracks=%d cells/track=%d frames=%d "
                    "total=%.3fs (%.3fs/frame %.1f fps)\n",
                    kTracks, kCellKeys, kFrames, projS, projS / kFrames,
                    kFrames / std::max(projS, 1e-9));
        INK_CHECK(!out.extent().isEmpty());

        // Incremental path: small dirty rect (the stroke case).
        t0 = Clock::now();
        const int kIncr = 20;
        for (int i = 0; i < kIncr; ++i) {
            Rect r{60 + (i % 5) * 8, 40, 64, 64};
            img.projectRect(0, r, out);
        }
        double incrS = secs(t0, Clock::now());
        std::printf("[perf] projectRectx%d (64x64) total=%.3fs (%.3fs each)\n",
                    kIncr, incrS, incrS / kIncr);

        // Camera-active frame (global cam forces full rebuild path).
        gc->params().setStaticValue(ParamId::PositionX, 12.0);
        t0 = Clock::now();
        for (int f = 0; f < 5; ++f) {
            out.clear();
            img.project(f, out);
        }
        double camS = secs(t0, Clock::now());
        std::printf("[perf] project camera-active x5 total=%.3fs (%.3fs each)\n",
                    camS, camS / 5);
        gc->params().setStaticValue(ParamId::PositionX, 0.0);
    }

    // === 6. Onion skin + FrameCache ==========================================
    {
        PaintDevice canvas, frame;
        frame.fillRect({0, 0, 128, 128}, {255, 255, 255, 255});
        auto t0 = Clock::now();
        const int kGhosts = 30;
        for (int i = 0; i < kGhosts; ++i) {
            canvas.clear();
            compositeOnionSkin(canvas, frame, {255, 60, 60, 255}, 0.35f);
        }
        double onionS = secs(t0, Clock::now());
        std::printf("[perf] onion 128x128 x%d total=%.3fs (%.3fs each)\n", kGhosts,
                    onionS, onionS / kGhosts);
        INK_CHECK(canvas.pixel(10, 10).a > 0);

        FrameCache fc(8, 2);
        fc.setRenderFn([](int f) {
            auto d = std::make_shared<PaintDevice>();
            TileMemento mm;
            d->fillRect({0, 0, 64, 64}, {(uint8_t)(f & 255), 0, 0, 255},
                        CompositeOp::Normal, &mm);
            return d;
        });
        t0 = Clock::now();
        fc.requestRange(0, 8);
        fc.waitFor(7);
        double fcS = secs(t0, Clock::now());
        std::printf("[perf] framecache 8x64x64 prefetch in %.3fs cached=%zu\n", fcS,
                    fc.cachedCount());
        INK_CHECK(fc.get(7) != nullptr);
    }

    // === 7. OOM-safety proofs ================================================
    {
        // 7a. Sparse tiles: 1M-px-spaced pixels allocate few tiles, never 1M px.
        PaintDevice dev;
        TileMemento m;
        for (int i = 0; i < 1000; ++i)
            dev.setPixel(i * 64, 0, {1, 2, 3, 255}, (i == 0) ? &m : nullptr);
        std::printf("[oom] sparse 1000 px -> tiles=%zu (cap 1000)\n", dev.tileCount());
        INK_CHECK(dev.tileCount() <= 1000);

        // 7b. shiftFrom past max drops keys instead of growing unbounded.
        RasterChannel rc;
        rc.setKey(0);
        rc.setKey(10);
        int moved = rc.shiftFrom(0, 100000, 119);
        INK_CHECK(rc.keys().empty() && moved == 0);

        // 7c. ScalarChannel with 10k keys stays queryable (map, no vector blowup).
        ScalarChannel big;
        for (int i = 0; i < 10000; i += 10) big.setKey(i, float(i));
        INK_CHECK(big.keyCount() == 1000);
        volatile float v = big.valueAt(5005);
        (void)v;

        // 7d. Empty-image projection is trivially cheap (no alloc storm).
        Image empty(64, 64, 24);
        empty.createFolder("F");
        PaintDevice out;
        auto t0 = Clock::now();
        for (int i = 0; i < 50; ++i) {
            out.clear();
            empty.project(i, out);
        }
        std::printf("[oom] empty project x50 in %.3fs tiles=%zu\n",
                    secs(t0, Clock::now()), out.tileCount());
        INK_CHECK(out.tileCount() == 0);
    }

    const long rss1 = rssMB();
    std::printf("[perf] rss_start=%ld MB rss_end=%ld MB delta=%ld MB (cap 800)\n",
                rss0, rss1, rss1 - rss0);
    INK_CHECK((rss1 - rss0) < 800);
    std::printf("test_perf_stress PASS (full=%d)\n", FULL ? 1 : 0);
    return 0;
}
