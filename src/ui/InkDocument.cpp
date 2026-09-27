#include "ui/InkDocument.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>

namespace ink {

namespace {
// A layer's stable id is "L<id>"; ids are never reused, so the number alone
// identifies a layer across renames and re-parenting.
uint64_t idFromStable(const std::string &s) {
    if (s.size() < 2 || s[0] != 'L') return 0;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str() + 1, &end, 10);
    return (end == s.c_str() + 1) ? 0 : uint64_t(v);
}

Layer *layerById(Image &img, uint64_t id) {
    if (!id) return nullptr;
    Layer *found = nullptr;
    std::function<void(GroupLayer &)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            if (found) return;
            if (c->id() == id) {
                found = c.get();
                return;
            }
            if (c->type() == LayerType::Group) walk(static_cast<GroupLayer &>(*c));
        }
    };
    walk(img.root());
    return found;
}
} // namespace

namespace {
// Frame indices are clamped to the project on the way in: a stray value would
// otherwise create a key nobody can reach on the timeline.
int clampF(Document *d, int f) { return std::clamp(f, 0, d->timeline().frameCount() - 1); }
} // namespace

Document::Document(int w, int h, int fps)
    : image_(std::make_unique<Image>(w, h, fps)), playback_(fps) {
    timeline_.setFps(fps);
    timeline_.setFrameCount(image_->frameCount());
    playback_.setRange(0, image_->frameCount() - 1);
    // Mandatory-folder boot: even a single-track project has a folder, and
    // the track starts EMPTY — no cell exists until the user adds one
    // (New cell / double-click on the lane) or paints on it (auto-key).
    // Names are defaults the user can rename; nothing project-specific.
    GroupLayer *folder = image_->createFolder("Folder 1");
    PaintLayer *bg = image_->createTrack(folder, "Layer 1");
    syncTimelineFromImage();
    currentFolder_ = folder;
    currentLayer_ = bg;
    selection_.reset(w, h);
    cache_ = std::make_unique<FrameCache>(64, 2);
    editMutex_ = &image_->treeMutex();
    cache_->setRenderFn([this](int frame) {
        // Worker thread: share the tree lock with every edit, so a render can
        // never walk a cell stack while the UI is changing it.
        std::shared_lock<std::shared_mutex> lock(image_->treeMutex());
        auto out = std::make_shared<PaintDevice>();
        image_->project(frame, *out);
        return out;
    });
}

// ------------------------------------------------------------ edit locking

void Document::beginEdit() {
    if (editDepth_++ == 0) editMutex_->lock();
}

void Document::endEdit() {
    if (--editDepth_ == 0) editMutex_->unlock();
}

void Document::undoEdit() {
    { auto edit = editGuard(); undo_.undo(); }
    // An undo may have restored older keys: the ants must settle on the pose
    // that exists NOW.
    syncSelectionTransform();
}

void Document::redoEdit() {
    { auto edit = editGuard(); undo_.redo(); }
    syncSelectionTransform();
}

GroupLayer *Document::createFolder(const std::string &name) {
    auto edit = editGuard();
    GroupLayer *g = image_->createFolder(name);
    std::string fid = g->stableId();
    timeline_.addFolder(g->name());
    // Re-sync to bind stable ids (folders keyed by stableId below).
    syncTimelineFromImage();
    currentFolder_ = g;
    cache_->invalidateAll();
    return g;
}

PaintLayer *Document::createTrack(GroupLayer *folder, const std::string &name) {
    auto edit = editGuard();
    if (!folder) folder = currentFolder_ ? currentFolder_ : ensureFolder();
    PaintLayer *l = image_->createTrack(folder, name);
    currentFolder_ = folder;
    currentLayer_ = l;
    touchLayer(l, currentFrame());
    return l;
}

GroupLayer *Document::ensureFolder() {
    GroupLayer *g = image_->ensureFolder();
    // Make sure timeline has a matching folder row.
    syncTimelineFromImage();
    if (!currentFolder_) currentFolder_ = g;
    return g;
}

// Insert a new track directly above `anchor` inside its folder (CSP/Krita:
// new layers go on top of the working layer, not at the stack bottom).
static PaintLayer *insertAbove(Image *img, GroupLayer *folder, PaintLayer *anchor,
                               const std::string &name) {
    std::string nm = name.empty() ? img->autoLayerName() : name;
    auto l = std::make_shared<PaintLayer>(nm);
    PaintLayer *p = l.get();
    auto &ch = folder->children();
    size_t idx = ch.size();
    if (anchor) {
        for (size_t i = 0; i < ch.size(); ++i)
            if (ch[i].get() == anchor) { idx = i + 1; break; }
    }
    ch.insert(ch.begin() + idx, std::move(l));
    (void)img;
    return p;
}

PaintLayer *Document::createTrackAboveCurrent(const std::string &name) {
    auto edit = editGuard();
    GroupLayer *folder = currentFolder_ ? currentFolder_ : ensureFolder();
    PaintLayer *l = insertAbove(image_.get(), folder, currentLayer_, name);
    currentFolder_ = folder;
    currentLayer_ = l;
    touchLayer(l, currentFrame());
    return l;
}

GroupLayer *Document::groupSelected(const std::vector<Layer*> &sel, const std::string &name) {
    auto edit = editGuard();
    GroupLayer *g = image_->groupLayers(sel, name);
    if (!g) return nullptr;
    currentFolder_ = g;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(timeline_.currentFrame());
    return g;
}

bool Document::ungroupFolder(GroupLayer *g) {
    auto edit = editGuard();
    if (!image_->ungroupFolder(g)) return false;
    if (currentFolder_ == g) currentFolder_ = nullptr;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(timeline_.currentFrame());
    return true;
}

bool Document::moveLayerToFolder(Layer *l, GroupLayer *to) {
    auto edit = editGuard();
    if (!image_->moveLayer(l, to)) return false;
    cache_->invalidateAll();
    syncTimelineFromImage();
    return true;
}

bool Document::deleteFolder(GroupLayer *g) {
    auto edit = editGuard();
    if (!g) return false;
    std::vector<GroupLayer*> folders;
    image_->collectFolders(folders);
    if (folders.size() <= 1) {
        // Folders are mandatory: emptying the last one is the closest thing
        // to deleting it.
        if (g->children().empty()) return false;
        g->children().clear();
    } else {
        auto &ch = image_->root().children();
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [&](const std::shared_ptr<Layer> &c) { return c.get() == g; }),
                 ch.end());
        if (currentFolder_ == g) currentFolder_ = nullptr;
    }
    if (currentLayer_ && !image_->findFolderOf(currentLayer_)) currentLayer_ = nullptr;
    cache_->invalidateAll();
    syncTimelineFromImage();
    return true;
}

bool Document::moveLayerInFolder(Layer *l, int delta) {
    auto edit = editGuard();
    if (!l || delta == 0) return false;
    GroupLayer *folder = image_->findFolderOf(l);
    if (!folder) return false;
    auto &ch = folder->children();
    int at = -1;
    for (size_t i = 0; i < ch.size(); ++i)
        if (ch[i].get() == l) { at = (int)i; break; }
    if (at < 0) return false;
    int target = at + delta;
    if (target < 0 || target >= (int)ch.size()) return false;
    auto held = ch[(size_t)at];
    ch.erase(ch.begin() + at);
    ch.insert(ch.begin() + target, held);
    cache_->invalidateAll();
    syncTimelineFromImage();
    return true;
}

PaintLayer *Document::duplicateTrack(PaintLayer *src) {
    auto edit = editGuard();
    if (!src) return nullptr;
    GroupLayer *folder = image_->findFolderOf(src);
    if (!folder) folder = ensureFolder();
    PaintLayer *dst = insertAbove(image_.get(), folder, src, src->name() + " copy");
    dst->setVisible(src->visible());
    dst->setLocked(false);
    dst->setOpacity(src->opacity());
    dst->setBlend(src->blend());
    for (int k : src->keys()) dst->setKeyAt(k, true);
    // real content + exposure copy (the whole per-cell layer stack)
    for (int k : src->keys()) {
        dst->setSpanEnd(k, src->spanEnd(k));
        if (const Cell *scell = src->cellAt(k)) {
            if (Cell *dcell = dst->cellAt(k, false)) dcell->copyFrom(*scell);
        }
    }
    currentLayer_ = dst;
    currentFolder_ = folder;
    touchLayer(dst, currentFrame());
    return dst;
}

bool Document::deleteTrack(PaintLayer *layer) {
    auto edit = editGuard();
    if (!layer) return false;
    GroupLayer *owner = image_->findFolderOf(layer);
    if (!owner) return false;
    auto &ch = owner->children();
    for (auto it = ch.begin(); it != ch.end(); ++it) {
        if (it->get() == layer) {
            // pick neighbour as new current (above preferred, Krita-like)
            PaintLayer *next = nullptr;
            for (auto jt = it + 1; jt != ch.end(); ++jt)
                if (jt->get()->type() == LayerType::Paint) { next = static_cast<PaintLayer*>(jt->get()); break; }
            if (!next)
                for (auto jt = ch.begin(); jt != it; ++jt)
                    if (jt->get()->type() == LayerType::Paint) next = static_cast<PaintLayer*>(jt->get());
            ch.erase(it);
            if (currentLayer_ == layer) {
                if (!next) {
                    std::vector<PaintLayer*> all;
                    image_->collectPaintLayers(all);
                    next = all.empty() ? nullptr : all.front();
                }
                currentLayer_ = next;
            }
            cache_->invalidateAll();
            syncTimelineFromImage();
            return true;
        }
    }
    return false;
}

// --- camera layers -----------------------------------------------------------

CameraLayer *Document::createCameraForLayer(PaintLayer *target, const std::string &name) {
    auto edit = editGuard();
    if (!target) return nullptr;
    GroupLayer *folder = image_->findFolderOf(target);
    if (!folder) folder = ensureFolder();
    // Directly below the target track: the row reads as belonging to the
    // layer above it, instead of piling up at the folder's bottom.
    int at = -1;
    auto &ch = folder->children();
    for (size_t i = 0; i < ch.size(); ++i)
        if (ch[i].get() == static_cast<Layer*>(target)) { at = int(i) + 1; break; }
    CameraLayer *cam =
        image_->createCamera(folder, name, CameraScope::Layer, target->id(), at);
    currentFolder_ = folder;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(currentFrame());
    return cam;
}

