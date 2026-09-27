#include "core/InkImage.h"
#include <algorithm>
#include <functional>
#include <set>
#include <utility>

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

namespace {

// Composite a cell's layer stack (bottom..top). Chrome (opacity/blend) is
// applied per layer; a group with its own chrome is isolated through a scratch
// device so the group composites as one unit. Default layers (opacity 1,
// Normal) take the direct blit path, which is exactly what a single-device
// cell did before stacks existed.
void compositeStack(const std::vector<std::shared_ptr<CellLayer>> &layers, PaintDevice &out) {
    for (auto &l : layers) {
        if (!l->visible() || l->opacity() <= 0.0f) continue;
        const bool isolated = l->opacity() < 1.0f || l->blend() != BlendMode::Normal;
        if (l->isPaint()) {
            const PaintDevice *d = l->device();
            if (!d) continue;
            // No extent() pre-check here: blit() probes the source extent
            // itself and returns immediately on empty content, so a pre-check
            // would scan + lock every layer's tiles twice per frame.
            out.blit(*d, 0, 0, isolated ? l->opacity() : 1.0f,
                     isolated ? toOp(l->blend()) : CompositeOp::Normal);
        } else {
            if (l->children().empty()) continue;
            if (!isolated) {
                compositeStack(l->children(), out);
                continue;
            }
            PaintDevice scratch;
            compositeStack(l->children(), scratch);
            if (scratch.extent().isEmpty()) continue;
            out.blit(scratch, 0, 0, l->opacity(), toOp(l->blend()));
        }
    }
}

void compositeStackRect(const std::vector<std::shared_ptr<CellLayer>> &layers, const Rect &rect,
                        PaintDevice &out) {
    for (auto &l : layers) {
        if (!l->visible() || l->opacity() <= 0.0f) continue;
        const bool isolated = l->opacity() < 1.0f || l->blend() != BlendMode::Normal;
        if (l->isPaint()) {
            const PaintDevice *d = l->device();
            if (!d) continue;
            Rect sub = intersectRect(rect, d->extent());
            if (sub.isEmpty()) continue;
            out.blitRect(*d, sub, sub.x, sub.y, isolated ? l->opacity() : 1.0f,
                         isolated ? toOp(l->blend()) : CompositeOp::Normal);
        } else {
            if (l->children().empty()) continue;
            if (!isolated) {
                compositeStackRect(l->children(), rect, out);
                continue;
            }
            PaintDevice scratch;
            compositeStackRect(l->children(), rect, scratch);
            Rect sub = intersectRect(rect, scratch.extent());
            if (sub.isEmpty()) continue;
            out.blitRect(scratch, sub, sub.x, sub.y, l->opacity(), toOp(l->blend()));
        }
    }
}

// The single default lane fast path: a cell that is exactly one visible paint
// layer with default chrome can blit straight through, so track-level opacity
// or blend costs no scratch device (and matches the pre-stack behaviour).
const PaintDevice *singlePlainLane(const Cell &c) {
    if (c.layers().size() != 1) return nullptr;
    const CellLayer *l = c.layers().front().get();
    if (!l->isPaint() || !l->visible() || l->opacity() < 1.0f ||
        l->blend() != BlendMode::Normal)
        return nullptr;
    const PaintDevice *d = l->device();
    return (d && !d->extent().isEmpty()) ? d : nullptr;
}

// Grow a rect by `pad` on every side. A resample needs source pixels beyond the
// content edge, or the filter has nothing to blend with and the boundary eats
// into the ink.
constexpr int kResamplePad = 4;
Rect expandRect(const Rect &r, int pad) {
    return {r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad};
}

// A rect pushed through a full matrix. The image of a rectangle under a
// homography is bounded by the images of its edges, so the mapped corners
// tightly enclose it — which is exactly what the incremental projection needs
// when it back-maps a refresh rect to find the source region the resample has
// to read from.
Rect mapRect(const Mat3 &m, const Rect &r) {
    Vec2 c[4], d[4];
    rectCorners(r, c);
    for (int i = 0; i < 4; ++i) d[i] = m.map(c[i]);
    return cornersBounds(d);
}

// Track chrome (opacity/blend/visibility) is applied by the caller on the way
// out; the resample itself is chrome-free, so the full-frame and incremental
// paths can share it and can never drift.
//
// `render` is the destination region (pixels to compute) and `sourceArea` the
// destination-sized region grown by the inverse map plus the filter's tap
// support: the lift is taken from there, so every tap the sampler reaches for
// is real. Outside `sourceArea` the lift reads transparent, and `render` ∩
// mapped `sourceArea` keeps the sampler off pixels whose result would be
// guessed anyway — the one place the clip can bite is a rotation whose arc
// sweeps wide, and it then clips conservatively, never wrongly.
FloatRegion transformRegion(PaintDevice &scratch, const Rect &sourceArea, const Mat3 &m,
                            const Rect &render) {
    const FloatRegion src = liftRegion(scratch, sourceArea, nullptr);
    if (src.empty()) return {};
    const Rect dst = intersectRect(render, mappedBounds(src, m));
    if (dst.isEmpty()) return {};
    // A lattice map (translation, flip, whole-number scale) lands pixel centres
    // on pixel centres, so nearest-neighbour is not an approximation there: it
    // is the exact answer, and dragging a layer by whole pixels stays lossless.
    return resampleRegion(src, dst, m,
                          isLatticeMap(m) ? Interpolation::Nearest : Interpolation::Bicubic);
}

} // namespace

