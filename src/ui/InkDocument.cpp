#include "ui/InkDocument.h"
#include <algorithm>
#include <map>

namespace ink {

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
    cache_ = std::make_unique<FrameCache>(64, 2);
    cache_->setRenderFn([this](int frame) {
        auto out = std::make_shared<PaintDevice>();
        image_->project(frame, *out);
        return out;
    });
}

GroupLayer *Document::createFolder(const std::string &name) {
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
    GroupLayer *folder = currentFolder_ ? currentFolder_ : ensureFolder();
    PaintLayer *l = insertAbove(image_.get(), folder, currentLayer_, name);
    currentFolder_ = folder;
    currentLayer_ = l;
    touchLayer(l, currentFrame());
    return l;
}

GroupLayer *Document::groupSelected(const std::vector<PaintLayer*> &sel, const std::string &name) {
    GroupLayer *g = image_->groupTracks(sel, name);
    if (!g) return nullptr;
    currentFolder_ = g;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(timeline_.currentFrame());
    return g;
}

bool Document::ungroupFolder(GroupLayer *g) {
    if (!image_->ungroupFolder(g)) return false;
    if (currentFolder_ == g) currentFolder_ = nullptr;
    cache_->invalidateAll();
    syncTimelineFromImage();
    cache_->requestRange(timeline_.currentFrame());
    return true;
}

bool Document::moveTrackToFolder(PaintLayer *l, GroupLayer *to) {
    if (!image_->moveTrack(l, to)) return false;
    cache_->invalidateAll();
    syncTimelineFromImage();
    return true;
}

