// The timeline gutter header must actually PAINT its caption and its four
// creation buttons (add layer, add folder, group, ungroup).
//
// This regressed silently: the rows are painted inside a clip that starts
// below the header, and paintHeaderRows was called while that clip was still
// active — so the entire header strip was clipped away. The buttons were
// correctly POSITIONED and their icons rendered fine in isolation, which is
// why it looked like the buttons were missing rather than clipped: the
// "LAYERS" caption vanished at the same time, and that was the tell.
//
// A hit-test alone would not catch this (clicks still worked), so this renders
// the widget and samples actual pixels.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTimelineWidget.h"
#include <QApplication>
#include <QImage>
#include <cstdio>
using namespace ink;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Document doc(400, 300, 24);
    GroupLayer *f = doc.ensureFolder();
    PaintLayer *l = doc.createTrack(f, "Layer 1");
    l->deviceAt(0, true);
    l->setSpanEnd(0, kOpenEnd);
    doc.setCurrentLayer(l);

    TimelineDock dock(&doc);
    dock.resize(1100, 620);
    auto *grid = dock.grid();
    grid->resize(900, 560);
    dock.show();
    QApplication::processEvents();
    grid->refresh();
    QApplication::processEvents();

    QImage img(grid->size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    grid->render(&img);

    const int bg = qGray(img.pixel(5, 4));
    // The caption: if this is missing the whole header is clipped, which is the
    // exact failure this test exists for.
    int caption = 0;
    for (int x = 8; x < 70; ++x)
        for (int y = 6; y < 24; ++y) caption = std::max(caption, qGray(img.pixel(x, y)));
    if (caption <= bg + 12)
        std::printf("   LAYERS caption BLANK (bg=%d, brightest=%d)\n", bg, caption);
    INK_CHECK(caption > bg + 12);
    std::printf("gutter header caption painted OK\n");

    // The four buttons, at the offsets headerButtons() places them.
    const int offs[] = {128, 102, 76, 50};
    const char *names[] = {"add layer", "add folder", "group", "ungroup"};
    for (int i = 0; i < 4; ++i) {
        const int x = InkTheme::gutterW() - offs[i];
        INK_CHECK(x >= 0 && x + 22 <= InkTheme::gutterW()); // inside the gutter
        int brightest = 0;
        for (int dx = 0; dx < 22; ++dx)
            for (int dy = 4; dy < 26; ++dy)
                brightest = std::max(brightest, qGray(img.pixel(x + dx, dy)));
        if (brightest <= bg + 12)
            std::printf("   %-12s BLANK at x=%d (bg=%d, brightest=%d)\n", names[i], x, bg,
                        brightest);
        INK_CHECK(brightest > bg + 12);
        std::printf("  %-12s painted at x=%d OK\n", names[i], x);
    }

    std::printf("test_timeline_header_buttons PASS\n");
    return 0;
}