void Image::compositeCell(const Cell &cell, PaintDevice &out) const {
    compositeStack(cell.layers(), out);
}

void Image::compositeCellRect(const Cell &cell, const Rect &rect, PaintDevice &out) const {
    compositeStackRect(cell.layers(), rect, out);
}

Rect Image::cellComposite(const PaintLayer &pl, int key, PaintDevice &out) const {
    out.clear();
    if (const Cell *c = pl.cellAt(key)) compositeStack(c->layers(), out);
    Rect e = out.extent();
    if (e.isEmpty()) return {};
    int x0 = std::max(e.x, 0), y0 = std::max(e.y, 0);
    int x1 = std::min(e.x + e.w, w_), y1 = std::min(e.y + e.h, h_);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

void Image::collectAdjustments(int frame, AdjustMap &out) const {
    // Plain recursive lambda (not std::function): the incremental path calls
    // this per dab, and std::function allocated per tree node per call.
    auto walk = [&](const GroupLayer &g, const auto &self) -> void {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Paint) {
                const auto &pl = static_cast<const PaintLayer &>(*c);
                // A layer whose parameters all evaluate to their defaults AT
                // THIS FRAME is inert here: compositing it would lift +
                // bicubic-resample + stamp a bit-identical copy of the track
                // and force projectRect into a full-canvas rebuild on every
                // brush dab. Skipping it keeps the animated frames free.
                if (pl.visible() && !pl.params().empty() &&
                    !pl.params().isIdentityAt(frame))
                    out[pl.id()] = AdjustRef{&pl};
            } else if (c->type() == LayerType::Group) {
                self(static_cast<const GroupLayer &>(*c), self);
            }
        }
    };
    walk(*root_, walk);
}

void Image::collectCameraLayers(int frame, CameraMap &layerCams, CameraMap &folderCams,
                                std::vector<CameraRef> &globalCams) const {
    auto walk = [&](const GroupLayer &g, const auto &self) -> void {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Camera) {
                const auto &cam = static_cast<const CameraLayer &>(*c);
                if (!cam.visible() || cam.params().empty() ||
                    cam.params().isIdentityAt(double(frame)))
                    continue;
                if (cam.scope() == CameraScope::Global) {
                    globalCams.push_back(CameraRef{&cam});
                } else if (cam.scope() == CameraScope::Layer) {
                    if (cam.targetId() == 0) continue;
                    if (!layerCams.count(cam.targetId()))
                        layerCams[cam.targetId()] = CameraRef{&cam};
                } else {
                    if (cam.targetId() == 0) continue;
                    if (!folderCams.count(cam.targetId()))
                        folderCams[cam.targetId()] = CameraRef{&cam};
                }
            } else if (c->type() == LayerType::Group) {
                self(static_cast<const GroupLayer &>(*c), self);
            }
        }
    };
    walk(*root_, walk);
}

void Image::collectFrame(int frame, AdjustMap &adj, CameraMap &layerCams,
                          CameraMap &folderCams, std::vector<CameraRef> &globalCams) const {
    auto walk = [&](const GroupLayer &g, const auto &self) -> void {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Paint) {
                const auto &pl = static_cast<const PaintLayer &>(*c);
                if (pl.visible() && !pl.params().empty() && !pl.params().isIdentityAt(frame))
                    adj[pl.id()] = AdjustRef{&pl};
            } else if (c->type() == LayerType::Camera) {
                const auto &cam = static_cast<const CameraLayer &>(*c);
                if (!cam.visible() || cam.params().empty() ||
                    cam.params().isIdentityAt(double(frame)))
                    continue;
                if (cam.scope() == CameraScope::Global) {
                    globalCams.push_back(CameraRef{&cam});
                } else if (cam.scope() == CameraScope::Layer) {
                    if (cam.targetId() == 0) continue;
                    if (!layerCams.count(cam.targetId()))
                        layerCams[cam.targetId()] = CameraRef{&cam};
                } else {
                    if (cam.targetId() == 0) continue;
                    if (!folderCams.count(cam.targetId()))
                        folderCams[cam.targetId()] = CameraRef{&cam};
                }
            } else if (c->type() == LayerType::Group) {
                self(static_cast<const GroupLayer &>(*c), self);
            }
        }
    };
    walk(*root_, walk);
}

