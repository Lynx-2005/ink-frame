#pragma once
// Undo + stroke queue. Production-grade serialisation:
// - strokes execute serially on worker (no torn tiles)
// - each stroke = begin/record/commit; undo = restore mementos LIFO
// - coalesces per-tile first-touch so undo is O(dirty tiles)
#include "core/InkTileStore.h"
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ink {

class PaintDevice;

struct StrokeCommand {
    std::string name;
    // device -> memento captured during stroke
    struct Target { PaintDevice *device; TileMemento memento; };
    std::vector<Target> targets;
    // After-image for exact redo: per-tile snapshots taken at stroke end.
    // Applied when redoFn is empty; redoFn (job replay) takes precedence.
    struct AfterTile { PaintDevice *device; TilePos pos; TileShared tile; };
    std::vector<AfterTile> after;
    // redo payload: re-apply function stored at commit
    std::function<void()> redoFn;
    std::function<void()> undoFnExtra; // e.g. key add/remove
};

class UndoStack {
public:
    explicit UndoStack(size_t limit = 100) : limit_(limit) {}
    void push(StrokeCommand cmd);
    // Structural action with no pixel mementos: undo/redo run the two
    // closures. Used by layer-tree edits (add/remove/reorder/group/rename/
    // opacity/blend) so Ctrl+Z covers them like a stroke does.
    void pushAction(std::string name, std::function<void()> redo, std::function<void()> undo);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    const std::string &undoName() const;
    const std::string &redoName() const;
    void undo();
    void redo();
    void clear();
    size_t undoDepth() const { return undo_.size(); }
private:
    size_t limit_;
    // Deque: dropping the oldest entry is O(1). The old vector erased from
    // the front, moving every (heavy: closures + tile vectors) command.
    std::deque<StrokeCommand> undo_, redo_;
    static const std::string kEmpty;
};

// Serial stroke queue: Krita KisStrokesQueue concept, simplified.
// beginStroke returns token; addJob appends lambdas; endStroke commits+undoable.
class StrokeQueue {
public:
    using Job = std::function<void(TileMemento &scratch)>;
    struct ActiveStroke {
        std::string name;
        PaintDevice *device;
        TileMemento memento;
        std::vector<Job> jobs;
    };
    explicit StrokeQueue(UndoStack &undo) : undo_(undo) {}

    ActiveStroke &begin(const std::string &name, PaintDevice *device);
    void addJob(const Job &j);
    void commit();   // run jobs serially, push undo
    void cancel();   // discard without running
    bool active() const { return active_ != nullptr; }
private:
    UndoStack &undo_;
    std::unique_ptr<ActiveStroke> active_;
    std::mutex mutex_;
};

} // namespace ink
