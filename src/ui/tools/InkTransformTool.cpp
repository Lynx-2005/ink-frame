#include "ui/tools/InkTransformTool.h"
#include "ui/InkDocument.h"
#include "ui/InkIcons.h"
#include "ui/InkOptionWidgets.h"
#include "ui/InkTheme.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkToolManager.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

// Handles are constant on SCREEN, not in the document: their size in document
// units therefore divides by the zoom (Krita's handleRadius/visualRadius pair).
constexpr double kHandlePx = 9.0;
// Lattice spacing of the warp field: fine enough to look smooth, coarse
// enough that a drag stays interactive (Krita's pixelPrecision is 8).
constexpr int kWarpCellPx = 12;
constexpr int kCornerGrips[4] = {0, 1, 2, 3}; // Grip order follows the corner order

// Can this quad still be mapped? quadToQuad() refuses a singular homography, so
// a corner drag that walks three corners into a line would otherwise leave the
// tool with corners the model cannot composite - the preview collapsing, the
// cage snapping to a page corner, and a keyed value no reload could reproduce.
// The test is the same one the compositor will apply, plus a relative-area
// floor so a quad squeezed to a sliver is refused too.
// Two maps are "the same pose" when they agree to a thousandth of a pixel
// anywhere. Used to notice that the animation moved under an open session.
bool sameMatrix(const Mat3 &a, const Mat3 &b, double eps = 1e-3) {
    for (int i = 0; i < 9; ++i)
        if (std::abs(a.m[i] - b.m[i]) > eps)
            return false;
    return true;
}

double quadArea(const Vec2 q[4]);

// A mesh is safe to resample when every control point is finite and the region
// it can reach stays within a sane multiple of the page. This is the one guard
// between a stray value and a huge allocation: movedBounds() is what decides
// the destination rect, and a non-finite or runaway bound there would ask for a
// resample of arbitrary size - the difference between a rejected drag and a
// hung or crashed app.
bool meshIsSane(const DeformMesh &m, const Rect &doc) {
    if (!m.valid())
        return false;
    for (const Vec2 &p : m.moved)
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return false;
    const Rect b = m.movedBounds();
    if (b.w <= 0 || b.h <= 0)
        return false;
    const double pageArea = double(std::max(1, doc.w)) * double(std::max(1, doc.h));
    const double reach = double(b.w) * double(b.h);
    return reach <= 16.0 * pageArea;
}

bool quadIsUsable(const Vec2 q[4]) {
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(q[i].x) || !std::isfinite(q[i].y))
            return false;
    }
    Vec2 from[4];
    rectCorners(cornersBounds(q), from);
    Mat3 m;
    if (!quadToQuad(from, q, m))
        return false;
    const double a = std::abs(quadArea(q));
    const double b = std::abs(quadArea(from));
    return a > 1e-6 * b;
}

double quadArea(const Vec2 q[4]) {
    // Shoelace over the four corners in order.
    double s = 0.0;
    for (int i = 0; i < 4; ++i) {
        const Vec2 &a = q[i];
        const Vec2 &b = q[(i + 1) % 4];
        s += a.x * b.y - b.x * a.y;
    }
    return s * 0.5;
}

// Spin-box steppers drawn in code, for the same reason as the combo chevron:
// once ::up-button/::down-button are styled Qt skips its own glyphs, and the
// app ships no image assets.
class InkSpinBox : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    void paintEvent(QPaintEvent *e) override {
        QDoubleSpinBox::paintEvent(e);
        QStyleOptionSpinBox opt;
        opt.initFrom(this);
        QRect up = style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxUp, this);
        QRect down =
            style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxDown, this);
        if (up.isEmpty() || down.isEmpty()) {
            const int bw = 16, half = (height() - 2) / 2;
            up = QRect(width() - bw - 1, 1, bw, half);
            down = QRect(width() - bw - 1, 1 + half, bw, height() - 2 - half);
        }
        QPainter p(this);
        const QColor ink = isEnabled() ? InkTheme::dim() : InkTheme::faint();
        InkIcons::drawChevron(p, up, /*up=*/true, ink, 7.0);
        InkIcons::drawChevron(p, down, /*up=*/false, ink, 7.0);
    }
};

double dist(const Vec2 &a, const Vec2 &b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool pointInQuad(const Vec2 &p, const Vec2 c[4]) {
    // Convex-quad containment: the point must stay on one side of every edge.
    bool neg = false, pos = false;
    for (int i = 0; i < 4; ++i) {
        const Vec2 &a = c[i], &b = c[(i + 1) % 4];
        const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross < 0)
            neg = true;
        else
            pos = true;
    }
    return !(neg && pos);
}

QString modeName(TransformTool::Mode m) {
    switch (m) {
    case TransformTool::Mode::Free: return QStringLiteral("Free");
    case TransformTool::Mode::Perspective: return QStringLiteral("Perspective");
    case TransformTool::Mode::Warp: return QStringLiteral("Warp");
    default: return QStringLiteral("Liquify");
    }
}

} // namespace

TransformTool::TransformTool(Mode initial, QString factoryId)
    : mode_(initial), factoryId_(std::move(factoryId)) {}

// --- session ---------------------------------------------------------------

Rect TransformTool::documentRect() const {
    const Image &img = ctx_->doc()->image();
    return {0, 0, img.width(), img.height()};
}

double TransformTool::handleRadiusInDoc(double zoom) const {
    return kHandlePx / std::max(0.05, zoom);
}

void TransformTool::resetSession() {
    // Close any open recording first: leaving one dangling would fold the NEXT
    // drag into the same undo step.
    if (ctx_ && ctx_->recordActive())
        ctx_->endParamRecord();
    recordMode_ = false;    recBaseValid_ = false;
    freshParams_.clear();
    recordFrame_ = Rect{};
    sessionFrame_ = -1;
    device_ = nullptr;
    lift_ = FloatRegion{};
    eraseMask_ = Selection{};
    memento_ = TileMemento{};
    area_ = Rect{};
    lastStamp_ = Rect{};
    selectionScope_ = false;
    session_ = false;
    pending_ = false;
    dragging_ = false;
    stateToDisplay_ = Mat3::identity();
    displayToState_ = Mat3::identity();
    grip_ = Grip::None;
    free_.reset();
    persp_.reset();
    warp_.reset();
    warpMesh_ = DeformMesh{};
    liquify_.resetState();
    liquify_.field = LiquifyField{};
}

// Rebuild the lift + mode state from the CURRENT device/params WITHOUT
// dropping the session: what commit() and cancelPrimary() settle into, so the
// cage survives a release and always outlines the latest state. Implemented
// as a reset + begin (one extra lift read on Apply/Esc, never per-frame),
// which guarantees the re-seeded session is identical to a fresh activation.
void TransformTool::reseedSession() {
    if (!ctx_)
        return;
    const bool hadSession = session_;
    resetSession();
    if (hadSession)
        beginSession();
    syncOptions();
}

// Lift the pixels to transform: the selection contents when there is one, else
// the layer's content extent (never the whole empty canvas).
bool TransformTool::beginSession() {
    resetSession();
    device_ = ctx_->editableDevice();
    if (!device_) {
        // A recording layer needs no paintable device: what it animates is the
        // composite, not the pixels. This is why keyframing works even on a
        // locked cel or a frame with no cell exposed.
        if (canRecord()) {
            if (beginRecordSession())
                return true;
        }
        ctx_->setStatus(ctx_->blockedHint());
        return false;
    }
    const Selection &sel = ctx_->selection();
    const Rect doc = documentRect();
    selectionScope_ = !sel.isEmpty();
    if (selectionScope_) {
        area_ = intersectRect(sel.bounds(), doc);
        eraseMask_ = sel;
    } else {
        // Pixel-precise ink bounds (not the tile store's extent): the lift,
        // the cage and the record frame all derive from this one rect, so a
        // tile edge can never move the pivot.
        area_ = intersectRect(device_->contentBounds(), doc);
        if (area_.isEmpty())
            area_ = doc; // empty layer: transform the canvas area
    }
    if (area_.isEmpty()) {
        device_ = nullptr;
        return false;
    }
    lift_ = liftRegion(*device_, area_, selectionScope_ ? &eraseMask_ : nullptr);
    if (lift_.empty()) {
        device_ = nullptr;
        // Record mode does not need pixels on the device: the frame may hold
        // nothing yet and still be animated.
        if (canRecord() && beginRecordSession())
            return true;
        ctx_->setStatus(QStringLiteral("Nothing to transform"));
        return false;
    }
    recordMode_ = false;
    free_.pivot = {double(lift_.bounds.x) + lift_.bounds.w * 0.5,
                   double(lift_.bounds.y) + lift_.bounds.h * 0.5};
    Vec2 corners[4];
    rectCorners(lift_.bounds, corners);
    for (int i = 0; i < 4; ++i)
        persp_.corners[i] = corners[i];
    // Keyframed ink DISPLACES at composite time (rotate, scale, shear, or
    // perspective-warp - not merely a slide), so the session needs to know how
    // the state space maps onto what the user SEES. refreshDisplayMapping owns
    // that pair; a record session re-derives it for its own space below.
    refreshDisplayMapping();

    session_ = true;
    sessionFrame_ = ctx_->doc()->currentFrame();
    if (mode_ == Mode::Warp)
        buildWarpAnchors();
    else if (mode_ == Mode::Liquify)
        liquify_.field.reset(lift_.bounds);
    // Recording does not need the lift, but lifting is a pure read and it is
    // what keeps the handles, the pivot and the hit-testing identical between
    // the two paths.
    if (canRecord()) {
        beginRecordSession();
        return true;
    }
    return true;
}

// --- keyframe recording ------------------------------------------------------

void TransformTool::setMode(Mode m) {
    if (m == mode_ || m < Mode::Free || m > Mode::Liquify)
        return;
    // Never carry a half-finished session across the switch: commit what the
    // old mode did (Krita commits on tool change too), then begin fresh.
    if (session_)
        commit();
    mode_ = m;
    if (settingsStack_) {
        settingsStack_->setCurrentIndex(int(m));
        if (settingsTitle_)
            settingsTitle_->setText(modeName(m));
    }
    if (!ctx_)
        return;
    beginSession();
    syncOptions();
}

void TransformTool::setRecordKeyframes(bool on) {
    if (recordKeyframes_ == on)
        return;
    recordKeyframes_ = on;
    if (!ctx_)
        return;
    // Never leave a half-record state behind: a live record session settles
    // with what was written so far (one undo step), and the next drag starts
    // clean in the newly chosen mode. A pixel session just re-begins so the
    // next grab takes the newly chosen path.
    if (session_)
        commit();
    beginSession();
    syncOptions();
}

bool TransformTool::canRecord() const {
    if (!ctx_ || !ctx_->recording())
        return false;
    // Warp and Liquify ARE raster deformations: their state is a mesh, not a
    // handful of scalars, so they have nothing to key in this version and keep
    // baking pixels as they always did.
    if (mode_ != Mode::Free && mode_ != Mode::Perspective)
        return false;
    // Strictly OPT-IN: the toggle alone decides. An animated layer must NOT
    // flip the tool into key-writing behind the artist's back — that silent
    // takeover is what made "add a keyframe, then transform" feel broken (the
    // gesture stopped moving pixels and only wrote keys). With the toggle off
    // the tool always bakes, keyframes or not; statusHint() says so.
    return recordKeyframes_;
}

QString TransformTool::recordCommitStatus() const {
    QString base = QStringLiteral("Keyframes recorded — pixels untouched");
    base += firstKeyHint();
    return base;
}

// Names the channels whose FIRST key this session created (they hold
// backwards, so earlier frames will not move). Never auto-keys: the commit
// hint — and the release preview — says so instead.
QString TransformTool::firstKeyHint() const {
    if (!ctx_ || freshParams_.empty())
        return {};
    const AdjustmentLayer *a = ctx_->recordTarget();
    if (!a)
        return {};
    const ParamBlock &b = a->params();
    QStringList first;
    for (ParamId id : freshParams_)
        if (b.isAnimated(id))
            first << QString::fromUtf8(paramDesc(id).label);
    if (first.isEmpty())
        return {};
    // Name at most two; the point is the mechanism, not the roster.
    while (first.size() > 2) first.removeLast();
    return QStringLiteral(" · first %1 key at frame %2 — Add-key an earlier frame to animate from there")
        .arg(first.join(QStringLiteral(", ")))
        .arg(sessionFrame_);
}

ParamId TransformTool::cornerXParam(int i) {
    switch (i) {
    case 0: return ParamId::Corner0X;
    case 1: return ParamId::Corner1X;
    case 2: return ParamId::Corner2X;
    default: return ParamId::Corner3X;
    }
}

ParamId TransformTool::cornerYParam(int i) {
    switch (i) {
    case 0: return ParamId::Corner0Y;
    case 1: return ParamId::Corner1Y;
    case 2: return ParamId::Corner2Y;
    default: return ParamId::Corner3Y;
    }
}

