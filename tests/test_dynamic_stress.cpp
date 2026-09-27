// Dynamic stress probe: the ACTUAL stroke path under animation (HEADLESS).
//
// test_extreme_stress.cpp measures each cost in isolation. This file replays
// the combined path the artist feels as lag, exactly as InkPaintTool drives it
// (src/ui/tools/InkPaintTool.cpp: sendEvent):
//   per tablet event: PixelBrushOp::addEvent -> dabs into the CELL device
//   (dirtyAccum) -> Image::projectRect(frame, dirty, out) -> canvas upload.
//
// Scenes combine what the report says hurts: keyframed (animated) tracks +
// camera layers + real brush strokes, all at once. Playback-with-onion and
// graph-drag-while-repainting are replayed the same way.
//
// MEASURES ONLY. Timings reported, never budgets. INK_CHECK = correctness.
// OOM-SAFE: 480x270 canvas, sparse 24x24 dots (1 tile/track), small dirty
// rects. Full tier (INKFRAME_EXTREME_FULL=1) adds the 1000-camera case and
// longer strokes; quick tier targets < ~60 s.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <vector>

#include "animation/InkAnimation.h"
#include "animation/InkKeyframe.h"
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
    return (long)(u.ru_maxrss / 1024);
}
bool dynFull() {
    const char *e = std::getenv("INKFRAME_EXTREME_FULL");
    return e && std::string(e) == "1";
}

// 1000-track scene with sparse dots; first nAnimated tracks get a 2-key
// PositionX slide over frames 0..30; cameras per request. Frame 5 is inside
// the animated range (non-identity), frame 100 is past it (rest pose).
void buildDynScene(Image &img, int nTracks, int nAnimated, int nLayerCams, bool globalCam,
                   bool paintAllKeys = false) {
    const int W = img.width(), H = img.height();
    GroupLayer *g = img.createFolder("G");
    std::vector<PaintLayer *> layers;
    layers.reserve(nTracks);
    for (int t = 0; t < nTracks; ++t) {
        PaintLayer *pl = img.createTrack(g, "T" + std::to_string(t));
        layers.push_back(pl);
        for (int k = 0; k < 3; ++k) {
            int key = k * 10;
            PaintDevice *d = pl->deviceAt(key, true);
            if (k == 0 || paintAllKeys) {
                int px = (t * 37) % (W - 24), py = (t * 53) % (H - 24);
                TileMemento mm;
                d->fillRect({px, py, 24, 24}, {(uint8_t)(t & 255), 120, 200, 255});
                (void)mm;
            }
            pl->setSpanEnd(key, kOpenEnd);
        }
    }
    for (int t = 0; t < nAnimated && t < nTracks; ++t) {
        layers[t]->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Linear, true, false);
        layers[t]->params().setKey(ParamId::PositionX, 30, 20.0, Interp::Linear, true, false);
    }
    for (int t = 0; t < nLayerCams && t < nTracks; ++t) {
        CameraLayer *c = img.createCamera(g, "LC" + std::to_string(t), CameraScope::Layer,
                                          layers[t]->id());
        c->params().setStaticValue(ParamId::PositionX, 4.0);
    }
    if (globalCam) {
        CameraLayer *gc = img.createCamera(g, "GC", CameraScope::Global, 0);
        gc->params().setStaticValue(ParamId::PositionX, 12.0);
    }
}

struct StrokeStat {
    double totalS{0}, paintS{0}, reprojS{0};
    int events{0}, reprojs{0};
};