CameraLayer *Document::createCameraForFolder(GroupLayer *folder, const std::string &name) {
    auto edit = editGuard();
    if (!folder) folder = ensureFolder();
    // First child of the folder: the timeline detaches folder cameras to a
    // row ABOVE the group, and first-in-tree-order also wins ties, so the
    // newest group camera is the live one.
    CameraLayer *cam =
        image_->createCamera(folder, name, CameraScope::Folder, folder->id(), 0);
    currentFolder_ = folder;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(currentFrame());
    return cam;
}

CameraLayer *Document::createGlobalCamera(const std::string &name) {
    auto edit = editGuard();
    GroupLayer *folder = currentFolder_ ? currentFolder_ : ensureFolder();
    CameraLayer *cam = image_->createCamera(folder, name, CameraScope::Global, 0);
    currentFolder_ = folder;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(currentFrame());
    return cam;
}

bool Document::deleteCamera(CameraLayer *cam) {
    auto edit = editGuard();
    if (!cam) return false;
    if (!image_->deleteCamera(cam)) return false;
    if (camRecord_.active && camRecord_.cam == cam) camRecord_ = CameraRecord{};
    cache_->invalidateAll();
    syncTimelineFromImage();
    return true;
}

bool Document::renameCamera(CameraLayer *cam, const std::string &name) {
    if (!cam || name.empty()) return false;
    auto edit = editGuard();
    cam->setName(name);
    syncTimelineFromImage();
    return true;
}

CameraLayer *Document::findCamera(const std::string &stableId) {
    return image_->cameraById(idFromStable(stableId));
}

void Document::collectCameraLayers(std::vector<CameraLayer*> &out) {
    image_->collectCameras(out);
}

bool Document::pushCameraEdit(const char *name, CameraLayer *c, const ParamBlock &before) {
    if (!c) return false;
    ParamBlock after = c->params();
    if (before == after) return false;
    undo_.pushAction(name,
                     [this, c, after]() {
                         c->params() = after;
                         touchCamera(c);
                     },
                     [this, c, before]() {
                         c->params() = before;
                         touchCamera(c);
                     });
    touchCamera(c);
    return true;
}

void Document::touchCamera(CameraLayer *c) {
    (void)c;
    syncSelectionTransform();
    if (camRecord_.active) {
        const int cur = currentFrame();
        invalidateAdjustmentRange(cur, cur);
        return;
    }
    cache_->invalidateAll();
    cache_->requestRange(currentFrame());
}

bool Document::setCameraKey(CameraLayer *c, ParamId id, int frame, double value, Interp interp,
                            bool updateExisting) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = c->params();
    if (!c->params().setKey(id, frame, value, interp, updateExisting)) return false;
    return pushCameraEdit("Set camera keyframe", c, before);
}

bool Document::removeCameraKey(CameraLayer *c, ParamId id, int frame) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (!c->params().removeKey(id, frame)) return false;
    return pushCameraEdit("Remove camera keyframe", c, before);
}

bool Document::addCameraKey(CameraLayer *c, ParamId id, int frame) {
    if (!c) return false;
    frame = clampF(this, frame);
    const double v = c->params().valueAt(id, frame);
    return setCameraKey(c, id, frame, v, Interp::Bezier, false);
}

bool Document::toggleCameraKey(CameraLayer *c, ParamId id, int frame) {
    if (!c) return false;
    if (c->params().hasKey(id, frame)) return removeCameraKey(c, id, frame);
    return addCameraKey(c, id, frame);
}

bool Document::moveCameraKey(CameraLayer *c, ParamId id, int from, int to) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    from = clampF(this, from);
    to = clampF(this, to);
    ParamBlock before = c->params();
    if (!c->params().moveKey(id, from, to)) return false;
    return pushCameraEdit("Move camera keyframe", c, before);
}

bool Document::setCameraKeyValue(CameraLayer *c, ParamId id, int frame, double value) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (!c->params().setKeyValue(id, frame, value)) return false;
    return pushCameraEdit("Change camera keyframe", c, before);
}

bool Document::setCameraKeyInterp(CameraLayer *c, ParamId id, int frame, Interp i) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (!c->params().setInterp(id, frame, i)) return false;
    return pushCameraEdit("Camera interpolation", c, before);
}

bool Document::setCameraKeyHandles(CameraLayer *c, ParamId id, int frame, double outInfl,
                                   double outSpeed, double inInfl, double inSpeed) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (!c->params().setHandles(id, frame, outInfl, outSpeed, inInfl, inSpeed)) return false;
    return pushCameraEdit("Shape camera keyframe", c, before);
}

bool Document::setCameraStaticValue(CameraLayer *c, ParamId id, double value) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (std::abs(c->params().staticValue(id) - value) < 1e-12) return false;
    c->params().setStaticValue(id, value);
    return pushCameraEdit("Change camera value", c, before);
}

bool Document::setCameraValue(CameraLayer *c, ParamId id, double value, int frame) {
    if (!c) return false;
    frame = clampF(this, frame);
    if (c->params().isAnimated(id)) {
        if (c->params().hasKey(id, frame)) return setCameraKeyValue(c, id, frame, value);
        return setCameraKey(c, id, frame, value, Interp::Bezier, true);
    }
    return setCameraStaticValue(c, id, value);
}

bool Document::applyCameraPreset(CameraLayer *c, int frame, Interp mode, bool easeIn,
                                 bool easeOut) {
    if (!c || c->locked()) return false;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = c->params();
    for (ParamId id : c->params().keysAt(frame)) {
        c->params().setInterp(id, frame, mode);
        if (mode != Interp::Bezier) continue;
        const ScalarChannel *ch = c->params().channel(id);
        const ScalarKey *k = ch ? ch->keyAt(frame) : nullptr;
        if (!k) continue;
        const double autoS = autoTangentSpeed(*ch, frame);
        c->params().setHandles(id, frame, k->outInfl, easeOut ? 0.0 : autoS, k->inInfl,
                               easeIn ? 0.0 : autoS);
    }
    return pushCameraEdit("Camera preset", c, before);
}

int Document::keyCameraAt(CameraLayer *c, int frame) {
    if (!c || c->locked()) return 0;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = c->params();
    std::vector<ParamId> ids = c->params().animatedParams();
    const bool fresh = ids.empty();
    if (fresh) {
        // Cameras frame the SHOT: position/pivot/scale/rotation/opacity only.
        // Corners stay manual (a camera is affine like CSP's 2D camera).
        ids = {ParamId::AnchorX, ParamId::AnchorY, ParamId::PositionX, ParamId::PositionY,
               ParamId::ScaleX, ParamId::ScaleY, ParamId::Rotation, ParamId::Opacity};
        const Rect canvas{0, 0, image_->width(), image_->height()};
        if (!c->params().hasFrame()) c->params().setFrame(canvas);
    }
    for (ParamId id : ids)
        c->params().setKey(id, frame, c->params().valueAt(id, frame), Interp::Bezier, true);
    c->params().refreshAllAutoHandles();
    if (!pushCameraEdit("Add camera keyframe", c, before)) return 0;
    return int(ids.size());
}

int Document::removeCameraKeysAt(CameraLayer *c, int frame) {
    if (!c || c->locked()) return 0;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = c->params();
    const int n = c->params().removeKeysAt(frame);
    if (n <= 0) return 0;
    if (!pushCameraEdit("Remove camera keyframe", c, before)) return 0;
    return n;
}

int Document::prevCameraKey(CameraLayer *c, int frame) {
    if (!c) return -1;
    int best = -1;
    for (ParamId id : c->params().animatedParams()) {
        const int k = c->params().prevKey(id, frame);
        if (k >= 0 && (best < 0 || k > best)) best = k;
    }
    return best;
}

int Document::nextCameraKey(CameraLayer *c, int frame) {
    if (!c) return -1;
    int best = -1;
    for (ParamId id : c->params().animatedParams()) {
        const int k = c->params().nextKey(id, frame);
        if (k >= 0 && (best < 0 || k < best)) best = k;
    }
    return best;
}

void Document::beginCameraRecord(const std::string &label, CameraLayer *c) {
    if (!c || c->locked()) return;
    if (camRecord_.active) endCameraRecord();
    camRecord_ = CameraRecord{};
    camRecord_.cam = c;
    camRecord_.before = c->params();
    camRecord_.label = label;
    camRecord_.frame = currentFrame();
    camRecord_.active = true;
}

void Document::recordCameraValue(CameraLayer *c, ParamId id, double value, int frame) {
    if (!camRecord_.active || !camRecord_.cam || camRecord_.cam != c) return;
    if (c->locked()) return;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock &b = c->params();
    const bool noop =
        b.hasKey(id, frame) && std::abs(b.valueAt(id, double(frame)) - value) < 1e-9;
    b.setKey(id, frame, value, Interp::Bezier, true);
    if (!noop) touchCamera(c);
}

void Document::endCameraRecord() {
    if (!camRecord_.active || !camRecord_.cam) {
        camRecord_ = CameraRecord{};
        return;
    }
    auto edit = editGuard();
    CameraLayer *c = camRecord_.cam;
    ParamBlock before = camRecord_.before;
    const std::string label = camRecord_.label;
    camRecord_ = CameraRecord{};
    c->params().refreshAllAutoHandles();
    ParamBlock after = c->params();
    if (before == after) return;
    undo_.pushAction(label,
                     [this, c, after]() {
                         c->params() = after;
                         touchCamera(c);
                     },
                     [this, c, before]() {
                         c->params() = before;
                         touchCamera(c);
                     });
    touchCamera(c);
}

void Document::cancelCameraRecord() {
    if (!camRecord_.active || !camRecord_.cam) {
        camRecord_ = CameraRecord{};
        return;
    }
    auto edit = editGuard();
    CameraLayer *c = camRecord_.cam;
    const ParamBlock before = camRecord_.before;
    camRecord_ = CameraRecord{};
    c->params() = before;
    touchCamera(c);
}

bool Document::cameraRecordActive(CameraLayer *c) const {
    return camRecord_.active && camRecord_.cam == c;
}