// Start recording: seed this tool's parameters from what the animation already
// evaluates to at the playhead, so the handles start on the moved picture
// instead of at the origin, and the first drag continues the motion rather than
// snapping back to it. Everything is normalized against the block's reference
// FRAME (content bbox for a fresh block, stable afterwards) — the same rect
// the compositor resolves through frameOr — so the live handles and the keyed
// pixels are one map, in Free and Perspective alike.
bool TransformTool::beginRecordSession() {
    recordMode_ = true;
    session_ = true;
    pending_ = false;
    sessionFrame_ = ctx_->doc()->currentFrame();
    const Rect doc = documentRect();
    // Recording works on a cel with no pixels at all. The frame handles still
    // need something to outline, so the canvas is the fallback.
    if (lift_.bounds.isEmpty()) {
        lift_.bounds = doc;
        area_ = doc;
    }
    AdjustmentLayer *target = ctx_->recordTarget();
    ParamBlock *block = target ? &target->params() : nullptr;
    // Whoever keys first fixes the frame; a first record drag on a fresh block
    // fixes it from the lift (selection bbox, else content extent). Re-reading
    // an animated block's frame is forbidden: the stored values are already
    // normalized against it.
    if (block && !block->hasFrame() && block->animatedParams().empty())
        block->setFrame(lift_.bounds);
    recordFrame_ = block ? block->frameOr(lift_.bounds) : lift_.bounds;
    const Rect &frame = recordFrame_;
    // Record state lives in its own space (Free: already posed; Perspective:
    // frame space), so the mapping the pointer is measured through is NOT the
    // animation's. Derive it now that the frame and the session frame are known.
    refreshDisplayMapping();
    auto recorded = [&](ParamId id) { return ctx_->recordedValue(id, sessionFrame_); };
    // The affine pose is mirrored into free_ in BOTH record modes. Perspective
    // only ever writes the quad, but the pose is still part of what the model
    // holds, and recordedPoseChanged() compares against this snapshot - with the
    // defaults left in place it would read every keyed pose as "changed" and
    // re-seed on every gesture. The pivot is frame-relative, like every other
    // value here.
    free_.translate = {recorded(ParamId::PositionX), recorded(ParamId::PositionY)};
    // The anchor is normalized to the frame, so its default (0.5) IS the
    // content centre on a fresh block — no special-case seeding needed,
    // and a keyed anchor stays authoritative forever, so a saved pivot
    // survives content edits.
    const double anchorX = recorded(ParamId::AnchorX);
    const double anchorY = recorded(ParamId::AnchorY);
    free_.pivot = {frame.x + anchorX * frame.w, frame.y + anchorY * frame.h};
    free_.scaleX = recorded(ParamId::ScaleX);
    free_.scaleY = recorded(ParamId::ScaleY);
    free_.angleDeg = recorded(ParamId::Rotation);
    free_.shearX = recorded(ParamId::ShearX);
    free_.shearY = recorded(ParamId::ShearY);
    free_.hasQuad = false;
    if (mode_ == Mode::Perspective) {
        // Corners live in FRAME space (device): animated ones are frame
        // fractions already, unanimated ones open on the lift. The CAGE is
        // what sits on the displayed stroke — frameCorners runs the drawn quad
        // through the recorded affine — so seeding display-space values here
        // would bake the pose into the keys as a ghost offset.
        Vec2 liftCorners[4];
        rectCorners(lift_.bounds, liftCorners);
        for (int i = 0; i < 4; ++i) {
            persp_.corners[i] = {
                block && block->isAnimated(cornerXParam(i)) ? recorded(cornerXParam(i)) * frame.w + frame.x
                                                            : liftCorners[i].x,
                block && block->isAnimated(cornerYParam(i)) ? recorded(cornerYParam(i)) * frame.h + frame.y
                                                            : liftCorners[i].y};
        }
    }
    snapshotRecordBase();
    // Remember which writable channels start keyless: a drag that keys one of
    // them creates its FIRST key at the playhead (holds backwards — earlier
    // frames stay put), and the commit hint must say so. Never auto-key.
    freshParams_.clear();
    if (const AdjustmentLayer *a = ctx_->recordTarget()) {
        const ParamBlock &b = a->params();
        if (mode_ == Mode::Perspective) {
            for (int i = 0; i < 4; ++i) {
                if (!b.isAnimated(cornerXParam(i))) freshParams_.push_back(cornerXParam(i));
                if (!b.isAnimated(cornerYParam(i))) freshParams_.push_back(cornerYParam(i));
            }
        } else {
            for (ParamId id : {ParamId::PositionX, ParamId::PositionY, ParamId::AnchorX,
                               ParamId::AnchorY, ParamId::ScaleX, ParamId::ScaleY,
                               ParamId::Rotation, ParamId::ShearX, ParamId::ShearY})
                if (!b.isAnimated(id)) freshParams_.push_back(id);
        }
    }
    ctx_->beginParamRecord(commandName(), sessionFrame_);
    ctx_->setStatus(QStringLiteral("Recording %1 keyframes — Auto-key is on")
                        .arg(mode_ == Mode::Perspective ? QStringLiteral("perspective")
                                                        : QStringLiteral("transform")));
    return true;
}

// Write this tool's current parameters into the linked adjustment layer, but
// ONLY the ones this drag has actually moved. The frame is the playhead, so
// dragging at different frames lays down the keys that define the move. Anchor
// and corners are normalized against the session's record frame (the block's
// reference rect), never the canvas.
void TransformTool::pushRecordedParams(int frame) {
    if (!ctx_ || !ctx_->recordActive() || !recBaseValid_)
        return;
    const Rect &rf = recordFrame_;
    if (rf.isEmpty())
        return;
    auto moved = [](double now, double was) { return std::abs(now - was) > 1e-9; };
    // Never let a non-finite value reach the model. A NaN key poisons every
    // interpolation it takes part in, is written to the project file as a
    // literal, and compares unequal to itself - so one bad frame would follow
    // the layer for the rest of the session and undoing the gesture would not
    // remove it. Refusing the write keeps the last good state instead.
    auto put = [&](ParamId id, double value) {
        if (std::isfinite(value))
            ctx_->recordParam(id, value, frame);
    };
    if (mode_ == Mode::Perspective) {
        // The quad is the only thing a perspective DRAG moves...
        for (int i = 0; i < 4; ++i) {
            if (moved(persp_.corners[i].x, recBase_.corners[i].x))
                put(cornerXParam(i), (persp_.corners[i].x - rf.x) / rf.w);
            if (moved(persp_.corners[i].y, recBase_.corners[i].y))
                put(cornerYParam(i), (persp_.corners[i].y - rf.y) / rf.h);
        }
    } else {
        // ...and the affine channels are the only things a Free drag moves.
        if (moved(free_.translate.x, recBase_.posX))
            put(ParamId::PositionX, free_.translate.x);
        if (moved(free_.translate.y, recBase_.posY))
            put(ParamId::PositionY, free_.translate.y);
        if (moved(free_.scaleX, recBase_.scaleX))
            put(ParamId::ScaleX, free_.scaleX);
        if (moved(free_.scaleY, recBase_.scaleY))
            put(ParamId::ScaleY, free_.scaleY);
        if (moved(free_.angleDeg, recBase_.rot))
            put(ParamId::Rotation, free_.angleDeg);
        if (moved(free_.shearX, recBase_.shearX))
            put(ParamId::ShearX, free_.shearX);
        if (moved(free_.shearY, recBase_.shearY))
            put(ParamId::ShearY, free_.shearY);
    }
    // The ANCHOR belongs to both. Alt-click places the rotation centre, and the
    // centre is this parameter: it is what the pivot handle and the drag math
    // read in either mode. Writing it only from the Free branch is what made
    // Alt in Perspective report "Rotation centre moved" while keying nothing at
    // all - a status line that lied, and a gesture that did not work.
    if (moved((free_.pivot.x - rf.x) / rf.w, recBase_.anchorX))
        put(ParamId::AnchorX, (free_.pivot.x - rf.x) / rf.w);
    if (moved((free_.pivot.y - rf.y) / rf.h, recBase_.anchorY))
        put(ParamId::AnchorY, (free_.pivot.y - rf.y) / rf.h);
}

void TransformTool::snapshotRecordBase() {
    // The anchor base is frame-normalized (like the values pushRecordedParams
    // writes), so the moved() comparison speaks one space.
    const Rect &rf = recordFrame_;
    recBase_.posX = free_.translate.x;
    recBase_.posY = free_.translate.y;
    recBase_.anchorX = rf.w > 0 ? (free_.pivot.x - rf.x) / rf.w : 0.5;
    recBase_.anchorY = rf.h > 0 ? (free_.pivot.y - rf.y) / rf.h : 0.5;
    recBase_.scaleX = free_.scaleX;
    recBase_.scaleY = free_.scaleY;
    recBase_.rot = free_.angleDeg;
    recBase_.shearX = free_.shearX;
    recBase_.shearY = free_.shearY;
    for (int i = 0; i < 4; ++i) recBase_.corners[i] = persp_.corners[i];
    recBaseValid_ = true;
}

// Has the ANIMATION under this session changed since the session was seeded?
// A record session's frame is built from the pose the params evaluate to at
// the playhead. Adding, moving or undoing a key changes that pose WITHOUT
// changing the playhead or the (null) device, so the old guard missed it and
// the frame kept showing a stale pose until the mode was re-picked — the
// "transform box stops matching until I click the mode again" report.
bool TransformTool::recordedPoseChanged() const {
    if (!ctx_ || !recBaseValid_)
        return false;
    if (ctx_->doc()->currentFrame() != sessionFrame_)
        return false; // handled by the playhead check in ensureSessionTarget
    const Rect doc = documentRect();
    if (doc.isEmpty())
        return false;
    // Compare exactly the parameters this drag can write, at the tolerance the
    // model itself stores them with. Corners are frame fractions in the model
    // and absolute points in the session base: convert before comparing. The
    // affine channels are checked in EVERY mode, Perspective included - a
    // rotation keyed from the panel while a perspective cage is up moves the
    // stroke out from under the box just as surely as moving a key does.
    const auto differs = [&](double now, double was) { return std::abs(now - was) > 1e-6; };
    if (mode_ == Mode::Perspective) {
        const Rect &rf = recordFrame_;
        const double w = rf.w > 0 ? rf.w : 1.0, h = rf.h > 0 ? rf.h : 1.0;
        for (int i = 0; i < 4; ++i)
            if (differs(ctx_->recordedValue(cornerXParam(i), sessionFrame_),
                        (recBase_.corners[i].x - rf.x) / w) ||
                differs(ctx_->recordedValue(cornerYParam(i), sessionFrame_),
                        (recBase_.corners[i].y - rf.y) / h))
                return true;
    }
    return differs(ctx_->recordedValue(ParamId::PositionX, sessionFrame_), recBase_.posX) ||
           differs(ctx_->recordedValue(ParamId::PositionY, sessionFrame_), recBase_.posY) ||
           differs(ctx_->recordedValue(ParamId::ScaleX, sessionFrame_), recBase_.scaleX) ||
           differs(ctx_->recordedValue(ParamId::ScaleY, sessionFrame_), recBase_.scaleY) ||
           differs(ctx_->recordedValue(ParamId::Rotation, sessionFrame_), recBase_.rot) ||
           differs(ctx_->recordedValue(ParamId::ShearX, sessionFrame_), recBase_.shearX) ||
           differs(ctx_->recordedValue(ParamId::ShearY, sessionFrame_), recBase_.shearY);
}

void TransformTool::ensureSessionTarget() {
    if (!session_ || !ctx_)
        return;
    if (recordMode_) {
        // A recorded session belongs to the frame it started on: a new playhead
        // means a new key target. Same frame = keep going, unless the animation
        // itself was edited underneath us, in which case the pose the frame is
        // built from is stale and must be re-read. commit() lands any preview
        // and re-seeds in one step.
        if (ctx_->doc()->currentFrame() == sessionFrame_ && !recordedPoseChanged())
            return;
        commit();
        return;
    }
    // The animation can move under a PIXEL session without the playhead or the
    // device changing - a key dragged in the timeline, a rotation scrubbed in
    // the panel. The session's map was captured when it began, so the box, the
    // grabs and the overlay would all be expressed through a stale one. Re-seed
    // rather than leave the user aiming at a frame that is no longer around the
    // stroke.
    if (ctx_->editableDevice() == device_) {
        const Mat3 before = stateToDisplay_;
        refreshDisplayMapping();
        if (!sameMatrix(before, stateToDisplay_)) {
            commit(); // lands any pending pixels, then re-seeds on the new pose
            return;
        }
        return;
    }
    // Another cel (or another layer) is current now: apply what was pending,
    // then start over on the new target.
    const int edited = sessionFrame_;
    commit();
    if (edited >= 0)
        ctx_->doc()->frameCache().invalidate(edited);
}

void TransformTool::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    if (session_)
        return; // re-activation keeps an open (untouched) frame
    beginSession();
    syncOptions();
}

void TransformTool::deactivate() {
    // Krita commits a pending transform when you leave the tool — and leaving
    // also tears the session down (commit() re-seeds, so close it after).
    // Esc is the explicit revert, so nothing is ever lost silently.
    if (session_) {
        commit();
        resetSession();
        syncOptions();
    }
}

// --- geometry --------------------------------------------------------------

Mat3 TransformTool::affineMatrix() const {
    FreeTransformParams affine = free_;
    affine.hasQuad = false; // the quad override is applied on top by matrix()
    return affine.matrix(transformSourceRect());
}

Mat3 TransformTool::currentMatrix() const {
    if (mode_ == Mode::Perspective) {
        Vec2 from[4];
        rectCorners(transformSourceRect(), from);
        Mat3 q;
        if (quadToQuad(from, persp_.corners, q))
            return q;
        return Mat3::identity();
    }
    return free_.matrix(transformSourceRect());
}

// The rect the quad override / perspective map is built against. Pixel mode
// keeps the lifted content bbox (the handles outline what will move). Record
// mode uses the session's record frame — the block's reference rect, i.e. the
// rect the anchor and corners normalize against — so the live handles, the
// keyed values and the compositor's matrixFor are one map. (The affine-only
// Free path ignores the source rect, but Perspective is pure quad-to-quad and
// the Free quad override feeds `rectCorners(src)`, so both need it.)
Rect TransformTool::transformSourceRect() const {
    if (recordMode_ && !recordFrame_.isEmpty())
        return recordFrame_;
    return lift_.bounds;
}