void Image::compositeTrack(const PaintLayer &pl, int frame, PaintDevice &out,
                           const AdjustRef *adj, const CameraRef *cam) const {
    const int key = pl.exposureAt(frame); // -1 = a gap between cells
    if (key < 0) return;
    const Cell *c = pl.cellAt(key);
    if (!c) return;
    const bool plain = pl.opacity() >= 1.0f && pl.blend() == BlendMode::Normal;

    if (!adj && !cam) {
        // Unadjusted: exactly the paths this walk used before keyframes existed.
        if (plain) {
            compositeStack(c->layers(), out);
            return;
        }
        if (const PaintDevice *d = singlePlainLane(*c)) {
            out.blit(*d, 0, 0, pl.opacity(), toOp(pl.blend()));
            return;
        }
        PaintDevice scratch;
        compositeStack(c->layers(), scratch);
        if (!scratch.extent().isEmpty())
            out.blit(scratch, 0, 0, pl.opacity(), toOp(pl.blend()));
        return;
    }

    // Animated: composite the cell as usual, then move the result. The order is
    // the whole point — the transform acts on whatever the cell already holds,
    // so every tool that ever painted into it is animated without knowing.
    // Layer-scoped cameras apply AFTER the layer's own keyframes:
    //   total = camera(canvas) * layer(content)
    // so a camera reframes the already-posed pixels like a real 2D camera.
    PaintDevice scratch;
    compositeStack(c->layers(), scratch);
    if (scratch.extent().isEmpty()) return;
    float adjOpacity = 1.0f;
    // Anchors and corners normalize against the block's own FRAME (the content
    // bbox the keys were authored against) — or the canvas for legacy blocks
    // that predate frames. A content bbox from the tile store would be
    // tile-granular (one extra pixel of ink could move the pivot by half a
    // tile); the stored frame is stable by construction instead.
    const Rect canvas{0, 0, w_, h_};
    Mat3 m = Mat3::identity();
    float camOpacity = 1.0f;
    if (adj) {
        m = adj->layer->matrixFor(adj->layer->params().frameOr(canvas), frame, &adjOpacity);
    }
    if (cam) {
        float co = 1.0f;
        // Cameras normalize against the CANVAS (the shot), never content.
        const Mat3 cm =
            cam->cam->matrixFor(canvas, frame, &co);
        camOpacity = co;
        m = cm.multiply(m);
    }
    // INVARIANT (see the same note in TransformTool): anchorX/Y and the corner
    // params are normalized against the block's frame everywhere — tool
    // seeding, curve editor, MoveTool and TransformTool's record mode all
    // speak this one space.
    const float opacity = pl.opacity() * adjOpacity * camOpacity;
    if (opacity <= 0.0f) return;
    if (m.isIdentity()) {
        out.blit(scratch, 0, 0, opacity, toOp(pl.blend()));
        return;
    }
    const FloatRegion moved =
        transformRegion(scratch, expandRect(scratch.extent(), kResamplePad), m, canvas);
    if (moved.empty()) return;
    PaintDevice stamp;
    stampRegion(stamp, moved, 0, 0, canvas, nullptr);
    if (stamp.extent().isEmpty()) return;
    out.blit(stamp, 0, 0, opacity, toOp(pl.blend()));
}