bool Document::cameraPanBy(CameraLayer *c, int frame, double dx, double dy) {
    if (!c || c->locked()) return false;
    frame = clampF(this, frame);
    const double nx = c->params().valueAt(ParamId::PositionX, frame) + dx;
    const double ny = c->params().valueAt(ParamId::PositionY, frame) + dy;
    // Tool path: animated params take keys, static params stay static unless
    // inside a record (a drag). Outside a record, pan keys when animated.
    if (camRecord_.active && camRecord_.cam == c) {
        recordCameraValue(c, ParamId::PositionX, nx, frame);
        recordCameraValue(c, ParamId::PositionY, ny, frame);
        return true;
    }
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (c->params().isAnimated(ParamId::PositionX) || c->params().isAnimated(ParamId::PositionY)) {
        c->params().setKey(ParamId::PositionX, frame, nx, Interp::Bezier, true);
        c->params().setKey(ParamId::PositionY, frame, ny, Interp::Bezier, true);
        c->params().refreshAllAutoHandles();
    } else {
        c->params().setStaticValue(ParamId::PositionX, nx);
        c->params().setStaticValue(ParamId::PositionY, ny);
    }
    return pushCameraEdit("Pan camera", c, before);
}

bool Document::cameraZoomBy(CameraLayer *c, int frame, double factor, bool uniform) {
    if (!c || c->locked() || !(factor > 0.0)) return false;
    frame = clampF(this, frame);
    const double sx = c->params().valueAt(ParamId::ScaleX, frame) * factor;
    const double sy =
        uniform ? c->params().valueAt(ParamId::ScaleY, frame) * factor : c->params().valueAt(ParamId::ScaleY, frame);
    if (camRecord_.active && camRecord_.cam == c) {
        recordCameraValue(c, ParamId::ScaleX, sx, frame);
        if (uniform) recordCameraValue(c, ParamId::ScaleY, sy, frame);
        return true;
    }
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (c->params().isAnimated(ParamId::ScaleX) || c->params().isAnimated(ParamId::ScaleY)) {
        c->params().setKey(ParamId::ScaleX, frame, sx, Interp::Bezier, true);
        if (uniform) c->params().setKey(ParamId::ScaleY, frame, sy, Interp::Bezier, true);
        c->params().refreshAllAutoHandles();
    } else {
        c->params().setStaticValue(ParamId::ScaleX, sx);
        if (uniform) c->params().setStaticValue(ParamId::ScaleY, sy);
    }
    return pushCameraEdit("Zoom camera", c, before);
}

bool Document::cameraRotateBy(CameraLayer *c, int frame, double degrees) {
    if (!c || c->locked()) return false;
    frame = clampF(this, frame);
    const double nr = c->params().valueAt(ParamId::Rotation, frame) + degrees;
    if (camRecord_.active && camRecord_.cam == c) {
        recordCameraValue(c, ParamId::Rotation, nr, frame);
        return true;
    }
    auto edit = editGuard();
    ParamBlock before = c->params();
    if (c->params().isAnimated(ParamId::Rotation)) {
        c->params().setKey(ParamId::Rotation, frame, nr, Interp::Bezier, true);
        c->params().refreshAllAutoHandles();
    } else {
        c->params().setStaticValue(ParamId::Rotation, nr);
    }
    return pushCameraEdit("Rotate camera", c, before);
}

void Document::syncTimelineFromImage() {
    // Rebuild folder/track rows from the layer tree, preserving keys,
    // expanded flags and folder colors across edits.
    std::vector<GroupLayer*> folders;
    image_->collectFolders(folders);
    if (folders.empty()) {
        GroupLayer *g = image_->createFolder("Folder 1");
        folders.push_back(g);
    }
    // Remember old state.
    std::map<std::string, bool> expanded;
    std::map<std::string, int> colors;
    std::map<std::string, bool> thumbs;
    std::map<std::string, int> overrides;
    for (auto &f : timeline_.folders()) {
        expanded[f.folderId] = f.expanded; colors[f.folderId] = f.colorIndex;
        for (auto &t : f.tracks) {
            thumbs[t.layerId] = t.showThumbs;
            overrides[t.layerId] = t.colorOverride;
        }
    }

    // Adopt stray root-level paint layers into first folder (legacy docs).
    {
        bool moved = false;
        auto &rootCh = image_->root().children();
        std::vector<std::shared_ptr<Layer>> strays;
        for (auto &c : rootCh)
            if (c->type() == LayerType::Paint) strays.push_back(c);
        if (!strays.empty()) {
            GroupLayer *dst = folders.front();
            for (auto &s : strays) {
                rootCh.erase(std::remove(rootCh.begin(), rootCh.end(), s), rootCh.end());
                dst->addChild(s);
            }
            moved = true;
            (void)moved;
        }
    }

    // Clear and rebuild (keys re-applied from layers).
    timeline_.folders().clear();
    int ci = 0;
    for (auto *g : folders) {
        std::string fid = g->stableId();
        std::string nm = g->name();
        // addFolder generates its own F<n> ids; patch to stable id after.
        std::string tmp = timeline_.addFolder(nm);
        TimelineFolder *tf = timeline_.folder(tmp);
        tf->folderId = fid;
        auto itE = expanded.find(fid);
        tf->expanded = (itE != expanded.end()) ? itE->second : true;
        auto itC = colors.find(fid);
        tf->colorIndex = (itC != colors.end()) ? itC->second : (ci++ % 8);
        for (auto &c : g->children()) {
            if (c->type() == LayerType::Paint) {
                auto *pl = static_cast<PaintLayer*>(c.get());
                timeline_.addTrack(fid, pl->stableId(), pl->name());
                if (auto *row = timeline_.row(pl->stableId())) {
                    row->frames = RasterChannel{};
                    row->spanEnd.clear();
                    for (int k : pl->keys()) {
                        row->frames.setKey(k);
                        // exposure lives on the layer; -1 mirrors "to the end"
                        row->spanEnd[k] = pl->spanOpen(k) ? -1 : pl->spanEnd(k);
                    }
                    row->layerName = pl->name();
                    auto itT = thumbs.find(pl->stableId());
                    // default: first track of each folder shows filmstrip
                    row->showThumbs =
                        (itT != thumbs.end())
                            ? itT->second
                            : (row == &timeline_.folder(fid)->tracks.front());
                    auto itO = overrides.find(pl->stableId());
                    row->colorOverride = (itO != overrides.end()) ? itO->second : -1;
                }
            } else if (c->type() == LayerType::Camera) {
                auto *cam = static_cast<CameraLayer*>(c.get());
                timeline_.addTrack(fid, cam->stableId(), cam->name());
                if (auto *row = timeline_.row(cam->stableId())) {
                    row->frames = RasterChannel{};
                    row->spanEnd.clear();
                    row->layerName = cam->name();
                    row->showThumbs = false;
                    row->colorOverride = -1;
                    row->isCamera = true;
                    row->cameraScope = int(cam->scope());
                    if (cam->scope() == CameraScope::Global) {
                        row->cameraTarget = {};
                    } else {
                        // Stable id of the scoped layer/folder ("L<id>").
                        row->cameraTarget = "L" + std::to_string(cam->targetId());
                    }
                    auto itO = overrides.find(cam->stableId());
                    if (itO != overrides.end()) row->colorOverride = itO->second;
                }
            }
        }
    }
    // Validate current pointers.
    if (currentLayer_) {
        bool alive = false;
        std::vector<PaintLayer*> layers;
        image_->collectPaintLayers(layers);
        for (auto *l : layers) if (l == currentLayer_) { alive = true; break; }
        if (!alive) {
            std::vector<PaintLayer*> all;
            image_->collectPaintLayers(all);
            currentLayer_ = all.empty() ? nullptr : all.front();
        }
    }
    if (currentFolder_) {
        bool alive = false;
        for (auto *g : folders) if (g == currentFolder_) { alive = true; break; }
        if (!alive) currentFolder_ = folders.front();
    } else {
        currentFolder_ = folders.front();
    }
}

void Document::setCurrentFrame(int f) {
    timeline_.setCurrentFrame(f);
    playback_.seek(timeline_.currentFrame());
    cache_->requestRange(timeline_.currentFrame());
    // The playhead moved: the keyframed stroke under the ants moved with it.
    syncSelectionTransform();
}

bool Document::setFrameCount(int n) {
    auto edit = editGuard();
    n = std::clamp(n, 1, 100000);
    if (n == timeline_.frameCount()) return false;
    image_->setFrameCount(n);
    timeline_.setFrameCount(n);
    timeline_.setCurrentFrame(timeline_.currentFrame()); // re-clamp the playhead
    playback_.setRange(0, n - 1);                        // caller re-applies in/out
    playback_.seek(timeline_.currentFrame());
    cache_->invalidateAll();
    cache_->requestRange(timeline_.currentFrame());
    return true;
}

// ---------------------------------------------------------------- cells

int Document::cellStartAt(PaintLayer *layer, int frame) const {
    return layer ? layer->exposureAt(frame) : -1;
}

int Document::cellEnd(PaintLayer *layer, int start) const {
    if (!layer || !layer->hasKeyAt(start)) return start;
    return layer->spanEndClamped(start, timeline_.frameCount() - 1);
}

int Document::cellLength(PaintLayer *layer, int start) const {
    return cellEnd(layer, start) - start + 1;
}

bool Document::setCellEnd(PaintLayer *layer, int start, int end) {
    auto edit = editGuard();
    if (!layer || !layer->hasKeyAt(start)) return false;
    const int last = timeline_.frameCount() - 1;
    if (end < 0 || end >= last) {
        layer->setSpanEnd(start, kOpenEnd);
    } else {
        if (end < start) end = start;
        layer->setSpanEnd(start, end);
    }
    touchLayer(layer, start);
    return true;
}

bool Document::setCellLength(PaintLayer *layer, int start, int length) {
    auto edit = editGuard();
    if (length < 1) length = 1;
    return setCellEnd(layer, start, start + length - 1);
}

int Document::extendCellToNext(PaintLayer *layer, int start) {
    auto edit = editGuard();
    if (!layer || !layer->hasKeyAt(start)) return -1;
    int target = timeline_.frameCount() - 1;
    for (int k : layer->keys())
        if (k > start) { target = k - 1; break; }
    setCellEnd(layer, start, target);
    return target;
}

int Document::extendCellToEnd(PaintLayer *layer, int start) {
    auto edit = editGuard();
    if (!layer || !layer->hasKeyAt(start)) return -1;
    layer->setSpanEnd(start, kOpenEnd);
    touchLayer(layer, start);
    return timeline_.frameCount() - 1;
}