void TransformTool::frameCorners(Vec2 out[4]) const {
    Vec2 src[4];
    // Same source rect currentMatrix() was built from, mapped through it.
    rectCorners(transformSourceRect(), src);
    const Mat3 m = currentMatrix();
    for (int i = 0; i < 4; ++i)
        out[i] = m.map(src[i]);
    // Does the state space still have to be carried out to the display? It does
    // UNLESS the tool's own matrix already holds the pose - which is true only
    // for a Free record session, where the recorded values ARE the transform.
    //
    //   pixel mode (Free and Perspective alike): the state is device space and
    //     the animation displaces the pixels at composite time, so the cage has
    //     to go through the animation's map. Skipping this drew the frame around
    //     the STORED pixels while the user saw them posed elsewhere - the box in
    //     the corner of the page.
    //   Perspective record: the quad is the tool's own state, and the recorded
    //     affine is applied on top by the compositor; stateToDisplay_ is exactly
    //     that affine, so reuse the one map rather than re-deriving a second,
    //     subtly different one.
    const bool stateAlreadyPosed = recordMode_ && mode_ != Mode::Perspective;
    if (!stateAlreadyPosed)
        for (int i = 0; i < 4; ++i)
            out[i] = stateToDisplay_.map(out[i]);
}

Vec2 TransformTool::pivotShown() const {
    return recordMode_ ? currentMatrix().map(free_.pivot)
                        : stateToDisplay_.map(currentMatrix().map(free_.pivot));
}

// (Re)derive the state <-> display pair. Pixel mode maps device space through
// the animation's own matrix; a record session's frame space through the
// recorded affine (the same pose the compositor applies on top of the quad).
// Everything that reads or writes pointer coordinates goes through this pair,
// so the two spaces can never be mixed up again.
void TransformTool::refreshDisplayMapping() {
    Mat3 fwd = Mat3::identity();
    if (!recordMode_) {
        if (selectionScope_) {
            const Selection &sel = ctx_->selection();
            if (sel.offsetActive())
                fwd = sel.documentMatrix();
        } else if (ctx_->doc() && ctx_->doc()->currentLayer()) {
            const PaintLayer *pl = ctx_->doc()->currentLayer();
            if (pl && !pl->params().empty()) {
                const Rect canvas = documentRect();
                fwd = pl->params().matrixFor(pl->params().frameOr(canvas),
                                            double(ctx_->doc()->currentFrame()), nullptr);
            }
        }
    } else {
        // Record mode: the state lives in FRAME space (the pivot, the anchor
        // and the perspective corners are all frame-relative), while the pointer
        // arrives in display space. The recorded affine is exactly the frame ->
        // display map the compositor applies, so its inverse is what puts a
        // dragged handle, a placed pivot or a dragged corner back where the
        // state can be stored. Identity would be wrong for BOTH record modes:
        // it left the pivot in frame space while the handle and the pointer were
        // measured in display space, which is what made a corner scale land on
        // a ratio of two different spaces.
        fwd = recordedAffine();
    }
    bool invertible = false;
    Mat3 inv = fwd.inverted(&invertible);
    if (!invertible) {
        // Degenerate pose: no mapping at all beats a singular one.
        fwd = Mat3::identity();
        inv = Mat3::identity();
    }
    stateToDisplay_ = fwd;
    displayToState_ = inv;
    seededDisplay_ = fwd;
}

QPolygonF TransformTool::mappedQuad(const Rect &r) const {
    Vec2 c[4];
    rectCorners(r, c);
    QPolygonF poly;
    for (int i = 0; i < 4; ++i) {
        const Vec2 d = toDisplay(c[i]);
        poly << QPointF(d.x, d.y);
    }
    return poly;
}

Mat3 TransformTool::recordedAffine() const {
    if (!ctx_ || recordFrame_.isEmpty())
        return Mat3::identity();
    FreeTransformParams a;
    a.translate = {ctx_->recordedValue(ParamId::PositionX, sessionFrame_),
                   ctx_->recordedValue(ParamId::PositionY, sessionFrame_)};
    a.pivot = {recordFrame_.x + ctx_->recordedValue(ParamId::AnchorX, sessionFrame_) * recordFrame_.w,
               recordFrame_.y + ctx_->recordedValue(ParamId::AnchorY, sessionFrame_) * recordFrame_.h};
    a.scaleX = ctx_->recordedValue(ParamId::ScaleX, sessionFrame_);
    a.scaleY = ctx_->recordedValue(ParamId::ScaleY, sessionFrame_);
    a.angleDeg = ctx_->recordedValue(ParamId::Rotation, sessionFrame_);
    a.shearX = ctx_->recordedValue(ParamId::ShearX, sessionFrame_);
    a.shearY = ctx_->recordedValue(ParamId::ShearY, sessionFrame_);
    a.hasQuad = false;
    return a.matrix(recordFrame_);
}