// One live stroke into target's exposed cell at `frame`, re-projecting the
// per-event dirty rect exactly like PaintToolBase::sendEvent. Pass
// reproject=false to split brush cost from compositor cost.
// Returns false when the scene is wrong (missing key/device); the caller
// turns that into INK_CHECK.
bool runLiveStroke(Image &img, PaintLayer *target, int frame, int nEvents,
                   PaintDevice &out, bool reproject, StrokeStat &st) {
    st = StrokeStat{};
    out.clear();
    img.project(frame, out); // warm: canvas holds the frame, as in a session
    const int key = target->exposureAt(frame);
    if (key < 0) return false;
    PaintDevice *dev = target->deviceAt(key, false);
    if (!dev) return false;
    TileMemento mm;
    Rect dirty;
    PixelBrushOp op(AutoBrushTip(BrushTipShape{12.0f, 0.7f, 1.0f, 0.05f}), BrushDynamics{});
    op.beginStroke({dev, {20, 120, 220, 255}, CompositeOp::Normal, &mm, &dirty});
    for (int e = 0; e < nEvents; ++e) {
        StrokeEvent ev;
        ev.state.x = float(40 + (e * 3) % 400);
        ev.state.y = float(40 + (e * 5) % 190);
        ev.state.pressure = 0.8f;
        dirty = Rect{};
        auto t0 = Clock::now();
        op.addEvent(ev);
        auto t1 = Clock::now();
        st.paintS += secs(t0, t1);
        if (reproject && !dirty.isEmpty()) {
            auto t2 = Clock::now();
            img.projectRect(frame, dirty, out);
            auto t3 = Clock::now();
            st.reprojS += secs(t2, t3);
            ++st.reprojs;
        }
        ++st.events;
    }
    dirty = Rect{};
    op.endStroke(); // final dabs flushed like endPrimary (not timed: same path)
    if (reproject && !dirty.isEmpty()) img.projectRect(frame, dirty, out);
    st.totalS = st.paintS + st.reprojS;
    return true;
}

void reportStroke(const char *tag, const StrokeStat &st) {    const double perEv = 1000.0 * st.totalS / std::max(st.events, 1);
    std::printf("[dynamic] stroke[%s] events=%d reprojs=%d paint=%.3fs reproj=%.3fs "
                "total=%.3fs (%.3fms/event paint=%.3fms reproj=%.3fms)\n",
                tag, st.events, st.reprojs, st.paintS, st.reprojS, st.totalS, perEv,
                1000.0 * st.paintS / std::max(st.events, 1),
                st.reprojS / std::max(st.reprojs, 1) * 1000.0);
}

// Pixel-exactness: `out` (incremental state) vs a fresh full project must
// agree over the refreshed region after one more dot. Proves the incremental
// path (animated + layer-camera) ships the same pixels as project().
bool checkIncrementalExact(Image &img, PaintLayer *target, int frame, PaintDevice &out,
                           const char *tag) {
    const int key = target->exposureAt(frame);
    if (key < 0) return false;
    PaintDevice *dev = target->deviceAt(key, false);
    if (!dev) return false;
    TileMemento mm;
    const Rect dot{100, 100, 24, 24};
    dev->fillRect(dot, {9, 200, 90, 255}, CompositeOp::Normal, &mm);
    const Rect aligned = img.projectRect(frame, dot, out);
    if (aligned.isEmpty()) return false;
    PaintDevice ref;
    img.project(frame, ref);
    const Rect cmp = intersectRect(aligned, {0, 0, img.width(), img.height()});
    if (cmp.isEmpty()) return false;
    std::vector<uint8_t> a((size_t)cmp.w * cmp.h * 4), b((size_t)cmp.w * cmp.h * 4);
    out.readRect(cmp, a.data());
    ref.readRect(cmp, b.data());
    const bool same = (a == b);
    std::printf("[dynamic] exact[%s] aligned=(%d,%d,%d,%d) %s\n", tag, aligned.x, aligned.y,
                aligned.w, aligned.h, same ? "IDENTICAL" : "MISMATCH");
    return same;
}

} // namespace

