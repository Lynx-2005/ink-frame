// Mandatory-folder model (CSP-style): tracks always live inside folders,
// TimelineModel mirrors folders, clips derive from holds.
#include "animation/InkTimelineModel.h"
#include "core/InkImage.h"
#include <cassert>
#include <iostream>

#define INK_CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

using namespace ink;

int main() {
    // 1. stable layer ids
    PaintLayer a("A"), b("B");
    INK_CHECK(a.stableId() != b.stableId());

    // 2. Image folder-first API
    Image img(64, 64, 12);
    GroupLayer *f1 = img.ensureFolder(); // auto-creates Folder 1
    INK_CHECK(f1 != nullptr);
    PaintLayer *t1 = img.createTrack(nullptr, "T1"); // null => first folder
    INK_CHECK(img.findFolderOf(t1) == f1);
    GroupLayer *f2 = img.createFolder("My Folder");
    PaintLayer *t2 = img.createTrack(f2, "T2");
    INK_CHECK(img.findFolderOf(t2) == f2);
    std::vector<GroupLayer*> folders;
    img.collectFolders(folders);
    INK_CHECK(folders.size() == 2);

    // 3. TimelineModel folders + cells
    TimelineModel tm;
    tm.setFrameCount(24);
    std::string fid = tm.addFolder("User Named"); // no hardcodes
    tm.addTrack(fid, "L1", "Track 1");
    tm.setKey("L1", 0);
    tm.setKey("L1", 10);
    auto cells = tm.cells("L1");
    INK_CHECK(cells.size() == 2);
    INK_CHECK(cells[0].start == 0 && cells[0].end == 9); // clipped by the next cell
    INK_CHECK(cells[1].start == 10 && cells[1].end == 23);
    INK_CHECK(tm.moveKey("L1", 10, 12));
    INK_CHECK(tm.row("L1")->frames.hasKey(12));
    INK_CHECK(!tm.row("L1")->frames.hasKey(10));

    // 4. legacy addLayer auto-creates folder (single-track => still foldered)
    TimelineModel tm2;
    tm2.addLayer("solo", "Solo");
    INK_CHECK(!tm2.folders().empty());
    INK_CHECK(tm2.row("solo") != nullptr);

    // 5. camera track keys
    tm.cameraKeys().setKey(0, 1.0f);
    tm.cameraKeys().setKey(12, 2.0f);
    INK_CHECK(tm.cameraKeys().keys().size() == 2);

    // 6. folder expand + move track across folders
    std::string fid2 = tm.addFolder("Second");
    INK_CHECK(tm.moveTrack("L1", fid2, 0));
    INK_CHECK(tm.folderOfTrack("L1") == fid2);
    tm.setFolderExpanded(fid2, false);
    INK_CHECK(!tm.folder(fid2)->expanded);

    std::cout << "test_folders OK\n";
    return 0;
}
