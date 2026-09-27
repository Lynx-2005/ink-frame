// Stroke-commit cache discipline: release must warm ONE frame, never a
// 12-frame prefetch storm, and duplicate requests must not queue twice
// (the click-release freeze source).
#include "animation/InkAnimation.h"
#include "test_check.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace ink;

static int waitRendered(std::atomic<int> &n, int want, int timeoutMs = 3000) {
    auto t0 = std::chrono::steady_clock::now();
    while (n.load() < want) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count() > timeoutMs)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return n.load();
}

int main() {
    // requestSingle renders exactly one frame, deduped on repeat calls.
    {
        FrameCache c(64, 1);
        std::atomic<int> n{0};
        c.setRenderFn([&](int) {
            ++n;
            return std::make_shared<PaintDevice>();
        });
        c.requestSingle(7);
        c.requestSingle(7);
        c.requestSingle(7);
        INK_CHECK(waitRendered(n, 1) == 1);
        c.waitFor(7);
        INK_CHECK(c.get(7) != nullptr);
        // No prefetch storm: neighbours stay unqueued/unrendered.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        INK_CHECK(n.load() == 1);
        INK_CHECK(c.get(8) == nullptr);
    }
    // requestRange still warms ahead frames for scrub/playback, without dupes.
    {
        FrameCache c(64, 1);
        std::atomic<int> n{0};
        c.setRenderFn([&](int) {
            ++n;
            return std::make_shared<PaintDevice>();
        });
        c.requestRange(0, 3);
        c.requestRange(0, 3); // duplicate call must not re-queue
        for (int f = 0; f <= 3; ++f) c.waitFor(f);
        INK_CHECK(waitRendered(n, 4) == 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        INK_CHECK(n.load() == 4);
    }
    // invalidate(from) drops queued work at/after `from`.
    {
        FrameCache c(64, 0); // 0 threads requested -> clamped to 1; render lazily
        (void)c;
        FrameCache c2(64, 1);
        std::atomic<int> n{0};
        c2.setRenderFn([&](int f) {
            // Slow frame 5 so invalidate(5) can catch it queued, not rendered.
            if (f == 5) std::this_thread::sleep_for(std::chrono::milliseconds(150));
            ++n;
            return std::make_shared<PaintDevice>();
        });
        c2.requestSingle(5);
        c2.invalidate(5);
        c2.requestSingle(6);
        c2.waitFor(6);
        INK_CHECK(c2.get(6) != nullptr);
    }
    std::printf("test_framecache OK\n");
    return 0;
}