bool Document::deleteFolder(GroupLayer *g) {
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

bool Document::moveTrackInFolder(PaintLayer *l, int delta) {
    if (!l || delta == 0) return false;
    GroupLayer *folder = image_->findFolderOf(l);
    if (!folder) return false;
    auto &ch = folder->children();
    int at = -1;
    for (size_t i = 0; i < ch.size(); ++i)
        if (ch[i].get() == static_cast<Layer*>(l)) { at = (int)i; break; }
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
    if (!src) return nullptr;
    GroupLayer *folder = image_->findFolderOf(src);
    if (!folder) folder = ensureFolder();
    PaintLayer *dst = insertAbove(image_.get(), folder, src, src->name() + " copy");
    dst->setVisible(src->visible());
    dst->setLocked(false);
    dst->setOpacity(src->opacity());
    dst->setBlend(src->blend());
    for (int k : src->keys()) dst->setKeyAt(k, true);
    // real content + exposure copy
    for (int k : src->keys()) {
        dst->setSpanEnd(k, src->spanEnd(k));
        if (const PaintDevice *sdev = src->deviceAt(k, false)) {
            if (PaintDevice *ddev = dst->deviceAt(k, false)) {
                ddev->clear();
                if (!sdev->extent().isEmpty()) ddev->blit(*sdev, 0, 0);
            }
        }
    }
    currentLayer_ = dst;
    currentFolder_ = folder;
    touchLayer(dst, currentFrame());
    return dst;
}

bool Document::deleteTrack(PaintLayer *layer) {
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
        for (auto &t : f.tracks) { thumbs[t.layerId] = t.showThumbs; overrides[t.layerId] = t.colorOverride; }
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
            if (c->type() != LayerType::Paint) continue;
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
                row->showThumbs = (itT != thumbs.end()) ? itT->second : (row == &timeline_.folder(fid)->tracks.front());
                auto itO = overrides.find(pl->stableId());
                row->colorOverride = (itO != overrides.end()) ? itO->second : -1;
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
}

bool Document::setFrameCount(int n) {
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
    if (length < 1) length = 1;
    return setCellEnd(layer, start, start + length - 1);
}

int Document::extendCellToNext(PaintLayer *layer, int start) {
    if (!layer || !layer->hasKeyAt(start)) return -1;
    int target = timeline_.frameCount() - 1;
    for (int k : layer->keys())
        if (k > start) { target = k - 1; break; }
    setCellEnd(layer, start, target);
    return target;
}

int Document::extendCellToEnd(PaintLayer *layer, int start) {
    if (!layer || !layer->hasKeyAt(start)) return -1;
    layer->setSpanEnd(start, kOpenEnd);
    touchLayer(layer, start);
    return timeline_.frameCount() - 1;
}

bool Document::moveCellStart(PaintLayer *layer, int start, int newStart) {
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
    if (!layer) return nullptr;
    const int key = layer->exposureAt(frame);
    if (key < 0) {
        // Auto-key like Krita/CSP: nothing exposed here, so a new one-frame
        // cell is created at this frame.
        layer->setKeyAt(frame, true);
        layer->setSpanEnd(frame, frame);
        touchLayer(layer, frame);
        return layer->deviceAt(frame, true);
    }
    return layer->deviceAt(key, true);
}

void Document::touchLayer(PaintLayer *layer, int frame) {
    (void)layer;
    cache_->invalidate(frame);
    syncTimelineFromImage();
    cache_->requestRange(timeline_.currentFrame());
}

PaintLayer *Document::findTrack(const std::string &stableId) {
    std::vector<PaintLayer*> layers;
    image_->collectPaintLayers(layers);
    for (auto *l : layers)
        if (l->stableId() == stableId) return l;
    return nullptr;
}

static int clampF(Document *d, int f) {
    return std::clamp(f, 0, d->timeline().frameCount() - 1);
}

bool Document::addBlankKey(PaintLayer *layer, int frame) {
    if (!layer) return false;
    frame = clampF(this, frame);
    if (layer->hasKeyAt(frame)) return false;
    layer->setKeyAt(frame, true);
    layer->setSpanEnd(frame, frame); // a new cell is one frame long
    touchLayer(layer, frame);
    return true;
}

bool Document::duplicateKey(PaintLayer *layer, int srcFrame, int dstFrame, bool overwrite) {
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
    if (!layer || delta == 0) return 0;
    const int moved = layer->shiftFrom(fromFrame, delta, timeline_.frameCount() - 1);
    if (moved) touchLayer(layer, std::max(0, fromFrame));
    return moved;
}

// "Insert hold" grows the cell exposing `frame`; it never touches its
// neighbours (spans are independent).
int Document::insertHold(PaintLayer *layer, int frame, int count) {
    if (!layer || count <= 0) return 0;
    const int key = layer->exposureAt(frame);
    if (key < 0) return 0;
    const int before = cellLength(layer, key);
    if (!layer->spanOpen(key)) setCellEnd(layer, key, cellEnd(layer, key) + count);
    return cellLength(layer, key) - before;
}

int Document::removeHold(PaintLayer *layer, int frame, int count) {
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
    if (!layer || hi <= lo) return 0;
    lo = clampF(this, lo);
    hi = clampF(this, hi);
    std::vector<int> in;
    for (int k : layer->keys())
        if (k >= lo && k <= hi) in.push_back(k);
    if (in.size() < 2) return 0;
    // snapshot content + exposure, clear, re-place mirrored
    struct Snapshot { std::shared_ptr<PaintDevice> dev; int length; };
    std::map<int, Snapshot> snap;
    for (int k : in) {
        auto d = std::make_shared<PaintDevice>();
        if (const PaintDevice *src = layer->deviceAt(k, false))
            if (!src->extent().isEmpty()) d->blit(*src, 0, 0);
        snap[k] = {d, cellLength(layer, k)};
        layer->setKeyAt(k, false);
    }
    for (auto &[k, s] : snap) {
        const int t = lo + (hi - k);
        layer->setKeyAt(t, true);
        if (!s.dev->extent().isEmpty()) {
            if (PaintDevice *dst = layer->deviceAt(t, false))
                dst->blit(*s.dev, 0, 0);
        }
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
        auto d = std::make_shared<PaintDevice>();
        if (const PaintDevice *src = layer->deviceAt(k, false))
            if (!src->extent().isEmpty()) d->blit(*src, 0, 0);
        clipboard_.keys.push_back({k - ks.front(), cellLength(layer, k), d});
    }
    return true;
}

int Document::pasteKeys(PaintLayer *layer, int dstFrame, bool overwrite) {
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
        if (!e.device->extent().isEmpty()) {
            if (PaintDevice *dst = layer->deviceAt(t, false)) {
                dst->clear();
                dst->blit(*e.device, 0, 0);
            }
        }
        if (firstTarget < 0) firstTarget = t;
        pasted++;
    }
    if (pasted) touchLayer(layer, firstTarget >= 0 ? firstTarget : dstFrame);
    return pasted;
}

} // namespace ink
