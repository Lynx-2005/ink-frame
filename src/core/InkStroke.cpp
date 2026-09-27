#include "core/InkStroke.h"
#include "core/InkPaintDevice.h"

namespace ink {

const std::string UndoStack::kEmpty{};

void UndoStack::push(StrokeCommand cmd) {
    undo_.push_back(std::move(cmd));
    redo_.clear();
    if (undo_.size() > limit_) undo_.pop_front();
}
void UndoStack::pushAction(std::string name, std::function<void()> redo, std::function<void()> undo) {
    StrokeCommand cmd;
    cmd.name = std::move(name);
    cmd.redoFn = std::move(redo);
    cmd.undoFnExtra = std::move(undo);
    push(std::move(cmd));
}
const std::string &UndoStack::undoName() const { return undo_.empty() ? kEmpty : undo_.back().name; }
const std::string &UndoStack::redoName() const { return redo_.empty() ? kEmpty : redo_.back().name; }
void UndoStack::undo() {
    if (undo_.empty()) return;
    StrokeCommand c = std::move(undo_.back());
    undo_.pop_back();
    for (auto &t : c.targets) t.device->store().restore(t.memento);
    if (c.undoFnExtra) c.undoFnExtra();
    redo_.push_back(std::move(c));
}
void UndoStack::redo() {
    if (redo_.empty()) return;
    StrokeCommand c = std::move(redo_.back());
    redo_.pop_back();
    if (c.redoFn) c.redoFn();
    else for (auto &a : c.after) a.device->store().setTile(a.pos, a.tile);
    undo_.push_back(std::move(c));
}
void UndoStack::clear() { undo_.clear(); redo_.clear(); }

StrokeQueue::ActiveStroke &StrokeQueue::begin(const std::string &name, PaintDevice *device) {
    std::lock_guard<std::mutex> lk(mutex_);
    active_ = std::make_unique<ActiveStroke>();
    active_->name = name;
    active_->device = device;
    return *active_;
}
void StrokeQueue::addJob(const Job &j) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (active_) active_->jobs.push_back(j);
}
void StrokeQueue::commit() {
    std::unique_ptr<ActiveStroke> s;
    { std::lock_guard<std::mutex> lk(mutex_); s = std::move(active_); }
    if (!s || !s->device) return;
    TileMemento scratch;
    for (auto &j : s->jobs) j(scratch);
    // Merge scratch into stroke memento via device writes already recorded?
    // Our PaintDevice writes recorded into per-job memento passed by jobs.
    // Here jobs receive `scratch` — after run, build command:
    StrokeCommand cmd;
    cmd.name = s->name;
    // NOTE: jobs were given `scratch` to record into; move it as the target.
    cmd.targets.push_back({s->device, std::move(scratch)});
    // Redo = re-run jobs without recording (simplified: jobs must be idempotent;
    // full impl would snapshot after-tiles. Production v2 stores after-image.)
    // For v1 correctness, redo re-executes jobs on fresh memento (acceptable:
    // brush dabs are deterministic given same input events).
    auto jobs = s->jobs;
    auto *dev = s->device;
    cmd.redoFn = [jobs, dev]() {
        TileMemento tmp;
        for (auto &j : jobs) j(tmp);
    };
    undo_.push(std::move(cmd));
}
void StrokeQueue::cancel() {
    std::lock_guard<std::mutex> lk(mutex_);
    active_.reset();
}

} // namespace ink
