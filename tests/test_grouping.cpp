// Layer-tree structure ops (headless): grouping selected tracks into a new
// folder, dissolving a folder back into its neighbour, moving a track across
// folders. These back the timeline gutter's Group / Ungroup buttons.
#include "core/InkImage.h"
#include <algorithm>
#include <iostream>

#define INK_CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

using namespace ink;

int main() {
    // 1. groupTracks: members from different folders land in ONE new folder, in
    //    tree order; source folders keep their other tracks; the folder count
    //    grows by one; the new folder sits where the layers came from.
    {
        Image img(64, 64, 12);
        GroupLayer *fa = img.createFolder("A");
        GroupLayer *fb = img.createFolder("B");
        PaintLayer *a1 = img.createTrack(fa, "a1");
        PaintLayer *a2 = img.createTrack(fa, "a2");
        PaintLayer *b1 = img.createTrack(fb, "b1");
        GroupLayer *g = img.groupTracks({b1, a1}, "Grouped");
        INK_CHECK(g != nullptr);
        INK_CHECK(g->name() == "Grouped");
        INK_CHECK(g->children().size() == 2);
        INK_CHECK(g->children()[0].get() == static_cast<Layer*>(a1)); // tree order
        INK_CHECK(g->children()[1].get() == static_cast<Layer*>(b1));
        INK_CHECK(img.findFolderOf(a1) == g);
        INK_CHECK(img.findFolderOf(b1) == g);
        INK_CHECK(img.findFolderOf(a2) == fa); // untouched
        std::vector<GroupLayer*> folders;
        img.collectFolders(folders);
        INK_CHECK(folders.size() == 3);
        INK_CHECK(folders[0] == fa && folders[1] == g && folders[2] == fb);
        INK_CHECK(img.groupTracks({}, "Empty") == nullptr);
        // all tracks still resolvable in tree order
        std::vector<PaintLayer*> all;
        img.collectPaintLayers(all);
        INK_CHECK(all.size() == 3);
    }
    // 2. ungroupFolder: children fold into the neighbouring folder; the last
    //    folder refuses to dissolve (folders are mandatory).
    {
        Image img(64, 64, 12);
        GroupLayer *fa = img.createFolder("A");
        img.createTrack(fa, "a1");
        GroupLayer *fb = img.createFolder("B");
        PaintLayer *b1 = img.createTrack(fb, "b1");
        PaintLayer *b2 = img.createTrack(fb, "b2");
        INK_CHECK(!img.ungroupFolder(nullptr));
        INK_CHECK(img.ungroupFolder(fb));
        std::vector<GroupLayer*> folders;
        img.collectFolders(folders);
        INK_CHECK(folders.size() == 1 && folders[0] == fa);
        INK_CHECK(img.findFolderOf(b1) == fa);
        INK_CHECK(img.findFolderOf(b2) == fa);
        INK_CHECK(fa->children().size() == 3);
        INK_CHECK(!img.ungroupFolder(fa)); // must keep at least one folder
        INK_CHECK(fa->children().size() == 3);
    }
    // 3. moveTrack: cross-folder move at an index, append, and same-folder no-op
    {
        Image img(64, 64, 12);
        GroupLayer *fa = img.createFolder("A");
        GroupLayer *fb = img.createFolder("B");
        PaintLayer *a1 = img.createTrack(fa, "a1");
        PaintLayer *a2 = img.createTrack(fa, "a2");
        PaintLayer *b1 = img.createTrack(fb, "b1");
        INK_CHECK(img.moveTrack(a2, fb, 0));
        INK_CHECK(img.findFolderOf(a2) == fb);
        INK_CHECK(fb->children()[0].get() == static_cast<Layer*>(a2));
        INK_CHECK(fb->children()[1].get() == static_cast<Layer*>(b1));
        INK_CHECK(fa->children().size() == 1);
        INK_CHECK(img.moveTrack(a1, fb, -1)); // append
        INK_CHECK(fb->children().back().get() == static_cast<Layer*>(a1));
        INK_CHECK(img.moveTrack(a1, fb)); // same folder: success, no move
        INK_CHECK(!img.moveTrack(nullptr, fb));
    }
    // 4. group -> ungroup round trip keeps every track and its keys
    {
        Image img(64, 64, 12);
        GroupLayer *fa = img.createFolder("A");
        PaintLayer *a1 = img.createTrack(fa, "a1");
        PaintLayer *a2 = img.createTrack(fa, "a2");
        a1->setKeyAt(0, true);
        a1->setKeyAt(6, true);
        a2->setKeyAt(3, true);
        GroupLayer *g = img.groupTracks({a1}, "Wrap");
        INK_CHECK(g && img.ungroupFolder(g));
        INK_CHECK(img.findFolderOf(a1) == fa);
        INK_CHECK(a1->keys().size() == 2);
        INK_CHECK(a2->keys().size() == 1);
        std::vector<GroupLayer*> folders;
        img.collectFolders(folders);
        INK_CHECK(folders.size() == 1);
    }
    std::cout << "test_grouping OK\n";
    return 0;
}