void Image::projectLayer(const Layer &l, int frame, PaintDevice &out,
                         const AdjustMap &adj, const CameraMap &layerCams,
                         const CameraMap &folderCams) const {
    if (!l.visible() || l.opacity() <= 0.0f) return;
    if (l.type() == LayerType::Paint) {
        const auto &pl = static_cast<const PaintLayer &>(l);
        // Skip hash probes when no animated/camera state is active at all:
        // the static-scene walk hits this per layer per frame, and even an
        // empty-map probe adds up at 1000 tracks x dozens of frames.
        const auto it = adj.empty() ? adj.end() : adj.find(pl.id());
        const auto cit =
            layerCams.empty() ? layerCams.end() : layerCams.find(pl.id());
        compositeTrack(pl, frame, out, it == adj.end() ? nullptr : &it->second,
                       cit == layerCams.end() ? nullptr : &cit->second);
    } else if (l.type() == LayerType::Group) {
        const auto &g = static_cast<const GroupLayer &>(l);
        const auto fit = folderCams.find(g.id());
        if (fit == folderCams.end()) {
            for (auto &c : g.children()) projectLayer(*c, frame, out, adj, layerCams, folderCams);
        } else {
            // Folder-scoped camera: composite the folder as one unit, then
            // reframe it. Isolating through a scratch keeps blend/opacity
            // inside the folder from seaming under the camera resample.
            PaintDevice scratch;
            for (auto &c : g.children())
                projectLayer(*c, frame, scratch, adj, layerCams, folderCams);
            if (scratch.extent().isEmpty()) return;
            const Rect canvas{0, 0, w_, h_};
            float camOp = 1.0f;
            const Mat3 m = fit->second.cam->matrixFor(canvas, frame, &camOp);
            const float opacity = l.opacity() * camOp;
            if (opacity <= 0.0f) return;
            if (m.isIdentity()) {
                if (!scratch.extent().isEmpty())
                    out.blit(scratch, 0, 0, opacity, toOp(l.blend()));
                return;
            }
            const FloatRegion moved = transformRegion(
                scratch, expandRect(scratch.extent(), kResamplePad), m, canvas);
            if (moved.empty()) return;
            PaintDevice stamp;
            stampRegion(stamp, moved, 0, 0, canvas, nullptr);
            if (stamp.extent().isEmpty()) return;
            out.blit(stamp, 0, 0, opacity, toOp(l.blend()));
        }
    }
    // Adjustment + Camera layers render nothing themselves: their contribution
    // is the transform already applied to their target when it was composited.
}