bool Document::moveCellStart(PaintLayer *layer, int start, int newStart) {
    auto edit = editGuard();
    if (!layer || !layer->hasKeyAt(start)) return false;
    if (newStart == start) return false;
    const int end = cellEnd(layer, start);
    if (newStart > end) newStart = end;   // a cell is at least one frame
    if (newStart < 0) newStart = 0;
    // never step onto another cell (its span would be swallowed)
    for (int k : layer->keys()) {
        if (k >= start) break;
        newStart = std::max(newStart, k + 1);
    }
    if (newStart == start || layer->hasKeyAt(newStart)) return false;
    if (!layer->moveDeviceContent(start, newStart, false)) return false;
    layer->setSpanEnd(newStart, end); // trimming from the left keeps the end
    touchLayer(layer, std::min(start, newStart));
    return true;
}

PaintDevice *Document::editableDevice(PaintLayer *layer, int frame) {
    auto edit = editGuard();
    if (!layer) return nullptr;
    int key = layer->exposureAt(frame);
    if (key < 0) {
        // Auto-key OFF: never conjure a cell — the caller shows a hint.
        if (!autoKey_) return nullptr;
        // Auto-key like Krita/CSP: nothing exposed here, so a new one-frame
        // cell is created at this frame. It boots with its own layer stack.
        layer->setKeyAt(frame, true);
        layer->setSpanEnd(frame, frame);
        touchLayer(layer, frame);
        key = frame;
    }
    Cell *cell = layer->cellAt(key, true);
    if (!cell) return nullptr;
    // Tools paint into the cell's active layer (the Layer panel's target).
    // A lock on any group above it blocks painting too: that is what makes a
    // locked group mean something.
    CellLayer *target = cell->activePaintLayer();
    if (!target || cell->lockedThrough(target)) return nullptr;
    return target->device();
}

void Document::touchLayer(PaintLayer *layer, int frame) {
    (void)layer;
    cache_->invalidate(frame);
    syncTimelineFromImage();
    // Stroke-commit path: warm ONLY the dirtied frame. Queueing a 12-frame
    // prefetch here woke both workers to re-render a dozen (possibly
    // adjustment-resampled) frames behind the UI thread's release paint — the
    // click-release freeze. Idle scrub/playback still prefetches via
    // setCurrentFrame()->requestRange.
    cache_->requestSingle(timeline_.currentFrame());
}

PaintLayer *Document::findTrack(const std::string &stableId) {
    std::vector<PaintLayer*> layers;
    image_->collectPaintLayers(layers);
    for (auto *l : layers)
        if (l->stableId() == stableId) return l;
    return nullptr;
}

Layer *Document::findLayer(const std::string &stableId) {
    return layerById(*image_, idFromStable(stableId));
}

// --- keyframes on the paint layer (compatibility views) ----------------------

// The compatibility view of the current paint layer's keyframes. One lazily
// created object per Document: it is a non-owning alias, never in the tree.
AdjustmentLayer *Document::currentAdjustment() {
    if (!currentLayer_) return nullptr;
    if (!currentAdjustment_) {
        view_ = std::make_unique<AdjustmentLayer>(currentLayer_->name() + " transform");
        currentAdjustment_ = view_.get();
    }
    currentAdjustment_->setTargetLayer(currentLayer_);
    currentAdjustment_->setName(currentLayer_->name() + " transform");
    return currentAdjustment_;
}

void Document::setCurrentAdjustment(AdjustmentLayer *a) {
    // The view's target IS a paint layer; selecting it selects that layer.
    if (a && a->target())
        setCurrentLayer(a->target());
    else if (a && a->targetId() != 0)
        if (PaintLayer *l = image_->paintLayerById(a->targetId())) setCurrentLayer(l);
}

AdjustmentLayer *Document::findAdjustment(const std::string &stableId) {
    if (Layer *l = findLayer(stableId); l && l->type() == LayerType::Paint) {
        auto *pl = static_cast<PaintLayer *>(l);
        // The current view already points at this layer: return it so pointer
        // identity holds for callers that compare them.
        if (currentAdjustment_ && currentAdjustment_->target() == pl)
            return currentAdjustment_;
        auto v = std::make_unique<AdjustmentLayer>(pl->name() + " transform");
        v->setTargetLayer(pl);
        view_ = std::move(v);
        currentAdjustment_ = view_.get();
        return currentAdjustment_;
    }
    return nullptr;
}

bool Document::pushParamEdit(const char *name, AdjustmentLayer *a, const ParamBlock &before) {
    if (!a) return false;
    ParamBlock after = a->params();
    if (before == after) return false;
    undo_.pushAction(name,
                     [this, a, after]() {
                         a->params() = after;
                         touchAdjustment(a);
                     },
                     [this, a, before]() {
                         a->params() = before;
                         touchAdjustment(a);
                     });
    touchAdjustment(a);
    return true;
}

void Document::touchAdjustment(AdjustmentLayer *a) {
    // Parameter keys changed (a drag, a key write, an undo): the selection's
    // displayed position may move with them.
    syncSelectionTransform();
    if (liveEditActive(a)) {
        // Inside a live drag only the frame under the playhead needs re-render
        // per mouse-move; endParamRecord/cancelParamRecord flush the whole
        // project once when the gesture lands.
        const int cur = currentFrame();
        invalidateAdjustmentRange(cur, cur);
        return;
    }
    // A paint layer's keyframes can move pixels at any frame: the whole cache
    // goes. (The old bar-scoped flush died with the bar.)
    cache_->invalidateAll();
    cache_->requestRange(currentFrame());
}

void Document::syncSelectionTransform() {
    // The ants follow the stroke the mask clips, in DISPLAY space. The mask
    // lives in device space (the cel pixels are where they were painted); the
    // ANIMATION displaces them at composite time, so the selection is handed
    // that full matrix to display through — a rotation or perspective included,
    // which a translation-only offset could not follow (the "lasso on its own
    // track" report). UI-thread only: this reads the layer tree without a lock
    // and writes UI selection state.
    Mat3 m;
    if (selection_.targetId() != 0) {
        const PaintLayer *pl = image_->paintLayerById(selection_.targetId());
        if (pl && pl->visible() && !pl->params().empty()) {
            const Rect canvas{0, 0, image_->width(), image_->height()};
            m = pl->params().matrixFor(pl->params().frameOr(canvas), double(currentFrame()),
                                       nullptr);
        }
    }
    selection_.setDocumentMatrix(m);
}

void Document::invalidateAdjustmentRange(int lo, int hi) {
    const int last = std::max(0, timeline_.frameCount() - 1);
    cache_->invalidateRange(std::clamp(lo, 0, last), std::clamp(hi, 0, last));
    cache_->requestRange(currentFrame());
}

AdjustmentLayer *Document::createAdjustmentLayer(PaintLayer *target, bool select) {
    // No layer object is created: the "adjustment layer" is a view of the
    // target's own keyframes. Selecting one selects the target.
    if (select && target) setCurrentLayer(target);
    return ensureAdjustmentFor(target);
}

AdjustmentLayer *Document::ensureAdjustmentFor(PaintLayer *target) {
    if (!target) return nullptr;
    if (currentAdjustment_ && currentAdjustment_->target() == target)
        return currentAdjustment_;
    auto v = std::make_unique<AdjustmentLayer>(target->name() + " transform");
    v->setTargetLayer(target);
    view_ = std::move(v);
    currentAdjustment_ = view_.get();
    return currentAdjustment_;
}

AdjustmentLayer *Document::adjustmentFor(PaintLayer *target) { return ensureAdjustmentFor(target); }

bool Document::linkAdjustment(AdjustmentLayer *a, PaintLayer *target) {
    // Re-pointing a view is meaningless now (the keyframes live on the layer);
    // keep the call compiling and make "unlink" simply detach the view.
    if (!a) return false;
    a->setTargetLayer(target);
    return true;
}

bool Document::deleteAdjustment(AdjustmentLayer *a) {
    // The layer object survives (it is the paint layer itself): "delete" now
    // means "clear this layer's keyframes", as one undo step.
    if (!a) return false;
    PaintLayer *target = a->target();
    if (!target) return false;
    auto edit = editGuard();
    ParamBlock before = target->params();
    if (before.empty()) return false;
    target->params() = ParamBlock{};
    undo_.pushAction("Remove keyframes",
                     [this, target]() {
                         target->params() = ParamBlock{};
                         touchAdjustment(nullptr);
                         syncTimelineFromImage();
                     },
                     [this, target, before]() {
                         target->params() = before;
                         touchAdjustment(nullptr);
                         syncTimelineFromImage();
                     });
    touchAdjustment(nullptr);
    syncTimelineFromImage();
    return true;
}

bool Document::setAdjustmentSpan(AdjustmentLayer *a, int, int) {
    (void)a; // the span concept is gone; keyframes are valid everywhere
    return true;
}

bool Document::renameAdjustment(AdjustmentLayer *a, const std::string &name) {
    // Renaming the view renames the paint layer it stands for.
    if (!a || !a->target()) return false;
    auto edit = editGuard();
    a->target()->setName(name);
    syncTimelineFromImage();
    return true;
}

void Document::previewAdjustmentBar(AdjustmentLayer *a, int, int, int keyDelta,
                                    const ParamBlock &base) {
    // The bar is gone: this is now just a key retime preview.
    if (!a || !a->target()) return;
    auto edit = editGuard();
    a->params() = base;
    if (keyDelta != 0) a->params().shiftKeys(0, keyDelta, timeline_.frameCount() - 1);
    a->params().refreshAllAutoHandles();
    touchAdjustment(a);
}

void Document::commitAdjustmentBar(AdjustmentLayer *a, int, int, const ParamBlock &base) {
    if (!a || !a->target()) return;
    auto edit = editGuard();
    ParamBlock before = base;
    ParamBlock after = a->params();
    if (before == after) return;
    undo_.pushAction("Move keyframes",
                     [this, a, after]() { a->params() = after; touchAdjustment(a); },
                     [this, a, before]() { a->params() = before; touchAdjustment(a); });
    touchAdjustment(a);
}

void Document::abortAdjustmentBar(AdjustmentLayer *a, int, int, const ParamBlock &base) {
    if (!a) return;
    a->params() = base;
    touchAdjustment(a);
}

