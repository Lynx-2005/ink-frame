#include "core/InkCell.h"
#include <algorithm>
#include <functional>
#include <set>

namespace ink {

uint64_t CellLayer::s_nextId_{1};

CellLayer::CellLayer(std::string name, LayerType type)
    : id_(s_nextId_++), name_(std::move(name)), type_(type) {
    if (type_ == LayerType::Paint) device_ = std::make_unique<PaintDevice>();
}

void CellLayer::setOpacity(float o) { opacity_ = std::clamp(o, 0.0f, 1.0f); }

bool CellLayer::removeChild(size_t i) {
    if (i >= children_.size()) return false;
    children_.erase(children_.begin() + (long)i);
    return true;
}

bool CellLayer::hasContent() const {
    if (type_ == LayerType::Paint)
        return device_ && !device_->extent().isEmpty();
    for (auto &c : children_)
        if (c->hasContent()) return true;
    return false;
}

std::shared_ptr<CellLayer> CellLayer::clone() const {
    auto copy = std::make_shared<CellLayer>(name_, type_);
    copy->setVisible(visible_);
    copy->setLocked(locked_);
    copy->setOpacity(opacity_);
    copy->setBlend(blend_);
    if (type_ == LayerType::Paint) {
        if (device_ && !device_->extent().isEmpty()) copy->device_->blit(*device_, 0, 0);
    } else {
        for (auto &c : children_) copy->addChild(c->clone());
    }
    return copy;
}

// ---------------------------------------------------------------- helpers

namespace {

std::vector<std::shared_ptr<CellLayer>> *parentVectorOf(
        std::vector<std::shared_ptr<CellLayer>> &vec, const CellLayer *l) {
    for (auto &c : vec)
        if (c.get() == l) return &vec;
    for (auto &c : vec)
        if (c->type() == LayerType::Group)
            if (auto *p = parentVectorOf(c->children(), l)) return p;
    return nullptr;
}

int indexIn(const std::vector<std::shared_ptr<CellLayer>> &vec, const CellLayer *l) {
    for (size_t i = 0; i < vec.size(); ++i)
        if (vec[i].get() == l) return (int)i;
    return -1;
}

int countPaints(const std::vector<std::shared_ptr<CellLayer>> &vec) {
    int n = 0;
    for (auto &c : vec) n += c->isPaint() ? 1 : countPaints(c->children());
    return n;
}

void collectPaints(const std::vector<std::shared_ptr<CellLayer>> &vec,
                   std::vector<CellLayer*> &out) {
    for (auto &c : vec) {
        if (c->isPaint()) out.push_back(c.get());
        collectPaints(c->children(), out);
    }
}

bool containsLayer(const CellLayer *root, const CellLayer *l) {
    if (!root || !l) return false;
    if (root == l) return true;
    for (auto &c : root->children())
        if (containsLayer(c.get(), l)) return true;
    return false;
}

std::string firstFreeName(const std::vector<std::shared_ptr<CellLayer>> &vec,
                          const char *prefix) {
    std::set<std::string> used;
    std::function<void(const std::vector<std::shared_ptr<CellLayer>>&)> walk =
        [&](const std::vector<std::shared_ptr<CellLayer>> &v) {
            for (auto &c : v) {
                used.insert(c->name());
                walk(c->children());
            }
        };
    walk(vec);
    for (int i = 1; i < 100000; ++i) {
        std::string cand = std::string(prefix) + " " + std::to_string(i);
        if (!used.count(cand)) return cand;
    }
    return std::string(prefix);
}

} // namespace

// ------------------------------------------------------------------ Cell

Cell::Cell() {
    auto l = std::make_shared<CellLayer>("Layer 1", LayerType::Paint);
    activeId_ = l->id();
    layers_.push_back(std::move(l));
}

CellLayer *Cell::find(uint64_t id) {
    for (auto &c : layers_) {
        if (c->id() == id) return c.get();
        for (auto &g : c->children()) {
            // depth-first through the (shallow) group nesting
            std::vector<CellLayer*> stack{g.get()};
            while (!stack.empty()) {
                CellLayer *cur = stack.back();
                stack.pop_back();
                if (cur->id() == id) return cur;
                for (auto &k : cur->children()) stack.push_back(k.get());
            }
        }
    }
    return nullptr;
}

const CellLayer *Cell::find(uint64_t id) const {
    return const_cast<Cell *>(this)->find(id);
}

std::shared_ptr<CellLayer> Cell::findShared(uint64_t id) const {
    std::vector<std::shared_ptr<CellLayer>> stack{layers_.begin(), layers_.end()};
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        if (cur->id() == id) return cur;
        for (auto &g : cur->children()) stack.push_back(g);
    }
    return nullptr;
}

std::vector<std::shared_ptr<CellLayer>> *Cell::parentOf(const CellLayer *l) {
    return parentVectorOf(layers_, l);
}

