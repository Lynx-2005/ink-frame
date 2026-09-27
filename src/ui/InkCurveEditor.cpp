// The easing graph, implemented. See InkCurveEditor.h for why the view is
// normalised and how that maps back onto the stored handles.
#include "ui/InkCurveEditor.h"
#include "ui/InkDocument.h"
#include "ui/InkParamFormat.h"
#include "ui/InkTheme.h"
#include "ui/InkTransformPanel.h" // ParamField: the graph's value strip
#include <QComboBox>
#include <QFont>
#include <QLabel>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

QFont uiFont(int px, bool bold = false) {
    QFont f;
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

// Layout. The panel shares the timeline's row and is deliberately narrow, so the
// bands are thin and the plot takes the rest as a SQUARE: both axes are 0..1, and
// drawing the box non-square would make the linear diagonal look wrong.
constexpr int kHeaderH = 26;
constexpr int kValuesH = 30; // the value fields under the header
constexpr int kStatusH = 20;
constexpr int kMarginL = 34; // room for the progress labels
constexpr int kGrabPx = 14;  // handle grab radius: generous (flexible dragging)
constexpr int kHandleR = 7;  // drawn handle radius

// The ease PRESETS live in the keyframe right-click menu; this editor is the
// free-form shaping surface only. (The thumbnail column that sat on the right
// was removed — it duplicated the menu and squeezed the plot.)

// The 0..1 box with its frame and a light grid.
void paintBox(QPainter &p, const QRect &box) {
    p.setBrush(InkTheme::card());
    p.setPen(QPen(InkTheme::border(), 1));
    p.drawRect(box);
    p.setPen(QPen(InkTheme::hairline(), 1.0, Qt::DotLine));
    for (int i = 1; i <= 3; ++i) {
        const double f = double(i) / 4.0;
        const double y = box.bottom() - f * box.height();
        const double x = box.left() + f * box.width();
        p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
        p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
    }
    // The unit-square rails read as slightly brighter than the inner grid: they
    // are the "no overshoot" boundary animators reason about.
    p.setPen(QPen(InkTheme::tick(), 1));
    p.drawLine(box.topLeft(), box.topRight());
    p.drawLine(box.bottomLeft(), box.bottomRight());
    p.drawLine(box.topLeft(), box.bottomLeft());
    p.drawLine(box.topRight(), box.bottomRight());
}

} // namespace