// --- keyframe ops ------------------------------------------------------------

bool Document::setParamKey(AdjustmentLayer *a, ParamId id, int frame, double value, Interp interp,
                           bool updateExisting) {
    if (!a) return false;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = a->params();
    if (!a->params().setKey(id, frame, value, interp, updateExisting)) return false;
    return pushParamEdit("Set keyframe", a, before);
}

bool Document::removeParamKey(AdjustmentLayer *a, ParamId id, int frame) {
    if (!a) return false;
    auto edit = editGuard();
    ParamBlock before = a->params();
    if (!a->params().removeKey(id, frame)) return false;
    return pushParamEdit("Remove keyframe", a, before);
}

bool Document::addParamKey(AdjustmentLayer *a, ParamId id, int frame) {
    if (!a) return false;
    frame = clampF(this, frame);
    // Read BEFORE inserting: the key captures the value the parameter already
    // had, so turning animation on never moves the picture.
    const double v = a->params().valueAt(id, frame);
    return setParamKey(a, id, frame, v, Interp::Bezier, false);
}

bool Document::toggleParamKey(AdjustmentLayer *a, ParamId id, int frame) {
    if (!a) return false;
    if (a->params().hasKey(id, frame)) return removeParamKey(a, id, frame);
    return addParamKey(a, id, frame);
}

bool Document::moveParamKey(AdjustmentLayer *a, ParamId id, int from, int to) {
    if (!a) return false;
    auto edit = editGuard();
    from = clampF(this, from);
    to = clampF(this, to);
    ParamBlock before = a->params();
    if (!a->params().moveKey(id, from, to)) return false;
    return pushParamEdit("Move keyframe", a, before);
}

bool Document::setParamKeyValue(AdjustmentLayer *a, ParamId id, int frame, double value) {
    if (!a) return false;
    auto edit = editGuard();
    ParamBlock before = a->params();
    if (!a->params().setKeyValue(id, frame, value)) return false;
    return pushParamEdit("Change keyframe", a, before);
}

bool Document::setParamKeyInterp(AdjustmentLayer *a, ParamId id, int frame, Interp i) {
    if (!a) return false;
    auto edit = editGuard();
    ParamBlock before = a->params();
    if (!a->params().setInterp(id, frame, i)) return false;
    return pushParamEdit("Keyframe interpolation", a, before);
}

bool Document::setParamKeyHandles(AdjustmentLayer *a, ParamId id, int frame, double outInfl,
                                  double outSpeed, double inInfl, double inSpeed) {
    if (!a) return false;
    auto edit = editGuard();
    ParamBlock before = a->params();
    if (!a->params().setHandles(id, frame, outInfl, outSpeed, inInfl, inSpeed)) return false;
    return pushParamEdit("Shape keyframe", a, before);
}

bool Document::setParamStaticValue(AdjustmentLayer *a, ParamId id, double value) {
    if (!a) return false;
    auto edit = editGuard();
    ParamBlock before = a->params();
    if (std::abs(a->params().staticValue(id) - value) < 1e-12) return false;
    a->params().setStaticValue(id, value);
    return pushParamEdit("Change value", a, before);
}

bool Document::setParamValue(AdjustmentLayer *a, ParamId id, double value, int frame) {
    if (!a) return false;
    frame = clampF(this, frame);
    if (a->params().isAnimated(id)) {
        if (a->params().hasKey(id, frame)) return setParamKeyValue(a, id, frame, value);
        return setParamKey(a, id, frame, value, Interp::Bezier, true);
    }
    return setParamStaticValue(a, id, value);
}

bool Document::applyKeyPreset(AdjustmentLayer *a, int frame, Interp mode, bool easeIn,
                              bool easeOut) {
    if (!a) return false;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = a->params();
    for (ParamId id : a->params().keysAt(frame)) {
        a->params().setInterp(id, frame, mode);
        if (mode != Interp::Bezier) continue;
        const ScalarChannel *ch = a->params().channel(id);
        const ScalarKey *k = ch ? ch->keyAt(frame) : nullptr;
        if (!k) continue;
        // Easy Ease is exactly "zero speed on the handle that side", which is
        // how the handle is stored — no conversion from bezier control points
        // is needed or wanted.
        const double autoS = autoTangentSpeed(*ch, frame);
        a->params().setHandles(id, frame, k->outInfl, easeOut ? 0.0 : autoS, k->inInfl,
                               easeIn ? 0.0 : autoS);
    }
    return pushParamEdit("Keyframe preset", a, before);
}

AdjustmentLayer *Document::keyingTarget() {
    if (currentLayer_) return ensureAdjustmentFor(currentLayer_);
    return nullptr;
}

Rect Document::contentFrameFor(PaintLayer *target, int frame) {
    const Rect doc{0, 0, image_->width(), image_->height()};
    if (!selection_.isEmpty()) {
        const Rect b = intersectRect(selection_.bounds(), doc);
        if (!b.isEmpty()) return b;
    }
    if (target) {
        const int key = target->exposureAt(frame);
        if (key >= 0)
            if (const PaintDevice *d = target->deviceAt(key)) {
                // Pixel-precise, not the tile store's extent: a tile-snapped
                // frame would move the pivot whenever ink crosses a tile edge.
                const Rect e = intersectRect(d->contentBounds(), doc);
                if (!e.isEmpty()) return e;
            }
    }
    return doc;
}

int Document::keyCurrentLayer(int frame) {
    AdjustmentLayer *a = keyingTarget();
    if (!a) return 0;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = a->params();
    std::vector<ParamId> ids = a->params().animatedParams();
    const bool fresh = ids.empty();
    if (fresh) {
        ids = defaultKeySet();
        // Whoever keys first fixes the block's reference frame — the content,
        // so the pivot the tool seeds from it survives later edits. (Re-keying
        // an animated block never touches the frame: the stored values are
        // already normalized against it.)
        a->params().setFrame(contentFrameFor(currentLayer_, frame));
    }
    for (ParamId id : ids)
        a->params().setKey(id, frame, a->params().valueAt(id, frame), Interp::Bezier, true);
    a->params().refreshAllAutoHandles();
    if (!pushParamEdit("Add keyframe", a, before)) return 0;
    return (int)ids.size();
}

int Document::removeKeyCurrentLayer(int frame) {
    AdjustmentLayer *a = currentLayer_ ? adjustmentFor(currentLayer_) : nullptr;
    if (!a) return 0;
    auto edit = editGuard();
    frame = clampF(this, frame);
    ParamBlock before = a->params();
    const int n = a->params().removeKeysAt(frame);
    if (n <= 0) return 0;
    if (!pushParamEdit("Remove keyframe", a, before)) return 0;
    return n;
}

int Document::prevKeyCurrentLayer(int frame) {
    AdjustmentLayer *a = currentLayer_ ? adjustmentFor(currentLayer_) : nullptr;
    if (!a) return -1;
    int best = -1;
    for (ParamId id : a->params().animatedParams()) {
        const int k = a->params().prevKey(id, frame);
        if (k >= 0 && (best < 0 || k > best)) best = k;
    }
    return best;
}

int Document::nextKeyCurrentLayer(int frame) {
    AdjustmentLayer *a = currentLayer_ ? adjustmentFor(currentLayer_) : nullptr;
    if (!a) return -1;
    int best = -1;
    for (ParamId id : a->params().animatedParams()) {
        const int k = a->params().nextKey(id, frame);
        if (k >= 0 && (best < 0 || k < best)) best = k;
    }
    return best;
}

// --- recording (one undo step for a whole tool drag) -------------------------

void Document::beginParamRecord(const std::string &label, AdjustmentLayer *a) {
    if (!a) return;
    // A record may only be opened when none is in flight. Overwriting one
    // silently DROPS its before-snapshot, and the work since the last Apply
    // then merges into whatever record opens next: the user loses an undo step
    // they were told about, and the next Ctrl+Z jumps further than the gesture
    // they are undoing. Re-entering here is possible because endParamRecord
    // emits animationRecorded, whose timeline listener runs synchronously -
    // so this is a guard, not decoration.
    if (record_.active) {
        endParamRecord(); // land what is open, so its undo step is not lost
        if (record_.active)
            return;
    }
    record_ = ParamRecord{};
    record_.layer = a;
    record_.before = a->params();
    record_.label = label;
    record_.frame = currentFrame();
    record_.active = true;
}

void Document::recordParamValue(ParamId id, double value, int frame) {
    if (!record_.active || !record_.layer) return;
    auto edit = editGuard();
    frame = clampF(this, frame);
    // A fresh key auto-derives its handles; an existing one only changes value,
    // so a drag that passes over a key it already wrote does not churn its
    // curve. Same reason for the cache: a move that writes the value the key
    // already holds cannot change any projection, so it must not flush one.
    ParamBlock &b = record_.layer->params();
    // Values round-trip through float storage, so an exact == would flag every
    // rewritten key as a change and flush the frame cache per mouse-move. An
    // epsilon compare keeps a drag that rewrites the same value a true no-op.
    const bool noop = b.hasKey(id, frame) &&
                      std::abs(b.valueAt(id, double(frame)) - value) < 1e-9;
    b.setKey(id, frame, value, Interp::Bezier, true);
    if (!noop) touchAdjustment(record_.layer);
}

void Document::endParamRecord() {
    if (!record_.active || !record_.layer) {
        record_ = ParamRecord{};
        return;
    }
    auto edit = editGuard();
    AdjustmentLayer *a = record_.layer;
    ParamBlock before = record_.before;
    const std::string label = record_.label;
    record_ = ParamRecord{};
    a->params().refreshAllAutoHandles();
    ParamBlock after = a->params();
    if (before == after) return;
    undo_.pushAction(label,
                     [this, a, after]() {
                         a->params() = after;
                         touchAdjustment(a);
                     },
                     [this, a, before]() {
                         a->params() = before;
                         touchAdjustment(a);
                     });
    // The live moves flushed only the playhead frame; now that record_ is reset
    // (so touchAdjustment takes the bar path) flush the whole span once, and
    // nothing else: frames outside the bar never saw this layer.
    touchAdjustment(a);
}