void TransformTool::frameHandles(Vec2 out[8]) const {
    Vec2 c[4];
    frameCorners(c);
    for (int i = 0; i < 4; ++i) {
        out[i] = c[i];
        const Vec2 &a = c[i], &b = c[(i + 1) % 4];
        out[4 + i] = {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
    }
}

int TransformTool::cornerIndexOf(Grip g) const {
    switch (g) {
    case Grip::CornerTL: return 0;
    case Grip::CornerTR: return 1;
    case Grip::CornerBR: return 2;
    case Grip::CornerBL: return 3;
    default: return -1;
    }
}

TransformTool::Grip TransformTool::hitTest(const Vec2 &p, double zoom, bool *shear) const {
    if (shear)
        *shear = false;
    const double r = handleRadiusInDoc(zoom);
    Vec2 h[8];
    frameHandles(h);
    const Grip corners[4] = {Grip::CornerTL, Grip::CornerTR, Grip::CornerBR, Grip::CornerBL};
    for (int i = 0; i < 4; ++i)
        if (dist(p, h[kCornerGrips[i]]) <= r)
            return corners[i];
    if (mode_ == Mode::Perspective) {
        const Grip edges[4] = {Grip::EdgeTop, Grip::EdgeRight, Grip::EdgeBottom, Grip::EdgeLeft};
        for (int i = 0; i < 4; ++i)
            if (dist(p, h[4 + i]) <= r)
                return edges[i];
        // Perspective moves the whole quad from inside OR outside (the rotate
        // gesture belongs to Free, whose state owns the rotation channel), and
        // — like Free — supports Alt-placing the rotation centre. Without this
        // branch an Alt-placed pivot reported PointingHand on hover but a press
        // fell through to Body/Outside, so the pivot grab did nothing.
        if (dist(p, pivotShown()) <= r)
            return Grip::Pivot;
        Vec2 c[4];
        frameCorners(c);
        return pointInQuad(p, c) ? Grip::Body : Grip::Outside;
    }
    // Edge order follows frameHandles(): 4 = top, 5 = right, 6 = bottom, 7 = left.
    const Grip edges[4] = {Grip::EdgeTop, Grip::EdgeRight, Grip::EdgeBottom, Grip::EdgeLeft};
    // Free mode: rotate from outside the frame, move from inside, scale from the
    // handles, and shear when the grab is on an edge but away from its midpoint
    // (Krita's split between the two).
    for (int i = 0; i < 4; ++i) {
        const Vec2 &a = h[i], &b = h[(i + 1) % 4];
        const double len = dist(a, b);
        if (len < 1e-9)
            continue;
        const double t = std::clamp(((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) /
                                        (len * len),
                                    0.0, 1.0);
        const Vec2 proj{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        if (dist(p, proj) <= r && dist(p, h[4 + i]) > r) {
            if (shear)
                *shear = true;
            return edges[i];
        }
    }
    if (dist(p, pivotShown()) <= r)
        return Grip::Pivot;
    Vec2 c[4];
    frameCorners(c);
    return pointInQuad(p, c) ? Grip::Body : Grip::Outside;
}

// --- warp -------------------------------------------------------------------

// Anchor points: a (density+1) square spread over the lifted region, exactly
// where the lattice nodes are, so dragging an anchor moves that node.
void TransformTool::buildWarpAnchors() {
    warp_.reset();
    if (lift_.empty())
        return;
    const int n = std::max(1, warp_.density);
    warp_.origPts.reserve(size_t(n + 1) * size_t(n + 1));
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const double x = double(lift_.bounds.x) + double(lift_.bounds.w) * double(i) / double(n);
            const double y = double(lift_.bounds.y) + double(lift_.bounds.h) * double(j) / double(n);
            warp_.origPts.push_back({x, y});
        }
    }
    warp_.movedPts = warp_.origPts;
    rebuildWarpMesh();
}

void TransformTool::rebuildWarpMesh() {
    if (lift_.empty())
        return;
    if (!warpMesh_.valid())
        // Same floor as the liquify field: the lift is the content bounds, and
        // a 12px cell over a small stroke would leave a lattice too coarse to
        // carry the drag.
        warpMesh_ = DeformMesh::make(lift_.bounds,
                                     std::max(8, lift_.bounds.w / kWarpCellPx),
                                     std::max(8, lift_.bounds.h / kWarpCellPx));
    warpMesh_.moved = warpMesh_.orig;
    if (!warp_.origPts.empty() && warp_.origPts.size() == warp_.movedPts.size())
        warpMesh_.deformByPoints(warp_.origPts, warp_.movedPts, warp_.alpha, warp_.kind);
}

int TransformTool::warpAnchorAt(const Vec2 &p, double zoom) const {
    const double r = handleRadiusInDoc(zoom) * 1.4; // anchors are small dots
    // The anchors and the mesh live in DEVICE space (the pixels are stored
    // there), so the pointer - which is where the ink DISPLAYS - has to be
    // mapped back by the animation's own matrix before the test, or every grab
    // tests against a grid that is not under the cursor.
    const Vec2 q = toDevice(p);
    double best = r;
    int found = -1;
    for (size_t i = 0; i < warp_.movedPts.size(); ++i) {
        const double d = dist(q, warp_.movedPts[i]);
        if (d <= best) {
            best = d;
            found = int(i);
        }
    }
    return found;
}

const DeformMesh *TransformTool::activeMesh() const {
    if (mode_ == Mode::Warp)
        return warpMesh_.valid() ? &warpMesh_ : nullptr;
    if (mode_ == Mode::Liquify)
        return &liquify_.field.mesh();
    return nullptr;
}

void TransformTool::dragWarp(const ToolEvent &e) {
    if (warp_.dragIndex < 0)
        return;
    // In draw mode a drag that started on empty canvas creates a new anchor.
    if (size_t(warp_.dragIndex) >= warp_.movedPts.size())
        return;
    // Same device-space mapping as warpAnchorAt: the mesh edits the stored
    // pixels, so the pointer is brought back through the animation's matrix.
    warp_.movedPts[size_t(warp_.dragIndex)] = toDevice(e.docPos);
    rebuildWarpMesh();
}

// One liquify dab. Move pushes by the pointer movement; Scale and Rotate read
// the drag relative to where the stroke started, which is what makes them feel
// like a twist/pinch rather than a slider.
void TransformTool::liquifyDab(const Vec2 &at) {
    LiquifyField::Params params;
    params.radius = std::max(1.0, liquify_.size * 0.5);
    params.amount = liquify_.amount;
    params.flow = liquify_.flow;
    params.wash = liquify_.wash;
    const Vec2 delta{at.x - liquify_.lastDab.x, at.y - liquify_.lastDab.y};
    double factor = 1.0;
    double angle = 0.0;
    if (liquify_.brush == LiquifyField::Brush::Scale) {
        const double dNow = dist(at, liquify_.strokeRef);
        const double dPrev = dist(liquify_.lastDab, liquify_.strokeRef);
        factor = 1.0 + (dNow - dPrev) / params.radius;
        factor = std::clamp(factor, 0.5, 2.0);
    } else if (liquify_.brush == LiquifyField::Brush::Rotate) {
        const Vec2 a{at.x - liquify_.strokeRef.x, at.y - liquify_.strokeRef.y};
        const Vec2 b{liquify_.lastDab.x - liquify_.strokeRef.x,
                     liquify_.lastDab.y - liquify_.strokeRef.y};
        if (std::hypot(a.x, a.y) > 2.0 && std::hypot(b.x, b.y) > 2.0) {
            angle = std::atan2(a.y, a.x) - std::atan2(b.y, b.x);
            while (angle > M_PI)
                angle -= 2 * M_PI;
            while (angle < -M_PI)
                angle += 2 * M_PI;
        }
    }
    liquify_.field.dab(liquify_.brush, at, delta, factor, angle, params);
    liquify_.lastDab = at;
}

// --- interaction -----------------------------------------------------------

void TransformTool::beginPrimary(const ToolEvent &e) {
    if (!ctx_ || dragging_)
        return;
    ensureSessionTarget();
    if (!session_ && !beginSession())
        return;
    if (!session_)
        return;

    if (recordMode_)
        snapshotRecordBase(); // this drag's "before" picture

    if ((e.mods & Qt::AltModifier) && (mode_ == Mode::Free || mode_ == Mode::Perspective)) {
        // Alt places the rotation centre — the one modifier Krita reserves for
        // the pivot, and the only way to rotate around a corner. The pivot is
        // keyed as a frame-normalized anchor and stored in the session's state
        // space (device space in both modes — toDevice maps the pointer
        // there), so the drag math below measures like with like.
        free_.pivot = toDevice(e.docPos);
        if (recordMode_) {
            // The pivot IS a parameter (the anchor), so placing it keys it. Only
            // the anchor moves, so only the anchor is keyed.
            pushRecordedParams(sessionFrame_);
            snapshotRecordBase();
            ctx_->requestRebuild();
        }
        ctx_->setStatus(QStringLiteral("Rotation centre moved"));
        ctx_->requestRepaint();
        return;
    }

    if (mode_ == Mode::Liquify) {
        dragging_ = true;
        liquify_.stroking = true;
        // The field is in DEVICE space, so the whole stroke reference frame is
        // mapped once here and every later dab reuses it.
        liquify_.strokeRef = toDevice(e.docPos);
        liquify_.lastDab = toDevice(e.docPos);
        liquifyDab(liquify_.lastDab);
        renderPreview(false);
        ctx_->setStatus(QStringLiteral("Painting the deformation · release previews, Enter/Apply lands, Esc reverts"));
        return;
    }

    if (mode_ == Mode::Warp) {
        // Warp grabs anchors, not frame handles; a click on empty canvas adds
        // one in Draw mode.
        const int hit = warpAnchorAt(e.docPos, e.zoom);
        if (hit < 0) {
            if (warp_.drawMode) {
                warp_.origPts.push_back(toDevice(e.docPos));
                warp_.movedPts.push_back(toDevice(e.docPos));
                rebuildWarpMesh();
                warp_.dragIndex = int(warp_.movedPts.size()) - 1;
                dragging_ = true;
                dragStart_ = toDevice(e.docPos);
                ctx_->setStatus(QStringLiteral("Anchor added · drag it to reshape"));
                ctx_->requestRepaint();
            }
            return;
        }
        warp_.dragIndex = hit;
        dragging_ = true;
        dragStart_ = toDevice(e.docPos);
        ctx_->setStatus(QStringLiteral("Dragging an anchor · release previews, Enter/Apply lands, Esc reverts"));
        return;
    }

    grip_ = hitTest(e.docPos, e.zoom, &shearGrab_);
    if (grip_ == Grip::None)
        return;
    dragging_ = true;
    // hitTest works in DISPLAY space (that is where the frame is drawn), but
    // everything the drag then measures lives in DEVICE space: the pivot, the
    // translate, the quad and the keyed pixels. Capture the drag origin in
    // device space here, once, so dragFree compares like with like — mixing the
    // two is what skewed scale/rotate/shear on a displaced (keyframed) layer.
    dragStart_ = toDevice(e.docPos);
    frameCorners(perspStart_);
    // perspStart_ came from frameCorners(), which is display space; take it back
    // to device so the perspective drag works in the space the corners are in.
    for (int i = 0; i < 4; ++i)
        perspStart_[i] = toDevice(perspStart_[i]);
    dragHandleStart_ = dragStart_;
    {
        Vec2 h[8];
        frameHandles(h); // display space, like the frame the user sees
        const int ci = cornerIndexOf(grip_);
        if (ci >= 0)
            dragHandleStart_ = toDevice(h[ci]);
        else if (grip_ == Grip::EdgeTop)
            dragHandleStart_ = toDevice(h[4]);
        else if (grip_ == Grip::EdgeRight)
            dragHandleStart_ = toDevice(h[5]);
        else if (grip_ == Grip::EdgeBottom)
            dragHandleStart_ = toDevice(h[6]);
        else if (grip_ == Grip::EdgeLeft)
            dragHandleStart_ = toDevice(h[7]);
    }
    scaleXStart_ = free_.scaleX;
    scaleYStart_ = free_.scaleY;
    angleStart_ = free_.angleDeg;
    shearXStart_ = free_.shearX;
    shearYStart_ = free_.shearY;
    translateStart_ = free_.translate;
    pivotStart_ = free_.pivot;

    const int ci = cornerIndexOf(grip_);
    if (ci >= 0 && (e.mods & Qt::ControlModifier) && mode_ == Mode::Free) {
        if (recordMode_) {
            // A detached quad cannot be recorded: pushRecordedParams writes
            // only the corners and the affine channels, so a live quad would
            // silently vanish at commit. Keep the affine frame; Ctrl+corner stays
            // a pixel-mode-only distortion.
            //
            // The drag state MUST be undone on the way out. `dragging_` was set
            // before this check, so returning without clearing it left the tool
            // mid-gesture: the rest of the user's drag went on to scale the layer
            // and key that scale, while the status line said the gesture had been
            // refused. A refusal that still performs the drag is worse than no
            // refusal at all.
            dragging_ = false;
            grip_ = Grip::None;
            shearGrab_ = false;
            ctx_->setStatus(
                QStringLiteral("Quad distortion is pixel-only — turn off "
                               "“Record as keyframes” to distort freely"));
            return;
        }
        // Ctrl+corner detaches that corner: from here the frame is a quad.
        free_.hasQuad = true;
        rectCorners(lift_.bounds, free_.quadSrc);
        ctx_->setStatus(QStringLiteral("Free corner · Ctrl+drag to distort"));
        return;
    }
    ctx_->setStatus(statusHint());
}

void TransformTool::dragFree(const ToolEvent &e) {
    // The pointer arrives in DISPLAY space; every value it is measured against
    // here — dragStart_, dragHandleStart_, the pivot, the quad, the keyed
    // pixels — is in DEVICE space (beginPrimary already captured the origins
    // there). Mapping it back once keeps the whole body speaking one space; on
    // a displaced (keyframed) layer the two differ, and the mismatch is what
    // made every scale, rotate and shear land wrong.
    const Vec2 p = toDevice(e.docPos);
    const Vec2 d{p.x - dragStart_.x, p.y - dragStart_.y};
    switch (grip_) {
    case Grip::Body: {
        Vec2 t{d.x, d.y};
        if (e.mods & Qt::ShiftModifier) {
            if (std::abs(d.x) >= std::abs(d.y))
                t.y = 0; // Shift locks the axis
            else
                t.x = 0;
        }
        free_.translate = {translateStart_.x + t.x, translateStart_.y + t.y};
        return;
    }
    case Grip::Pivot:
        free_.pivot = {pivotStart_.x + d.x, pivotStart_.y + d.y};
        return;
    case Grip::Outside: {
        const Vec2 a{dragStart_.x - free_.pivot.x, dragStart_.y - free_.pivot.y};
        const Vec2 b{p.x - free_.pivot.x, p.y - free_.pivot.y};
        double deg = angleStart_ + std::atan2(b.y, b.x) * 180.0 / M_PI -
                     std::atan2(a.y, a.x) * 180.0 / M_PI;
        if (e.mods & Qt::ShiftModifier)
            deg = std::round(deg / 15.0) * 15.0; // Shift snaps to 15 degrees
        free_.angleDeg = deg;
        return;
    }
    default:
        break;
    }

    const int ci = cornerIndexOf(grip_);
    if (ci >= 0 && free_.hasQuad) {
        // Move the grabbed corner in SOURCE space: the opposite corners stay
        // anchored, which is what a perspective drag should do.
        bool ok = false;
        const Mat3 inv = affineMatrix().inverted(&ok);
        if (ok)
            free_.quadSrc[ci] = inv.map(p);
        else
            free_.quadSrc[ci] = p;
        return;
    }

    // Scale in the frame's own axes: rotate both the grabbed handle and the
    // pointer about the pivot by -angle, then take the ratio per axis.
    const double rad = -free_.angleDeg * M_PI / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    auto toLocal = [&](const Vec2 &v) {
        return Vec2{c * v.x - s * v.y, s * v.x + c * v.y};
    };
    const Vec2 base = toLocal({dragHandleStart_.x - free_.pivot.x,
                               dragHandleStart_.y - free_.pivot.y});
    const Vec2 now = toLocal({p.x - free_.pivot.x, p.y - free_.pivot.y});
    if (shearGrab_) {
        // Shear: the grabbed edge slides along itself while the pivot line
        // stays put, so the opposite edge swings the other way.
        if (grip_ == Grip::EdgeTop || grip_ == Grip::EdgeBottom) {
            if (std::abs(base.y) > 1.0)
                free_.shearX = shearXStart_ + (now.x - base.x) / std::abs(base.y);
        } else if (std::abs(base.x) > 1.0) {
            free_.shearY = shearYStart_ + (now.y - base.y) / std::abs(base.x);
        }
        return;
    }
    double fx = std::abs(base.x) > 1.0 ? now.x / base.x : 1.0;
    double fy = std::abs(base.y) > 1.0 ? now.y / base.y : 1.0;
    if (grip_ == Grip::EdgeLeft || grip_ == Grip::EdgeRight)
        fy = 1.0; // a side edge scales x only
    if (grip_ == Grip::EdgeTop || grip_ == Grip::EdgeBottom)
        fx = 1.0; // a top/bottom edge scales y only
    if (keepAspect_ || (e.mods & Qt::ShiftModifier)) {
        const double f = std::abs(fx - 1.0) > std::abs(fy - 1.0) ? fx : fy;
        fx = f;
        fy = f;
    }
    free_.scaleX = std::max(-20.0, std::min(20.0, scaleXStart_ * fx));
    free_.scaleY = std::max(-20.0, std::min(20.0, scaleYStart_ * fy));
}

void TransformTool::dragPerspective(const ToolEvent &e) {
    // Same one-space rule as dragFree: the pointer is display space, the corners
    // and the captured start are frame space.
    const Vec2 p = toDevice(e.docPos);
    if (grip_ == Grip::Body || grip_ == Grip::Outside) {
        // Both move the whole quad. Perspective deliberately does NOT rotate
        // from outside the way Free does: the recorded pose's rotation lives in
        // the model's affine channels, not in this tool's state, so a rotate
        // here would have to re-key a channel the quad does not own - the
        // gesture belongs to Free mode. The hit test and the cursor say
        // "moves the quad" for both grips, so nothing lies to the user.
        const Vec2 d{p.x - dragStart_.x, p.y - dragStart_.y};
        Vec2 t{d.x, d.y};
        if (e.mods & Qt::ShiftModifier) {
            if (std::abs(d.x) >= std::abs(d.y))
                t.y = 0;
            else
                t.x = 0;
        }
        for (int i = 0; i < 4; ++i)
            persp_.corners[i] = {perspStart_[i].x + t.x, perspStart_[i].y + t.y};
        return;
    }
    // A corner or an edge midpoint, in FRAME space. Refuse a move that would
    // collapse the quad: three near-collinear corners make the homography
    // singular, and the old code let the user walk straight into that - the
    // preview then collapsed or jumped and the keyed value was unusable. Keep
    // the last valid quad instead.
    auto acceptQuad = [&](const int a, const int b, const Vec2 pa, const Vec2 pb) {
        Vec2 trial[4];
        for (int i = 0; i < 4; ++i)
            trial[i] = persp_.corners[i];
        trial[a] = pa;
        if (b >= 0)
            trial[b] = pb;
        if (quadIsUsable(trial)) {
            persp_.corners[a] = pa;
            if (b >= 0)
                persp_.corners[b] = pb;
        }
    };
    const int ci = cornerIndexOf(grip_);
    if (ci >= 0) {
        acceptQuad(ci, -1, p, Vec2{});
        return;
    }
    // Edge midpoint: drag both of its corners along the drag delta (Krita's
    // perspective strategy does exactly this).
    int a = -1, b = -1;
    switch (grip_) {
    case Grip::EdgeTop: a = 0; b = 1; break;
    case Grip::EdgeRight: a = 1; b = 2; break;
    case Grip::EdgeBottom: a = 2; b = 3; break;
    default: a = 3; b = 0; break;
    }
    const Vec2 d{p.x - dragStart_.x, p.y - dragStart_.y};
    acceptQuad(a, b, {perspStart_[a].x + d.x, perspStart_[a].y + d.y},
               {perspStart_[b].x + d.x, perspStart_[b].y + d.y});
}

void TransformTool::continuePrimary(const ToolEvent &e) {
    if (!dragging_ || !session_)
        return;
    // One throttle for every mode's drag: a move either re-resamples the
    // region or, in record mode, re-projects the whole canvas, and doing that
    // per mouse-move event is what made Warp and Perspective feel like a hang.
    // The LAST move of a burst is never dropped - endPrimary renders the
    // settled result at full quality.
    const bool due = previewDue();
    if (!due) {
        previewQueued_ = true;
    }
    if (mode_ == Mode::Liquify) {
        // Space the dabs evenly along the pointer path, then re-render (the
        // field accumulates, so the pixels are rebuilt from the pristine lift
        // only when a frame of the drag is actually shown). lastDab is in the
        // state's space, so the pointer is mapped before interpolating.
        const Vec2 to = toDevice(e.docPos);
        const double step = std::max(1.0, liquify_.spacing * liquify_.size);
        const double total = dist(liquify_.lastDab, to);
        const int dabs = std::max(1, int(total / step));
        for (int i = 1; i <= dabs; ++i) {
            const double t = double(i) / double(dabs);
            liquifyDab({liquify_.lastDab.x + (to.x - liquify_.lastDab.x) * t,
                        liquify_.lastDab.y + (to.y - liquify_.lastDab.y) * t});
        }
        if (due)
            renderPreview(false);
        return;
    }
    switch (mode_) {
    case Mode::Perspective: dragPerspective(e); break;
    case Mode::Warp: dragWarp(e); break;
    default: dragFree(e); break;
    }
    if (due)
        renderPreview(fastPreview_);
    syncOptions();
}

void TransformTool::endPrimary(const ToolEvent &e) {
    Q_UNUSED(e);
    if (!dragging_)
        return;
    dragging_ = false;
    grip_ = Grip::None;
    warp_.dragIndex = -1;
    if (mode_ == Mode::Liquify)
        liquify_.stroking = false;
    // The release always renders the settled result at the CHOSEN quality, not
    // the drag's: a throttled drag would otherwise leave a half-finished frame
    // on screen until the next move. This is also the moment the last queued
    // move finally lands, so the picture matches the parameters that were
    // written.
    if (previewDue() || previewQueued_) {
        previewQueued_ = false;
        renderPreview(false);
    }
    syncOptions();
    // Explicit Apply (Krita model): the release only PREVIEWS. Enter, the
    // options-bar Apply button, double-click or any context switch lands it;
    // Esc reverts to the last Apply. The cage stays up throughout.
    if (ctx_)
        ctx_->setStatus(previewStatus());
}

// Undo everything the current session has written to the device, leaving the
// frame ready for another attempt.
void TransformTool::revertPending() {
    if (!device_ || !pending_)
        return;
    if (!memento_.empty())
        device_->store().restore(memento_);
    memento_ = TileMemento{};
    lastStamp_ = Rect{};
    pending_ = false;
    ctx_->doc()->frameCache().invalidate(ctx_->doc()->currentFrame());
    ctx_->requestRebuild();
}

void TransformTool::cancelPrimary() {
    if (!ctx_)
        return;
    if (recordMode_) {
        // Esc abandons everything since the last Apply: the parameters go back
        // to what they were then, and no undo entry is created (so Esc is not
        // something the user has to Ctrl+Z away). The session re-seeds around
        // the restored pose and stays live.
        ctx_->doc()->cancelParamRecord();
        reseedSession();
        ctx_->requestRebuild();
        ctx_->setStatus(statusHint());
        return;
    }
    if (pending_ && device_)
        revertPending(); // Esc reverts uncommitted work; history is untouched
    // Back to the last Apply (or the session start) with the cage still up.
    reseedSession();
    ctx_->setStatus(statusHint());
}

// --- preview + commit ------------------------------------------------------

// The drag throttle. Reads the host's clock, and treats "no clock" (0, the
// ToolHost default) as "never throttle" so a headless test host still sees every
// intermediate frame.
bool TransformTool::previewDue() {
    const double now = (ctx_ && ctx_->host()) ? ctx_->host()->nowSec() : 0.0;
    if (now <= 0.0)
        return true;
    if (now - lastPreviewSec_ < 0.033)
        return false;
    lastPreviewSec_ = now;
    return true;
}

void TransformTool::renderPreview(bool fast) {
    if (!session_)
        return;
    if (recordMode_) {
        // Nothing is written to the device - the composite evaluates the keys,
        // so the preview IS the real result. The whole canvas is rebuilt
        // because an animated layer can end up anywhere, and a partial repaint
        // would leave a ghost behind. That rebuild is a FULL re-projection
        // (adjustment lift + bicubic resample at 1920x1080), which is why the
        // DRAG path throttles it (previewDue) instead of calling this per
        // mouse-move; a one-shot change (slider, flip, Apply) still lands now.
        pending_ = false;
        pushRecordedParams(sessionFrame_);
        ctx_->requestRebuild();
        return;
    }
    if (!device_ || lift_.empty())
        return;
    const Rect doc = documentRect();
    Rect dst;
    const DeformMesh *mesh = activeMesh();
    Mat3 m = Mat3::identity();
    if (mesh) {
        // Warp / liquify: no matrix to invert, the field defines the mapping.
        if (mesh->isIdentity()) {
            revertPending(); // dragged back to where it started
            return;
        }
        // A runaway or non-finite field is refused outright: keep the last good
        // picture rather than hand a garbage destination rect to the resampler.
        if (!meshIsSane(*mesh, doc)) {
            revertPending();
            return;
        }
        dst = intersectRect(mesh->movedBounds(), doc);
    } else {
        m = currentMatrix();
        if (m.isIdentity()) {
            revertPending(); // nothing moved: keep the device pristine so a
                             // Reset-to-defaults never records a no-op undo step
            return;
        }
        bool ok = false;
        m.inverted(&ok);
        if (!ok)
            return; // degenerate frame (zero scale): keep the last good preview
        dst = intersectRect(mappedBounds(lift_, m), doc);
    }
    if (dst.isEmpty())
        return;

    // Rebuild from the pristine lift: intermediate drags can never smear.
    // The previous frame's stamp is repainted too: it was already restored to
    // pristine on the device, but the canvas only re-projects the dirty rect,
    // so omitting it would leave a ghost of the old position behind.
    if (!memento_.empty())
        device_->store().restore(memento_);
    memento_ = TileMemento{};
    Rect dirty = eraseRegion(*device_, area_, selectionScope_ ? &eraseMask_ : nullptr, &memento_);
    dirty = uniteRect(dirty, lastStamp_);
    const Interpolation filter = isLatticeMap(m)
                                     ? Interpolation::Nearest
                                     : (fast ? Interpolation::Bilinear : filter_);
    const FloatRegion mapped = mesh ? warpRegion(lift_, dst, *mesh)
                                    : resampleRegion(lift_, dst, m, filter);
    const Rect stamped = stampRegion(*device_, mapped, 0, 0, doc, &memento_);
    lastStamp_ = stamped;
    dirty = uniteRect(dirty, stamped);
    pending_ = !memento_.empty();
    if (!dirty.isEmpty())
        ctx_->markDirty(dirty);
    ctx_->requestRepaint();
}

bool TransformTool::commit() {
    if (!session_)
        return false;
    if (recordMode_) {
        // The keys are already written; closing the record turns everything
        // since the last Apply into ONE undo step. No pixel command is pushed,
        // because no pixel moved. The session is then RE-SEEDED (not dropped),
        // so the cage stays up around the new pose for the next drag.
        const QString done = recordCommitStatus();
        reseedSession();
        ctx_->requestRebuild();
        if (ctx_)
            ctx_->setStatus(done);
        return true;
    }
    const bool touched = pending_ && !memento_.empty();
    if (touched) {
        if (selectionScope_) {
            // The selection travels with the pixels it clips (Krita transforms
            // the selection outline along with the contents): through the mesh
            // in the deform modes, through the matrix otherwise.
            if (const DeformMesh *mesh = activeMesh()) {
                ctx_->selection().resampleMask(*mesh);
            } else {
                // Match the pixels: a lattice map is exact with nearest, and a
                // mask wants crisp edges rather than a filtered fringe — Lanczos
                // /Bicubic ring an 8-bit step edge into ghost outline segments,
                // so the mask is clamped to Nearest/Bilinear however the FILTER
                // combo is set (that choice is about image resampling quality,
                // not about a boolean coverage edge). The matrix is in device
                // space (the session state never bakes the animation offset in),
                // so the mask resamples with exactly the map the pixels got.
                const Mat3 m = currentMatrix();
                const Interpolation maskFilter =
                    isLatticeMap(m) ? Interpolation::Nearest : Interpolation::Bilinear;
                ctx_->selection().resampleMask(m, maskFilter);
            }
            ctx_->selection().invalidateOutline();
            ctx_->notifySelectionChanged();
        }
        ctx_->pushStrokeCommand(commandName(), device_, std::move(memento_));
        if (sessionFrame_ >= 0)
            ctx_->doc()->frameCache().invalidate(sessionFrame_);
        ctx_->setStatus(QStringLiteral("Applied %1").arg(commandName().toLower()));
    }
    // Krita-style persistence: Apply lands the work but the tool stays live —
    // the frame is re-seeded around the committed state for the next drag,
    // instead of vanishing until the next press.
    reseedSession();
    ctx_->requestRebuild();
    return touched;
}

QString TransformTool::commandName() const {
    switch (mode_) {
    case Mode::Free: return QStringLiteral("Transform");
    case Mode::Perspective: return QStringLiteral("Perspective transform");
    case Mode::Warp: return QStringLiteral("Warp");
    default: return QStringLiteral("Liquify");
    }
}

void TransformTool::flipSelection(bool horizontal) {
    if (!ctx_)
        return;
    if (!session_ && !beginSession())
        return;
    if (mode_ == Mode::Warp) {
        // Mirror the dragged anchors about the region's centre line.
        const double axis = horizontal ? double(lift_.bounds.x) + lift_.bounds.w * 0.5
                                       : double(lift_.bounds.y) + lift_.bounds.h * 0.5;
        for (Vec2 &p : warp_.movedPts) {
            if (horizontal)
                p.x = 2.0 * axis - p.x;
            else
                p.y = 2.0 * axis - p.y;
        }
        rebuildWarpMesh();
        renderPreview(false);
        syncOptions();
        if (pending_)
            commit(); // one-shot op: lands immediately, no Apply step
        ctx_->setStatus(horizontal ? QStringLiteral("Flipped horizontally")
                                   : QStringLiteral("Flipped vertically"));
        return;
    }
    if (mode_ == Mode::Perspective) {
        // Mirror the quad about its own centre line.
        double sum = 0;
        for (int i = 0; i < 4; ++i)
            sum += horizontal ? persp_.corners[i].x : persp_.corners[i].y;
        const double axis = sum * 0.25;
        for (int i = 0; i < 4; ++i) {
            if (horizontal)
                persp_.corners[i].x = 2.0 * axis - persp_.corners[i].x;
            else
                persp_.corners[i].y = 2.0 * axis - persp_.corners[i].y;
        }
        // A mirror swaps the winding: re-express the quad in canonical order.
        std::swap(persp_.corners[1], persp_.corners[3]);
    } else if (horizontal) {
        free_.scaleX = -free_.scaleX;
    } else {
        free_.scaleY = -free_.scaleY;
    }
    renderPreview(false);
    syncOptions();
    // In record mode renderPreview only wrote keys into the still-open record
    // (pending_ stays false there), so gate on it too: the flip must close its
    // own undo step instead of leaking into the next drag's.
    if (pending_ || recordMode_)
        commit(); // one-shot op: lands immediately, no Apply step
    ctx_->setStatus(horizontal ? QStringLiteral("Flipped horizontally")
                               : QStringLiteral("Flipped vertically"));
}

void TransformTool::rotateQuarter(bool clockwise) {
    if (!ctx_)
        return;
    if (!session_ && !beginSession())
        return;
    if (mode_ == Mode::Warp) {
        const Vec2 c{double(lift_.bounds.x) + lift_.bounds.w * 0.5,
                     double(lift_.bounds.y) + lift_.bounds.h * 0.5};
        const Mat3 r = Mat3::translation(c.x, c.y)
                           .multiply(Mat3::rotation(clockwise ? 90.0 : -90.0))
                           .multiply(Mat3::translation(-c.x, -c.y));
        for (Vec2 &p : warp_.movedPts)
            p = r.map(p);
        rebuildWarpMesh();
        renderPreview(false);
        syncOptions();
        ctx_->setStatus(clockwise ? QStringLiteral("Rotated 90° clockwise")
                                  : QStringLiteral("Rotated 90° counter-clockwise"));
        return;
    }
    if (mode_ == Mode::Perspective) {
        // Rotate the quad about its centroid.
        Vec2 c{0, 0};
        for (int i = 0; i < 4; ++i)
            c = {c.x + persp_.corners[i].x * 0.25, c.y + persp_.corners[i].y * 0.25};
        const Mat3 r = Mat3::translation(c.x, c.y)
                           .multiply(Mat3::rotation(clockwise ? 90.0 : -90.0))
                           .multiply(Mat3::translation(-c.x, -c.y));
        for (int i = 0; i < 4; ++i)
            persp_.corners[i] = r.map(persp_.corners[i]);
    } else {
        free_.angleDeg += clockwise ? 90.0 : -90.0;
    }
    renderPreview(false);
    syncOptions();
    // Same record-mode close as flipSelection: the keys are written, the open
    // record must become their undo step now, not the next drag's.
    if (pending_ || recordMode_)
        commit(); // one-shot op: lands immediately, no Apply step
    ctx_->setStatus(clockwise ? QStringLiteral("Rotated 90° clockwise")
                              : QStringLiteral("Rotated 90° counter-clockwise"));
}

// --- cursor + overlay ------------------------------------------------------

QCursor TransformTool::cursor() const {
    if (mode_ == Mode::Liquify)
        return QCursor(Qt::BlankCursor); // the brush ring is the cursor
    return QCursor(Qt::ArrowCursor);
}

QCursor TransformTool::cursorAt(const Vec2 &docPos, double zoom) const {
    if (!session_ || mode_ == Mode::Liquify)
        return cursor(); // liquify paints, the overlay ring is the cursor
    if (mode_ == Mode::Warp)
        return QCursor(warpAnchorAt(docPos, zoom) >= 0 ? Qt::PointingHandCursor
                                                       : (warp_.drawMode ? Qt::CrossCursor
                                                                         : Qt::ArrowCursor));
    switch (hitTest(docPos, zoom)) {
    case Grip::CornerTL:
    case Grip::CornerBR:
        return QCursor(Qt::SizeFDiagCursor);
    case Grip::CornerTR:
    case Grip::CornerBL:
        return QCursor(Qt::SizeBDiagCursor);
    case Grip::EdgeLeft:
    case Grip::EdgeRight:
        return QCursor(Qt::SizeHorCursor);
    case Grip::EdgeTop:
    case Grip::EdgeBottom:
        return QCursor(Qt::SizeVerCursor);
    case Grip::Pivot:
        return QCursor(Qt::PointingHandCursor);
    case Grip::Outside:
        return QCursor(Qt::CrossCursor);
    default:
        return QCursor(Qt::SizeAllCursor);
    }
}

void TransformTool::paintOverlay(QPainter &p, const OverlayInfo &info) const {
    // Bounds, not pixels: in RECORD mode the lift is a bounds-only region (the
    // pixels are never touched), and `lift_.empty()` (which also tests
    // `px.empty()`) hid the whole frame — the "bounding box disappears when
    // the layer is animated" bug. Every branch below draws from bounds and
    // param state only.
    if (!session_ || lift_.bounds.isEmpty())
        return;
    const double px = 1.0 / std::max(0.05, info.zoom); // one screen pixel in doc units

    if (mode_ == Mode::Warp) {
        // Anchors, with the ones that have been dragged highlighted. The mesh
        // is device-space, so the dots are mapped through the animation's
        // matrix - drawing them raw strands them on the stored pixels while the
        // stroke displays rotated/scaled elsewhere.
        p.save();
        const double r = 3.5 * px;
        for (size_t i = 0; i < warp_.movedPts.size(); ++i) {
            const bool moved = std::hypot(warp_.movedPts[i].x - warp_.origPts[i].x,
                                          warp_.movedPts[i].y - warp_.origPts[i].y) > 0.01;
            p.setPen(QPen(QColor(20, 22, 26, 220), 1.2 * px));
            p.setBrush(moved ? QColor(120, 190, 240, 240) : QColor(240, 242, 245, 220));
            const Vec2 d = toDisplay(warp_.movedPts[i]);
            p.drawEllipse(QPointF(d.x, d.y), r, r);
        }
        // Region outline: the MOVED bounds while deformed (the raw lift rect
        // stays behind at the pre-drag position), drawn as the TRANSFORMED
        // quad. An axis-aligned rect through a display offset cannot describe a
        // rotated or scaled layer, and it was the reason the "box around the
        // stroke" looked wrong for Warp and Perspective.
        Rect b = lift_.bounds;
        if (warpMesh_.valid() && !warpMesh_.isIdentity())
            b = warpMesh_.movedBounds();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 120), 0, Qt::DashLine));
        p.drawPolygon(mappedQuad(b));
        p.restore();
        return;
    }

    if (mode_ == Mode::Liquify) {
        // The region the brush is allowed to deform, in the same terms as Warp:
        // the field's moved bounds once it has been deformed, drawn as the
        // transformed quad so it surrounds the stroke the user sees. Without it
        // Liquify had NO box at all - only the brush ring.
        p.save();
        const double r = std::max(1.0, liquify_.size * 0.5);
        const QPointF c(info.hoverPos.x, info.hoverPos.y);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 210), 0));
        p.drawEllipse(c, r, r);
        p.setPen(QPen(QColor(0, 0, 0, 170), 0));
        p.drawEllipse(c, r + 1.5 * px, r + 1.5 * px);
        const DeformMesh &mesh = liquify_.field.mesh();
        Rect b = lift_.bounds;
        if (mesh.valid() && !mesh.isIdentity())
            b = mesh.movedBounds();
        p.setPen(QPen(QColor(255, 255, 255, 110), 0, Qt::DashLine));
        p.drawPolygon(mappedQuad(b));
        p.restore();
        return;
    }

    Vec2 h[8];
    frameHandles(h);

    QPolygonF poly;
    for (int i = 0; i < 4; ++i)
        poly << QPointF(h[i].x, h[i].y);

    p.save();
    p.setBrush(Qt::NoBrush);
    // Frame: dark under-stroke + dashed white over it, matching the ants.
    p.setPen(QPen(QColor(0, 0, 0, 190), 0));
    p.drawPolygon(poly);
    p.setPen(QPen(QColor(255, 255, 255, 230), 0, Qt::DashLine));
    p.drawPolygon(poly);

    // Edge handles: small squares on the frame's outline.
    const double hs = 4.0 * px;
    p.setPen(QPen(QColor(20, 22, 26, 220), 1.4 * px));
    p.setBrush(QColor(240, 242, 245, 235));
    for (int i = 4; i < 8; ++i)
        p.drawRect(QRectF(h[i].x - hs, h[i].y - hs, hs * 2, hs * 2));

    // Corner handles: slightly larger squares, the primary grab points.
    const double cs = 5.0 * px;
    for (int i = 0; i < 4; ++i)
        p.drawRect(QRectF(h[i].x - cs, h[i].y - cs, cs * 2, cs * 2));

    if (mode_ != Mode::Perspective) {
        // Pivot: circle with a crosshair, at the SHOWN pivot (display space),
        // so it sits on the frame even when the ink is displaced by keyframes.
        const Vec2 piv = pivotShown();
        const double r = 6.0 * px;
        p.setBrush(QColor(20, 22, 26, 200));
        p.setPen(QPen(QColor(255, 255, 255, 235), 1.4 * px));
        p.drawEllipse(QPointF(piv.x, piv.y), r, r);
        p.drawLine(QPointF(piv.x - r * 1.6, piv.y), QPointF(piv.x + r * 1.6, piv.y));
        p.drawLine(QPointF(piv.x, piv.y - r * 1.6), QPointF(piv.x, piv.y + r * 1.6));
    }
    p.restore();
}

