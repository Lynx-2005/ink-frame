#include "core/InkImage.h"
#include <algorithm>
#include <functional>
#include <set>

namespace ink {

Image::Image(int w, int h, int fps) : w_(w), h_(h), fps_(fps) {
    if (w_ < 1) w_ = 1;
    if (h_ < 1) h_ = 1;
    if (fps_ < 1) fps_ = 1;
    root_ = std::make_unique<GroupLayer>("root");
}
void Image::setFps(int f) { if (f >= 1 && f <= 120) fps_ = f; }
void Image::setFrameCount(int n) { if (n >= 1 && n <= 100000) frameCount_ = n; }

static CompositeOp toOp(BlendMode b) {
    switch (b) {
        case BlendMode::Erase: return CompositeOp::Erase;
        case BlendMode::Multiply: return CompositeOp::Multiply;
        case BlendMode::Screen: return CompositeOp::Screen;
        default: return CompositeOp::Normal;
    }
}

void Image::projectLayer(const Layer &l, int frame, PaintDevice &out) const {
    if (!l.visible() || l.opacity() <= 0.0f) return;
    if (l.type() == LayerType::Paint) {
        const auto &pl = static_cast<const PaintLayer&>(l);
        int key = pl.exposureAt(frame); // -1 = a gap between cells
        if (key < 0) return;
        const PaintDevice *d = pl.deviceAt(key);
        if (!d) return;
        out.blit(*d, 0, 0, l.opacity(), toOp(l.blend()));
    } else {
        const auto &g = static_cast<const GroupLayer&>(l);
        for (auto &c : g.children()) projectLayer(*c, frame, out);
    }
}

Rect Image::project(int frame, PaintDevice &out) const {
    out.clear();
    projectLayer(*root_, frame, out);
    Rect e = out.extent();
    if (e.isEmpty()) return {};
    // clamp to canvas
    int x0 = std::max(e.x, 0), y0 = std::max(e.y, 0);
    int x1 = std::min(e.x + e.w, w_), y1 = std::min(e.y + e.h, h_);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

void Image::projectLayerRect(const Layer &l, int frame, const Rect &rect,
                             PaintDevice &out) const {
    if (!l.visible() || l.opacity() <= 0.0f || rect.isEmpty()) return;
    if (l.type() == LayerType::Paint) {
        const auto &pl = static_cast<const PaintLayer &>(l);
        int key = pl.exposureAt(frame); // -1 = a gap between cells
        if (key < 0) return;
        const PaintDevice *d = pl.deviceAt(key);
        if (!d) return;
        // Intersect with actual content: skips layers with nothing in rect
        // without touching their tiles (this is what makes hundreds of
        // layers cheap).
        Rect sub = intersectRect(rect, d->extent());
        if (sub.isEmpty()) return;
        out.blitRect(*d, sub, sub.x, sub.y, l.opacity(), toOp(l.blend()));
    } else {
        const auto &g = static_cast<const GroupLayer &>(l);
        for (auto &c : g.children()) projectLayerRect(*c, frame, rect, out);
    }
}

Rect Image::projectRect(int frame, const Rect &rect, PaintDevice &out) const {
    Rect canvas{0, 0, w_, h_};
    Rect r = intersectRect(rect, canvas);
    if (r.isEmpty()) return {};
    // Align to tile boundaries: clearRect drops whole overlapping tiles, so
    // the re-composite must cover those tiles fully or their surviving
    // content outside r would be lost. Returns the aligned (actual) region.
    int tx0 = floorDiv(r.x, kTileSize), ty0 = floorDiv(r.y, kTileSize);
    int tx1 = floorDiv(r.x + r.w - 1, kTileSize), ty1 = floorDiv(r.y + r.h - 1, kTileSize);
    Rect aligned{tx0 * kTileSize, ty0 * kTileSize, (tx1 - tx0 + 1) * kTileSize,
                (ty1 - ty0 + 1) * kTileSize};
    out.store().clearRect(aligned);
    projectLayerRect(*root_, frame, aligned, out);
    return aligned;
}

void Image::collectPaintLayers(std::vector<PaintLayer*> &out) {
    std::function<void(GroupLayer&)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Paint) out.push_back(static_cast<PaintLayer*>(c.get()));
            else walk(static_cast<GroupLayer&>(*c));
        }
    };
    walk(*root_);
}
void Image::collectPaintLayers(std::vector<const PaintLayer*> &out) const {
    std::function<void(const GroupLayer&)> walk = [&](const GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Paint) out.push_back(static_cast<const PaintLayer*>(c.get()));
            else walk(static_cast<const GroupLayer&>(*c));
        }
    };
    walk(*root_);
}
void Image::collectFolders(std::vector<GroupLayer*> &out) {
    std::function<void(GroupLayer&)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Group) {
                out.push_back(static_cast<GroupLayer*>(c.get()));
                walk(static_cast<GroupLayer&>(*c));
            }
        }
    };
    walk(*root_);
}
void Image::collectFolders(std::vector<const GroupLayer*> &out) const {
    std::function<void(const GroupLayer&)> walk = [&](const GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Group) {
                out.push_back(static_cast<const GroupLayer*>(c.get()));
                walk(static_cast<const GroupLayer&>(*c));
            }
        }
    };
    walk(*root_);
}

namespace {
// First unused "<prefix> N" across the whole tree, so a name the user (or the
// boot path) already took is never handed out twice.
std::string uniqueAutoName(GroupLayer &root, const char *prefix) {
    std::set<std::string> used;
    std::function<void(GroupLayer&)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            used.insert(c->name());
            if (c->type() == LayerType::Group) walk(static_cast<GroupLayer&>(*c));
        }
    };
    walk(root);
    for (int i = 1; i < 100000; ++i) {
        std::string cand = std::string(prefix) + " " + std::to_string(i);
        if (!used.count(cand)) return cand;
    }
    return std::string(prefix);
}
} // namespace

