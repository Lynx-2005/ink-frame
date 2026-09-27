// Camera placement: layer cameras land directly below their target layer
// (not piled at the folder bottom), folder cameras paint on a row ABOVE
// their group (not as sub-rows inside it), and deleting a group deletes its
// folder cameras with it.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTimelineWidget.h"
#include <QApplication>
#include <cstdio>

using namespace ink;

namespace {
int rowIndex(const QStringList &order, const QString &id) {
    for (int i = 0; i < order.size(); ++i)
        if (order[i] == id) return i;
    return -1;
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Document doc(400, 300, 24);
    GroupLayer *f = doc.ensureFolder();
    PaintLayer *l1 = doc.currentLayer();
    PaintLayer *l2 = doc.createTrack(f, "Layer 2");
    PaintLayer *l3 = doc.createTrack(f, "Layer 3");
    (void)l3;

    TimelineDock dock(&doc);
    dock.resize(1100, 620);
    auto *grid = dock.grid();
    grid->resize(900, 560);
    dock.show();
    QApplication::processEvents();

    // 1. Layer camera for the FIRST layer lands directly below it.
    CameraLayer *lc = doc.createCameraForLayer(l1, {});
    INK_CHECK(lc != nullptr);
    {
        auto &ch = f->children();
        int li = -1, ci = -1;
        for (size_t i = 0; i < ch.size(); ++i) {
            if (ch[i].get() == static_cast<Layer*>(l1)) li = int(i);
            if (ch[i].get() == static_cast<Layer*>(lc)) ci = int(i);
        }
        INK_CHECK(li >= 0 && ci == li + 1);
    }
    {
        const QStringList order = grid->trackRowOrder();
        const QString lid = QString::fromStdString(l1->stableId());
        const QString cid = QString::fromStdString(lc->stableId());
        INK_CHECK(rowIndex(order, cid) == rowIndex(order, lid) + 1);
    }
    std::printf("layer camera below target OK\n");

    // 2. Layer camera for a MIDDLE layer slots in below it, not at bottom.
    CameraLayer *lc2 = doc.createCameraForLayer(l2, {});
    {
        const QStringList order = grid->trackRowOrder();
        const QString lid = QString::fromStdString(l2->stableId());
        const QString cid = QString::fromStdString(lc2->stableId());
        INK_CHECK(rowIndex(order, cid) == rowIndex(order, lid) + 1);
    }
    std::printf("middle layer camera slots in OK\n");

    // 3. Folder camera: first child in the tree, row ABOVE the group header.
    CameraLayer *fc = doc.createCameraForFolder(f, {});
    INK_CHECK(fc != nullptr);
    INK_CHECK(f->children().front().get() == static_cast<Layer*>(fc));
    {
        const QStringList order = grid->trackRowOrder();
        const QString cid = QString::fromStdString(fc->stableId());
        const QString hdr = QStringLiteral("folder:") + QString::fromStdString(f->stableId());
        const int ci = rowIndex(order, cid), hi = rowIndex(order, hdr);
        INK_CHECK(ci >= 0 && hi >= 0 && ci + 1 == hi);
    }
    std::printf("folder camera above group OK\n");

    // 4. Deleting the group deletes its folder camera with it.
    GroupLayer *g2 = doc.createFolder("Folder 2");
    const std::string fcId = fc->stableId();
    INK_CHECK(doc.deleteFolder(f));
    INK_CHECK(doc.findCamera(fcId) == nullptr);
    (void)g2;
    std::printf("folder delete drops its camera OK\n");

    std::printf("test_camera_placement PASS\n");
    return 0;
}