void Document::cancelParamRecord() {
    if (!record_.active || !record_.layer) {
        record_ = ParamRecord{};
        return;
    }
    auto edit = editGuard();
    AdjustmentLayer *a = record_.layer;
    const ParamBlock before = record_.before;
    record_ = ParamRecord{};
    a->params() = before;
    touchAdjustment(a);
}

// --- live key editing --------------------------------------------------------

bool Document::liveEditActive(AdjustmentLayer *a) const {
    return record_.active && record_.layer == a;
}

void Document::liveSetKey(AdjustmentLayer *a, ParamId id, int frame, double value, Interp interp) {
    if (!liveEditActive(a)) return;
    frame = clampF(this, frame);
    a->params().setKey(id, frame, value, interp, /*updateExisting=*/true, /*deriveHandles=*/true);
    touchAdjustment(a);
}

void Document::liveSetKeyValue(AdjustmentLayer *a, ParamId id, int frame, double value) {
    if (!liveEditActive(a)) return;
    if (!a->params().setKeyValue(id, frame, value)) return;
    touchAdjustment(a);
}

void Document::liveMoveKey(AdjustmentLayer *a, ParamId id, int from, int to) {
    if (!liveEditActive(a)) return;
    to = clampF(this, to);
    if (!a->params().moveKey(id, from, to)) return;
    touchAdjustment(a);
}

void Document::liveSetStaticParam(AdjustmentLayer *a, ParamId id, double value) {
    if (!liveEditActive(a)) return;
    a->params().setStaticValue(id, value);
    touchAdjustment(a);
}

void Document::liveSetHandles(AdjustmentLayer *a, ParamId id, int frame, double outInfl,
                              double outSpeed, double inInfl, double inSpeed) {
    if (!liveEditActive(a)) return;
    if (!a->params().setHandles(id, frame, outInfl, outSpeed, inInfl, inSpeed)) return;
    touchAdjustment(a);
}

bool Document::liveCameraEditActive(CameraLayer *c) const {
    return camRecord_.active && camRecord_.cam == c;
}

void Document::liveMoveCameraKey(CameraLayer *c, ParamId id, int from, int to) {
    if (!liveCameraEditActive(c)) return;
    to = clampF(this, to);
    if (!c->params().moveKey(id, from, to)) return;
    touchCamera(c);
}

void Document::liveSetCameraKeyValue(CameraLayer *c, ParamId id, int frame, double value) {
    if (!liveCameraEditActive(c)) return;
    if (!c->params().setKeyValue(id, frame, value)) return;
    touchCamera(c);
}

void Document::liveSetCameraKey(CameraLayer *c, ParamId id, int frame, double value,
                                Interp interp) {
    if (!liveCameraEditActive(c)) return;
    frame = clampF(this, frame);
    c->params().setKey(id, frame, value, interp, /*updateExisting=*/true, /*deriveHandles=*/true);
    touchCamera(c);
}

void Document::liveSetCameraStaticParam(CameraLayer *c, ParamId id, double value) {
    if (!liveCameraEditActive(c)) return;
    c->params().setStaticValue(id, value);
    touchCamera(c);
}

void Document::previewCameraBar(CameraLayer *c, int keyDelta, const ParamBlock &base) {
    if (!c) return;
    auto edit = editGuard();
    c->params() = base;
    if (keyDelta != 0) c->params().shiftKeys(0, keyDelta, timeline_.frameCount() - 1);
    c->params().refreshAllAutoHandles();
    touchCamera(c);
}

void Document::commitCameraBar(CameraLayer *c, const ParamBlock &base) {
    if (!c) return;
    // The drag ran inside a camera record (beginCameraRecord): ending it
    // pushes exactly one undo step (before == base). If no record is open
    // (programmatic move), fall back to a single snapshot edit.
    if (camRecord_.active && camRecord_.cam == c) {
        endCameraRecord();
        return;
    }
    auto edit = editGuard();
    ParamBlock before = base;
    ParamBlock after = c->params();
    if (before == after) return;
    undo_.pushAction("Move camera keyframes",
                     [this, c, after]() { c->params() = after; touchCamera(c); },
                     [this, c, before]() { c->params() = before; touchCamera(c); });
    touchCamera(c);
}

void Document::abortCameraBar(CameraLayer *c, const ParamBlock &base) {
    if (!c) return;
    if (camRecord_.active && camRecord_.cam == c) {
        cancelCameraRecord();
        return;
    }
    c->params() = base;
    touchCamera(c);
}

// The one place a normalised ease becomes stored handles. Shared by the live
// (drag) and recorded (preset click) paths so the two can never drift.
namespace {
bool easeHandles(const ScalarKey &ka, const ScalarKey &kb, double x1, double y1, double x2,
                 double y2, double *inflA, double *speedA, double *inflB, double *speedB) {
    const double dF = double(kb.frame - ka.frame);
    if (dF <= 0.0) return false;
    const double dV = double(kb.value) - double(ka.value);
    // A segment with no height has no progress axis: its handles' TIMING is still
    // real, but their slope is meaningless, so the stored speed (zero) is kept
    // rather than invented from a divide-by-nothing.
    const bool flat = std::abs(dV) < 1e-6;
    *inflA = std::clamp(x1, 0.01, 1.0);
    *inflB = std::clamp(1.0 - x2, 0.01, 1.0);
    // Deriving both from buildSegment's own definitions (InkKeyframe.cpp):
    //   p1y = v0 + outSpeed * inflA * dF        -> y1 =  speedA * inflA * dF / dV
    //   p2y = v1 - inSpeed  * inflB * dF        -> y2 = 1 - speedB * inflB * dF / dV
    // The B handle is measured BACK from key B, hence (1 - y2): getting this
    // sign wrong makes a stored ease read back as its own mirror image.
    *speedA = flat ? double(ka.outSpeed) : (y1 * dV) / (*inflA * dF);
    *speedB = flat ? double(kb.inSpeed) : ((1.0 - y2) * dV) / (*inflB * dF);
    return true;
}
} // namespace

bool Document::liveSetSegmentEase(AdjustmentLayer *a, ParamId id, int frameA, double x1, double y1,
                                  double x2, double y2, int mirrorEnd) {
    if (!liveEditActive(a) || !a) return false;
    const ScalarChannel *ch = a->params().channel(id);
    if (!ch) return false;
    const int frameB = ch->nextKey(frameA);
    if (frameB < 0) return false;
    const ScalarKey *ka = ch->keyAt(frameA);
    const ScalarKey *kb = ch->keyAt(frameB);
    if (!ka || !kb) return false;
    double iA, sA, iB, sB;
    if (!easeHandles(*ka, *kb, x1, y1, x2, y2, &iA, &sA, &iB, &sB)) return false;
    // A stored handle speed IS the tangent slope at that key (value units per
    // frame), so mirroring a tangent is a straight copy, not a conversion.
    const double inA = mirrorEnd == 0 ? iA : ka->inInfl;
    const double spA = mirrorEnd == 0 ? sA : ka->inSpeed;
    const double outB = mirrorEnd == 1 ? iB : kb->outInfl;
    const double spB = mirrorEnd == 1 ? sB : kb->outSpeed;
    a->params().setHandles(id, frameA, iA, sA, inA, spA);
    a->params().setHandles(id, frameB, outB, spB, iB, sB);
    touchAdjustment(a);
    return true;
}

void Document::liveSetSegmentInterp(AdjustmentLayer *a, ParamId id, int frameA, Interp i) {
    if (!liveEditActive(a) || !a) return;
    a->params().setInterp(id, frameA, i);
    touchAdjustment(a);
}

bool Document::liveSetCameraSegmentEase(CameraLayer *c, ParamId id, int frameA, double x1,
                                        double y1, double x2, double y2, int mirrorEnd) {
    if (!liveCameraEditActive(c) || !c) return false;
    const ScalarChannel *ch = c->params().channel(id);
    if (!ch) return false;
    const int frameB = ch->nextKey(frameA);
    if (frameB < 0) return false;
    const ScalarKey *ka = ch->keyAt(frameA);
    const ScalarKey *kb = ch->keyAt(frameB);
    if (!ka || !kb) return false;
    double iA, sA, iB, sB;
    if (!easeHandles(*ka, *kb, x1, y1, x2, y2, &iA, &sA, &iB, &sB)) return false;
    const double inA = mirrorEnd == 0 ? iA : ka->inInfl;
    const double spA = mirrorEnd == 0 ? sA : ka->inSpeed;
    const double outB = mirrorEnd == 1 ? iB : kb->outInfl;
    const double spB = mirrorEnd == 1 ? sB : kb->outSpeed;
    c->params().setHandles(id, frameA, iA, sA, inA, spA);
    c->params().setHandles(id, frameB, outB, spB, iB, sB);
    touchCamera(c);
    return true;
}

void Document::liveSetCameraSegmentInterp(CameraLayer *c, ParamId id, int frameA, Interp i) {
    if (!liveCameraEditActive(c) || !c) return;
    c->params().setInterp(id, frameA, i);
    touchCamera(c);
}

bool Document::addBlankKey(PaintLayer *layer, int frame) {
    auto edit = editGuard();
    if (!layer) return false;
    frame = clampF(this, frame);
    if (layer->hasKeyAt(frame)) return false;
    layer->setKeyAt(frame, true);
    layer->setSpanEnd(frame, frame); // a new cell is one frame long
    touchLayer(layer, frame);
    return true;
}

bool Document::insertCell(PaintLayer *layer, int atFrame) {
    auto edit = editGuard();
    if (!layer) return false;
    atFrame = clampF(this, atFrame);
    // A frame inside a cell's hold means "after that cell": inserting in the
    // middle would split the exposure and leave a blank gap behind.
    const int cover = layer->exposureAt(atFrame);
    if (cover >= 0 && cover != atFrame) atFrame = cellEnd(layer, cover) + 1;
    // The ripple must never drop a key: make room at the end of the project when
    // the key sitting on the last frame has to move, or the new cell lands past it.
    const int last = timeline_.frameCount() - 1;
    const auto keys = layer->keys();
    if ((!keys.empty() && keys.back() >= atFrame && keys.back() >= last) || atFrame > last)
        if (!setFrameCount(timeline_.frameCount() + 1)) return false;
    layer->shiftFrom(atFrame, +1, timeline_.frameCount() - 1);
    if (layer->hasKeyAt(atFrame)) return false; // no room even after the ripple
    layer->setKeyAt(atFrame, true);
    layer->setSpanEnd(atFrame, atFrame); // a new cell is one frame long
    touchLayer(layer, atFrame);
    return true;
}