std::string Image::autoFolderName() { return uniqueAutoName(*root_, "Folder"); }
std::string Image::autoLayerName() { return uniqueAutoName(*root_, "Layer"); }

GroupLayer *Image::createFolder(const std::string &name) {
    std::string nm = name.empty() ? autoFolderName() : name;
    auto g = std::make_shared<GroupLayer>(nm);
    GroupLayer *p = g.get();
    root_->addChild(std::move(g));
    return p;
}

PaintLayer *Image::createTrack(GroupLayer *folder, const std::string &name) {
    if (!folder) folder = ensureFolder();
    std::string nm = name.empty() ? autoLayerName() : name;
    auto l = std::make_shared<PaintLayer>(nm);
    PaintLayer *p = l.get();
    folder->addChild(std::move(l));
    return p;
}

GroupLayer *Image::ensureFolder() {
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    if (!folders.empty()) return folders.front();
    return createFolder("Folder 1");
}

GroupLayer *Image::findFolderOf(PaintLayer *track) {
    if (!track) return nullptr;
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    for (auto *g : folders)
        for (auto &c : g->children())
            if (c.get() == track) return g;
    return nullptr;
}

GroupLayer *Image::findFolderByName(const std::string &name) {
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    for (auto *g : folders) if (g->name() == name) return g;
    return nullptr;
}

GroupLayer *Image::groupTracks(const std::vector<PaintLayer*> &members,
                               const std::string &name) {
    if (members.empty()) return nullptr;
    // 1. pin the members in tree order (top..bottom of the stack).
    std::vector<Layer*> wanted;
    for (auto *m : members)
        if (m && std::find(wanted.begin(), wanted.end(), static_cast<Layer*>(m)) == wanted.end())
            wanted.push_back(m);
    std::vector<std::shared_ptr<Layer>> ordered;
    std::function<void(GroupLayer&)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            if (std::find(wanted.begin(), wanted.end(), c.get()) != wanted.end())
                ordered.push_back(c);
            else if (c->type() == LayerType::Group)
                walk(static_cast<GroupLayer&>(*c));
        }
    };
    walk(*root_);
    if (ordered.empty()) return nullptr;
    GroupLayer *anchor = findFolderOf(static_cast<PaintLayer*>(ordered.front().get()));

    // 2. detach them from whatever folder holds them (folders are kept).
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    for (auto *f : folders) {
        auto &ch = f->children();
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [&](const std::shared_ptr<Layer> &c) {
                                    return std::find(ordered.begin(), ordered.end(), c) != ordered.end();
                                }),
                 ch.end());
    }

    // 3. the new folder lands right after the folder the top member came from.
    GroupLayer *group = createFolder(name);
    auto &rootCh = root_->children();
    size_t at = rootCh.size() - 1; // createFolder appended
    if (anchor) {
        for (size_t i = 0; i < rootCh.size(); ++i)
            if (rootCh[i].get() == static_cast<Layer*>(anchor)) { at = i + 1; break; }
    }
    if (at != rootCh.size() - 1) {
        auto held = rootCh.back();
        rootCh.pop_back();
        rootCh.insert(rootCh.begin() + std::min(at, rootCh.size()), held);
    }
    for (auto &l : ordered) group->addChild(l);
    return group;
}

bool Image::ungroupFolder(GroupLayer *g) {
    if (!g) return false;
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    if (folders.size() <= 1) return false; // folders are mandatory
    auto &rootCh = root_->children();
    int idx = -1;
    for (size_t i = 0; i < rootCh.size(); ++i)
        if (rootCh[i].get() == static_cast<Layer*>(g)) { idx = (int)i; break; }
    if (idx < 0) return false;
    // destination: nearest root-level folder above, else the nearest below.
    GroupLayer *dst = nullptr;
    for (int i = idx - 1; i >= 0 && !dst; --i)
        if (rootCh[(size_t)i]->type() == LayerType::Group)
            dst = static_cast<GroupLayer*>(rootCh[(size_t)i].get());
    for (size_t i = (size_t)idx + 1; i < rootCh.size() && !dst; ++i)
        if (rootCh[i]->type() == LayerType::Group)
            dst = static_cast<GroupLayer*>(rootCh[i].get());
    if (!dst) return false;
    std::vector<std::shared_ptr<Layer>> paints, nested;
    for (auto &c : g->children())
        (c->type() == LayerType::Paint ? paints : nested).push_back(c);
    g->children().clear();
    for (auto &p : paints) dst->addChild(p);
    rootCh.erase(rootCh.begin() + idx);
    for (size_t i = 0; i < nested.size(); ++i)
        rootCh.insert(rootCh.begin() + idx + (long)i, nested[i]);
    return true;
}

bool Image::moveTrack(PaintLayer *l, GroupLayer *to, int index) {
    if (!l || !to) return false;
    GroupLayer *from = findFolderOf(l);
    if (!from) return false;
    if (from == to) return true; // same folder: order is a UI concern
    std::shared_ptr<Layer> held;
    auto &src = from->children();
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it->get() == static_cast<Layer*>(l)) { held = *it; src.erase(it); break; }
    }
    if (!held) return false;
    auto &dst = to->children();
    if (index < 0 || index > (int)dst.size()) dst.push_back(held);
    else dst.insert(dst.begin() + index, held);
    return true;
}

} // namespace ink