Rect Image::project(int frame, PaintDevice &out) const {
    AdjustMap adj;
    CameraMap layerCams, folderCams;
    std::vector<CameraRef> globalCams;
    collectFrame(frame, adj, layerCams, folderCams, globalCams);
    out.clear();
    projectLayer(*root_, frame, out, adj, layerCams, folderCams);
    // Global cameras reframe the final composite in tree order (chained).
    if (!globalCams.empty() && !out.extent().isEmpty()) {
        const Rect canvas{0, 0, w_, h_};
        for (auto &gcam : globalCams) {
            float co = 1.0f;
            const Mat3 m = gcam.cam->matrixFor(canvas, frame, &co);
            if (co <= 0.0f) {
                out.clear();
                break;
            }
            if (m.isIdentity()) {
                if (std::abs(co - 1.0f) > 1e-6f) {
                    PaintDevice tmp;
                    tmp.blit(out, 0, 0, co, CompositeOp::Normal);
                    out.clear();
                    out.blit(tmp, 0, 0, 1.0f, CompositeOp::Normal);
                }
                continue;
            }
            PaintDevice srcCopy;
            srcCopy.blit(out, 0, 0, 1.0f, CompositeOp::Normal);
            out.clear();
            if (srcCopy.extent().isEmpty()) break;
            const FloatRegion moved = transformRegion(
                srcCopy, expandRect(srcCopy.extent(), kResamplePad), m, canvas);
            if (moved.empty()) break;
            PaintDevice stamp;
            stampRegion(stamp, moved, 0, 0, canvas, nullptr);
            if (stamp.extent().isEmpty()) break;
            out.blit(stamp, 0, 0, co, CompositeOp::Normal);
        }
    }
    Rect e = out.extent();
    if (e.isEmpty()) return {};
    // clamp to canvas
    int x0 = std::max(e.x, 0), y0 = std::max(e.y, 0);
    int x1 = std::min(e.x + e.w, w_), y1 = std::min(e.y + e.h, h_);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

// The animated path of the incremental walk. `rect` is the (tile-aligned)
// refresh region. Pixels INSIDE rect whose source lies outside the inverse
// image of rect cannot be computed from a rect-clipped composite — but they CAN
// be computed from a composite clipped to the inverse image of an EXPANDED
// rect. So the caller grows `rect` (projectRect) and this function simply maps
// the exact inverse of rect through the matrix, lifts+resamples only that
// source region, and stamps the result clipped to rect. Bit-exact with a full
// project() restricted to rect for the region the sampler can legitimately
// reach; edges the growth did not cover fall back to the full rebuild.
void Image::compositeTrackRect(const PaintLayer &pl, int frame, const Rect &rect,
                               PaintDevice &out, const AdjustRef *adj,
                               const CameraRef *cam) const {
    const int key = pl.exposureAt(frame);
    if (key < 0) return;
    const Cell *c = pl.cellAt(key);
    if (!c) return;
    const Rect canvas{0, 0, w_, h_};
    float adjOpacity = 1.0f;
    // Total map mirrors compositeTrack exactly: the layer's own keyframes
    // first (frame-normalized), then the layer-scoped camera (canvas-
    // normalized), so total = camera * layer in both walks.
    Mat3 m = Mat3::identity();
    if (adj) {
        // INVARIANT: the block's frame (see compositeTrack) — anchors/corners are
        // frame-normalized, never canvas-assumed.
        m = adj->layer->matrixFor(adj->layer->params().frameOr(canvas), frame, &adjOpacity);
    }
    float camOpacity = 1.0f;
    if (cam) {
        float co = 1.0f;
        const Mat3 cm = cam->cam->matrixFor(canvas, frame, &co);
        camOpacity = co;
        m = cm.multiply(m);
    }
    const float opacity = pl.opacity() * adjOpacity * camOpacity;
    if (opacity <= 0.0f) return;
    // The forward map is the one the full path uses; an identity matrix can
    // only arrive here if collectAdjustments let a near-identity through, so
    // treat it as the plain clipped blit rather than resampling.
    bool ok = false;
    const Mat3 inv = m.inverted(&ok);
    if (!ok) return; // singular: caller's growth check already caught this
    if (m.isIdentity()) {
        PaintDevice scratch;
        compositeStackRect(c->layers(), rect, scratch);
        Rect sub = intersectRect(rect, scratch.extent());
        if (!sub.isEmpty()) out.blitRect(scratch, sub, sub.x, sub.y, opacity, toOp(pl.blend()));
        return;
    }
    // Source region = inverse image of the refresh rect, grown by the tap
    // support and clipped to the cell's own content, then to the canvas.
    const Rect need = intersectRect(expandRect(mapRect(inv, rect), kResamplePad), canvas);
    if (need.isEmpty()) return;
    PaintDevice scratch;
    compositeStackRect(c->layers(), need, scratch);
    if (scratch.extent().isEmpty()) return;
    const FloatRegion moved = transformRegion(scratch, need, m, rect);
    if (moved.empty()) return;
    PaintDevice stamp;
    stampRegion(stamp, moved, 0, 0, rect, nullptr);
    const Rect sub = stamp.extent(); // already in DESTINATION coords, ⊆ rect
    if (sub.isEmpty()) return;
    out.blitRect(stamp, sub, sub.x, sub.y, opacity, toOp(pl.blend()));
}

void Image::projectLayerRect(const Layer &l, int frame, const Rect &rect, PaintDevice &out,
                             const AdjustMap &adj, const CameraMap &layerCams,
                             const CameraMap &folderCams) const {
    (void)folderCams; // folder/global cameras take the full-rebuild fallback in projectRect
    if (!l.visible() || l.opacity() <= 0.0f || rect.isEmpty()) return;
    if (l.type() == LayerType::Paint) {
        const auto &pl = static_cast<const PaintLayer &>(l);
        // Skip the hash probes entirely when no animated/camera state is
        // active: the stroke path walks every track per dab and even an
        // empty-map probe adds up at 1000 tracks x hundreds of dabs.
        if (adj.empty() && layerCams.empty()) {
            int key = pl.exposureAt(frame); // -1 = a gap between cells
            if (key < 0) return;
            const Cell *c = pl.cellAt(key);
            if (!c) return;
            const bool plain = l.opacity() >= 1.0f && l.blend() == BlendMode::Normal;
            if (plain) {
                // Intersect with actual content: skips layers with nothing in
                // rect without touching their tiles (this is what makes
                // hundreds of layers cheap).
                compositeStackRect(c->layers(), rect, out);
                return;
            }
            if (const PaintDevice *d = singlePlainLane(*c)) {
                Rect sub = intersectRect(rect, d->extent());
                if (!sub.isEmpty())
                    out.blitRect(*d, sub, sub.x, sub.y, l.opacity(), toOp(l.blend()));
                return;
            }
            PaintDevice scratch;
            compositeStackRect(c->layers(), rect, scratch);
            Rect sub = intersectRect(rect, scratch.extent());
            if (!sub.isEmpty())
                out.blitRect(scratch, sub, sub.x, sub.y, l.opacity(), toOp(l.blend()));
            return;
        }
        const auto it = adj.empty() ? adj.end() : adj.find(pl.id());
        // Skip the hash probe entirely when no layer camera is active: the
        // stroke path walks every track per dab and even an empty-map probe
        // adds up at 1000 tracks x hundreds of dabs.
        const auto cit = layerCams.empty() ? layerCams.end() : layerCams.find(pl.id());
        const AdjustRef *adjRef = (it == adj.end()) ? nullptr : &it->second;
        const CameraRef *camRef = (cit == layerCams.end()) ? nullptr : &cit->second;
        if (adjRef || camRef) {
            compositeTrackRect(pl, frame, rect, out, adjRef, camRef);
            return;
        }
        int key = pl.exposureAt(frame); // -1 = a gap between cells
        if (key < 0) return;
        const Cell *c = pl.cellAt(key);
        if (!c) return;
        const bool plain = l.opacity() >= 1.0f && l.blend() == BlendMode::Normal;
        if (plain) {
            // Intersect with actual content: skips layers with nothing in rect
            // without touching their tiles (this is what makes hundreds of
            // layers cheap).
            compositeStackRect(c->layers(), rect, out);
            return;
        }
        if (const PaintDevice *d = singlePlainLane(*c)) {
            Rect sub = intersectRect(rect, d->extent());
            if (!sub.isEmpty()) out.blitRect(*d, sub, sub.x, sub.y, l.opacity(), toOp(l.blend()));
            return;
        }
        PaintDevice scratch;
        compositeStackRect(c->layers(), rect, scratch);
        Rect sub = intersectRect(rect, scratch.extent());
        if (!sub.isEmpty()) out.blitRect(scratch, sub, sub.x, sub.y, l.opacity(), toOp(l.blend()));
    } else if (l.type() == LayerType::Group) {
        const auto &g = static_cast<const GroupLayer &>(l);
        for (auto &c : g.children()) projectLayerRect(*c, frame, rect, out, adj, layerCams, folderCams);
    }
}

Rect Image::projectRect(int frame, const Rect &rect, PaintDevice &out) const {
    Rect canvas{0, 0, w_, h_};
    Rect r = intersectRect(rect, canvas);
    if (r.isEmpty()) return {};
    // Animated tracks make the refresh region a superset question, not a
    // cheaper one. Pixels whose SOURCE lies in r can end up anywhere m(r)
    // (forward-mapped), so `out` must be rebuilt across that union too — the
    // stale destination a moved dab leaves behind is exactly the bug an
    // un-grown rect would ship. Pixels flowing the OTHER way (content outside
    // r moving in) are handled by compositeTrackRect, which lifts the
    // INVERSE-mapped source it needs. Identity-valued adjustments never reach
    // here at all: collectAdjustments drops them (isIdentityAt), which is what
    // keeps the plain-stroke path — and the no-growth branch below — intact.
    AdjustMap adj;
    CameraMap layerCams, folderCams;
    std::vector<CameraRef> globalCams;
    collectFrame(frame, adj, layerCams, folderCams, globalCams);
    // Folder and global cameras reframe whole composites, whose pixels can
    // legitimately come from anywhere: they keep the full-rebuild path.
    // Identity and invisible cameras never reach here (collectFrame drops
    // them), which keeps the plain-stroke fast path intact. Layer-scoped
    // cameras warp exactly one track each, so they walk the incremental path
    // with their combined (camera * layer) map grown below, like any map.
    if (!folderCams.empty() || !globalCams.empty()) {
        project(frame, out);
        return canvas;
    }
    auto growFor = [&](const Mat3 &m, Rect &grown, bool &guarantee) {
        if (!guarantee) return;
        bool ok = false;
        m.inverted(&ok); // a singular map can land pixels anywhere — trust nothing
        if (!ok) {
            guarantee = false;
            return;
        }
        // The pad is SOURCE-space (tap support around the lifted region),
        // so it is applied BEFORE the forward map: a 4x zoom must grow the
        // refresh by 4*pad destination pixels, not pad.
        grown = uniteRect(grown, intersectRect(mapRect(m, expandRect(r, kResamplePad)), canvas));
    };
    Rect grown = r;
    if (!adj.empty() || !layerCams.empty()) {
        bool guarantee = true;
        for (auto &[targetId, ref] : adj) {
            (void)targetId;
            // INVARIANT: the block's frame (see compositeTrack) — anchors/corners
            // are frame-normalized, never canvas-assumed.
            Mat3 m = ref.layer->matrixFor(ref.layer->params().frameOr(canvas), frame, nullptr);
            const auto cit = layerCams.find(ref.layer->id());
            if (cit != layerCams.end()) {
                float co = 1.0f;
                m = cit->second.cam->matrixFor(canvas, frame, &co).multiply(m);
            }
            growFor(m, grown, guarantee);
        }
        for (auto &[targetId, cam] : layerCams) {
            if (adj.find(targetId) != adj.end()) continue; // combined above
            float co = 1.0f;
            growFor(cam.cam->matrixFor(canvas, frame, &co), grown, guarantee);
        }
        if (!guarantee) {
            project(frame, out); // correctness first: rebuild the whole frame
            return canvas;
        }
    }
    // Align to tile boundaries: clearRect drops whole overlapping tiles, so
    // the re-composite must cover those tiles fully or their surviving
    // content outside r would be lost. Returns the aligned (actual) region.
    int tx0 = floorDiv(grown.x, kTileSize), ty0 = floorDiv(grown.y, kTileSize);
    int tx1 = floorDiv(grown.x + grown.w - 1, kTileSize),
        ty1 = floorDiv(grown.y + grown.h - 1, kTileSize);
    Rect aligned{tx0 * kTileSize, ty0 * kTileSize, (tx1 - tx0 + 1) * kTileSize,
                 (ty1 - ty0 + 1) * kTileSize};
    out.store().clearRect(aligned);
    projectLayerRect(*root_, frame, aligned, out, adj, layerCams, folderCams);
    return aligned;
}

void Image::collectCameras(std::vector<CameraLayer*> &out) {
    std::function<void(GroupLayer&)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Camera) out.push_back(static_cast<CameraLayer*>(c.get()));
            else if (c->type() == LayerType::Group) walk(static_cast<GroupLayer&>(*c));
        }
    };
    walk(*root_);
}
void Image::collectCameras(std::vector<const CameraLayer*> &out) const {
    std::function<void(const GroupLayer&)> walk = [&](const GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Camera)
                out.push_back(static_cast<const CameraLayer*>(c.get()));
            else if (c->type() == LayerType::Group) walk(static_cast<const GroupLayer&>(*c));
        }
    };
    walk(*root_);
}

