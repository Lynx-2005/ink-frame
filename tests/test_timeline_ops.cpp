// Krita-parity keyframe ops (headless): hold shift, mirror, device
// copy/move with collision policy, clip spans after edits.
#include "animation/InkKeyframe.h"
#include "animation/InkTimelineModel.h"
#include "core/InkImage.h"
#include <cassert>
#include <iostream>

#define INK_CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

using namespace ink;

int main() {
    // 1. RasterChannel hold extend/collapse
    {
        RasterChannel ch;
        ch.setKey(0); ch.setKey(10); ch.setKey(20);
        INK_CHECK(ch.shiftFrom(11, 4, 119) == 1); // only key 20 moves
        INK_CHECK(ch.hasKey(24) && !ch.hasKey(20));
        INK_CHECK(ch.shiftFrom(1, -5, 119) == 2); // keys 10,24 -> 5,19
        INK_CHECK(ch.hasKey(5) && ch.hasKey(19) && ch.hasKey(0));
        INK_CHECK(ch.shiftFrom(0, 200, 119) == 0); // pushed past max: dropped
        INK_CHECK(!ch.hasKey(0) && ch.keys().empty());
    }
    // 2. mirror
    {
        RasterChannel ch;
        ch.setKey(2); ch.setKey(6); ch.setKey(10);
        INK_CHECK(ch.mirrorRange(2, 10) == 3);
        INK_CHECK(ch.hasKey(2) && ch.hasKey(6) && ch.hasKey(10)); // symmetric
        ch.removeKey(6);
        INK_CHECK(ch.mirrorRange(2, 10) == 2);
        INK_CHECK(ch.hasKey(2) && ch.hasKey(10));
        INK_CHECK(ch.mirrorRange(5, 5) == 0); // degenerate
    }
    // 3. PaintLayer device copy/move with collision policy
    {
        PaintLayer l("L");
        l.setKeyAt(0, true);
        l.deviceAt(0, false)->setPixel(3, 3, {255, 0, 0, 255});
        l.copyDeviceContent(0, 5);
        INK_CHECK(l.hasKeyAt(5));
        auto px = l.deviceAt(5, false)->pixel(3, 3);
        INK_CHECK(px.r == 255 && px.a == 255);
        INK_CHECK(l.hasKeyAt(0)); // copy keeps source
        INK_CHECK(!l.moveDeviceContent(0, 5, false)); // collision, no overwrite
        INK_CHECK(l.hasKeyAt(0) && l.hasKeyAt(5));
        INK_CHECK(l.moveDeviceContent(0, 5, true)); // overwrite allowed
        INK_CHECK(!l.hasKeyAt(0) && l.hasKeyAt(5));
        INK_CHECK(l.moveDeviceContent(5, 5, false)); // self-move ok
    }
    // 4. Cells carry their own exposure: resizing one cell never resizes
    //    another, and moving a cell keeps the exposure length.
    {
        TimelineModel tm;
        tm.setFrameCount(30);
        std::string fid = tm.addFolder("F");
        tm.addTrack(fid, "T", "Track");
        tm.setKey("T", 0); tm.setKey("T", 8); tm.setKey("T", 9);
        auto cs = tm.cells("T");
        INK_CHECK(cs.size() == 3);
        INK_CHECK(cs[0].start == 0 && cs[0].end == 7); // clipped by the next key
        INK_CHECK(cs[1].start == 8 && cs[1].end == 8);
        INK_CHECK(cs[2].start == 9 && cs[2].end == 29);
        // shrink the last cell: the first two are untouched
        tm.setCellEnd("T", 9, 12);
        cs = tm.cells("T");
        INK_CHECK(cs[2].end == 12);
        INK_CHECK(cs[0].end == 7 && cs[1].start == 8 && cs[1].end == 8);
        // grow it again: still only this cell changes
        tm.setCellEnd("T", 9, 20);
        cs = tm.cells("T");
        INK_CHECK(cs[2].end == 20);
        INK_CHECK(cs[0].end == 7 && cs[1].end == 8);
        // growing cell 1 stops at its neighbour instead of swallowing it
        tm.setCellEnd("T", 0, 20);
        cs = tm.cells("T");
        INK_CHECK(cs[0].end == 7);
        INK_CHECK(cs[1].start == 8 && cs[1].end == 8);
        INK_CHECK(cs[2].end == 20);
        // move a cell: its length travels, and an OPEN neighbour simply fills
        // the space the moved cell left behind (explicit spans never change)
        INK_CHECK(tm.moveKey("T", 8, 12));
        cs = tm.cells("T");
        INK_CHECK(cs[0].end == 8); // open cell: clipped by the next key (9)
        INK_CHECK(cs[1].start == 9 && cs[1].end == 11);  // clipped by the moved key
        INK_CHECK(cs[2].start == 12 && cs[2].end == 12); // 1-frame cell kept its length
        // an explicitly resized cell ignores its neighbour's later moves
        tm.setCellEnd("T", 0, 4);
        INK_CHECK(tm.cellEnd("T", 0) == 4);
        INK_CHECK(tm.moveKey("T", 9, 14));
        INK_CHECK(tm.cellEnd("T", 0) == 4); // unchanged
    }
    // 5. folder expand + no camera row in the visible count
    {
        TimelineModel tm;
        std::string a = tm.addFolder("A"), b = tm.addFolder("B");
        tm.addTrack(a, "t1", "one");
        tm.addTrack(b, "t2", "two");
        INK_CHECK(tm.visibleRowCount() == 2 + 2); // 2 headers + 2 tracks (no camera)
        tm.setFolderExpanded(a, false);
        INK_CHECK(tm.visibleRowCount() == 2 + 1);
        tm.cameraKeys().setKey(0, 1.0f);
        INK_CHECK(tm.cameraKeys().hasKey(0));
    }
    // 6. cells: ordinal numbering, gaps, empty track = no cell at all
    {
        TimelineModel tm;
        tm.setFrameCount(30);
        std::string fid = tm.addFolder("F");
        tm.addTrack(fid, "T", "Track");
        INK_CHECK(tm.cells("T").empty()); // empty track
        INK_CHECK(tm.cellCount("T") == 0);
        INK_CHECK(tm.cellIndexAt("T", 5) == -1);
        tm.setKey("T", 4);
        tm.setKey("T", 10);
        tm.setCellEnd("T", 4, 6);   // exposed 4..6, then a gap at 7..9
        tm.setKey("T", 11);
        auto cs = tm.cells("T");
        INK_CHECK(cs.size() == 3);
        INK_CHECK(cs[0].index == 0 && cs[0].start == 4 && cs[0].end == 6);
        INK_CHECK(cs[1].index == 1 && cs[1].start == 10 && cs[1].end == 10);
        INK_CHECK(cs[2].index == 2 && cs[2].start == 11 && cs[2].end == 29);
        INK_CHECK(tm.cellCount("T") == 3);
        INK_CHECK(tm.cellIndexAt("T", 3) == -1); // before the first cell
        INK_CHECK(tm.cellIndexAt("T", 4) == 0);
        INK_CHECK(tm.cellIndexAt("T", 6) == 0);  // last exposed frame
        INK_CHECK(tm.cellIndexAt("T", 7) == -1); // gap: nothing exposed
        INK_CHECK(tm.cellIndexAt("T", 10) == 1);
        INK_CHECK(tm.cellIndexAt("T", 12) == 2); // inside the last cell's hold
        INK_CHECK(tm.cellStartAt("T", 8) == -1);
        INK_CHECK(tm.cellLength("T", 4) == 3);
        INK_CHECK(tm.cellLength("T", 11) == 19);
        // "to the end" is stored as an open span, not a frozen number
        tm.setCellEnd("T", 10, -1);
        INK_CHECK(tm.cells("T")[1].end == 10); // still clipped by key 11
        tm.setFrameCount(60);
        INK_CHECK(tm.cells("T")[2].end == 59); // open span follows the length
    }
    std::cout << "test_timeline_ops OK\n";
    return 0;
}