CurveEditor::CurveEditor(Document *doc, QWidget *parent) : QWidget(parent), doc_(doc) {
    setObjectName(QStringLiteral("CurveEditor"));
    setFocusPolicy(Qt::ClickFocus);
    setMinimumWidth(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // The transform dropdown: the ONE place a parameter is chosen. It sits in
    // the header's empty space beside Reset and lists Anchor, Position, Scale,
    // Rotation, Opacity — never separate X/Y entries.
    combo_ = new QComboBox(this);
    combo_->setObjectName(QStringLiteral("CurveGroup"));
    combo_->setToolTip(QStringLiteral("Which transform this easing shapes"));
    for (const TransformGroup &g : transformGroups())
        combo_->addItem(QString::fromUtf8(g.label));
    connect(combo_, &QComboBox::currentIndexChanged, this, [this](int i) { selectGroup(i); });
}

// --- dropdown + value strip --------------------------------------------------

int CurveEditor::groupCount() const { return int(transformGroups().size()); }

int CurveEditor::currentGroup() const {
    return group_ >= 0 && group_ < groupCount() ? group_ : -1;
}

QString CurveEditor::groupLabel(int i) const {
    if (i < 0 || i >= groupCount()) return {};
    return QString::fromUtf8(transformGroups()[size_t(i)].label);
}

QRect CurveEditor::dropdownRect() const {
    // Left of Reset, in the header's empty middle space.
    const int w = 132;
    return QRect(width() - 62 - 8 - w, 4, w, kHeaderH - 8);
}

void CurveEditor::selectGroup(int i) {
    if (i < 0 || i >= groupCount()) return;
    group_ = i;
    if (combo_->currentIndex() != i) {
        const QSignalBlocker b(combo_);
        combo_->setCurrentIndex(i);
    }
    rebuildChannels(); // re-picks chan_ inside the new group
    rebuildValueFields();
    if (!locateSegment()) segValid_ = false;
    emit statusMessage(statusText());
    update();
}

void CurveEditor::rebuildValueFields() {
    for (ParamField *f : std::as_const(valueFields_)) f->deleteLater();
    for (QLabel *l : std::as_const(valueLabels_)) l->deleteLater();
    valueFields_.clear();
    valueLabels_.clear();
    valueIds_.clear();
    const ParamBlock *pb = block();
    if (!pb || group_ < 0 || group_ >= groupCount()) {
        update();
        return;
    }
    const TransformGroup &g = transformGroups()[size_t(group_)];
    const bool pair = g.field[1] != ParamId::Count;
    for (ParamId id : {g.field[0], g.field[1]}) {
        if (id == ParamId::Count) continue;
        // Only the values RELATED to this transform: X and Y for the 2-axis
        // groups, one field for Rotation/Opacity.
        auto *lab = new QLabel(pair ? (id == g.field[0] ? QStringLiteral("X")
                                                        : QStringLiteral("Y"))
                                    : QString(), this);
        lab->setObjectName(QStringLiteral("CurveValueLabel"));
        lab->setFont(uiFont(9, true));
        lab->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lab->setStyleSheet(QStringLiteral("color:%1;").arg(InkTheme::dim().name()));
        auto *f = new ParamField(id, this);
        f->setToolTip(QStringLiteral("Value at the playhead — type, scrub or wheel"));
        connect(f, &ParamField::scrubbed, this, [this, id](double v) { onValueScrub(id, v); });
        connect(f, &ParamField::committed, this, [this, id](double v) { onValueCommit(id, v); });
        valueLabels_.push_back(lab);
        valueFields_.push_back(f);
        valueIds_.push_back(id);
    }
    layoutValueFields();
    // Prime the fields with the live values.
    for (int i = 0; i < valueFields_.size(); ++i)
        if (!valueFields_[i]->hasFocus())
            valueFields_[i]->showValue(pb->valueAt(valueIds_[i],
                                                              double(doc_->currentFrame())));
    update();
}

void CurveEditor::layoutValueFields() {
    const int y = kHeaderH + 1;
    int x = kMarginL;
    for (int i = 0; i < valueFields_.size(); ++i) {
        const int labW = valueLabels_[i]->text().isEmpty() ? 0 : 14;
        if (labW) {
            valueLabels_[i]->setGeometry(x, y, labW - 4, kValuesH - 6);
            x += labW;
        }
        valueFields_[i]->setGeometry(x, y, 96, kValuesH - 6);
        x += 96 + 14;
    }
    update();
}

ParamField *CurveEditor::valueField(ParamId id) const {
    for (int i = 0; i < valueIds_.size(); ++i)
        if (valueIds_[i] == id) return valueFields_[i];
    return nullptr;
}

void CurveEditor::onValueScrub(ParamId id, double v) {
    if (!doc_) return;
    const ParamDesc &d = paramDesc(id);
    v = std::clamp(v, d.lo, d.hi);
    const int f = doc_->currentFrame();
    if (CameraLayer *cam = camera()) {
        if (!scrubbingValue_) {
            scrubbingValue_ = true;
            doc_->beginCameraRecord("Change camera value", cam);
        }
        if (cam->params().hasKey(id, f))
            doc_->liveSetCameraKeyValue(cam, id, f, v);
        else if (cam->params().isAnimated(id) && doc_->autoKeyEnabled())
            doc_->liveSetCameraKey(cam, id, f, v, Interp::Bezier);
        else
            doc_->liveSetCameraStaticParam(cam, id, v);
        emit paramsEdited();
        return;
    }
    AdjustmentLayer *adj_ = adjustment();
    if (!adj_) return;
    if (!scrubbingValue_) {
        scrubbingValue_ = true;
        doc_->beginParamRecord("Change value", adj_);
    }
    // A scrub on a key edits the key. A scrub on an ANIMATED channel with
    // Auto-key armed keys the frame (like a typed value does below): writing
    // the static value instead was a silent no-op, because valueAt() ignores
    // statics while keys exist — the scrub felt dead. Animating a parameter
    // from scratch stays the stopwatch's job.
    if (adj_->params().hasKey(id, f))
        doc_->liveSetKeyValue(adj_, id, f, v);
    else if (adj_->params().isAnimated(id) && doc_->autoKeyEnabled())
        doc_->liveSetKey(adj_, id, f, v, Interp::Bezier);
    else
        doc_->liveSetStaticParam(adj_, id, v);
    emit paramsEdited();
}

void CurveEditor::onValueCommit(ParamId id, double v) {
    if (!doc_) return;
    const ParamDesc &d = paramDesc(id);
    v = std::clamp(v, d.lo, d.hi);
    if (CameraLayer *cam = camera()) {
        if (scrubbingValue_) { // a drag ended: the open record already holds it
            scrubbingValue_ = false;
            doc_->endCameraRecord();
            refresh();
            emit paramsEdited();
            return;
        }
        // Typed / wheel / arrow: one undo step through the ordinary key path.
        const int f = doc_->currentFrame();
        const bool animated = cam->params().isAnimated(id);
        bool changed = false;
        if (animated && cam->params().hasKey(id, f)) {
            changed = doc_->setCameraKeyValue(cam, id, f, v);
        } else if (animated && doc_->autoKeyEnabled()) {
            changed = doc_->setCameraKey(cam, id, f, v, Interp::Bezier, true);
        } else {
            changed = doc_->setCameraStaticValue(cam, id, v);
        }
        if (changed) {
            refresh();
            emit paramsEdited();
        }
        return;
    }
    AdjustmentLayer *adj_ = adjustment();
    if (!adj_) return;
    if (scrubbingValue_) { // a drag ended: the open record already holds it
        scrubbingValue_ = false;
        doc_->endParamRecord();
        refresh();
        emit paramsEdited();
        return;
    }
    // Typed / wheel / arrow: one undo step through the ordinary key path.
    const int f = doc_->currentFrame();
    const bool animated = adj_->params().isAnimated(id);
    bool changed = false;
    if (animated && adj_->params().hasKey(id, f)) {
        changed = doc_->setParamKeyValue(adj_, id, f, v);
    } else if (animated && doc_->autoKeyEnabled()) {
        changed = doc_->setParamKey(adj_, id, f, v, Interp::Bezier, true);
    } else {
        changed = doc_->setParamStaticValue(adj_, id, v);
    }
    if (changed) {
        refresh();
        emit paramsEdited();
    }
}

// Does any channel of group `g` hold a real segment (>= 2 keys)?
bool CurveEditor::groupHasSegment(int g) const {
    const ParamBlock *pb = block();
    if (!pb || g < 0 || g >= groupCount()) return false;
    const TransformGroup &gr = transformGroups()[size_t(g)];
    for (ParamId id : {gr.graph[0], gr.graph[1]}) {
        if (id == ParamId::Count) continue;
        const ScalarChannel *ch = pb->channel(id);
        if (ch && ch->keyCount() >= 2) return true;
    }
    return false;
}

// Resolve the view instead of caching it: Document recycles the single
// adjustment view whenever the keyed layer changes, so a stored pointer dangles
// as soon as the artist selects another layer. The TARGET LAYER is what is worth
// remembering - the tree owns it, and the keys live on it.
AdjustmentLayer *CurveEditor::adjustment() const {
    if (!doc_ || !target_)
        return nullptr; // no layer picked: the graph is empty, not "whatever"
    AdjustmentLayer *live = doc_->currentAdjustment();
    // Only a view that still points at OUR layer. The document recycles the
    // single view when the keyed layer changes, so a pointer handed out earlier
    // may be freed (a use-after-free the moment the artist picks another layer),
    // and silently showing the NEW layer's curves here would be its own bug -
    // setAdjustment() says the panel must never show a curve from another layer,
    // so the dock re-points it and until then the graph is simply empty.
    return (live && live->target() == target_) ? live : nullptr;
}

CameraLayer *CurveEditor::camera() const {
    if (camId_.empty() || !doc_) return nullptr;
    return doc_->findCamera(camId_);
}

QString CurveEditor::targetTitle() const {
    if (CameraLayer *cam = camera()) return QString::fromStdString(cam->name());
    if (AdjustmentLayer *a = adjustment()) return QString::fromStdString(a->name());
    return {};
}

const ParamBlock *CurveEditor::block() const {
    if (CameraLayer *cam = camera()) return &cam->params();
    if (AdjustmentLayer *a = adjustment()) return &a->params();
    return nullptr;
}

void CurveEditor::setCamera(CameraLayer *c) {
    camId_ = c ? c->stableId() : std::string();
    target_ = nullptr;
    if (combo_)
        combo_->setToolTip(QStringLiteral("Which camera motion this easing shapes"));
    // Same auto-pick as the paint path: keep the current group when it eases
    // something, else jump to the first group that does.
    if (camera() && !groupHasSegment(group_)) {
        for (int g = 0; g < groupCount(); ++g)
            if (groupHasSegment(g)) {
                group_ = g;
                const QSignalBlocker b(combo_);
                combo_->setCurrentIndex(g);
                break;
            }
    }
    rebuildChannels();
    rebuildValueFields();
    if (!locateSegment()) {
        segValid_ = false;
        segIndex_ = -1;
        segCount_ = 0;
    }
    emit statusMessage(statusText());
    update();
}

void CurveEditor::setAdjustment(AdjustmentLayer *a) {
    camId_.clear();
    target_ = a ? a->target() : nullptr;
    if (combo_) combo_->setToolTip(QStringLiteral("Which transform this easing shapes"));
    // Keep showing the CURRENT group when it has something to ease; otherwise
    // jump to the first group that does — the old auto-pick, group-scoped, so
    // the panel never opens on an empty graph while a segment exists.
    if (adjustment() && !groupHasSegment(group_)) {
        for (int g = 0; g < groupCount(); ++g)
            if (groupHasSegment(g)) {
                group_ = g;
                const QSignalBlocker b(combo_);
                combo_->setCurrentIndex(g);
                break;
            }
    }
    rebuildChannels();
    rebuildValueFields();
    if (!locateSegment()) {
        segValid_ = false;
        segIndex_ = -1;
        segCount_ = 0;
    }
    update();
}

void CurveEditor::rebuildChannels() {
    // The dropdown picks the GROUP; the curve edits one of its channels: the
    // first with a real segment (>= 2 keys), else the group's primary channel,
    // so the "no segment yet" hint names a channel of the group the user chose.
    chanList_.clear();
    const ParamBlock *pb = block();
    if (!pb || group_ < 0 || group_ >= groupCount()) {
        chan_ = ParamId::Count;
        return;
    }
    const TransformGroup &g = transformGroups()[size_t(group_)];
    ParamId primary = ParamId::Count;
    for (ParamId id : {g.graph[0], g.graph[1]}) {
        if (id == ParamId::Count) continue;
        if (primary == ParamId::Count) primary = id;
        const ScalarChannel *ch = pb->channel(id);
        // A single key has no segment: nothing between two points to ease.
        if (!ch || ch->keyCount() < 2) continue;
        ChanEntry e;
        e.id = id;
        e.label = QString::fromUtf8(paramDesc(id).label);
        chanList_.push_back(e);
    }
    if (primary != ParamId::Count) {
        ChanEntry e;
        e.id = primary;
        e.label = QString::fromUtf8(paramDesc(primary).label);
        if (chanList_.isEmpty()) chanList_.push_back(e);
    }
    // Re-pick chan_: keep it when it still belongs to this group, else the
    // first channel with a segment, else the group's primary.
    bool keep = false;
    for (const ChanEntry &e : chanList_)
        if (e.id == chan_) keep = true;
    if (!keep)
        chan_ = chanList_.isEmpty() ? primary : chanList_.first().id;
}

bool CurveEditor::locateSegment() {
    segValid_ = false;
    const ParamBlock *pb = block();
    if (!pb || chan_ == ParamId::Count) return false;
    const ScalarChannel *ch = pb->channel(chan_);
    if (!ch) return false;
    const std::vector<int> ks = ch->keys();
    if (ks.size() < 2) return false;

    // The PLAYHEAD picks the segment: the motion you shape is the one happening
    // at the frame you are looking at, and the timeline already owns the
    // playhead, so both panes stay in step without a second time axis.
    const int pf = doc_ ? doc_->currentFrame() : ks.front();
    int idx = -1;
    for (size_t i = 0; i + 1 < ks.size(); ++i) {
        if (pf >= ks[i] && pf < ks[i + 1]) { idx = int(i); break; }
    }
    if (idx < 0) idx = (pf < ks.front()) ? 0 : int(ks.size()) - 2;

    const ScalarKey *ka = ch->keyAt(ks[size_t(idx)]);
    const ScalarKey *kb = ch->keyAt(ks[size_t(idx) + 1]);
    if (!ka || !kb) return false;

    fA_ = ka->frame;
    fB_ = kb->frame;
    vA_ = double(ka->value);
    vB_ = double(kb->value);
    keyA_ = *ka;
    keyB_ = *kb;
    segIndex_ = idx;
    segCount_ = int(ks.size()) - 1;
    mode_ = ch->segmentInterp(fA_);
    const double dV = vB_ - vA_;
    flat_ = std::abs(dV) < 1e-6;

    CubicSegment s{};
    ch->segment(fA_, &s);
    // The stored handles ARE the normalised points, modulo dividing the y
    // control values by the segment's height (InkEasing keeps y absolute so that
    // flat segments stay well defined).
    npA_ = QPointF(s.x.x1, flat_ ? 1.0 / 3.0 : (s.p1y - vA_) / dV);
    npB_ = QPointF(s.x.x2, flat_ ? 2.0 / 3.0 : (s.p2y - vA_) / dV);
    segValid_ = true;
    return true;
}

void CurveEditor::refresh() {
    AdjustmentLayer *adj_ = adjustment();
    // Mid-drag the model is BEHIND the pointer (handles are written on move,
    // but the undo record is only built at release), so re-reading the segment
    // here re-clamps and snaps the handle back — the "drag gets stopped"
    // bug. While the user holds a handle the refresh must not touch the
    // geometry: only a structural change (channel list) is merged, and the
    // full re-read happens on release.
    if (grab_ != Grab::None) {
        update();
        return;
    }
    rebuildChannels();
    bool keep = false;
    for (const ChanEntry &e : chanList_)
        if (e.id == chan_) keep = true;
    if (!keep) chan_ = chanList_.isEmpty() ? ParamId::Count : chanList_.first().id;
    if (!locateSegment()) segValid_ = false;
    // Keep the strip's numbers live (playhead moved, keys changed, undo).
    const ParamBlock *pbNow = block();
    if (!scrubbingValue_)
        for (int i = 0; i < valueFields_.size(); ++i)
            if (valueFields_[i] && !valueFields_[i]->hasFocus() && pbNow)
                valueFields_[i]->showValue(pbNow->valueAt(
                    valueIds_[i], double(doc_ ? doc_->currentFrame() : 0)));
    update();
}

void CurveEditor::followPlayhead() {
    if (!locateSegment()) segValid_ = false;
    const ParamBlock *pb = block();
    if (!scrubbingValue_)
        for (int i = 0; i < valueFields_.size(); ++i)
            if (valueFields_[i] && !valueFields_[i]->hasFocus() && pb)
                valueFields_[i]->showValue(pb->valueAt(
                    valueIds_[i], double(doc_ ? doc_->currentFrame() : 0)));
    update();
}

void CurveEditor::stepSegment(int dir) {
    const ParamBlock *pb = block();
    if (!segValid_ || !pb) return;
    const ScalarChannel *ch = pb->channel(chan_);
    if (!ch) return;
    const std::vector<int> ks = ch->keys();
    const int target = std::clamp(segIndex_ + dir, 0, int(ks.size()) - 2);
    if (target == segIndex_) return;
    // Paging the segment means paging the playhead, so the timeline and this
    // panel agree about which motion is being shaped.
    if (doc_) doc_->setCurrentFrame(ks[size_t(target)]);
    if (!locateSegment()) segValid_ = false;
    emit statusMessage(statusText());
    update();
}

void CurveEditor::resetEase() {
    if (!segValid_ || !doc_) return;
    // A linear ramp is the neutral shape: infinite handle speed with 1/3
    // influence on both ends produces exactly the diagonal. One undo step, like
    // any drag.
    if (CameraLayer *cam = camera()) {
        doc_->beginCameraRecord("Reset camera easing", cam);
        doc_->liveSetCameraSegmentEase(cam, chan_, fA_, 1.0 / 3.0, 1.0 / 3.0, 2.0 / 3.0,
                                       2.0 / 3.0, -1);
        doc_->endCameraRecord();
    } else {
        AdjustmentLayer *adj_ = adjustment();
        if (!adj_) return;
        doc_->beginParamRecord("Reset easing", adj_);
        doc_->liveSetSegmentEase(adj_, chan_, fA_, 1.0 / 3.0, 1.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0,
                                 -1);
        doc_->endParamRecord();
    }
    if (!locateSegment()) segValid_ = false;
    emit graphChanged();
    emit statusMessage(statusText());
    update();
}

// --- interaction -------------------------------------------------------------

void CurveEditor::writeHandle(Which which, const QPointF &np, bool unified) {
    if (!doc_ || !segValid_) return;
    if (CameraLayer *cam = camera()) {
        QPointF a = npA_, b = npB_;
        if (which == Which::A) a = np; else b = np;
        if (mode_ != Interp::Bezier) {
            a = (which == Which::A) ? np : QPointF(1.0 / 3.0, 1.0 / 3.0);
            b = (which == Which::B) ? np : QPointF(2.0 / 3.0, 2.0 / 3.0);
            doc_->liveSetCameraSegmentInterp(cam, chan_, fA_, Interp::Bezier);
            mode_ = Interp::Bezier;
        }
        const int mirror = unified ? (which == Which::A ? 0 : 1) : -1;
        doc_->liveSetCameraSegmentEase(cam, chan_, fA_, a.x(), a.y(), b.x(), b.y(), mirror);
        npA_ = a;
        npB_ = b;
        return;
    }
    AdjustmentLayer *adj_ = adjustment();
    if (!adj_) return;
    QPointF a = npA_, b = npB_;
    if (which == Which::A) a = np; else b = np;
    // Handles only MEAN something on a Bezier segment: Linear and Hold evaluate
    // without them, so grabbing one is the intent to shape this move and the
    // segment converts first. It converts to the STRAIGHT line and then bends at
    // the handle being dragged — otherwise a leftover auto tangent from the linear
    // phase would jump the curve sideways and the drag would start from a shape
    // the user never asked for.
    if (mode_ != Interp::Bezier) {
        a = (which == Which::A) ? np : QPointF(1.0 / 3.0, 1.0 / 3.0);
        b = (which == Which::B) ? np : QPointF(2.0 / 3.0, 2.0 / 3.0);
        doc_->liveSetSegmentInterp(adj_, chan_, fA_, Interp::Bezier);
        mode_ = Interp::Bezier;
    }
    // A key shared by two segments must stay smooth: pulling one handle swings
    // its partner on the far side of the SAME key (Alt breaks that, exactly as
    // elsewhere in the app). Only the dragged side of this segment changes shape.
    const int mirror = unified ? (which == Which::A ? 0 : 1) : -1;
    doc_->liveSetSegmentEase(adj_, chan_, fA_, a.x(), a.y(), b.x(), b.y(), mirror);
    npA_ = a;
    npB_ = b;
}

int CurveEditor::presetCount() { return 0; }

// The preset column was removed; the highlight seam stays for compatibility and
// always reports "no preset" (the keyframe right-click menu owns presets now).
int CurveEditor::presetHighlight() const {
    if (!segValid_) return -1;
    if (mode_ == Interp::Hold) return 5;
    if (mode_ == Interp::Linear) return 0;
    return -1;
}

// --- geometry ----------------------------------------------------------------

int CurveEditor::topBand() const { return kHeaderH + kValuesH; }

QRect CurveEditor::plot() const {
    const int left = kMarginL;
    // Full width: the preset column that used to squeeze the plot is gone.
    const int right = std::max(left + 40, width() - 12);
    const int top = topBand() + 10;
    const int bottom = std::max(top + 40, height() - kStatusH - 18);
    const int side = std::max(40, std::min(right - left, bottom - top));
    // Centre the square in the space available, so resizing keeps it square.
    const int x = left + (right - left - side) / 2;
    const int y = top + (bottom - top - side) / 2;
    return QRect(x, y, side, side);
}

QPointF CurveEditor::toScreen(const QPointF &np) const {
    const QRect r = plot();
    // y flips: progress grows upward, widget coordinates grow downward.
    return QPointF(r.left() + np.x() * r.width(), r.bottom() - np.y() * r.height());
}

QPointF CurveEditor::toNorm(const QPoint &px) const {
    const QRect r = plot();
    if (r.width() <= 0 || r.height() <= 0) return {};
    return QPointF(double(px.x() - r.left()) / r.width(),
                   double(r.bottom() - px.y()) / r.height());
}

QPointF CurveEditor::handlePointA() const { return toScreen(npA_); }
QPointF CurveEditor::handlePointB() const { return toScreen(npB_); }

static QRect grabRect(const QPointF &c) {
    return QRect(QPoint(int(c.x()) - kGrabPx, int(c.y()) - kGrabPx),
                 QPoint(int(c.x()) + kGrabPx, int(c.y()) + kGrabPx));
}

QRect CurveEditor::handleRectA() const { return grabRect(toScreen(npA_)); }
QRect CurveEditor::handleRectB() const { return grabRect(toScreen(npB_)); }

QRect CurveEditor::presetCell(int) const {
    return {}; // the thumbnail column is gone (presets live in the key menu)
}

QRect CurveEditor::prevSegmentRect() const { return QRect(kMarginL - 26, 4, 22, kHeaderH - 8); }
QRect CurveEditor::nextSegmentRect() const {
    const QRect p = prevSegmentRect();
    return QRect(p.right() + 4, p.top(), p.width(), p.height());
}
// Reset sits at the RIGHT edge of the header, away from the arrows it must not
// be mistaken for.
QRect CurveEditor::resetRect() const {
    return QRect(width() - 62, 4, 56, kHeaderH - 8);
}

QString CurveEditor::statusText() const {
    const bool camMode = isCameraMode();
    const QString camPrefix = camMode ? QStringLiteral("Camera ") : QString();
    if (!segValid_)
        return camMode ? QStringLiteral("Camera: nothing to ease yet")
                       : QStringLiteral("Nothing to ease yet");
    if (mode_ == Interp::Hold)
        return QStringLiteral("%1%2 \u00b7 frames %3\u2013%4 \u00b7 Hold: the value jumps at %5")
            .arg(camPrefix)
            .arg(QString::fromUtf8(paramDesc(chan_).label))
            .arg(fA_)
            .arg(fB_)
            .arg(fB_);
    if (flat_) return QStringLiteral("%1%2 \u00b7 frames %3\u2013%4 \u00b7 flat segment: "
                                     "handles shape timing only")
                     .arg(camPrefix)
                      .arg(chanList_.isEmpty() ? QString() : QString::fromUtf8(
                          paramDesc(chan_).label))
                     .arg(fA_)
                     .arg(fB_);
    // The same formatter the transform panel uses: a scale must read "100% \u2192
    // 160%" in both places, not 1.0 \u2192 1.6 here and 100% there.
    return QStringLiteral("%1%2 \u00b7 frames %3\u2013%4 \u00b7 %5 \u2192 %6")
        .arg(camPrefix)
        .arg(QString::fromUtf8(paramDesc(chan_).label))
        .arg(fA_)
        .arg(fB_)
        .arg(formatParamValue(chan_, vA_))
        .arg(formatParamValue(chan_, vB_));
}

// --- painting ----------------------------------------------------------------

void CurveEditor::paintEvent(QPaintEvent *) {
    const bool hasTarget = block() != nullptr;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), InkTheme::lane());

    // --- header: the paging arrows and which segment is on screen ---------
    const bool canPrev = segValid_ && segIndex_ > 0;
    const bool canNext = segValid_ && segIndex_ + 1 < segCount_;
    auto arrow = [&](const QRect &r, bool left, bool enabled) {
        if (enabled && hoverArrow_ == (left ? -1 : 1)) {
            p.setPen(Qt::NoPen);
            p.setBrush(InkTheme::toolHover());
            p.drawRoundedRect(r, 3, 3);
        }
        p.setPen(QPen(enabled ? InkTheme::toolGlyph() : InkTheme::faint(), 1.5,
                      Qt::SolidLine, Qt::RoundCap));
        const QPointF c(r.center().x(), r.center().y());
        const double s = left ? -1.0 : 1.0;
        p.drawLine(c + QPointF(-2.5 * s, -4.0), c + QPointF(2.5 * s, 0.0));
        p.drawLine(c + QPointF(2.5 * s, 0.0), c + QPointF(-2.5 * s, 4.0));
    };
    arrow(prevSegmentRect(), true, canPrev);
    arrow(nextSegmentRect(), false, canNext);

    // Title and segment counter share one baseline: stacking them put the counter
    // under the chip row, where it read as a clipped artefact.
    const int textX = nextSegmentRect().right() + 12;
    p.setFont(uiFont(11, true));
    p.setPen(InkTheme::text());
    // A camera's curves live in this same panel, but the header must say so:
    // otherwise a camera selection that still reads "Easing" looks like the
    // regular transform graph and the artist cannot tell which object is
    // being shaped. Camera mode also names its target.
    const bool camModePaint = isCameraMode();
    const QString title = camModePaint
        ? (targetTitle().isEmpty()
               ? QStringLiteral("Camera Easing")
               : QStringLiteral("Camera Easing \u2014 %1").arg(targetTitle()))
        : QStringLiteral("Easing");
    p.drawText(QRect(textX, 0, width(), kHeaderH), Qt::AlignVCenter | Qt::AlignLeft, title);
    const int titleW = QFontMetrics(uiFont(11, true)).horizontalAdvance(title);
    p.setFont(uiFont(9));
    p.setPen(InkTheme::dim());
    const QString which =
        segValid_ ? QStringLiteral("segment %1 of %2").arg(segIndex_ + 1).arg(segCount_)
                  : QStringLiteral("no segment");
    p.drawText(QRect(textX + titleW + 10, 0, width(), kHeaderH), Qt::AlignVCenter | Qt::AlignLeft,
               which);

    // --- Reset: restore THIS segment's ease to linear --------------------
    if (segValid_) {
        const QRect rr = resetRect();
        const bool hot = hoverReset_;
        p.setPen(Qt::NoPen);
        p.setBrush(hot ? InkTheme::toolHover() : QColor());
        p.drawRoundedRect(rr, 3, 3);
        p.setFont(uiFont(9, true));
        p.setPen(hot ? InkTheme::text() : InkTheme::dim());
        p.drawText(rr, Qt::AlignCenter, QStringLiteral("Reset"));
    }

    // --- the transform dropdown lives left of Reset (child widget, paints
    //     itself); the value strip below the header holds child fields too.
    combo_->setGeometry(dropdownRect());
    combo_->setEnabled(hasTarget);
    // A hairline under the value strip so the graph reads as its own band.
    p.setPen(InkTheme::hairline());
    p.drawLine(0, kHeaderH + kValuesH - 1, width(), kHeaderH + kValuesH - 1);

    const QRect box = plot();
    paintBox(p, box);

    if (!segValid_) {
        // Say what is missing and how to fix it, rather than showing an empty
        // grid the user has to decode.
        p.setPen(InkTheme::faint());
        p.setFont(uiFont(10));
        const QString hint = chanList_.isEmpty()
            ? (isCameraMode()
                   ? QStringLiteral("Animate the camera on two or more frames, then come back "
                                    "to shape the ease between them.")
                   : QStringLiteral("Animate a parameter on two or more frames, then come back to "
                                    "shape the ease between them."))
            : (isCameraMode()
                   ? QStringLiteral("This camera motion has no segment at the playhead.")
                   : QStringLiteral("This parameter has no segment at the playhead."));
        p.drawText(box.adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, hint);
        drawStatus(p);
        return;
    }

    // --- the curve --------------------------------------------------------
    const QPointF p0(box.left(), box.bottom()), p3(box.right(), box.top());
    const QPointF c1 = toScreen(npA_), c2 = toScreen(npB_);

    // The linear diagonal underneath: every ease is read as a departure from it.
    p.setPen(QPen(InkTheme::faint(), 1.0, Qt::DashLine));
    p.drawLine(p0, p3);

    // Progress labels: only the ends carry meaning (0% and 100%), plus the
    // overshoot rail above the box.
    p.setFont(uiFont(8));
    p.setPen(InkTheme::dim());
    p.drawText(QRect(0, box.bottom() - 8, kMarginL - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("0%"));
    p.drawText(QRect(0, box.top() - 8, kMarginL - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("100%"));
    // Clear of the 0% / 100% rail labels on the left.
    const QRect xLabels(box.left() + 12, box.bottom() + 4, box.width() - 12, 14);
    p.drawText(xLabels, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("in"));
    p.drawText(xLabels, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("out"));

    const QColor line = InkTheme::curveColor((int)chan_);
    QPainterPath path;
    path.moveTo(p0);
    if (mode_ == Interp::Hold) {
        // What the compositor actually evaluates: the value sits at key A until
        // key B arrives. Drawing the bezier here would show a shape that is a lie.
        path.lineTo(p3.x(), p0.y());
        path.lineTo(p3);
    } else if (mode_ == Interp::Linear) {
        // A linear segment ignores its handles, so the graph shows the line it
        // really evaluates. The handles are still drawn (grabbing one converts).
        path.lineTo(p3);
    } else {
        path.cubicTo(c1, c2, p3);
    }
    p.setPen(QPen(line, 2.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Tethers from each corner to its handle: which key a handle belongs to must
    // be obvious before it is grabbed.
    p.setPen(QPen(InkTheme::faint(), 1.0));
    p.drawLine(p0, c1);
    p.drawLine(p3, c2);

    // Keys at both ends, so the segment's boundaries are visible in the box too.
    auto keyDot = [&](const QPointF &c) {
        p.setPen(QPen(InkTheme::keyRing(), 1.0));
        p.setBrush(InkTheme::keyFill());
        p.drawEllipse(c, 3.2, 3.2);
    };
    keyDot(p0);
    keyDot(p3);

    // The two big handles. Primary (A) is filled, secondary (B) is outlined, so
    // the pair reads as in/out rather than two identical dots.
    auto handle = [&](const QPointF &c, bool primary, bool hot) {
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(InkTheme::focusRing().red(), InkTheme::focusRing().green(),
                              InkTheme::focusRing().blue(), 60));
            p.drawEllipse(c, kHandleR + 4.0, kHandleR + 4.0);
        }
        p.setPen(QPen(hot ? InkTheme::focusRing() : InkTheme::keyRing(), hot ? 2.0 : 1.6));
        p.setBrush(primary ? line : InkTheme::card());
        p.drawEllipse(c, double(kHandleR), double(kHandleR));
        p.setPen(Qt::NoPen);
        p.setBrush(primary ? InkTheme::keyRing() : line);
        p.drawEllipse(c, 2.2, 2.2);
    };
    handle(c1, true, grab_ == Grab::HandleA);
    handle(c2, false, grab_ == Grab::HandleB);

    drawStatus(p);

    // The widget fills its rect, so the dock separator has to be painted here.
    p.setPen(QPen(InkTheme::border(), 1));
    p.drawLine(0, 0, 0, height());
}

void CurveEditor::drawStatus(QPainter &p) {
    p.setFont(uiFont(9));
    p.setPen(InkTheme::dim());
    p.drawText(QRect(kMarginL, height() - kStatusH, width() - kMarginL - 8, kStatusH),
               Qt::AlignVCenter | Qt::AlignLeft, statusText());
}

// --- mouse -------------------------------------------------------------------

void CurveEditor::mousePressEvent(QMouseEvent *e) {
    if (!block() || e->button() != Qt::LeftButton) return;
    const QPoint pos = e->pos();

    if (prevSegmentRect().contains(pos) && segValid_ && segIndex_ > 0) {
        stepSegment(-1);
        return;
    }
    if (nextSegmentRect().contains(pos) && segValid_ && segIndex_ + 1 < segCount_) {
        stepSegment(+1);
        return;
    }
    if (segValid_ && resetRect().contains(pos)) {
        resetEase();
        return;
    }
    if (dropdownRect().contains(pos)) return; // the combo child owns the click
    if (!segValid_) return;

    // Handles first: they sit on top of everything and are the point of the panel.
    if (handleRectA().contains(pos)) {
        grab_ = Grab::HandleA;
    } else if (handleRectB().contains(pos)) {
        grab_ = Grab::HandleB;
    } else {
        return;
    }
    dragMoved_ = false;
    if (CameraLayer *cam = camera())
        doc_->beginCameraRecord("Shape camera easing", cam);
    else if (AdjustmentLayer *adj_ = adjustment())
        doc_->beginParamRecord("Shape easing", adj_);
    else
        return;
    emit statusMessage(QStringLiteral("Drag to shape \u00b7 Shift snaps \u00b7 Alt breaks the "
                                      "tangent \u00b7 release to keep"));
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent *e) {
    const QPoint pos = e->pos();
    if (grab_ != Grab::None && segValid_) {
        QPointF np = toNorm(pos);
        // FLEXIBLE DRAGGING, bounded: y may overshoot the box (that is what
        // anticipation/follow-through looks like on the curve) but not run
        // away — beyond ±50% of the box the ease stops being musically
        // useful and the handle would climb out of the panel under the
        // channel chips. The old ±0.6 clamp blocked overshoot entirely; this
        // keeps it but keeps the handle reachable.
        np.ry() = std::clamp(np.y(), -0.5, 1.5);
        // x is the handle's reach across the segment, so it lives in the unit
        // box and must reach BOTH edges: 0 = tucked against the far key,
        // 1 = the handle spans the whole segment. Clamp HERE (not just at
        // rest) keeps the drag 1:1 with the pointer instead of snapping back
        // on release. The model keeps a 0.01 floor so a zero-width handle can
        // never make the x-curve degenerate; 1% of the box is imperceptible,
        // so the handle still reads as sitting on the edge.
        np.rx() = std::clamp(np.x(), 0.0, 1.0);
        if (e->modifiers() & Qt::ShiftModifier) {
            // Snap to the quarter grid: exactly-linear, exactly-eased stops.
            np = QPointF(std::round(np.x() * 4.0) / 4.0, std::round(np.y() * 4.0) / 4.0);
            np.rx() = std::clamp(np.x(), 0.0, 1.0);
        }
        if (flat_) np.ry() = (grab_ == Grab::HandleA) ? 1.0 / 3.0 : 2.0 / 3.0;
        writeHandle(grab_ == Grab::HandleA ? Which::A : Which::B, np,
                    !(e->modifiers() & Qt::AltModifier));
        dragMoved_ = true;
        emit graphChanged();
        update();
        return;
    }
    hover(pos);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *e) {
    if (grab_ == Grab::None) return;
    const bool moved = dragMoved_;
    grab_ = Grab::None;
    dragMoved_ = false;
    if (!camId_.empty())
        doc_->endCameraRecord();
    else
        doc_->endParamRecord();
    if (moved) {
        if (!locateSegment()) segValid_ = false;
        emit graphChanged();
    }
    emit statusMessage(statusText());
    update();
    Q_UNUSED(e);
}

void CurveEditor::hover(const QPoint &pos) {
    const int oldArrow = hoverArrow_;
    const bool oldReset = hoverReset_;
    hoverArrow_ = 0;
    hoverReset_ = false;
    if (prevSegmentRect().contains(pos)) hoverArrow_ = -1;
    else if (nextSegmentRect().contains(pos)) hoverArrow_ = 1;
    else if (segValid_ && resetRect().contains(pos)) hoverReset_ = true;
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (hoverArrow_ != 0 || hoverReset_) shape = Qt::PointingHandCursor;
    else if (segValid_ && (handleRectA().contains(pos) || handleRectB().contains(pos)))
        shape = Qt::SizeAllCursor;
    setCursor(shape);
    if (oldArrow != hoverArrow_ || oldReset != hoverReset_) update();
}

void CurveEditor::leaveEvent(QEvent *e) {
    hoverArrow_ = 0;
    hoverReset_ = false;
    update();
    Q_UNUSED(e);
}

void CurveEditor::keyPressEvent(QKeyEvent *e) {
    if (!segValid_) return QWidget::keyPressEvent(e);
    switch (e->key()) {
    case Qt::Key_Left:
        stepSegment(-1);
        return;
    case Qt::Key_Right:
        stepSegment(+1);
        return;
    case Qt::Key_Escape:
        if (grab_ != Grab::None) {
            grab_ = Grab::None;
            if (!camId_.empty())
                doc_->cancelCameraRecord();
            else
                doc_->cancelParamRecord();
            if (!locateSegment()) segValid_ = false;
            emit graphChanged();
            update();
        }
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

} // namespace ink