bool Document::duplicateKey(PaintLayer *layer, int srcFrame, int dstFrame, bool overwrite) {
    auto edit = editGuard();
    if (!layer) return false;
    dstFrame = clampF(this, dstFrame);
    const int src = layer->exposureAt(srcFrame);
    if (src < 0) return false;
    if (layer->hasKeyAt(dstFrame) && !overwrite) return false;
    const bool open = layer->spanOpen(src);
    const int len = cellLength(layer, src);
    layer->setKeyAt(dstFrame, true);
    layer->copyDeviceContent(src, dstFrame);
    layer->setSpanEnd(dstFrame, open ? kOpenEnd : dstFrame + len - 1);
    touchLayer(layer, dstFrame);
    return true;
}

int Document::removeKeys(PaintLayer *layer, const std::vector<int> &frames, bool pull) {
    auto edit = editGuard();
    if (!layer || frames.empty()) return 0;
    int lo = *std::min_element(frames.begin(), frames.end());
    int n = 0;
    for (int f : frames) {
        if (layer->hasKeyAt(f)) { layer->setKeyAt(f, false); n++; }
    }
    if (pull && n > 0) {
        // pull later keys left by n, with content
        auto keys = layer->keys();
        std::sort(keys.begin(), keys.end());
        for (int k : keys) {
            if (k <= lo) continue;
            int t = std::max(lo, k - n);
            if (t == k) continue;
            if (layer->hasKeyAt(t)) continue; // never clobber on pull
            layer->moveDeviceContent(k, t, false);
        }
    }
    touchLayer(layer, lo);
    return n;
}

int Document::shiftKeys(PaintLayer *layer, int fromFrame, int delta) {
    auto edit = editGuard();
    if (!layer || delta == 0) return 0;
    const int moved = layer->shiftFrom(fromFrame, delta, timeline_.frameCount() - 1);
    if (moved) touchLayer(layer, std::max(0, fromFrame));
    return moved;
}

// "Insert hold" grows the cell exposing `frame`; it never touches its
// neighbours (spans are independent).
int Document::insertHold(PaintLayer *layer, int frame, int count) {
    auto edit = editGuard();
    if (!layer || count <= 0) return 0;
    const int key = layer->exposureAt(frame);
    if (key < 0) return 0;
    const int before = cellLength(layer, key);
    if (!layer->spanOpen(key)) setCellEnd(layer, key, cellEnd(layer, key) + count);
    return cellLength(layer, key) - before;
}

int Document::removeHold(PaintLayer *layer, int frame, int count) {
    auto edit = editGuard();
    if (!layer || count <= 0) return 0;
    const int key = layer->exposureAt(frame);
    if (key < 0) return 0;
    const int before = cellLength(layer, key);
    if (!layer->spanOpen(key)) {
        const int target = std::max(key, cellEnd(layer, key) - count);
        setCellEnd(layer, key, target);
    }
    return before - cellLength(layer, key);
}

int Document::mirrorKeys(PaintLayer *layer, int lo, int hi) {
    auto edit = editGuard();
    if (!layer || hi <= lo) return 0;
    lo = clampF(this, lo);
    hi = clampF(this, hi);
    std::vector<int> in;
    for (int k : layer->keys())
        if (k >= lo && k <= hi) in.push_back(k);
    if (in.size() < 2) return 0;
    // snapshot the whole cell (stack + exposure), clear, re-place mirrored
    struct Snapshot { std::shared_ptr<Cell> cell; int length; };
    std::map<int, Snapshot> snap;
    for (int k : in) {
        auto c = std::make_shared<Cell>();
        if (const Cell *src = layer->cellAt(k)) c->copyFrom(*src);
        snap[k] = {c, cellLength(layer, k)};
        layer->setKeyAt(k, false);
    }
    for (auto &[k, s] : snap) {
        const int t = lo + (hi - k);
        layer->setKeyAt(t, true);
        if (Cell *dst = layer->cellAt(t, false)) dst->copyFrom(*s.cell);
        layer->setSpanEnd(t, t + s.length - 1);
    }
    touchLayer(layer, lo);
    return (int)in.size();
}

bool Document::copyKeys(PaintLayer *layer, const std::vector<int> &frames) {
    clipboard_ = {};
    if (!layer || frames.empty()) return false;
    std::vector<int> ks;
    for (int f : frames)
        if (layer->hasKeyAt(f)) ks.push_back(f);
    if (ks.empty()) return false;
    std::sort(ks.begin(), ks.end());
    clipboard_.valid = true;
    clipboard_.srcTrackId = layer->stableId();
    for (int k : ks) {
        auto c = std::make_shared<Cell>();
        if (const Cell *src = layer->cellAt(k)) c->copyFrom(*src);
        clipboard_.keys.push_back({k - ks.front(), cellLength(layer, k), c});
    }
    return true;
}

int Document::pasteKeys(PaintLayer *layer, int dstFrame, bool overwrite) {
    auto edit = editGuard();
    if (!layer || !clipboard_.valid || clipboard_.keys.empty()) return 0;
    const int maxF = timeline_.frameCount() - 1;
    int pasted = 0;
    int firstTarget = -1;
    for (const auto &e : clipboard_.keys) {
        const int t = dstFrame + e.offset;
        if (t < 0 || t > maxF) continue;
        if (layer->hasKeyAt(t) && !overwrite) continue;
        layer->setKeyAt(t, true);
        layer->setSpanEnd(t, t + std::max(0, e.length - 1));
        // clone-on-paste: the clipboard keeps its own copy, so pasting the
        // same cells twice never links two drawings to one set of pixels
        if (Cell *dst = layer->cellAt(t, false)) {
            if (e.cell) dst->copyFrom(*e.cell);
        }
        if (firstTarget < 0) firstTarget = t;
        pasted++;
    }
    if (pasted) touchLayer(layer, firstTarget >= 0 ? firstTarget : dstFrame);
    return pasted;
}

// ------------------------------------------------- per-cell layer stacks
//
// The panel edits the stack of the cell exposed at the playhead on the
// current track. Every structural op records a "slot" (parent + index) per
// moved layer, so undo/redo restores the exact stack — layers are held by
// shared_ptr in the closures, so an undone delete brings the pixels back too.

Document::CellRef Document::currentCellRef(bool createIfMissing) {
    auto edit = editGuard();
    CellRef r;
    r.track = currentLayer_;
    if (!r.track) return r;
    int key = r.track->exposureAt(currentFrame());
    if (key < 0) {
        // Nothing exposed here: a gap, or an empty track. Respect auto-key
        // exactly like drawing does (the panel shows the block hint).
        if (!createIfMissing || !autoKey_) return r;
        r.track->setKeyAt(currentFrame(), true);
        r.track->setSpanEnd(currentFrame(), currentFrame());
        touchLayer(r.track, currentFrame());
        key = currentFrame();
    }
    r.key = key;
    r.cell = r.track->cellAt(key, true);
    return r;
}

Cell *Document::currentCell() { return currentCellRef(false).cell; }

Cell *Document::currentCellOrCreate() { return currentCellRef(true).cell; }

int Document::currentCellKey() const {
    return currentLayer_ ? currentLayer_->exposureAt(currentFrame()) : -1;
}

CellLayer *Document::currentCellLayer() {
    Cell *c = currentCell();
    return c ? c->activePaintLayer() : nullptr;
}

const PaintDevice *Document::compositeOfCurrentCell() {
    // The working layer may be an ADJUSTMENT layer (currentLayer_ == nullptr):
    // then the ink to grab lives in the layer the adjustment drives, composited
    // the way it is EXPOSED at the playhead — so the whole-stroke lasso keeps
    // grabbing whole strokes while an adjustment layer is selected, instead of
    // silently degrading to a plain area selection.
    PaintLayer *track = currentLayer_;
    int key = -1;
    if (track) {
        key = track->exposureAt(currentFrame());
    } else {
        track = currentLayer_;
        if (!track) return nullptr;
        key = track->exposureAt(currentFrame());
    }
    Cell *c = track->cellAt(key, true);
    if (!c) return nullptr;
    // Fill the scratch now, on demand: one composite per lasso release is the
    // whole cost, and a cached-but-stale device (the panel edits layers without
    // any notification seam here) is exactly what a hit-test must never read.
    // cellComposite() clears first and skips track chrome — the lasso selects
    // cell content as the stack composites it, not as the animation shows it.
    image_->cellComposite(*track, key, cellScratch_);
    return &cellScratch_;
}

void Document::setCellLayerActive(CellLayer *l) {
    if (!l || !l->isPaint()) return;
    if (Cell *c = currentCell())
        if (c->find(l->id())) c->setActiveId(l->id());
}

bool Document::paintBlockedByLock() {
    if (currentLayer_ && currentLayer_->locked()) return true;
    Cell *c = currentCell();
    if (!c) return false;
    CellLayer *l = c->activePaintLayer();
    return l && l->locked();
}

void Document::touchCell(int key) {
    cache_->invalidate(std::max(0, key));
    cache_->requestRange(timeline_.currentFrame());
}

Document::CellSlot Document::slotOfCellLayer(PaintLayer *track, int key, Cell *cell,
                                             const CellLayer *l) const {
    CellSlot s;
    s.trackId = track ? track->stableId() : std::string();
    s.key = key;
    s.layerId = l ? l->id() : 0;
    if (cell && l) cell->slotOf(l, s.parentId, s.index);
    return s;
}

std::shared_ptr<CellLayer> Document::cellLayerById(const CellSlot &slot) const {
    PaintLayer *t = const_cast<Document *>(this)->findTrack(slot.trackId);
    if (!t) return nullptr;
    const Cell *c = t->cellAt(slot.key);
    return c ? c->findShared(slot.layerId) : nullptr;
}

bool Document::placeCellLayer(const CellSlot &slot, std::shared_ptr<CellLayer> layer) {
    auto edit = editGuard();
    if (!layer) return false;
    PaintLayer *t = findTrack(slot.trackId);
    if (!t) return false;
    Cell *c = t->cellAt(slot.key, false);
    if (!c) return false;
    c->detach(layer->id()); // no-op when it is not in the tree
    if (!c->insertAt(layer, slot.parentId, slot.index))
        if (!c->insertAt(layer, 0, slot.index)) return false;
    touchCell(slot.key);
    return true;
}

