// Onion skin colour + coverage contract (headless).
// Regression: the tint used to be `tint * luminance(ink)`, so black ink
// ghosted black and thick opaque strokes lost their red/blue hue entirely.
#include "animation/InkAnimation.h"
#include "core/InkPaintDevice.h"
#include <iostream>

#define INK_CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

using namespace ink;

static void putPixel(PaintDevice &d, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    d.setPixel(x, y, PixelRGBA8{r, g, b, a});
}

int main() {
    const PixelRGBA8 red{255, 60, 60, 255};
    const PixelRGBA8 blue{60, 160, 255, 255};

    // 1. Opaque black ink ghosts as EXACTLY the tint (not black).
    {
        PaintDevice ghost, canvas;
        putPixel(ghost, 4, 4, 0, 0, 0, 255);
        compositeOnionSkin(canvas, ghost, red, 1.0f);
        PixelRGBA8 p = canvas.pixel(4, 4);
        INK_CHECK(p.r == 255 && p.g == 60 && p.b == 60 && p.a == 255);
    }

    // 2. The ink value never changes the hue: black / grey / white ghost alike.
    {
        const PixelRGBA8 inks[3] = {{0, 0, 0, 255}, {128, 128, 128, 255}, {255, 255, 255, 255}};
        for (const PixelRGBA8 &ink : inks) {
            PaintDevice ghost, canvas;
            putPixel(ghost, 2, 2, ink.r, ink.g, ink.b, ink.a);
            compositeOnionSkin(canvas, ghost, blue, 1.0f);
            PixelRGBA8 p = canvas.pixel(2, 2);
            INK_CHECK(p.r == 60 && p.g == 160 && p.b == 255 && p.a == 255);
        }
    }

    // 3. Coverage is preserved: half-alpha ink ghosts at half alpha, same hue
    //    (channels may land 1/255 off through the blend, never a different hue).
    {
        PaintDevice ghost, canvas;
        putPixel(ghost, 1, 1, 0, 0, 0, 128);
        compositeOnionSkin(canvas, ghost, red, 1.0f);
        PixelRGBA8 p = canvas.pixel(1, 1);
        INK_CHECK(p.r >= 254 && p.g >= 59 && p.g <= 60 && p.b >= 59 && p.b <= 60);
        INK_CHECK(p.a >= 126 && p.a <= 130);
    }

    // 4. tinted=false keeps the frame's own colours (the Tint toggle).
    {
        PaintDevice ghost, canvas;
        putPixel(ghost, 3, 3, 10, 200, 30, 255);
        compositeOnionSkin(canvas, ghost, red, 1.0f, nullptr, false);
        PixelRGBA8 p = canvas.pixel(3, 3);
        INK_CHECK(p.r == 10 && p.g == 200 && p.b == 30 && p.a == 255);
    }

    // 5. Opacity falloff: the same ghost lands fainter at a lower opacity.
    {
        PaintDevice ghost, near, far;
        putPixel(ghost, 0, 0, 0, 0, 0, 255);
        compositeOnionSkin(near, ghost, red, 0.35f);
        compositeOnionSkin(far, ghost, red, 0.10f);
        INK_CHECK(near.pixel(0, 0).a > far.pixel(0, 0).a);
        INK_CHECK(near.pixel(0, 0).a < 255);
    }

    // 6. Clipped composite touches only the clip: the incremental stroke path
    //    must not leak tint outside the refreshed rect.
    {
        PaintDevice ghost, canvas;
        putPixel(ghost, 0, 0, 0, 0, 0, 255);
        putPixel(ghost, 40, 40, 0, 0, 0, 255);
        const Rect clip{0, 0, 8, 8};
        compositeOnionSkin(canvas, ghost, red, 1.0f, &clip);
        INK_CHECK(canvas.pixel(0, 0).a == 255);
        INK_CHECK(canvas.pixel(40, 40).a == 0);
    }

    // 7. Ghost UNDER the active frame (the canvas order): blitting the frame
    //    over a ghost keeps the drawing's own colour, and the ghost still shows
    //    wherever the frame is transparent.
    {
        PaintDevice ghost, canvas;
        putPixel(ghost, 0, 0, 0, 0, 0, 255);
        putPixel(ghost, 5, 5, 0, 0, 0, 255);
        compositeOnionSkin(canvas, ghost, red, 1.0f);
        PaintDevice frame;
        putPixel(frame, 5, 5, 20, 20, 20, 255);
        canvas.blit(frame, 0, 0);
        PixelRGBA8 drawn = canvas.pixel(5, 5);
        INK_CHECK(drawn.r == 20 && drawn.g == 20 && drawn.b == 20 && drawn.a == 255);
        PixelRGBA8 ghostOnly = canvas.pixel(0, 0);
        INK_CHECK(ghostOnly.r == 255 && ghostOnly.g == 60 && ghostOnly.b == 60);
    }

    // 8. Exactly two onion colours: previous red, next blue.
    {
        OnionSkinConfig cfg;
        INK_CHECK(cfg.prevTint.r > cfg.prevTint.g && cfg.prevTint.r > cfg.prevTint.b);
        INK_CHECK(cfg.nextTint.b > cfg.nextTint.r && cfg.nextTint.b > cfg.nextTint.g);
    }

    std::cout << "onion skin: colour + coverage contract OK\n";
    return 0;
}
