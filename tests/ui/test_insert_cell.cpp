// Inserting a cell between two existing cells (the timeline gap that could not
// be created before): the new blank cell takes the target frame and every key
// at/after it ripples one frame right, content and exposure travelling together.
// The op must never split a hold into a gap and never drop the last key.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include <QApplication>

using namespace ink;

namespace {

// One cell: a key at `start` exposed for `len` frames.
void cell(Document &doc, PaintLayer *l, int start, int len) {
    doc.addBlankKey(l, start);
    doc.setCellLength(l, start, len);
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // 1. insert between two contiguous cells: cell 3 ripples right with its
    //    length, cell 2 keeps its exposure, the new cell is one frame long.
    {
        Document doc(64, 64, 12);
        doc.setFrameCount(30);
        PaintLayer *l = doc.currentLayer();
        cell(doc, l, 0, 5);   // cell 1: 0-4
        cell(doc, l, 5, 5);   // cell 2: 5-9
        cell(doc, l, 10, 5);  // cell 3: 10-14

        INK_CHECK(doc.insertCell(l, 10)); // between cell 2 and cell 3
        INK_CHECK(l->hasKeyAt(10) && l->hasKeyAt(11));
        INK_CHECK(doc.cellEnd(l, 10) == 10);       // new cell: 1 frame
        INK_CHECK(doc.cellEnd(l, 5) == 9);         // cell 2 untouched
        INK_CHECK(l->hasKeyAt(0) && doc.cellEnd(l, 0) == 4);
        INK_CHECK(doc.cellEnd(l, 11) == 15);       // cell 3 moved, length kept
        INK_CHECK(doc.cellLength(l, 11) == 5);
        INK_CHECK(doc.frameCount() == 30);         // room was there, no growth
        INK_CHECK(doc.cellStartAt(l, 10) == 10 && doc.cellStartAt(l, 11) == 11);
    }

    // 2. a frame inside a hold means "after that cell": the hold is never split
    //    into a blank gap.
    {
        Document doc(64, 64, 12);
        doc.setFrameCount(20);
        PaintLayer *l = doc.currentLayer();
        cell(doc, l, 0, 10);  // 0-9
        cell(doc, l, 10, 5);  // 10-14

        INK_CHECK(doc.insertCell(l, 4)); // mid-hold
        INK_CHECK(doc.cellStartAt(l, 4) == 0);     // cell 1 still covers frame 4
        INK_CHECK(doc.cellEnd(l, 0) == 9);
        INK_CHECK(l->hasKeyAt(10) && doc.cellEnd(l, 10) == 10); // new cell after it
        INK_CHECK(l->hasKeyAt(11) && doc.cellEnd(l, 11) == 15); // old cell 2 rippled
    }

    // 3. insert at the project end: the project grows so no key is dropped.
    {
        Document doc(64, 64, 12);
        doc.setFrameCount(10); // frames 0-9
        PaintLayer *l = doc.currentLayer();
        cell(doc, l, 0, 1);
        cell(doc, l, 9, 1); // last frame

        INK_CHECK(doc.insertCell(l, 10)); // after the last cell
        INK_CHECK(doc.frameCount() == 11);
        INK_CHECK(l->hasKeyAt(9) && l->hasKeyAt(10)); // nothing dropped
        INK_CHECK(doc.cellEnd(l, 10) == 10);
    }

    // 4. insert before a key sitting on the last frame: it moves, project grows.
    {
        Document doc(64, 64, 12);
        doc.setFrameCount(10);
        PaintLayer *l = doc.currentLayer();
        cell(doc, l, 9, 1);

        INK_CHECK(doc.insertCell(l, 9));
        INK_CHECK(doc.frameCount() == 11);
        INK_CHECK(l->hasKeyAt(9) && l->hasKeyAt(10));
        INK_CHECK(doc.cellEnd(l, 9) == 9 && doc.cellEnd(l, 10) == 10);
    }

    // 5. insert on a free frame still ripples the cells after it.
    {
        Document doc(64, 64, 12);
        doc.setFrameCount(20);
        PaintLayer *l = doc.currentLayer();
        cell(doc, l, 0, 2); // 0-1
        cell(doc, l, 8, 2); // 8-9

        INK_CHECK(doc.insertCell(l, 4)); // free frame in the gap
        INK_CHECK(l->hasKeyAt(4) && doc.cellEnd(l, 4) == 4);
        INK_CHECK(l->hasKeyAt(9) && doc.cellEnd(l, 9) == 10); // rippled, length kept
        INK_CHECK(doc.cellEnd(l, 0) == 1);
    }

    // 6. empty layer: a plain new cell, no ripple, no project growth.
    {
        Document doc(64, 64, 12);
        doc.setFrameCount(12);
        PaintLayer *l = doc.currentLayer();

        INK_CHECK(doc.insertCell(l, 5));
        INK_CHECK(l->hasKeyAt(5) && doc.cellEnd(l, 5) == 5);
        INK_CHECK(doc.frameCount() == 12);
        INK_CHECK(!doc.insertCell(nullptr, 5));
    }

    std::printf("test_insert_cell OK\n");
    return 0;
}