// --- options ---------------------------------------------------------------

void TransformTool::syncOptions() {
    // Blocked: these are the live field mirrors — a programmatic refresh must
    // not re-fire the realtime handlers below.
    const QSignalBlocker b1(wBox_), b2(hBox_), b3(angleBox_), b4(shearXBox_), b5(shearYBox_);
    if (wBox_)
        wBox_->setValue(free_.scaleX * 100.0);
    if (hBox_)
        hBox_->setValue(free_.scaleY * 100.0);
    if (angleBox_)
        angleBox_->setValue(free_.angleDeg);
    if (shearXBox_)
        shearXBox_->setValue(free_.shearX * 100.0);
    if (shearYBox_)
        shearYBox_->setValue(free_.shearY * 100.0);
    // The bar RECORD toggle follows the mode and the state (programmatic flips
    // commit through the setter). It stays VISIBLE in the mesh modes, disabled
    // with a reason: hiding it made a checked RECORD look like choosing another
    // mode had switched it off.
    if (recordToggle_) {
        const bool keyable = mode_ == Mode::Free || mode_ == Mode::Perspective;
        recordToggle_->setEnabled(keyable);
        recordToggle_->setText(keyable ? QStringLiteral("Record")
                                       : QStringLiteral("Record (bakes pixels)"));
        if (recordToggle_->isChecked() != recordKeyframes_) {
            const QSignalBlocker b(recordToggle_);
            recordToggle_->setChecked(recordKeyframes_);
        }
    }
}

