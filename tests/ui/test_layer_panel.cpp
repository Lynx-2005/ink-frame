// The right-side Layer panel and the per-cell stack it drives (offscreen).
// The panel is a view over Document's cell-layer API, so this drives the same
// calls the widget makes and checks the model, the paint target, undo, and the
// per-cell isolation that the whole feature is about.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/InkLayerPanel.h"
#include "ui/InkTheme.h"
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSlider>
#include <QToolButton>
#include <cstdio>

using namespace ink;

namespace {

// One 1-frame cell on the current track at `frame`.
void cell(Document &doc, PaintLayer *l, int frame) {
    doc.addBlankKey(l, frame);
    doc.setCellLength(l, frame, 1);
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // 1. the panel's model path: + adds a layer to the CURRENT cell and it
    //    becomes the paint target; the panel reports a change.
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        int changes = 0;
        QObject::connect(&panel, &LayerPanel::cellLayersChanged, [&changes] { ++changes; });

        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        panel.refresh();
        INK_CHECK(doc.currentCell() != nullptr);
        INK_CHECK(doc.currentCell()->layers().size() == 1);

        CellLayer *added = doc.addCellLayer({}, nullptr);
        INK_CHECK(added != nullptr);
        INK_CHECK(doc.currentCell()->layers().size() == 2);
        INK_CHECK(doc.currentCellLayer() == added); // new layer is the paint target
        INK_CHECK(doc.editableDevice(t, 0) == added->device());
        panel.refresh();
        INK_CHECK(changes == 0); // refresh alone does not emit
    }

    // 2. per-cell isolation: every cell owns its own stack
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        cell(doc, t, 10);
        doc.setCurrentFrame(0);
        doc.addCellLayer({}, nullptr);
        INK_CHECK(doc.currentCell()->layers().size() == 2);

