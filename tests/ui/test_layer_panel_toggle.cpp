// The layer panel toggle must take effect on EVERY press.
//
// Two checkable controls drive one panel: the top-bar three-dot button and the
// F7 QAction. The action re-emits `toggled` when setChecked() changes it, so
// mirroring into it without blocking signals re-enters the setter and lets the
// button and the action drift off the panel's real visibility. That drift is
// what made the panel need two or three presses to collapse, and left it open
// at startup with an unchecked button.
//
// So this drives the real controls and asserts the invariant directly: the
// panel's visibility STRICTLY ALTERNATES with each press, and the two controls
// always agree with it.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/InkLayerPanel.h"
#include "ui/InkMainWindow.h"
#include "ui/InkTopBar.h"
#include <QAction>
#include <QApplication>
#include <QToolButton>
#include <cstdio>
using namespace ink;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(1600, 900);
    w.show();
    QApplication::processEvents();

    LayerPanel *lp = w.findChild<LayerPanel *>();
    INK_CHECK(lp != nullptr);
    if (!lp) {
        std::printf("no LayerPanel in the window\n");
        return 1;
    }
    QAction *action = nullptr;
    for (QAction *a : w.actions())
        if (a->shortcut() == QKeySequence(QStringLiteral("F7"))) action = a;
    INK_CHECK(action != nullptr);
    if (!action) return 1;
    QToolButton *btn = nullptr;
    for (QToolButton *b : w.findChildren<QToolButton *>())
        if (b->toolTip().startsWith(QLatin1String("Layer panel"))) btn = b;
    INK_CHECK(btn != nullptr);
    if (!btn) return 1;

    // The panel is on screen at startup, so both controls must SAY so. An
    // unchecked button beside a visible panel is the reported symptom.
    INK_CHECK(lp->isVisible());
    INK_CHECK(action->isChecked());
    INK_CHECK(btn->isChecked());
    std::printf("startup: panel visible, action and button both checked OK\n");

    // Every press flips exactly one thing, and the controls follow.
    for (int press = 1; press <= 6; ++press) {
        const bool before = lp->isVisible();
        action->setChecked(!action->isChecked());
        QApplication::processEvents();
        const bool after = lp->isVisible();
        if (after == before)
            std::printf("   press %d did NOT toggle the panel (still %s)\n", press,
                        after ? "open" : "closed");
        INK_CHECK(after != before);
        INK_CHECK(action->isChecked() == after);
        INK_CHECK(btn->isChecked() == after);
    }
    std::printf("six F7 presses each toggled the panel exactly once OK\n");

    // And the same from the top-bar button, which is the other control.
    for (int press = 1; press <= 4; ++press) {
        const bool before = lp->isVisible();
        btn->setChecked(!btn->isChecked());
        QApplication::processEvents();
        const bool after = lp->isVisible();
        if (after == before)
            std::printf("   button press %d did NOT toggle the panel\n", press);
        INK_CHECK(after != before);
        INK_CHECK(action->isChecked() == after);
        INK_CHECK(btn->isChecked() == after);
    }
    std::printf("four button presses each toggled the panel exactly once OK\n");

    std::printf("test_layer_panel_toggle PASS\n");
    return 0;
}