QString TransformTool::statusHint() const {
    if (recordMode_) {
        return QStringLiteral("Recording: drags preview keyframes at the playhead and leave the "
                              "pixels untouched · Enter/Apply lands them · Esc reverts");
    }
    QString base;
    switch (mode_) {
    case Mode::Free:
        base = QStringLiteral("Drag inside to move · outside to rotate · corners/edges to scale "
                              "· Ctrl+corner distorts · Alt sets the pivot · release previews, "
                              "Enter/Apply lands");
        break;
    case Mode::Perspective:
        base = QStringLiteral("Drag the corners; edge handles move both ends · release previews, "
                              "Enter/Apply lands");
        break;
    case Mode::Warp:
        base = QStringLiteral("Drag the anchor points to reshape · release previews, Enter/Apply lands");
        break;
    default:
        base = QStringLiteral("Paint to push the pixels · release previews, Enter/Apply lands");
        break;
    }
    // Warp and Liquify are raster deformations: their state is a mesh, not a
    // handful of scalars, so there is nothing for the 18 transform channels to
    // hold and they always bake into the cel. Say so while the layer HAS
    // keyframes - otherwise a user who checked RECORD in another mode finds the
    // toggle gone and the deformation silently baked, which reads as "it did
    // not work".
    if (mode_ == Mode::Warp || mode_ == Mode::Liquify) {
        if (ctx_ && ctx_->doc() && ctx_->doc()->autoKeyEnabled()) {
            if (const AdjustmentLayer *a = ctx_->recordTarget())
                if (a->params().keyCount() > 0)
                    base += QStringLiteral(" · this deformation bakes into the cel (a mesh cannot "
                                           "be keyframed) and travels with the keyed pose");
        }
    }
    // Opt-in discoverability: pixel-baking on an ANIMATED layer is exactly
    // where the old implicit takeover confused people, so say where the
    // key-writing lives instead of staying silent about it.
    if ((mode_ == Mode::Free || mode_ == Mode::Perspective) && ctx_ && ctx_->doc() &&
        !recordKeyframes_ && ctx_->doc()->autoKeyEnabled()) {
        if (const AdjustmentLayer *a = ctx_->recordTarget())
            if (a->params().keyCount() > 0)
                base += QStringLiteral(" · layer has keys — enable RECORD to animate it");
    }
    return base;
}

QString TransformTool::previewStatus() const {
    if (recordMode_) {
        // The first-key hint belongs here too, not just at Apply: the keys
        // are already written into the open record, so what-Applies is known
        // the moment the drag ends.
        QString s = QStringLiteral("Previewing keyframes — Enter/Apply lands them · Esc reverts");
        const QString hint = firstKeyHint();
        if (!hint.isEmpty()) s += hint;
        return s;
    }
    return QStringLiteral("Preview — Enter/Apply lands it · Esc reverts");
}

// Reset ONLY the selected mode's parameters (other modes keep theirs).
// Device pixels are the caller's job (revertPending first when uncommitted
// work is showing).
void TransformTool::resetModeParams() {
    switch (mode_) {
    case Mode::Free:
        free_.reset();
        if (session_) {
            // The pivot resets to the centre of the source rect — the content
            // bbox in pixel mode, the block's reference frame in record mode —
            // matching the anchor default (0.5, 0.5) the compositor normalizes
            // against that same rect.
            const Rect src = transformSourceRect();
            free_.pivot = {double(src.x) + src.w * 0.5, double(src.y) + src.h * 0.5};
        }
        break;
    case Mode::Perspective:
        persp_.reset();
        if (session_)
            rectCorners(transformSourceRect(), persp_.corners);
        break;
    case Mode::Warp:
        if (session_)
            buildWarpAnchors();
        else
            warp_.reset();
        break;
    default:
        liquify_.resetState();
        if (session_)
            liquify_.field.reset(lift_.bounds);
        break;
    }
}