// Same walk, but also hands back the group that holds `l` (null = top level).
std::vector<std::shared_ptr<CellLayer>> *Cell::parentOf(const CellLayer *l, CellLayer **holder) {
    if (holder) *holder = nullptr;
    for (auto &c : layers_) {
        if (c->isPaint()) continue;
        if (auto *vec = parentVectorOf(c->children(), l)) {
            if (holder) *holder = c.get();
            return vec;
        }
    }
    return &layers_;
}

bool Cell::lockedThrough(const CellLayer *l) const {
    if (!l) return false;
    if (l->locked()) return true;
    for (const CellLayer *p = l;;) {
        CellLayer *holder = nullptr;
        const_cast<Cell *>(this)->parentOf(p, &holder);
        if (!holder) return false;
        if (holder->locked()) return true;
        p = holder;
    }
}

bool Cell::visibleThrough(const CellLayer *l) const {
    if (!l) return false;
    if (!l->visible()) return false;
    for (const CellLayer *p = l; p->visible();) {
        CellLayer *holder = nullptr;
        const_cast<Cell *>(this)->parentOf(p, &holder);
        if (!holder) return true;
        if (!holder->visible()) return false;
        p = holder;
    }
    return true;
}

CellLayer *Cell::activePaintLayer() {
    if (auto *l = find(activeId_))
        if (l->isPaint()) return l;
    std::vector<CellLayer*> paints;
    collectPaints(layers_, paints);
    return paints.empty() ? nullptr : paints.back();
}

const CellLayer *Cell::activePaintLayer() const {
    return const_cast<Cell *>(this)->activePaintLayer();
}

// Shared by addLayer/addGroup: bind the new layer into `parent` at index `idx`
// (-1 = on top of the stack). New layers always land at the top, top level, so
// they stay independent of whatever group happens to be selected.
static CellLayer *insertLayer(std::vector<std::shared_ptr<CellLayer>> **holder,
                              std::shared_ptr<CellLayer> l, int idx) {
    auto *parent = *holder;
    if (idx < 0 || idx > (int)parent->size()) idx = (int)parent->size();
    CellLayer *p = l.get();
    parent->insert(parent->begin() + idx, std::move(l));
    return p;
}

CellLayer *Cell::addLayer(const std::string &name, CellLayer *above) {
    std::string nm = name.empty() ? autoLayerName() : name;
    auto *parent = above ? parentOf(above) : nullptr;
    if (!parent) parent = &layers_;
    const int idx = above ? indexIn(*parent, above) + 1 : -1;
    return insertLayer(&parent, std::make_shared<CellLayer>(nm, LayerType::Paint), idx);
}

CellLayer *Cell::addGroup(const std::string &name, CellLayer *above) {
    std::string nm = name.empty() ? firstFreeName(layers_, "Group") : name;
    auto *parent = above ? parentOf(above) : nullptr;
    if (!parent) parent = &layers_;
    const int idx = above ? indexIn(*parent, above) + 1 : -1;
    return insertLayer(&parent, std::make_shared<CellLayer>(nm, LayerType::Group), idx);
}

CellLayer *Cell::duplicate(const CellLayer *l) {
    if (!l) return nullptr;
    auto *parent = parentOf(l);
    if (!parent) return nullptr;
    auto copy = l->clone();
    CellLayer *p = copy.get();
    int idx = indexIn(*parent, l) + 1;
    parent->insert(parent->begin() + idx, std::move(copy));
    return p;
}

bool Cell::canRemove(const CellLayer *l) const {
    if (!l) return false;
    const int inSubtree = l->isPaint() ? 1 : countPaints(l->children());
    return paintLayerCount() - inSubtree > 0;
}

bool Cell::remove(CellLayer *l) {
    // A cell must keep at least one paint layer: it is what tools draw into.
    if (!canRemove(l)) return false;
    return detach(l->id()) != nullptr;
}

bool Cell::move(CellLayer *l, int delta) {
    if (!l || delta == 0) return false;
    auto *parent = parentOf(l);
    if (!parent) return false;
    int at = indexIn(*parent, l);
    if (at < 0) return false;
    int target = at + delta;
    if (target < 0 || target >= (int)parent->size()) return false;
    auto held = (*parent)[(size_t)at];
    parent->erase(parent->begin() + at);
    parent->insert(parent->begin() + target, held);
    return true;
}

bool Cell::moveToGroup(CellLayer *l, CellLayer *group) {
    if (!l) return false;
    if (group && (group->isPaint() || group == l || containsLayer(l, group))) return false;
    auto *parent = parentOf(l);
    if (!parent) return false;
    if (group && parent == &group->children()) return true; // already there
    auto held = detach(l->id());
    if (!held) return false;
    if (group) group->addChild(held);
    else layers_.push_back(held);
    return true;
}

