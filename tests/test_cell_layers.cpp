// Per-cell layer stacks (headless): every cell owns its own stack of paint and
// group layers. This covers the model, the compositing that the canvas and the
// thumbnails read, the deep-copy paths (duplicate/mirror/paste) and the save
// format — i.e. everything the right-side Layer panel drives.
#include "core/InkCell.h"
#include "core/InkImage.h"
#include "core/InkStroke.h"
#include "io/InkProject.h"
#include "test_check.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ink;
namespace fs = std::filesystem;

namespace {

PaintDevice *paint(CellLayer *l) { return l->device(); }

// Solid pixel probe: projection is premultiplied sRGB, so compare channels
// loosely rather than exactly.
bool near(int a, int b, int tol = 6) { return a > b - tol && a < b + tol; }

} // namespace

int main() {
    // 1. a fresh cell boots with exactly one paint layer, and deviceAt targets
    //    it — the contract every tool and the old tests rely on.
    {
        PaintLayer l("T");
        l.setKeyAt(0, true);
        Cell *c = l.cellAt(0, false);
        INK_CHECK(c != nullptr);
        INK_CHECK(c->layers().size() == 1);
        INK_CHECK(c->paintLayerCount() == 1);
        INK_CHECK(c->layers().front()->isPaint());
        INK_CHECK(l.deviceAt(0, false) == c->layers().front()->device());
        INK_CHECK(!c->hasContent()); // blank sheet
        c->layers().front()->device()->setPixel(3, 3, {255, 0, 0, 255});
        INK_CHECK(c->hasContent());
        INK_CHECK(l.cellAt(0, false)->hasContent());
        INK_CHECK(l.cellAt(9, false) == nullptr); // no cell, no stack
    }

    // 2. structure: add / reorder / remove, active layer fallback, last-paint guard
    {
        Cell c; // "Layer 1"
        CellLayer *base = c.layers().front().get();
        CellLayer *top = c.addLayer({}, base);
        INK_CHECK(top != nullptr && c.layers().size() == 2);
        INK_CHECK(c.layers().back().get() == top); // added above
        INK_CHECK(c.paintLayerCount() == 2);
        INK_CHECK(c.autoLayerName() == "Layer 3");

        c.setActiveId(top->id());
        INK_CHECK(c.activePaintLayer() == top);
        INK_CHECK(c.move(top, -1)); // toward the back
        INK_CHECK(c.layers().front().get() == top);
        INK_CHECK(c.move(top, +1));
        INK_CHECK(c.layers().back().get() == top);
        INK_CHECK(!c.move(top, +1)); // already front-most

        INK_CHECK(c.remove(top));
        INK_CHECK(c.layers().size() == 1);
        // active layer was removed -> falls back to a live paint layer
        INK_CHECK(c.activePaintLayer() == base);
        // the last paint layer can never be removed
        INK_CHECK(!c.canRemove(base));
        INK_CHECK(!c.remove(base));
        INK_CHECK(c.layers().size() == 1);
    }

    // 3. grouping: children move in/out, order is preserved, ungroup restores
    {
        Cell c; // Layer 1
        CellLayer *a = c.layers().front().get();
        CellLayer *b = c.addLayer("B", a);
        CellLayer *d = c.addLayer("D", b);
        INK_CHECK(c.layers().size() == 3);

        CellLayer *g = c.group({b, d}, "G");
        INK_CHECK(g != nullptr);
        INK_CHECK(c.layers().size() == 2);           // A + group
        INK_CHECK(g->children().size() == 2);
        INK_CHECK(g->children()[0].get() == b);      // stack order kept
        INK_CHECK(g->children()[1].get() == d);
        INK_CHECK(c.find(b->id()) == b);             // find is recursive
        INK_CHECK(c.parentOf(b) == &g->children());
        INK_CHECK(c.paintLayerCount() == 3);

        INK_CHECK(!c.moveToGroup(g, g));             // no self-parenting
        INK_CHECK(c.ungroup(g));
        INK_CHECK(c.layers().size() == 3);
        INK_CHECK(c.layers()[0].get() == a);
        INK_CHECK(c.layers()[1].get() == b);
        INK_CHECK(c.layers()[2].get() == d);
    }

    // 4. compositing: opacity, visibility and blend reach the pixels; a group
    //    with its own opacity isolates its children
    {
        Image img(16, 16, 12);
        GroupLayer *f = img.ensureFolder();
        PaintLayer *t = img.createTrack(f, "T");
        t->setKeyAt(0, true);
        Cell *c = t->cellAt(0, false);
        CellLayer *under = c->layers().front().get();
        paint(under)->fillRect({0, 0, 16, 16}, {0, 0, 255, 255}); // blue

        CellLayer *over = c->addLayer("Over", under);
        paint(over)->fillRect({0, 0, 16, 16}, {255, 0, 0, 255}); // red

        PaintDevice out;
        img.project(0, out);
        PixelRGBA8 p = out.pixel(4, 4);
        INK_CHECK(near(p.r, 255) && near(p.b, 0)); // red on top

        // hiding the top layer falls back to blue
        over->setVisible(false);
        img.project(0, out);
        p = out.pixel(4, 4);
        INK_CHECK(near(p.b, 255) && near(p.r, 0));

        // 50% red over blue is a mix of both
        over->setVisible(true);
        over->setOpacity(0.5f);
        img.project(0, out);
        p = out.pixel(4, 4);
        INK_CHECK(p.r > 90 && p.r < 190 && p.b > 90 && p.b < 190);

        // Multiply darkens instead of replacing
        over->setOpacity(1.0f);
        over->setBlend(BlendMode::Multiply);
        img.project(0, out);
        p = out.pixel(4, 4);
        INK_CHECK(p.r < 40 && p.b < 40);

        // group opacity isolates its children from what is under the group
        over->setBlend(BlendMode::Normal);
        CellLayer *grp = c->group({over}, "G");
        INK_CHECK(grp != nullptr);
        grp->setOpacity(0.5f);
        img.project(0, out);
        p = out.pixel(4, 4);
        INK_CHECK(p.r > 90 && p.r < 190 && p.b > 90 && p.b < 190);
    }

    // 5. each cell keeps its OWN stack: editing one never touches another
    {
        Image img(16, 16, 12);
        GroupLayer *f = img.ensureFolder();
        PaintLayer *t = img.createTrack(f, "T");
        t->setKeyAt(0, true);
        t->setKeyAt(20, true);
        Cell *c0 = t->cellAt(0, false);
        Cell *c20 = t->cellAt(20, false);
        INK_CHECK(c0 != c20);
        INK_CHECK(c0->layers().front()->id() != c20->layers().front()->id());
        c0->addLayer("Only in cell 1", c0->layers().front().get());
        INK_CHECK(c0->layers().size() == 2);
        INK_CHECK(c20->layers().size() == 1);
        INK_CHECK(c20->find(c0->layers().back()->id()) == nullptr);
    }

    // 6. moving / copying a cell carries the whole stack, and copies are deep
    {
        PaintLayer l("T");
        l.setKeyAt(0, true);
        l.setSpanEnd(0, 3);
        Cell *c = l.cellAt(0, false);
        CellLayer *extra = c->addLayer("Extra", c->layers().front().get());
        paint(extra)->setPixel(2, 2, {0, 255, 0, 255});

        INK_CHECK(l.moveDeviceContent(0, 20, false));
        INK_CHECK(l.spanEnd(20) == 23);
        Cell *moved = l.cellAt(20, false);
        INK_CHECK(moved != nullptr && moved->layers().size() == 2);
        INK_CHECK(moved->layers().back()->name() == "Extra");

        l.copyDeviceContent(20, 30);
        Cell *copy = l.cellAt(30, false);
        INK_CHECK(copy->layers().size() == 2);
        // deep: the copy has its own layer objects and its own pixels
        INK_CHECK(copy->layers().back()->id() != moved->layers().back()->id());
        INK_CHECK(copy->layers().back()->device() != moved->layers().back()->device());
        paint(copy->layers().back().get())->setPixel(5, 5, {255, 255, 0, 255});
        INK_CHECK(moved->layers().back()->device()->pixel(5, 5).a == 0);
    }

    // 7. undo actions: a structural op is reversible with its pixels intact
    {
        UndoStack undo;
        auto shared = std::make_shared<int>(7);
        int value = 0;
        undo.pushAction(
            "test", [&value, shared] { value = *shared; }, [&value] { value = -1; });
        INK_CHECK(undo.canUndo() && undo.undoName() == "test");
        undo.undo();
        INK_CHECK(value == -1);
        INK_CHECK(undo.canRedo() && undo.redoName() == "test");
        undo.redo();
        INK_CHECK(value == 7);
        INK_CHECK(undo.undoDepth() == 1);
    }

    // 8. save format v2: the stack round-trips structurally (names, nesting,
    //    crop rects, spans) — the loader itself is a separate feature
    {
        Image img(24, 24, 12);
        GroupLayer *f = img.ensureFolder();
        f->setName("Folder 1");
        PaintLayer *t = img.createTrack(f, "Track 1");
        t->setKeyAt(4, true);
        t->setSpanEnd(4, 9);
        Cell *c = t->cellAt(4, false);
        CellLayer *base = c->layers().front().get();
        base->setName("Lineart");
        paint(base)->fillRect({1, 1, 4, 4}, {10, 20, 30, 255});
        CellLayer *g = c->addGroup("FX", base);
        CellLayer *glow = c->addLayer("Glow", nullptr);
        glow->setOpacity(0.5f);
        glow->setBlend(BlendMode::Screen);
        INK_CHECK(c->moveToGroup(glow, g));

        const fs::path dir = fs::temp_directory_path() / "inkframe_v2_test";
        fs::remove_all(dir);
        ProjectSaveResult r = saveProject(img, dir.string());
        INK_CHECK(r.ok);
        std::ifstream jf(dir / "project.json");
        std::stringstream ss;
        ss << jf.rdbuf();
        const std::string json = ss.str();
        INK_CHECK(json.find("\"version\":4") != std::string::npos);
        INK_CHECK(json.find("\"name\":\"Folder 1\"") != std::string::npos);
        INK_CHECK(json.find("\"name\":\"Track 1\"") != std::string::npos);
        INK_CHECK(json.find("\"frame\":4") != std::string::npos);
        INK_CHECK(json.find("\"spanEnd\":9") != std::string::npos);
        INK_CHECK(json.find("\"name\":\"Lineart\"") != std::string::npos);
        INK_CHECK(json.find("\"name\":\"Glow\"") != std::string::npos);
        INK_CHECK(json.find("\"blend\":2") != std::string::npos); // Screen
        // v3 tags every track with its kind, so an adjustment entry is
        // distinguishable from a paint layer without guessing.
        INK_CHECK(json.find("\"kind\":\"paint\"") != std::string::npos);
        // one raw per paint layer, named by track_key_path: the drawn layer
        // carries pixels (cropped to its extent), the blank one is empty
        INK_CHECK(fs::exists(dir / "cell_0_4_0.raw"));
        INK_CHECK(fs::exists(dir / "cell_0_4_1_0.raw"));
        INK_CHECK(fs::file_size(dir / "cell_0_4_0.raw") > 0);
        INK_CHECK(fs::file_size(dir / "cell_0_4_1_0.raw") == 0);
        fs::remove_all(dir);
    }

    // 15. groups really mean something: a lock or hide on a group reaches the
    //     layers inside it, and "add into group" lands on top of its contents
    {
        Cell c; // one paint layer
        CellLayer *base = c.layers().front().get();
        CellLayer *g = c.addGroup("G", nullptr);
        CellLayer *inner = c.addLayer("Inner", nullptr); // new layers start top level
        INK_CHECK(c.parentOf(inner) == &c.layers());
        INK_CHECK(c.moveToGroup(inner, g));
        INK_CHECK(g->children().size() == 1);
        INK_CHECK(c.parentOf(inner) == &g->children());  // really inside it
        CellLayer *nested = c.addGroup("Nested", nullptr);
        INK_CHECK(c.moveToGroup(nested, g));
        INK_CHECK(c.parentOf(nested) == &g->children());
        CellLayer *deep = c.addLayer("Deep", nullptr);
        INK_CHECK(c.moveToGroup(deep, nested));
        INK_CHECK(c.parentOf(deep) == &nested->children());

        INK_CHECK(!c.lockedThrough(inner));
        INK_CHECK(c.visibleThrough(inner));
        g->setLocked(true);
        INK_CHECK(c.lockedThrough(inner));   // the group's lock covers it
        INK_CHECK(c.lockedThrough(deep));    // two levels down as well
        INK_CHECK(!c.lockedThrough(base));   // other layers are unaffected
        g->setLocked(false);
        INK_CHECK(!c.lockedThrough(inner));
        g->setVisible(false);
        INK_CHECK(!c.visibleThrough(inner)); // the group's hide covers it
        INK_CHECK(!c.visibleThrough(deep));
        INK_CHECK(c.visibleThrough(base));
        g->setVisible(true);
        INK_CHECK(c.visibleThrough(deep));
        inner->setVisible(false);            // its own flag still counts
        INK_CHECK(!c.visibleThrough(inner));
        INK_CHECK(c.visibleThrough(g));

        // and a plain add stays independent: top level, on top of the stack
        CellLayer *fresh = c.addLayer("Fresh", nullptr);
        INK_CHECK(c.parentOf(fresh) == &c.layers());
        INK_CHECK(c.layers().back().get() == fresh);
        CellLayer *freshG = c.addGroup("FreshG", nullptr);
        INK_CHECK(c.parentOf(freshG) == &c.layers());
        INK_CHECK(freshG->children().empty());
    }

    std::printf("test_cell_layers OK\n");
    return 0;
}