QWidget *TransformTool::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto *modeBox = InkOptions::combo({modeName(Mode::Free), modeName(Mode::Perspective),
                                       modeName(Mode::Warp), modeName(Mode::Liquify)},
                                      page, 110);
    modeBox->setCurrentIndex(int(mode_));
    InkOptions::addField(row, QStringLiteral("MODE"), modeBox);
    row->addWidget(InkOptions::separator(page));

    // Warp/Liquify/Perspective are MODES of this one tool: the combo switches
    // modes in place (commit in-flight work, re-configure for the new mode).
    connect(modeBox, &QComboBox::currentIndexChanged, page, [this](int index) {
        if (!ctx_ || !ctx_->doc())
            return;
        setMode(TransformTool::Mode(index));
    });
    row->addWidget(InkOptions::separator(page));

    // Settings live in a collapsible floating panel showing ONLY the active
    // mode's controls on a fixed form grid: the popup floats over the canvas
    // and contributes nothing to the main window's minimum size, so the bar
    // can never widen the app past the screen. One flat card surface, one
    // fixed width for every mode (no resize jump when switching).
    auto *popup = new QFrame(page, Qt::Popup);
    popup->setObjectName(QStringLiteral("TransformPopup"));
    popup->setFixedWidth(336);
    auto *popLay = new QVBoxLayout(popup);
    popLay->setContentsMargins(14, 10, 14, 10);
    popLay->setSpacing(6);
    // Header: mode title + per-mode Reset (resets only the selected mode), over a
    // hairline so the panel opens with a title bar instead of a floating caption.
    auto *headRow = new QHBoxLayout();
    headRow->setContentsMargins(0, 0, 0, 0);
    headRow->setSpacing(8);
    auto *title = new QLabel(modeName(mode_), popup);
    title->setObjectName(QStringLiteral("PopupTitle"));
    headRow->addWidget(title);
    headRow->addStretch(1);
    auto *modeReset = InkOptions::button(QStringLiteral("Reset"), popup);
    modeReset->setObjectName(QStringLiteral("OptButton"));
    modeReset->setToolTip(QStringLiteral("Reset this mode's settings (Esc)"));
    headRow->addWidget(modeReset);
    popLay->addLayout(headRow);
    auto *headRule = new QFrame(popup);
    headRule->setObjectName(QStringLiteral("OptSepH"));
    headRule->setFrameShape(QFrame::HLine);
    headRule->setFrameShadow(QFrame::Plain);
    popLay->addWidget(headRule);
    connect(modeReset, &QToolButton::clicked, page, [this] {
        if (pending_ && device_)
            revertPending(); // restore pre-tweak pixels first
        resetModeParams();
        syncOptions();
        if (session_)
            renderPreview(false); // identity guard keeps this a clean no-op
        if (ctx_)
            ctx_->setStatus(statusHint());
    });
    auto *stack = new QStackedWidget(popup);
    stack->setObjectName(QStringLiteral("CardStack"));
    popLay->addWidget(stack);
    settingsStack_ = stack;
    settingsTitle_ = title;
    // One page = a column holding a fixed form grid (captions + controls in
    // straight columns) plus, for most modes, the shared quality block.
    struct PageBuild {
        QWidget *page;
        QVBoxLayout *col;
        QGridLayout *grid;
    };
    auto makePage = [&](const QString &section) -> PageBuild {
        auto *pg = new QWidget(stack);
        pg->setObjectName(QStringLiteral("CardStack"));
        auto *col = new QVBoxLayout(pg);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(6);
        auto *box = new QWidget(pg);
        box->setObjectName(QStringLiteral("CardStack"));
        auto *grid = new QGridLayout(box);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(6);
        grid->setColumnStretch(1, 1);
        InkOptions::addSection(col, section, pg);
        col->addWidget(box);
        stack->addWidget(pg);
        return {pg, col, grid};
    };
    // Sliders stretch with the grid column; the shared fixed-width helper
    // would pin them, so relax them back to a flexible minimum here.
    auto flexSlider = [](QSlider *s, int minWidth) {
        s->setMinimumWidth(minWidth);
        s->setMaximumWidth(QWIDGETSIZE_MAX);
        return s;
    };
    // Same for the popup's combos: every row fills the panel instead of leaving
    // a ragged empty right edge next to a pinned control.
    auto flexCombo = [](QComboBox *c, int minWidth) {
        c->setMinimumWidth(minWidth);
        c->setMaximumWidth(QWIDGETSIZE_MAX);
        c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return c;
    };

    auto spin = [](QWidget *owner, double min, double max, double value, const QString &suffix,
                   int width, int decimals) {
        auto *s = new InkSpinBox(owner);
        s->setRange(min, max);
        s->setDecimals(decimals);
        s->setSuffix(suffix);
        s->setValue(value);
        s->setMinimumWidth(width);
        s->setAlignment(Qt::AlignRight); // values sit next to their steppers
        s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        s->setKeyboardTracking(false);
        return s;
    };

    // --- free page: the same numbers the handles drive, one per grid row
    //     (Krita's W/H/angle/shear group). The Link toggle rides the W row;
    //     H gets its own row so every caption sits in the same column. ---
    const PageBuild freeBuilt = makePage(QStringLiteral("TRANSFORM"));
    auto *freePage = freeBuilt.page;
    auto *freeCol = freeBuilt.col;
    auto *freeGrid = freeBuilt.grid;
    wBox_ = spin(freePage, -2000, 2000, free_.scaleX * 100.0, QStringLiteral(" %"), 84, 1);
    hBox_ = spin(freePage, -2000, 2000, free_.scaleY * 100.0, QStringLiteral(" %"), 84, 1);
    angleBox_ = spin(freePage, -3600, 3600, free_.angleDeg, QStringLiteral("°"), 84, 1);
    shearXBox_ = spin(freePage, -200, 200, free_.shearX * 100.0, QStringLiteral(" %"), 84, 1);
    shearYBox_ = spin(freePage, -200, 200, free_.shearY * 100.0, QStringLiteral(" %"), 84, 1);
    // Realtime numeric edits: typing a value lifts (if needed) and previews
    // immediately, exactly like a handle drag. The result lands with Apply
    // (Enter, the Apply button) or any context switch — Esc reverts.
    auto spinLive = [this](bool) {
        if (!ctx_)
            return false;
        if (!session_ && !beginSession()) {
            syncOptions();
            return false;
        }
        return true;
    };
    connect(wBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), freePage, [this, spinLive](double v) {
        if (!spinLive(true))
            return;
        free_.scaleX = v / 100.0;
        if (keepAspect_)
            free_.scaleY = free_.scaleX;
        renderPreview(false);
        syncOptions();
    });
    connect(hBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), freePage, [this, spinLive](double v) {
        if (!spinLive(true))
            return;
        free_.scaleY = v / 100.0;
        if (keepAspect_)
            free_.scaleX = free_.scaleY;
        renderPreview(false);
        syncOptions();
    });
    connect(angleBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), freePage,
            [this, spinLive](double v) {
                if (!spinLive(true))
                    return;
                free_.angleDeg = v;
                renderPreview(false);
                syncOptions();
            });
    connect(shearXBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), freePage,
            [this, spinLive](double v) {
                if (!spinLive(true))
                    return;
                free_.shearX = v / 100.0;
                renderPreview(false);
                syncOptions();
            });
    connect(shearYBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), freePage,
            [this, spinLive](double v) {
                if (!spinLive(true))
                    return;
                free_.shearY = v / 100.0;
                renderPreview(false);
                syncOptions();
            });
    auto *link = InkOptions::toggle(QStringLiteral("Link"), keepAspect_, freePage);
    link->setToolTip(QStringLiteral("Keep the aspect ratio while scaling"));
    connect(link, &QToolButton::toggled, freePage, [this](bool on) { keepAspect_ = on; });
    InkOptions::formRow(freeGrid, 0, QStringLiteral("W"), wBox_, link);
    InkOptions::formRow(freeGrid, 1, QStringLiteral("H"), hBox_);
    InkOptions::formRow(freeGrid, 2, QStringLiteral("ANGLE"), angleBox_);
    InkOptions::formRow(freeGrid, 3, QStringLiteral("SHEAR X"), shearXBox_);
    InkOptions::formRow(freeGrid, 4, QStringLiteral("SHEAR Y"), shearYBox_);

    // --- perspective page: handles speak for themselves ---
    const PageBuild perspBuilt = makePage(QStringLiteral("PERSPECTIVE"));
    auto *perspPage = perspBuilt.page;
    auto *perspCol = perspBuilt.col;
    auto *perspHint = new QLabel(QStringLiteral("Drag the corners · edge handles move both ends"),
                                 perspPage);
    perspHint->setObjectName(QStringLiteral("OptHint"));
    perspHint->setWordWrap(true);
    perspBuilt.grid->addWidget(perspHint, 0, 0, 1, 3);

    // --- warp page (Krita's warp group: anchor strength, flexibility,
    //     subdivide-or-draw, clear points), one control per grid row ---
    const PageBuild warpBuilt = makePage(QStringLiteral("WARP"));
    auto *warpPage = warpBuilt.page;
    auto *warpCol = warpBuilt.col;
    auto *warpGrid = warpBuilt.grid;
    auto *anchorBox = InkOptions::combo({QStringLiteral("Default (Affine)"),
                                         QStringLiteral("Strong (Rigid)"),
                                         QStringLiteral("Strongest (Similitude)")},
                                        warpPage, 150);
    flexCombo(anchorBox, 150);
    anchorBox->setCurrentIndex(warp_.kind == MlsKind::Affine
                                   ? 0
                                   : (warp_.kind == MlsKind::Rigid ? 1 : 2));
    anchorBox->setToolTip(QStringLiteral("How the deformation is fitted around each anchor"));
    connect(anchorBox, &QComboBox::currentIndexChanged, warpPage, [this](int i) {
        warp_.kind = i == 0 ? MlsKind::Affine : (i == 1 ? MlsKind::Rigid : MlsKind::Similitude);
        rebuildWarpMesh();
        renderPreview(false);
    });
    InkOptions::formRow(warpGrid, 0, QStringLiteral("ANCHOR"), anchorBox);

    auto *flex = flexSlider(InkOptions::slider(1, 50, int(warp_.alpha * 10.0 + 0.5), warpPage, 96),
                            110);
    auto *flexVal = InkOptions::value(QStringLiteral("%1").arg(warp_.alpha, 0, 'g', 2), warpPage, 44);
    flex->setToolTip(QStringLiteral("Flexibility: higher keeps the drag closer to the anchor"));
    InkOptions::formRow(warpGrid, 1, QStringLiteral("FLEX"), flex, flexVal);
    connect(flex, &QSlider::valueChanged, warpPage, [this, flexVal](int v) {
        warp_.alpha = double(v) / 10.0;
        flexVal->setText(QStringLiteral("%1").arg(warp_.alpha, 0, 'g', 2));
        rebuildWarpMesh();
        renderPreview(false);
    });

    auto *density = spin(warpPage, 1, 30, warp_.density, QString(), 64, 0);
    density->setEnabled(!warp_.drawMode);
    density->setToolTip(QStringLiteral("Anchor points per side (subdivide mode)"));
    InkOptions::formRow(warpGrid, 2, QStringLiteral("POINTS"), density);
    connect(density, qOverload<double>(&QDoubleSpinBox::valueChanged), warpPage, [this](double v) {
        warp_.density = int(v);
        if (!warp_.drawMode && session_) {
            buildWarpAnchors();
            renderPreview(false);
        }
    });

    auto *drawMode = InkOptions::toggle(QStringLiteral("Draw"), warp_.drawMode, warpPage);
    drawMode->setToolTip(QStringLiteral("Place the anchors yourself instead of subdividing"));
    connect(drawMode, &QToolButton::toggled, warpPage, [this, density](bool on) {
        warp_.drawMode = on;
        density->setEnabled(!on);
        if (!session_)
            return;
        if (on) {
            // Start from an empty field so the first click lands where it was
            // asked for; a single anchor is a plain translation.
            warp_.reset();
            rebuildWarpMesh();
            renderPreview(false);
        } else {
            buildWarpAnchors();
            renderPreview(false);
        }
    });
    auto *clearPts = InkOptions::button(QStringLiteral("Clear points"), warpPage);
    clearPts->setObjectName(QStringLiteral("OptButton"));
    connect(clearPts, &QToolButton::clicked, warpPage, [this] {
        buildWarpAnchors();
        renderPreview(false);
        if (ctx_)
            ctx_->setStatus(QStringLiteral("Anchors reset"));
    });
    // Both actions share one full-width row under the fields (like the brush
    // toggles on the liquify page), instead of being pulled apart by the
    // stretched grid column.
    auto *warpActions = new QWidget(warpPage);
    warpActions->setObjectName(QStringLiteral("CardStack"));
    auto *warpActionsRow = new QHBoxLayout(warpActions);
    warpActionsRow->setContentsMargins(0, 0, 0, 0);
    warpActionsRow->setSpacing(8);
    warpActionsRow->addWidget(drawMode);
    warpActionsRow->addWidget(clearPts);
    warpActionsRow->addStretch(1);
    warpGrid->addWidget(warpActions, 3, 0, 1, 3);

    // --- liquify page (Krita's liquify group: mode, size, amount, flow,
    //     spacing, build-up/wash), one control per grid row ---
    const PageBuild liquifyBuilt = makePage(QStringLiteral("BRUSH"));
    auto *liquifyPage = liquifyBuilt.page;
    auto *liquifyCol = liquifyBuilt.col;
    auto *liquifyGrid = liquifyBuilt.grid;
    auto *brushBox = new QWidget(liquifyPage);
    brushBox->setObjectName(QStringLiteral("CardStack"));
    auto *brushRow = new QHBoxLayout(brushBox);
    brushRow->setContentsMargins(0, 0, 0, 0);
    brushRow->setSpacing(6);
    auto *brushGroup = new QButtonGroup(liquifyPage);
    brushGroup->setExclusive(true);
    const struct {
        const char *label;
        LiquifyField::Brush brush;
    } brushModes[4] = {{"Move", LiquifyField::Brush::Move},
                       {"Scale", LiquifyField::Brush::Scale},
                       {"Rotate", LiquifyField::Brush::Rotate},
                       {"Undo", LiquifyField::Brush::Undo}};
    for (const auto &bm : brushModes) {
        auto *b = InkOptions::toggle(QString::fromLatin1(bm.label), liquify_.brush == bm.brush,
                                     liquifyPage);
        connect(b, &QToolButton::toggled, liquifyPage, [this, bm](bool on) {
            if (on)
                liquify_.brush = bm.brush;
        });
        brushGroup->addButton(b);
        brushRow->addWidget(b);
    }
    // The four brush toggles need the full panel width; they sit on their own
    // row under the section header so none of the labels elide.
    brushRow->addStretch(1);
    liquifyGrid->addWidget(brushBox, 0, 0, 1, 3);

    auto *size =
        flexSlider(InkOptions::slider(5, 400, int(liquify_.size), liquifyPage, 96), 110);
    auto *sizeVal =
        InkOptions::value(QStringLiteral("%1 px").arg(int(liquify_.size)), liquifyPage, 44);
    size->setToolTip(QStringLiteral("Brush size"));
    InkOptions::formRow(liquifyGrid, 1, QStringLiteral("SIZE"), size, sizeVal);
    connect(size, &QSlider::valueChanged, liquifyPage, [this, sizeVal](int v) {
        liquify_.size = double(v);
        sizeVal->setText(QStringLiteral("%1 px").arg(v));
    });

    auto *amount = flexSlider(
        InkOptions::slider(1, 100, int(liquify_.amount * 100.0 + 0.5), liquifyPage, 96), 110);
    auto *amountVal = InkOptions::value(QStringLiteral("%1%").arg(amount->value()), liquifyPage, 44);
    amount->setToolTip(QStringLiteral("How hard one dab pushes"));
    InkOptions::formRow(liquifyGrid, 2, QStringLiteral("AMOUNT"), amount, amountVal);
    connect(amount, &QSlider::valueChanged, liquifyPage, [this, amountVal](int v) {
        liquify_.amount = double(v) / 100.0;
        amountVal->setText(QStringLiteral("%1%").arg(v));
    });

    auto *flow = flexSlider(
        InkOptions::slider(1, 100, int(liquify_.flow * 100.0 + 0.5), liquifyPage, 96), 110);
    auto *flowVal = InkOptions::value(QStringLiteral("%1%").arg(flow->value()), liquifyPage, 44);
    flow->setToolTip(QStringLiteral("Wash mode: how much each dab blends"));
    InkOptions::formRow(liquifyGrid, 3, QStringLiteral("FLOW"), flow, flowVal);
    connect(flow, &QSlider::valueChanged, liquifyPage, [this, flowVal](int v) {
        liquify_.flow = double(v) / 100.0;
        flowVal->setText(QStringLiteral("%1%").arg(v));
    });

    auto *spacing = flexSlider(
        InkOptions::slider(1, 100, int(liquify_.spacing * 100.0 + 0.5), liquifyPage, 96), 110);
    auto *spacingVal = InkOptions::value(QStringLiteral("%1%").arg(spacing->value()), liquifyPage, 44);
    spacing->setToolTip(QStringLiteral("Distance between dabs, as a share of the brush"));
    InkOptions::formRow(liquifyGrid, 4, QStringLiteral("SPACING"), spacing, spacingVal);
    connect(spacing, &QSlider::valueChanged, liquifyPage, [this, spacingVal](int v) {
        liquify_.spacing = double(v) / 100.0;
        spacingVal->setText(QStringLiteral("%1%").arg(v));
    });

    auto *buildup = InkOptions::combo({QStringLiteral("Build Up"), QStringLiteral("Wash")},
                                       liquifyPage, 120);
    flexCombo(buildup, 120);
    buildup->setCurrentIndex(liquify_.wash ? 1 : 0);
    buildup->setToolTip(QStringLiteral("Build Up applies every dab; Wash blends by flow"));
    connect(buildup, &QComboBox::currentIndexChanged, liquifyPage,
            [this](int i) { liquify_.wash = (i == 1); });
    InkOptions::formRow(liquifyGrid, 5, QStringLiteral("MODE"), buildup);

    // --- shared quality block (FILTER + PREVIEW): ONE instance, moved into
    //     the active mode's page on every mode switch, so Free/Perspective get
    //     the kernel choice, Free/Perspective/Warp get drag quality, and
    //     Liquify (lattice-only) shows neither. No duplicated controls. ---
    //     (Parented to the Free page; applyMode() reparents it from there.)
    auto *qualityBox = new QWidget(freePage);
    qualityBox->setObjectName(QStringLiteral("CardStack"));
    auto *qualityCol = new QVBoxLayout(qualityBox);
    qualityCol->setContentsMargins(0, 0, 0, 0);
    qualityCol->setSpacing(6);
    InkOptions::addSection(qualityCol, QStringLiteral("QUALITY"), qualityBox);
    auto *qualityGridBox = new QWidget(qualityBox);
    qualityGridBox->setObjectName(QStringLiteral("CardStack"));
    auto *qualityGrid = new QGridLayout(qualityGridBox);
    qualityGrid->setContentsMargins(0, 0, 0, 0);
    qualityGrid->setHorizontalSpacing(10);
    qualityGrid->setVerticalSpacing(8);
    qualityGrid->setColumnStretch(1, 1);
    qualityCol->addWidget(qualityGridBox);
    auto *filterBox = InkOptions::combo({QStringLiteral("Nearest"), QStringLiteral("Bilinear"),
                                         QStringLiteral("Bicubic"), QStringLiteral("Lanczos3")},
                                        qualityBox, 120);
    flexCombo(filterBox, 120);
    filterBox->setCurrentIndex(int(filter_));
    filterBox->setToolTip(QStringLiteral("Resampling filter used when the transform is applied"));
    connect(filterBox, &QComboBox::currentIndexChanged, qualityBox,
            [this](int i) { filter_ = Interpolation(i); });
    InkOptions::formRow(qualityGrid, 0, QStringLiteral("FILTER"), filterBox);

    auto *previewBox = InkOptions::combo({QStringLiteral("Accurate"), QStringLiteral("Fast")},
                                         qualityBox, 120);
    flexCombo(previewBox, 120);
    previewBox->setCurrentIndex(fastPreview_ ? 1 : 0);
    previewBox->setToolTip(QStringLiteral("Fast drags sample bilinear; commit always uses the filter"));
    connect(previewBox, &QComboBox::currentIndexChanged, qualityBox, [this](int i) {
        fastPreview_ = (i == 1);
    });
    InkOptions::formRow(qualityGrid, 1, QStringLiteral("PREVIEW"), previewBox);

    // --- slim bar: mode + settings + Record + Apply. Every edit previews
    //     in realtime and lands with Apply (Enter, the button, double-click)
    //     or any context switch; Esc reverts. Reset lives in the popup
    //     header and acts on the selected mode only. Flip/rotate live in
    //     Edit menu. ---
    row->addWidget(InkOptions::separator(page));
    auto *settingsBtn = InkOptions::button(QStringLiteral("Settings"), page);
    settingsBtn->setToolTip(QStringLiteral("Transform settings for this mode"));
    row->addWidget(settingsBtn);
    // Recording drags as keyframes is OPT-IN (default off): with it off the
    // tool always bakes pixels, even when Auto-key is armed and the layer
    // already holds keyframes. The ONE toggle lives here in the bar — not two
    // clicks deep in the popup — because nobody found it there. Shown only for
    // the matrix modes: Warp/Liquify always bake (their state is a mesh).
    row->addWidget(InkOptions::separator(page));
    recordToggle_ = InkOptions::toggle(QStringLiteral("Record"), recordKeyframes_, page);
    recordToggle_->setToolTip(QStringLiteral("Record as keyframes: with Auto-key on, drags write "
                                             "keyframes on the layer itself and leave the pixels "
                                             "untouched. Off = always bake pixels."));
    row->addWidget(recordToggle_);
    {
        const bool keyable = mode_ == Mode::Free || mode_ == Mode::Perspective;
        recordToggle_->setEnabled(keyable);
        if (!keyable)
            recordToggle_->setText(QStringLiteral("Record (bakes pixels)"));
        recordToggle_->setToolTip(
            keyable ? QStringLiteral("Record as keyframes: with Auto-key on, drags write keyframes "
                                     "on the layer itself and leave the pixels untouched. "
                                     "Off = always bake pixels.")
                    : QStringLiteral("Not available for Warp and Liquify: they deform a mesh, "
                                     "which keyframes cannot hold. Their result is always baked "
                                     "into the cel, and it travels with the layer's keyed pose."));
    }
    connect(recordToggle_, &QToolButton::toggled, page, [this](bool on) {
        setRecordKeyframes(on); // one path for UI + programmatic changes
    });
    // Landing the preview is explicit (Krita's Apply): the button, Enter and
    // double-click all funnel into commit(). Harmless with nothing pending —
    // commit drops empty records and no-op bakes silently, then re-seeds.
    row->addWidget(InkOptions::separator(page));
    auto *applyBtn = InkOptions::button(QStringLiteral("Apply"), page);
    applyBtn->setToolTip(QStringLiteral("Land the transform preview (Enter). Esc reverts it."));
    row->addWidget(applyBtn);
    connect(applyBtn, &QToolButton::clicked, page, [this] {
        if (!ctx_)
            return;
        commit();
        ctx_->requestRepaint();
    });
    connect(settingsBtn, &QToolButton::clicked, page, [popup, settingsBtn] {
        if (popup->isVisible()) {
            popup->hide();
            return;
        }
        popup->adjustSize();
        QPoint at = settingsBtn->mapToGlobal(QPoint(0, settingsBtn->height() + 4));
        // Keep the whole panel on-screen: never spill past the resolution.
        if (QScreen *screen = settingsBtn->screen()) {
            const QRect avail = screen->availableGeometry();
            const QSize want = popup->size();
            at.setX(std::max(avail.left(), std::min(at.x(), avail.right() - want.width())));
            at.setY(std::max(avail.top(), std::min(at.y(), avail.bottom() - want.height())));
        }
        popup->move(at);
        popup->show();
    });

    // Mode switching, wired last: shows ONLY the selected mode's page in the
    // popup (plus the shared quality block where it applies) and toggles the
    // controls that only apply to one mode.
    auto applyMode = [this, stack, freePage, perspPage, warpPage, liquifyPage, freeCol,
                      perspCol, warpCol, liquifyCol, qualityBox, filterBox] {
        // Pages follow creation order: 0 Free, 1 Perspective, 2 Warp, 3 Liquify.
        QWidget *active = freePage;
        QVBoxLayout *activeCol = freeCol;
        int section = 0;
        if (mode_ == Mode::Perspective) {
            active = perspPage;
            activeCol = perspCol;
            section = 1;
        } else if (mode_ == Mode::Warp) {
            active = warpPage;
            activeCol = warpCol;
            section = 2;
        } else if (mode_ == Mode::Liquify) {
            active = liquifyPage;
            activeCol = liquifyCol;
            section = 3;
        }
        // The kernel choice applies to the matrix modes; drag quality to
        // everything but the liquify brush (always accurate).
        const bool showQuality = mode_ != Mode::Liquify;
        // (The initial parent already equals the Free page, so the layout
        // membership check matters: without it the block floats at (0,0).)
        if (qualityBox->parentWidget() != active || activeCol->indexOf(qualityBox) == -1) {
            qualityBox->setParent(active);
            activeCol->addWidget(qualityBox);
        }
        qualityBox->setVisible(showQuality);
        filterBox->setEnabled(mode_ == Mode::Free || mode_ == Mode::Perspective);
        if (settingsStack_) {
            // Hidden pages stay Ignored so the popup sizes to the active page.
            for (int i = 0; i < settingsStack_->count(); ++i)
                if (QWidget *w = settingsStack_->widget(i))
                    w->setSizePolicy(i == section ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                     QSizePolicy::Preferred);
            settingsStack_->setCurrentIndex(section);
            settingsStack_->updateGeometry();
        }
        if (settingsTitle_)
            settingsTitle_->setText(modeName(mode_));
        if (session_ && mode_ == Mode::Warp && warp_.origPts.size() < 2)
            buildWarpAnchors();
        if (session_ && mode_ == Mode::Liquify && !liquify_.field.mesh().valid())
            liquify_.field.reset(lift_.bounds);
        syncOptions();
    };
    connect(modeBox, &QComboBox::currentIndexChanged, page, [this, applyMode](int index) {
        const Mode wanted = Mode(index);
        if (wanted == mode_ || !ctx_)
            return;
        if (pending_) {
            // Realtime edits are never lost on a mode switch: bake whatever is
            // showing into one undo step, then start the new mode clean.
            // (commit() also resets every mode's params via resetSession.)
            commit();
        } else if (mode_ == Mode::Free && session_) {
            // Carry the current frame over to the quad, so switching modes
            // never moves the pixels.
            Vec2 c[4];
            frameCorners(c);
            for (int i = 0; i < 4; ++i)
                persp_.corners[i] = c[i];
        }
        mode_ = wanted;
        applyMode();
        ctx_->setStatus(statusHint());
        ctx_->requestRepaint();
    });

    row->addWidget(InkOptions::separator(page));
    auto *hint = new QLabel(statusHint(), page);
    hint->setObjectName(QStringLiteral("OptHint"));
    row->addWidget(hint);
    row->addStretch(1);

    applyMode();
    return page;
}