CellLayer *Cell::group(const std::vector<CellLayer*> &sel, const std::string &name) {
    if (sel.empty()) return nullptr;
    // Members in tree order (bottom..top), duplicates/absentees dropped.
    std::vector<std::shared_ptr<CellLayer>> ordered;
    // Fill the stack reversed so the LIFO walk pops bottom-most first.
    std::vector<std::shared_ptr<CellLayer>> stack;
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) stack.push_back(*it);
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        if (std::find(sel.begin(), sel.end(), cur.get()) != sel.end()) {
            ordered.push_back(cur);
            continue; // a member is never grouped into itself
        }
        auto &ch = cur->children();
        for (auto it = ch.rbegin(); it != ch.rend(); ++it) stack.push_back(*it);
    }
    if (ordered.empty()) return nullptr;

    // The group takes the place of the topmost member, then the members move
    // in — so the stack reads the same before and after.
    auto *anchor = ordered.back().get();
    auto *parent = parentVectorOf(layers_, anchor);
    if (!parent) parent = &layers_;
    int at = std::max(0, indexIn(*parent, anchor));
    auto g = std::make_shared<CellLayer>(name.empty() ? firstFreeName(layers_, "Group") : name,
                                         LayerType::Group);
    CellLayer *gp = g.get();
    parent->insert(parent->begin() + at, std::move(g));
    for (auto &m : ordered) {
        auto held = detach(m->id());
        if (held) gp->addChild(held);
    }
    return gp;
}

bool Cell::ungroup(CellLayer *g) {
    if (!g || g->isPaint()) return false;
    auto *parent = parentVectorOf(layers_, g);
    if (!parent) return false;
    int at = indexIn(*parent, g);
    if (at < 0) return false;
    auto kids = g->children();
    g->children().clear();
    parent->erase(parent->begin() + at);
    for (size_t i = 0; i < kids.size(); ++i)
        parent->insert(parent->begin() + at + (long)i, kids[i]);
    return true;
}

bool Cell::canRemovePaint() const {
    int n = 0;
    for (auto &c : layers_) n += c->isPaint() ? 1 : countPaints(c->children());
    return n > 1;
}

int Cell::paintLayerCount() const {
    int n = 0;
    for (auto &c : layers_) n += c->isPaint() ? 1 : countPaints(c->children());
    return n;
}

bool Cell::hasContent() const {
    for (auto &c : layers_)
        if (c->hasContent()) return true;
    return false;
}

std::string Cell::autoLayerName() const { return firstFreeName(layers_, "Layer"); }

void Cell::copyFrom(const Cell &src) {
    layers_.clear();
    activeId_ = 0;
    for (auto &c : src.layers_) layers_.push_back(c->clone());
    std::vector<CellLayer*> paints;
    collectPaints(layers_, paints);
    if (!paints.empty()) activeId_ = paints.back()->id();
}

bool Cell::insertAt(std::shared_ptr<CellLayer> l, uint64_t parentId, int index) {
    if (!l) return false;
    std::vector<std::shared_ptr<CellLayer>> *parent = &layers_;
    if (parentId != 0) {
        CellLayer *g = find(parentId);
        if (!g || g->isPaint()) return false;
        if (g == l.get() || containsLayer(l.get(), g)) return false; // no cycles
        parent = &g->children();
    }
    if (index < 0 || index > (int)parent->size()) index = (int)parent->size();
    parent->insert(parent->begin() + index, std::move(l));
    return true;
}

std::shared_ptr<CellLayer> Cell::detach(uint64_t id) {
    std::shared_ptr<CellLayer> held;
    std::function<bool(std::vector<std::shared_ptr<CellLayer>>&)> walk =
        [&](std::vector<std::shared_ptr<CellLayer>> &vec) -> bool {
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i]->id() == id) {
                held = vec[i];
                vec.erase(vec.begin() + (long)i);
                return true;
            }
        }
        for (auto &c : vec)
            if (c->type() == LayerType::Group && walk(c->children())) return true;
        return false;
    };
    if (!walk(layers_)) return nullptr;
    if (!find(activeId_)) {
        activeId_ = 0;
        std::vector<CellLayer*> paints;
        collectPaints(layers_, paints);
        if (!paints.empty()) activeId_ = paints.back()->id();
    }
    return held;
}

bool Cell::slotOf(const CellLayer *l, uint64_t &parentId, int &index) const {
    if (!l) return false;
    std::function<bool(const std::vector<std::shared_ptr<CellLayer>>&, uint64_t)> walk =
        [&](const std::vector<std::shared_ptr<CellLayer>> &vec, uint64_t pid) -> bool {
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i].get() == l) {
                parentId = pid;
                index = (int)i;
                return true;
            }
        }
        for (auto &c : vec)
            if (c->type() == LayerType::Group && walk(c->children(), c->id())) return true;
        return false;
    };
    return walk(layers_, 0);
}

} // namespace ink