int main() {
    const long rss0 = rssMB();
    const bool FULL = dynFull();
    std::printf("[dynamic] rss_start=%ld MB full=%d\n", rss0, FULL ? 1 : 0);
    const int W = 480, H = 270, FPS = 24;
    const int kTracks = 1000, kAnimated = 50, kFrame = 5;
    const int kEvQD = FULL ? 300 : 150; // events for cheap (non-fallback) cases
    const int kEvCam = FULL ? 100 : 40; // events for camera-fallback cases

    // === D1. Live stroke, static scene (baseline: brush + fast path) =========
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, 0, 0, false);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        PaintDevice out;
        StrokeStat paintOnly; INK_CHECK(runLiveStroke(img, pls[500], kFrame, kEvQD, out, false, paintOnly));
        reportStroke("static/paint-only", paintOnly);
        StrokeStat live; INK_CHECK(runLiveStroke(img, pls[501], kFrame, kEvQD, out, true, live));
        reportStroke("static/paint+reproj", live);
        INK_CHECK(!out.extent().isEmpty());
    }

    // === D2. Live stroke, 50 animated tracks, no cameras ======================
    // Animated tracks take compositeTrackRect (inverse-map + resample) inside
    // the dirty rect: the per-dab tax of animating while drawing.
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, kAnimated, 0, false);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        PaintDevice out;
        // Stroke into a STATIC track while OTHERS animate (background anim).
        StrokeStat live; INK_CHECK(runLiveStroke(img, pls[900], kFrame, kEvQD, out, true, live));
        reportStroke("animated-bg/paint+reproj", live);
        // Stroke into an ANIMATED track itself (its own cell resamples).
        StrokeStat self; INK_CHECK(runLiveStroke(img, pls[3], kFrame, kEvQD, out, true, self));
        reportStroke("animated-self/paint+reproj", self);
        INK_CHECK(checkIncrementalExact(img, pls[3], kFrame, out, "animated-self"));
    }

    // === D3. Live stroke + 1 global camera (fallback per dab) =================
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, kAnimated, 0, true);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        PaintDevice out;
        StrokeStat live; INK_CHECK(runLiveStroke(img, pls[900], kFrame, kEvCam, out, true, live));
        reportStroke("1-global-cam/paint+reproj", live);
    }

    // === D4. Live stroke + 100 layer cameras ==================================
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, kAnimated, 100, false);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        PaintDevice out;
        StrokeStat live; INK_CHECK(runLiveStroke(img, pls[900], kFrame, kEvCam, out, true, live));
        reportStroke("100-layer-cams/paint+reproj", live);
        INK_CHECK(checkIncrementalExact(img, pls[900], kFrame, out, "100-layer-cams"));
    }

    // === D5. FULL: live stroke + 1000 layer cameras ("camera in each layer") ==
    if (FULL) {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, kAnimated, 1000, false);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        PaintDevice out;
        StrokeStat live; INK_CHECK(runLiveStroke(img, pls[900], kFrame, 40, out, true, live));
        reportStroke("1000-layer-cams/paint+reproj", live);
        INK_CHECK(checkIncrementalExact(img, pls[900], kFrame, out, "1000-layer-cams"));
    } else {
        std::printf("[dynamic] stroke[1000-layer-cams] SKIPPED (needs INKFRAME_EXTREME_FULL=1)\n");
    }

    // === D6. Playback scrub with onion skin (animated strokes in motion) ======
    // Per frame: project current + previous + tint pass, i.e. what onion
    // playback pays over plain projection.
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, kAnimated, 0, false, true); // ink on every key
        PaintDevice cur, prv;
        const int kFrames = FULL ? 60 : 30;
        auto t0 = Clock::now();
        for (int f = 0; f < kFrames; ++f) {
            cur.clear();
            img.project(f, cur);
        }
        double plainS = secs(t0, Clock::now());
        t0 = Clock::now();
        for (int f = 0; f < kFrames; ++f) {
            cur.clear();
            img.project(f, cur);
            if (f > 0) compositeOnionSkin(cur, prv, {255, 60, 60, 255}, 0.35f);
            prv.clear();
            prv.blit(cur, 0, 0);
        }
        double onionS = secs(t0, Clock::now());
        std::printf("[dynamic] playback frames=%d plain=%.3fs (%.3fs/f %.1f fps) "
                    "with-onion=%.3fs (%.3fs/f %.1f fps, onion overhead x%.2f)\n",
                    kFrames, plainS, plainS / kFrames, kFrames / std::max(plainS, 1e-9),
                    onionS, onionS / kFrames, kFrames / std::max(onionS, 1e-9),
                    onionS / std::max(plainS, 1e-9));
        INK_CHECK(!cur.extent().isEmpty());
    }

    // === D7. Graph-drag while repainting (handle edit + re-project/step) ======
    // One mouse-move step = setKeyValue (O(K) auto refresh) + matrixFor +
    // projectRect of the stroke-sized dirty rect on the animated scene.
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, kAnimated, 0, false);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        // 2000-key animated channel on the dragged track (E2b scale).
        for (int i = 0; i < 2000; ++i)
            pls[3]->params().setKey(ParamId::PositionX, i, double(i) * 0.5,
                                    Interp::Bezier, true, false);
        pls[3]->params().refreshAllAutoHandles();
        PaintDevice out;
        out.clear();
        img.project(kFrame, out);
        const int kSteps = FULL ? 100 : 50;
        auto t0 = Clock::now();
        double editS = 0, projS = 0;
        for (int s = 0; s < kSteps; ++s) {
            int f = (s * 37) % 2000;
            auto te = Clock::now();
            pls[3]->params().setKeyValue(ParamId::PositionX, f, double(f) * 0.5 + 10.0);
            float op = 1.0f;
            Rect canvas{0, 0, W, H};
            Mat3 m = pls[3]->params().matrixFor(canvas, double(kFrame), &op);
            (void)m;
            (void)op;
            auto tp = Clock::now();
            editS += secs(te, tp);
            Rect dirty{60 + (s % 5) * 8, 40, 64, 64};
            img.projectRect(kFrame, dirty, out);
            projS += secs(tp, Clock::now());
        }
        double totalS = secs(t0, Clock::now());
        std::printf("[dynamic] graph-drag steps=%d edit=%.3fs (%.3fms/step) "
                    "reproj=%.3fs (%.3fms/step) total=%.3fs (%.3fms/step)\n",
                    kSteps, editS, 1000.0 * editS / kSteps, projS,
                    1000.0 * projS / kSteps, totalS, 1000.0 * totalS / kSteps);
    }

    // === D8. Move-tool drag (static-value nudge + repaint/step, unkeyed) ======
    {
        Image img(W, H, FPS);
        buildDynScene(img, kTracks, 0, 0, false);
        std::vector<PaintLayer *> pls;
        img.collectPaintLayers(pls);
        PaintDevice out;
        out.clear();
        img.project(kFrame, out);
        const int kSteps = FULL ? 200 : 100;
        auto t0 = Clock::now();
        for (int s = 0; s < kSteps; ++s) {
            pls[10]->params().setStaticValue(ParamId::PositionX, double(s) * 0.5);
            Rect dirty{60 + (s % 5) * 8, 40, 64, 64};
            img.projectRect(kFrame, dirty, out);
        }
        double totalS = secs(t0, Clock::now());
        std::printf("[dynamic] move-drag steps=%d total=%.3fs (%.3fms/step)\n", kSteps,
                    totalS, 1000.0 * totalS / kSteps);
    }

    const long rss1 = rssMB();
    std::printf("[dynamic] rss_start=%ld MB rss_end=%ld MB delta=%ld MB\n", rss0, rss1,
                rss1 - rss0);
    std::printf("test_dynamic_stress PASS (full=%d)\n", FULL ? 1 : 0);
    return 0;
}