// --- one-shot region ops (Edit > Transform) ---------------------------------

bool applyRegionOp(ToolContext &ctx, RegionOp op) {
    PaintDevice *device = ctx.editableDevice();
    if (!device) {
        ctx.setStatus(ctx.blockedHint());
        return false;
    }
    const Image &img = ctx.doc()->image();
    const Rect doc{0, 0, img.width(), img.height()};
    Selection &sel = ctx.selection();
    const bool scoped = !sel.isEmpty();
    // Pixel-precise ink bounds: mirroring about a tile-snapped centre would
    // shift the content by up to half a tile.
    const Rect area =
        scoped ? intersectRect(sel.bounds(), doc) : intersectRect(device->contentBounds(), doc);
    if (area.isEmpty())
        return false;
    Selection mask;
    if (scoped)
        mask = sel; // the live mask travels at the end; erase uses this snapshot
    FloatRegion lift = liftRegion(*device, area, scoped ? &mask : nullptr);
    if (lift.empty())
        return false;

    // Turn about the region's centre (flip about its centre line) so the
    // content stays where the user left it.
    const Rect region = lift.bounds;
    const Vec2 c{double(region.x) + region.w * 0.5, double(region.y) + region.h * 0.5};
    Mat3 about = Mat3::translation(c.x, c.y);
    switch (op) {
    case RegionOp::FlipHorizontal:
        about = about.multiply(Mat3::scale(-1.0, 1.0));
        break;
    case RegionOp::FlipVertical:
        about = about.multiply(Mat3::scale(1.0, -1.0));
        break;
    case RegionOp::RotateCW:
        about = about.multiply(Mat3::rotation(90.0));
        break;
    case RegionOp::RotateCCW:
        about = about.multiply(Mat3::rotation(-90.0));
        break;
    }
    const Mat3 m = about.multiply(Mat3::translation(-c.x, -c.y));
    const Rect dst = intersectRect(mappedBounds(lift, m), doc);
    if (dst.isEmpty())
        return false;

    const QString name = op == RegionOp::FlipHorizontal ? QStringLiteral("Flip horizontal")
                         : op == RegionOp::FlipVertical ? QStringLiteral("Flip vertical")
                         : op == RegionOp::RotateCW      ? QStringLiteral("Rotate 90° clockwise")
                                                         : QStringLiteral("Rotate 90° counter-clockwise");
    TileMemento memento;
    Rect dirty = eraseRegion(*device, area, scoped ? &mask : nullptr, &memento);
    // A quarter turn is lossless: rotate the lifted block instead of resampling.
    const FloatRegion mapped =
        (op == RegionOp::RotateCW || op == RegionOp::RotateCCW)
            ? rotateRegionQuarter(lift, op == RegionOp::RotateCW ? 3 : 1)
            : resampleRegion(lift, dst, m, Interpolation::Nearest);
    Rect placed = mapped.bounds;
    // rotateRegionQuarter anchors the block at the source origin; move it back
    // under the centre the matrix asked for.
    const int dx = dst.x + (dst.w - placed.w) / 2;
    const int dy = dst.y + (dst.h - placed.h) / 2;
    dirty = uniteRect(dirty, stampRegion(*device, mapped, dx - placed.x, dy - placed.y, doc, &memento));
    if (memento.empty())
        return false;
    if (scoped) {
        sel.resampleMask(m, Interpolation::Bicubic);
        sel.invalidateOutline();
        ctx.notifySelectionChanged();
    }
    ctx.markDirty(dirty);
    ctx.pushStrokeCommand(name, device, std::move(memento));
    ctx.setStatus(name + QStringLiteral(" applied"));
    return true;
}

} // namespace ink