        doc.setCurrentFrame(10);
        panel.refresh();
        INK_CHECK(doc.currentCell() != nullptr);
        INK_CHECK(doc.currentCell()->layers().size() == 1); // its own, untouched stack
        INK_CHECK(doc.currentCellKey() == 10);
    }

    // 3. property ops reach the engine (visibility changes the projection,
    //    opacity/blend/rename land on the layer, lock blocks painting)
    {
        Document doc(64, 64, 12);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *l = doc.currentCellLayer();
        l->device()->fillRect({0, 0, 8, 8}, {255, 0, 0, 255});

        PaintDevice out;
        doc.image().project(0, out);
        INK_CHECK(!out.extent().isEmpty());
        INK_CHECK(doc.setCellLayerVisible(l, false));
        doc.frameCache().invalidateAll();
        doc.image().project(0, out);
        INK_CHECK(out.extent().isEmpty()); // hidden layer leaves nothing

        INK_CHECK(doc.setCellLayerVisible(l, true));
        INK_CHECK(doc.setCellLayerOpacity(l, 0.5f));
        INK_CHECK(l->opacity() > 0.49f && l->opacity() < 0.51f);
        INK_CHECK(doc.setCellLayerBlend(l, BlendMode::Multiply));
        INK_CHECK(l->blend() == BlendMode::Multiply);
        INK_CHECK(doc.renameCellLayer(l, "Lineart"));
        INK_CHECK(l->name() == "Lineart");
        INK_CHECK(!doc.renameCellLayer(l, "")); // empty name is refused

        INK_CHECK(doc.setCellLayerLocked(l, true));
        INK_CHECK(doc.editableDevice(t, 0) == nullptr); // locked: nothing to paint into
        INK_CHECK(doc.paintBlockedByLock());
        INK_CHECK(doc.setCellLayerLocked(l, false));
        INK_CHECK(doc.editableDevice(t, 0) == l->device());
    }

    // 4. delete refuses the last paint layer; undo brings a deleted layer back
    //    with its pixels
    {
        Document doc(64, 64, 12);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *keep = doc.currentCellLayer();
        INK_CHECK(!doc.deleteCellLayer(keep)); // last paint layer stays
        INK_CHECK(doc.currentCell()->layers().size() == 1);

        CellLayer *extra = doc.addCellLayer("Extra", keep);
        extra->device()->fillRect({2, 2, 3, 3}, {0, 0, 255, 255});
        const uint64_t extraId = extra->id();
        INK_CHECK(doc.deleteCellLayer(extra));
        INK_CHECK(doc.currentCell()->find(extraId) == nullptr);

        doc.undo().undo();
        CellLayer *back = doc.currentCell()->find(extraId);
        INK_CHECK(back != nullptr);
        INK_CHECK(back->hasContent()); // pixels survived the round trip
        INK_CHECK(back->name() == "Extra");
        doc.undo().redo();
        INK_CHECK(doc.currentCell()->find(extraId) == nullptr);
        doc.undo().undo();
        INK_CHECK(doc.currentCell()->find(extraId) != nullptr);
    }

    // 5. grouping / ungrouping / reordering through the model path
    {
        Document doc(64, 64, 12);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        CellLayer *c = doc.addCellLayer("C", b);
        INK_CHECK(doc.currentCell()->layers().size() == 3);

        CellLayer *g = doc.groupCellLayers({b, c});
        INK_CHECK(g != nullptr && !g->isPaint());
        INK_CHECK(doc.currentCell()->layers().size() == 2);
        INK_CHECK(g->children().size() == 2);
        INK_CHECK(!doc.groupCellLayers({a})); // one layer is not a group

        INK_CHECK(doc.moveCellLayer(g, -1)); // toward the back
        INK_CHECK(doc.currentCell()->layers().front().get() == g);
        INK_CHECK(doc.ungroupCellLayer(g));
        INK_CHECK(doc.currentCell()->layers().size() == 3);
        INK_CHECK(!doc.ungroupCellLayer(a)); // a paint layer is not a group

        // drag-style drop: move C to the very back
        INK_CHECK(doc.moveCellLayerTo(c, nullptr, 0));
        INK_CHECK(doc.currentCell()->layers().front().get() == c);
        // ...and undo puts it back
        doc.undo().undo();
        INK_CHECK(doc.currentCell()->layers().front().get() != c);
        doc.undo().redo();
        INK_CHECK(doc.currentCell()->layers().front().get() == c);
    }

    // 6. drawing inside a hold edits that cell's stack (no surprise new cell),
    //    and auto-key creates the stack only in a gap
    {
        Document doc(64, 64, 12);
        PaintLayer *t = doc.currentLayer();
        doc.addBlankKey(t, 0);
        doc.setCellLength(t, 0, 6); // 0..5, one drawing
        doc.setCurrentFrame(3);
        INK_CHECK(doc.currentCellKey() == 0);   // the hold exposes cell 0
        INK_CHECK(doc.currentCell() != nullptr);
        CellLayer *l = doc.currentCellLayer();
        l->device()->setPixel(1, 1, {255, 0, 0, 255});
        INK_CHECK(t->keys().size() == 1);       // still one cell
        INK_CHECK(doc.currentCell()->hasContent());

        doc.setCurrentFrame(7);                 // a gap: auto-key makes a cell
        INK_CHECK(doc.currentCellKey() == -1);
        INK_CHECK(doc.currentCellOrCreate() != nullptr);
        INK_CHECK(t->hasKeyAt(7));
        INK_CHECK(t->keys().size() == 2);
        // ...and the new cell has its own, empty stack
        INK_CHECK(!doc.currentCell()->hasContent());
        INK_CHECK(doc.currentCell()->layers().size() == 1);
    }

    // 7. auto-key off: no cell, no stack, and the panel says why
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        doc.setAutoKeyEnabled(false);
        doc.setCurrentFrame(2);
        INK_CHECK(doc.currentCell() == nullptr);
        INK_CHECK(doc.currentCellOrCreate() == nullptr);
        INK_CHECK(doc.addCellLayer({}, nullptr) == nullptr);
        INK_CHECK(doc.editableDevice(doc.currentLayer(), 2) == nullptr);
        panel.refresh(); // must not crash with no cell
        INK_CHECK(panel.listWidget() != nullptr);
        INK_CHECK(panel.listWidget()->sizeHint().height() > 0);
    }

    // 8. the panel builds one row per layer (and follows the cell it shows)
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        doc.addCellLayer("B", a);
        CellLayer *g = doc.addCellGroup("G", nullptr);
        INK_CHECK(g != nullptr);
        panel.refresh();
        // 3 rows (2 paint + 1 group), each at track/folder height
        INK_CHECK(panel.listWidget()->sizeHint().height() ==
                  InkTheme::trackH() * 2 + InkTheme::folderH());
    }

    // 9. the panel's own controls exist and are wired (opacity slider + blend)
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        panel.refresh();
        auto *slider = panel.findChild<QSlider *>();
        auto *blend = panel.findChild<QComboBox *>();
        INK_CHECK(slider != nullptr);
        INK_CHECK(blend != nullptr);
        INK_CHECK(blend->count() == 4);
        CellLayer *l = doc.currentCellLayer();
        slider->setValue(40); // drives Document::setCellLayerOpacity
        INK_CHECK(l->opacity() > 0.39f && l->opacity() < 0.41f);
        blend->setCurrentIndex(2); // Screen
        INK_CHECK(l->blend() == BlendMode::Screen);
        panel.refresh();
        INK_CHECK(slider->value() == 40); // read back from the model
    }

    // 10. the real click path: pressing a row makes that layer the paint target.
    //     (Rows are front-most first, so row 0 is the topmost layer.)
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        panel.resize(240, 400);
        panel.show();
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        panel.refresh();

        QWidget *list = panel.listWidget();
        INK_CHECK(list != nullptr);
        list->resize(240, 400);
        auto clickRow = [&](int row) {
            const QPoint p(60, row * InkTheme::trackH() + InkTheme::trackH() / 2);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(list->mapToGlobal(p)),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
        };
        INK_CHECK(doc.currentCellLayer() == b); // the newly added layer is the target
        clickRow(1);                            // row 1 = the layer below = A
        INK_CHECK(doc.currentCellLayer() == a);
        clickRow(0);                            // back to the top row = B
        INK_CHECK(doc.currentCellLayer() == b);
        // and the target is what tools paint into
        INK_CHECK(doc.editableDevice(t, 0) == b->device());
    }

    // 11. the selection checkbox: tick two layers, group them. This is the
    //     control the multi-select (grouping/moving/deleting) runs on.
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        panel.resize(240, 400);
        panel.show();
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        panel.refresh();

        QWidget *list = panel.listWidget();
        list->resize(240, 400);
        auto clickAt = [&](int x, int row) {
            const QPoint p(x, row * InkTheme::trackH() + InkTheme::trackH() / 2);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(list->mapToGlobal(p)),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
        };
        const int cbX = 24 + 7; // checkbox column centre

        QToolButton *groupBtn = nullptr;
        for (auto *btn : panel.findChildren<QToolButton *>())
            if (btn->toolTip().contains(QStringLiteral("Group selected"))) groupBtn = btn;
        INK_CHECK(groupBtn != nullptr);
        INK_CHECK(!groupBtn->isEnabled()); // nothing ticked yet

        clickAt(cbX, 0);                   // tick the top row (B)
        INK_CHECK(doc.currentCellLayer() == b);
        clickAt(cbX, 1);                   // tick the row below (A)
        INK_CHECK(doc.currentCellLayer() == a);
        INK_CHECK(doc.currentCell()->layers().size() == 2); // ticking is not a group
        INK_CHECK(groupBtn->isEnabled());  // two ticked -> grouping is available

        groupBtn->click();                 // group the ticked layers
        INK_CHECK(doc.currentCell()->layers().size() == 1);
        CellLayer *g = doc.currentCell()->layers().front().get();
        INK_CHECK(!g->isPaint());
        INK_CHECK(g->children().size() == 2); // both ticked layers moved in
        doc.undoEdit();                       // and the whole thing undoes
        INK_CHECK(doc.currentCell()->layers().size() == 2);
    }

    // 12. Delete acts on the ticked set (and the last paint layer survives)
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        panel.resize(240, 400);
        panel.show();
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        CellLayer *c2 = doc.addCellLayer("C", b);
        panel.refresh();

        QWidget *list = panel.listWidget();
        list->resize(240, 400);
        auto clickAt = [&](int x, int row) {
            const QPoint p(x, row * InkTheme::trackH() + InkTheme::trackH() / 2);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(list->mapToGlobal(p)),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
        };
        const int cbX = 24 + 7;
        clickAt(cbX, 0); // tick C
        clickAt(cbX, 1); // tick B
        INK_CHECK(doc.currentCell()->layers().size() == 3);
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(list, &del);
        INK_CHECK(doc.currentCell()->layers().size() == 1); // both ticked gone
        INK_CHECK(doc.currentCell()->layers().front().get() == a);
        doc.undoEdit();
        doc.undoEdit();
        INK_CHECK(doc.currentCell()->layers().size() == 3); // both undos restore
        (void)c2;
    }

    // 13. right-clicking a ticked layer keeps the ticked set (it used to
    //     collapse the selection, which made grouping impossible), and the menu
    //     built for it offers to group the whole set
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        auto *view = qobject_cast<LayerListView *>(panel.listWidget());
        INK_CHECK(view != nullptr);
        panel.resize(240, 400);
        panel.show();
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        CellLayer *c2 = doc.addCellLayer("C", b);
        panel.refresh();
        view->resize(240, 400);

        auto clickAt = [&](int x, int row) {
            const QPoint p(x, row * InkTheme::trackH() + InkTheme::trackH() / 2);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(view->mapToGlobal(p)),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &press);
        };
        auto rightClick = [&](int row) {
            const QPoint p(100, row * InkTheme::trackH() + InkTheme::trackH() / 2);
            QContextMenuEvent ev(QContextMenuEvent::Mouse, p, view->mapToGlobal(p));
            QApplication::sendEvent(view, &ev);
        };
        const int cbX = 24 + 7;
        clickAt(cbX, 0); // tick C (top row)
        clickAt(cbX, 1); // tick B
        INK_CHECK(doc.currentCellLayer() == b);
        // only the modal popup is cut out; everything else stays wired
        QObject::disconnect(view, &LayerListView::menuRequested, &panel, nullptr);
        int selChanges = 0;
        QObject::connect(view, &LayerListView::selectRequested, [&] { ++selChanges; });
        rightClick(0); // on a ticked row
        INK_CHECK(selChanges == 0);
        rightClick(1); // on the other ticked row
        INK_CHECK(selChanges == 0);
        rightClick(0);
        INK_CHECK(selChanges == 0);
        rightClick(2); // an UNticked row does become the selection
        INK_CHECK(selChanges == 1);
        INK_CHECK(doc.currentCellLayer() == a);

        // the menu a ticked row offers groups the whole set, not just one layer
        clickAt(cbX, 2); // untick A again (that right-click had selected it)
        clickAt(cbX, 0); // tick C
        clickAt(cbX, 1); // tick B
        QMenu *menu = panel.buildContextMenu(c2->id());
        QAction *groupAct = nullptr;
        for (QAction *act : menu->actions())
            if (act->text().startsWith(QStringLiteral("Group 2 selected"))) groupAct = act;
        INK_CHECK(groupAct != nullptr);
        groupAct->trigger();
        menu->deleteLater();
        INK_CHECK(doc.currentCell()->layers().size() == 2); // A + the new group
        CellLayer *g = doc.currentCell()->layers().back().get();
        INK_CHECK(!g->isPaint() && g->children().size() == 2); // both ticked layers
        doc.undoEdit();
        INK_CHECK(doc.currentCell()->layers().size() == 3);
    }

    // 15. new layers stay independent of groups: "+" always adds at the TOP of
    //     the stack on the top level, even with a group (or a layer in one)
    //     selected, and it undoes
    {
        Document doc(64, 64, 12);
        LayerPanel panel(&doc);
        panel.resize(240, 400);
        panel.show();
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        Cell *c = doc.currentCell();
        CellLayer *g = doc.groupCellLayers({a, b});
        INK_CHECK(g != nullptr && c->layers().size() == 1);
        panel.refresh();

        QWidget *list = panel.listWidget();
        list->resize(240, 400);
        auto clickRow = [&](int row) {
            const QPoint p(100, row * InkTheme::trackH() + InkTheme::trackH() / 2);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(list->mapToGlobal(p)),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
        };
        QToolButton *addBtn = nullptr;
        for (auto *btn : panel.findChildren<QToolButton *>())
            if (btn->toolTip().contains(QStringLiteral("Add a layer or a group"))) addBtn = btn;
        INK_CHECK(addBtn != nullptr && addBtn->menu() != nullptr);
        auto menuNew = [&](int index) { addBtn->menu()->actions()[index]->trigger(); };

        clickRow(0); // select the group itself
        menuNew(0);  // "New layer"
        INK_CHECK(c->layers().size() == 2);       // a new TOP-LEVEL layer
        INK_CHECK(g->children().size() == 2);     // the group is untouched
        CellLayer *added = c->layers().back().get();
        INK_CHECK(added->isPaint());
        INK_CHECK(c->parentOf(added) == &c->layers());
        INK_CHECK(added == doc.currentCellLayer()); // and it becomes the target
        doc.undoEdit();
        INK_CHECK(c->layers().size() == 1);

        // a layer inside the group is not an insertion point either
        clickRow(1); // row 1 = B, inside the group
        INK_CHECK(doc.currentCellLayer() == b);
        menuNew(0);
        INK_CHECK(c->layers().size() == 2);
        INK_CHECK(g->children().size() == 2);
        INK_CHECK(c->parentOf(c->layers().back().get()) == &c->layers());
        doc.undoEdit();
        INK_CHECK(c->layers().size() == 1);

        // "New group" behaves the same way
        clickRow(0);
        menuNew(1);
        INK_CHECK(c->layers().size() == 2);
        INK_CHECK(!c->layers().back()->isPaint());
        INK_CHECK(c->layers().back()->children().empty());
        doc.undoEdit();
        INK_CHECK(c->layers().size() == 1);
    }

    // 16. a locked group blocks painting
    {
        Document doc(64, 64, 12);
        PaintLayer *t = doc.currentLayer();
        cell(doc, t, 0);
        doc.setCurrentFrame(0);
        CellLayer *a = doc.currentCellLayer();
        CellLayer *b = doc.addCellLayer("B", a);
        CellLayer *g = doc.groupCellLayers({a, b});
        INK_CHECK(g != nullptr);
        doc.setCellLayerActive(b);
        INK_CHECK(doc.editableDevice(t, 0) == b->device());
        doc.setCellLayerLocked(g, true);                // lock the GROUP
        INK_CHECK(doc.editableDevice(t, 0) == nullptr); // painting is blocked
        doc.setCellLayerLocked(g, false);
        INK_CHECK(doc.editableDevice(t, 0) == b->device());
    }

    std::printf("test_layer_panel OK\n");
    return 0;
}