void Image::collectPaintLayers(std::vector<PaintLayer*> &out) {
    std::function<void(GroupLayer&)> walk = [&](GroupLayer &g) {
        for (auto &c : g.children()) {
            // Match on Group explicitly, never on "not paint": a third layer
            // kind would otherwise be cast to a group and walked as one.
            if (c->type() == LayerType::Paint) out.push_back(static_cast<PaintLayer*>(c.get()));
            else if (c->type() == LayerType::Group) walk(static_cast<GroupLayer&>(*c));
        }
    };
    walk(*root_);
}
void Image::collectPaintLayers(std::vector<const PaintLayer*> &out) const {
    std::function<void(const GroupLayer&)> walk = [&](const GroupLayer &g) {
        for (auto &c : g.children()) {
            if (c->type() == LayerType::Paint) out.push_back(static_cast<const PaintLayer*>(c.get()));
            else if (c->type() == LayerType::Group) walk(static_cast<const GroupLayer&>(*c));
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
std::string Image::autoCameraName() { return uniqueAutoName(*root_, "Camera"); }

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

CameraLayer *Image::createCamera(GroupLayer *folder, const std::string &name, CameraScope scope,
                                 uint64_t targetId, int index) {
    if (!folder) folder = ensureFolder();
    std::string nm = name.empty() ? autoCameraName() : name;
    auto c = std::make_shared<CameraLayer>(nm, scope, targetId);
    // Cameras normalize against the shot, never content: seed the reference
    // frame to the canvas so the pivot is stable from the first key.
    c->params().setFrame(Rect{0, 0, w_, h_});
    CameraLayer *p = c.get();
    auto &ch = folder->children();
    if (index < 0 || index > (int)ch.size()) ch.push_back(std::move(c));
    else ch.insert(ch.begin() + index, std::move(c));
    return p;
}

GroupLayer *Image::ensureFolder() {
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    if (!folders.empty()) return folders.front();
    return createFolder("Folder 1");
}

GroupLayer *Image::findFolderOf(const Layer *track) {
    if (!track) return nullptr;
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    for (auto *g : folders)
        for (auto &c : g->children())
            if (c.get() == track) return g;
    return nullptr;
}

const PaintLayer *Image::paintLayerById(uint64_t id) const {
    if (id == 0) return nullptr;
    // Early-exit search (was: collect ALL paint layers into a vector, then
    // scan — O(N) allocs per call on frame-pump paths).
    const PaintLayer *found = nullptr;
    auto walk = [&](const GroupLayer &g, const auto &self) -> void {
        for (auto &c : g.children()) {
            if (found) return;
            if (c->type() == LayerType::Paint && c->id() == id) {
                found = static_cast<const PaintLayer *>(c.get());
                return;
            }
            if (c->type() == LayerType::Group) self(static_cast<const GroupLayer &>(*c), self);
        }
    };
    walk(*root_, walk);
    return found;
}

PaintLayer *Image::paintLayerById(uint64_t id) {
    return const_cast<PaintLayer*>(std::as_const(*this).paintLayerById(id));
}

const CameraLayer *Image::cameraById(uint64_t id) const {
    if (id == 0) return nullptr;
    // Early-exit search (was: collect ALL cameras into a vector, then scan).
    const CameraLayer *found = nullptr;
    auto walk = [&](const GroupLayer &g, const auto &self) -> void {
        for (auto &c : g.children()) {
            if (found) return;
            if (c->type() == LayerType::Camera && c->id() == id) {
                found = static_cast<const CameraLayer *>(c.get());
                return;
            }
            if (c->type() == LayerType::Group) self(static_cast<const GroupLayer &>(*c), self);
        }
    };
    walk(*root_, walk);
    return found;
}

CameraLayer *Image::cameraById(uint64_t id) {
    return const_cast<CameraLayer*>(std::as_const(*this).cameraById(id));
}

static const Layer *findLayerById(const GroupLayer &root, uint64_t id) {
    if (id == 0) return nullptr;
    const Layer *found = nullptr;
    std::function<void(const GroupLayer &)> walk = [&](const GroupLayer &g) {
        for (auto &c : g.children()) {
            if (found) return;
            if (c->id() == id) {
                found = c.get();
                return;
            }
            if (c->type() == LayerType::Group)
                walk(static_cast<const GroupLayer &>(*c));
        }
    };
    walk(root);
    return found;
}

Layer *Image::layerById(uint64_t id) {
    return const_cast<Layer*>(findLayerById(*root_, id));
}

const Layer *Image::layerById(uint64_t id) const { return findLayerById(*root_, id); }

GroupLayer *Image::folderById(uint64_t id) {
    const Layer *l = findLayerById(*root_, id);
    return (l && l->type() == LayerType::Group) ? static_cast<GroupLayer*>(const_cast<Layer*>(l))
                                                : nullptr;
}

const GroupLayer *Image::folderById(uint64_t id) const {
    const Layer *l = findLayerById(*root_, id);
    return (l && l->type() == LayerType::Group) ? static_cast<const GroupLayer*>(l) : nullptr;
}

bool Image::deleteCamera(CameraLayer *cam) {
    if (!cam) return false;
    GroupLayer *owner = findFolderOf(cam);
    if (!owner) return false;
    auto &ch = owner->children();
    for (auto it = ch.begin(); it != ch.end(); ++it) {
        if (it->get() == cam) {
            ch.erase(it);
            return true;
        }
    }
    return false;
}

GroupLayer *Image::findFolderByName(const std::string &name) {
    std::vector<GroupLayer*> folders;
    collectFolders(folders);
    for (auto *g : folders) if (g->name() == name) return g;
    return nullptr;
}

GroupLayer *Image::groupLayers(const std::vector<Layer*> &members,
                               const std::string &name) {
    if (members.empty()) return nullptr;
    // 1. pin the members in tree order (top..bottom of the stack).
    std::vector<Layer*> wanted;
    for (auto *m : members)
        if (m && std::find(wanted.begin(), wanted.end(), m) == wanted.end()) wanted.push_back(m);
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
    GroupLayer *anchor = findFolderOf(ordered.front().get());

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
    for (auto &c : g->children()) {
        // A folder-scoped camera dies with its group: it frames the folder as
        // a unit, and the dissolved folder no longer exists to frame (moving
        // it would silently reframe the destination folder instead).
        if (c->type() == LayerType::Camera) {
            const auto &cam = static_cast<const CameraLayer&>(*c);
            if (cam.scope() == CameraScope::Folder && cam.targetId() == g->id()) continue;
        }
        ((c->type() == LayerType::Paint || c->type() == LayerType::Camera) ? paints : nested)
            .push_back(c);
    }
    g->children().clear();
    for (auto &p : paints) dst->addChild(p);
    rootCh.erase(rootCh.begin() + idx);
    for (size_t i = 0; i < nested.size(); ++i)
        rootCh.insert(rootCh.begin() + idx + (long)i, nested[i]);
    return true;
}

bool Image::moveLayer(Layer *l, GroupLayer *to, int index) {
    if (!l || !to) return false;
    GroupLayer *from = findFolderOf(l);
    if (!from) return false;
    if (from == to) return true; // same folder: order is a UI concern
    std::shared_ptr<Layer> held;
    auto &src = from->children();
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it->get() == l) { held = *it; src.erase(it); break; }
    }
    if (!held) return false;
    auto &dst = to->children();
    if (index < 0 || index > (int)dst.size()) dst.push_back(held);
    else dst.insert(dst.begin() + index, held);
    return true;
}

} // namespace ink