bool Document::detachCellLayerAt(const CellSlot &slot) {
    auto edit = editGuard();
    PaintLayer *t = findTrack(slot.trackId);
    if (!t) return false;
    Cell *c = t->cellAt(slot.key, false);
    if (!c) return false;
    const bool ok = c->detach(slot.layerId) != nullptr;
    if (ok) touchCell(slot.key);
    return ok;
}

CellLayer *Document::addCellLayer(const std::string &name, CellLayer *above) {
    auto edit = editGuard();
    CellRef r = currentCellRef(true);
    if (!r.cell) return nullptr;
    if (above && r.cell->find(above->id()) != above) above = nullptr;
    CellLayer *l = r.cell->addLayer(name, above);
    if (!l) return nullptr;
    r.cell->setActiveId(l->id());
    touchCell(r.key);
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    auto held = r.cell->findShared(l->id());
    undo_.pushAction(
        "Add layer", [this, slot, held] { placeCellLayer(slot, held); },
        [this, slot] { detachCellLayerAt(slot); });
    return l;
}

CellLayer *Document::addCellGroup(const std::string &name, CellLayer *above) {
    auto edit = editGuard();
    CellRef r = currentCellRef(true);
    if (!r.cell) return nullptr;
    if (above && r.cell->find(above->id()) != above) above = nullptr;
    CellLayer *g = r.cell->addGroup(name, above);
    if (!g) return nullptr;
    touchCell(r.key);
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, g);
    auto held = r.cell->findShared(g->id());
    undo_.pushAction(
        "Add group", [this, slot, held] { placeCellLayer(slot, held); },
        [this, slot] { detachCellLayerAt(slot); });
    return g;
}

CellLayer *Document::duplicateCellLayer(CellLayer *l) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return nullptr;
    CellLayer *copy = r.cell->duplicate(l);
    if (!copy) return nullptr;
    if (copy->isPaint()) r.cell->setActiveId(copy->id());
    touchCell(r.key);
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, copy);
    auto held = r.cell->findShared(copy->id());
    undo_.pushAction(
        "Duplicate layer", [this, slot, held] { placeCellLayer(slot, held); },
        [this, slot] { detachCellLayerAt(slot); });
    return copy;
}

bool Document::deleteCellLayer(CellLayer *l) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    if (!r.cell->canRemove(l)) return false; // keep one paint layer per cell
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    auto held = r.cell->detach(l->id());
    if (!held) return false;
    touchCell(r.key);
    undo_.pushAction(
        "Delete layer", [this, slot] { detachCellLayerAt(slot); },
        [this, slot, held] { placeCellLayer(slot, held); });
    return true;
}

bool Document::moveCellLayer(CellLayer *l, int delta) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l || delta == 0) return false;
    const CellSlot before = slotOfCellLayer(r.track, r.key, r.cell, l);
    if (!r.cell->move(l, delta)) return false;
    touchCell(r.key);
    const CellSlot after = slotOfCellLayer(r.track, r.key, r.cell, l);
    auto held = r.cell->findShared(l->id());
    undo_.pushAction(
        "Reorder layer", [this, after, held] { placeCellLayer(after, held); },
        [this, before, held] { placeCellLayer(before, held); });
    return true;
}

bool Document::moveCellLayerToGroup(CellLayer *l, CellLayer *group) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    if (group && r.cell->find(group->id()) != group) return false;
    const CellSlot before = slotOfCellLayer(r.track, r.key, r.cell, l);
    if (!r.cell->moveToGroup(l, group)) return false;
    touchCell(r.key);
    const CellSlot after = slotOfCellLayer(r.track, r.key, r.cell, l);
    auto held = r.cell->findShared(l->id());
    undo_.pushAction(
        "Move layer", [this, after, held] { placeCellLayer(after, held); },
        [this, before, held] { placeCellLayer(before, held); });
    return true;
}

bool Document::moveCellLayerTo(CellLayer *l, CellLayer *group, int index) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    if (group && (group->isPaint() || r.cell->find(group->id()) != group)) return false;
    const CellSlot before = slotOfCellLayer(r.track, r.key, r.cell, l);
    const uint64_t targetParent = group ? group->id() : 0;
    int idx = index;
    // Detaching the layer shifts everything above it down by one.
    if (before.parentId == targetParent && idx > before.index) idx -= 1;
    auto held = r.cell->detach(l->id());
    if (!held) return false;
    if (!r.cell->insertAt(held, targetParent, idx)) r.cell->insertAt(held, 0, 0);
    touchCell(r.key);
    const CellSlot after = slotOfCellLayer(r.track, r.key, r.cell, l);
    undo_.pushAction(
        "Move layer", [this, after, held] { placeCellLayer(after, held); },
        [this, before, held] { placeCellLayer(before, held); });
    return true;
}

CellLayer *Document::groupCellLayers(const std::vector<CellLayer*> &sel, const std::string &name) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell) return nullptr;
    std::vector<CellLayer*> valid;
    for (auto *l : sel)
        if (l && r.cell->find(l->id()) == l) valid.push_back(l);
    if (valid.size() < 2) return nullptr; // grouping one layer changes nothing
    std::vector<CellSlot> before;
    for (auto *l : valid) before.push_back(slotOfCellLayer(r.track, r.key, r.cell, l));
    CellLayer *g = r.cell->group(valid, name);
    if (!g) return nullptr;
    touchCell(r.key);
    const CellSlot gSlot = slotOfCellLayer(r.track, r.key, r.cell, g);
    auto heldGroup = r.cell->findShared(g->id());
    std::vector<std::shared_ptr<CellLayer>> kids = g->children(); // tree order
    undo_.pushAction(
        "Group layers",
        [this, gSlot, heldGroup, kids] {
            placeCellLayer(gSlot, heldGroup);
            for (size_t i = 0; i < kids.size(); ++i)
                placeCellLayer(CellSlot{gSlot.trackId, gSlot.key, kids[i]->id(), heldGroup->id(),
                                        (int)i},
                               kids[i]);
        },
        [this, before, heldGroup] {
            for (auto &s : before) placeCellLayer(s, cellLayerById(s));
            detachCellLayerAt(CellSlot{before[0].trackId, before[0].key, heldGroup->id(), 0, 0});
        });
    return g;
}

bool Document::ungroupCellLayer(CellLayer *g) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !g || g->isPaint() || r.cell->find(g->id()) != g) return false;
    const CellSlot gSlot = slotOfCellLayer(r.track, r.key, r.cell, g);
    auto heldGroup = r.cell->findShared(g->id());
    std::vector<std::shared_ptr<CellLayer>> kids = g->children();
    std::vector<CellSlot> kidSlots;
    for (auto &k : kids) kidSlots.push_back(slotOfCellLayer(r.track, r.key, r.cell, k.get()));
    if (!r.cell->ungroup(g)) return false;
    touchCell(r.key);
    undo_.pushAction(
        "Ungroup layers",
        [this, gSlot, heldGroup, kids] {
            placeCellLayer(gSlot, heldGroup);
            for (size_t i = 0; i < kids.size(); ++i)
                placeCellLayer(CellSlot{gSlot.trackId, gSlot.key, kids[i]->id(), heldGroup->id(),
                                        (int)i},
                               kids[i]);
        },
        [this, kidSlots, heldGroup] {
            for (size_t i = 0; i < kidSlots.size(); ++i)
                placeCellLayer(kidSlots[i], cellLayerById(kidSlots[i]));
            detachCellLayerAt(CellSlot{kidSlots.empty() ? std::string() : kidSlots[0].trackId,
                                       kidSlots.empty() ? -1 : kidSlots[0].key, heldGroup->id(), 0,
                                       0});
        });
    return true;
}

bool Document::renameCellLayer(CellLayer *l, const std::string &name) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l || name.empty()) return false;
    const std::string old = l->name();
    if (old == name) return false;
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    l->setName(name);
    touchCell(r.key);
    auto set = [this, slot](const std::string &n) {
        if (auto held = cellLayerById(slot)) {
            held->setName(n);
            touchCell(slot.key);
        }
    };
    undo_.pushAction("Rename layer", [set, name] { set(name); }, [set, old] { set(old); });
    return true;
}

bool Document::setCellLayerVisible(CellLayer *l, bool v) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    const bool old = l->visible();
    if (old == v) return false;
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    l->setVisible(v);
    touchCell(r.key);
    auto set = [this, slot](bool on) {
        if (auto held = cellLayerById(slot)) {
            held->setVisible(on);
            touchCell(slot.key);
        }
    };
    undo_.pushAction("Toggle layer", [set, v] { set(v); }, [set, old] { set(old); });
    return true;
}

bool Document::setCellLayerLocked(CellLayer *l, bool v) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    const bool old = l->locked();
    if (old == v) return false;
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    l->setLocked(v);
    auto set = [this, slot](bool on) {
        if (auto held = cellLayerById(slot)) held->setLocked(on);
    };
    undo_.pushAction("Lock layer", [set, v] { set(v); }, [set, old] { set(old); });
    return true;
}

bool Document::setCellLayerOpacity(CellLayer *l, float o) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    const float old = l->opacity();
    if (std::abs(old - o) < 0.0005f) return false;
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    l->setOpacity(o);
    touchCell(r.key);
    auto set = [this, slot](float v) {
        if (auto held = cellLayerById(slot)) {
            held->setOpacity(v);
            touchCell(slot.key);
        }
    };
    undo_.pushAction("Layer opacity", [set, o] { set(o); }, [set, old] { set(old); });
    return true;
}

bool Document::setCellLayerBlend(CellLayer *l, BlendMode b) {
    auto edit = editGuard();
    CellRef r = currentCellRef(false);
    if (!r.cell || !l || r.cell->find(l->id()) != l) return false;
    const BlendMode old = l->blend();
    if (old == b) return false;
    const CellSlot slot = slotOfCellLayer(r.track, r.key, r.cell, l);
    l->setBlend(b);
    touchCell(r.key);
    auto set = [this, slot](BlendMode v) {
        if (auto held = cellLayerById(slot)) {
            held->setBlend(v);
            touchCell(slot.key);
        }
    };
    undo_.pushAction("Layer blend", [set, b] { set(b); }, [set, old] { set(old); });
    return true;
}

} // namespace ink
