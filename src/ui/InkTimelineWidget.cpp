#include "ui/InkTimelineWidget.h"
#include "ui/InkDocument.h"
#include "ui/InkTheme.h"
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace ink {

namespace {
constexpr int kPadR = 40;
constexpr int kPadB = 10;
constexpr int kHit = 22;      // gutter icon hit box
constexpr int kChipW = 20;    // layer thumbnail chip
constexpr int kChipH = 13;
constexpr int kThumbPx = 24;  // cached thumbnail height
constexpr int kEdgeTol = 5;   // px around a cell edge that starts a resize
// Range handles: the painted tab reaches kTabW px from its stem, and the grab
// zone is at least kRangeTol px wide either side of it — a miss used to fall
// through to scrubbing, which read as "the playhead steals the in/out drag".
constexpr int kTabW = 8;
constexpr int kRangeTol = 10;

// Resize tolerance scales with the cell so a narrow cell stays draggable in
// its middle and its edges stay reachable.
int edgeTolFor(int cellPx) {
    if (cellPx < 6) return 0; // too small to grab an edge: move only
    return std::max(2, std::min(kEdgeTol, cellPx / 3));
}

int GW() { return InkTheme::gutterW(); }
int RH() { return InkTheme::rulerH(); }
int FH() { return InkTheme::folderH(); }
int TH() { return InkTheme::trackH(); }
// Diamond size in the lane, and the grab radius for hit-testing. The grab is
// deliberately larger than the glyph: a 7px target you cannot hit is worse than
// no target at all.
constexpr int kKeyR = 4;
constexpr int kKeyHit = 7;
// The bar's trim grips: a fixed pixel band at each end, so grabbing an edge is
// equally easy at every zoom level (a fraction-of-width tolerance made trimming
// impossible when zoomed out and twitchy when zoomed in).
constexpr int kBarEdgeTol = 8;

QFont uiFont(int px, bool bold = false) {
    QFont f;
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}


// Small bounded cache: painting runs on every hover move, so re-rendering
// path icons per frame would be wasteful.
QPixmap cachedIcon(IconId id, const QColor &tint, int size) {
    static QHash<QString, QPixmap> cache;
    const QString k = QString::number(int(id)) + QLatin1Char('#') +
                      tint.name(QColor::HexArgb) + QLatin1Char('#') + QString::number(size);
    auto it = cache.constFind(k);
    if (it != cache.constEnd()) return it.value();
    QPixmap pm = InkIcons::get(id, tint, size).pixmap(size);
    if (cache.size() > 256) cache.clear();
    cache.insert(k, pm);
    return pm;
}

void drawPix(QPainter &p, const QPixmap &pm, const QRect &rc) {
    p.drawPixmap(rc.center() - QPoint(pm.width() / 2, pm.height() / 2), pm);
}

QPainterPath chevronPath(const QRectF &r, bool open) {
    QPainterPath path;
    if (open) {
        path.moveTo(r.left(), r.top() + r.height() * 0.28);
        path.lineTo(r.center().x(), r.top() + r.height() * 0.72);
        path.lineTo(r.right(), r.top() + r.height() * 0.28);
    } else {
        path.moveTo(r.left() + r.width() * 0.28, r.top());
        path.lineTo(r.left() + r.width() * 0.72, r.center().y());
        path.lineTo(r.left() + r.width() * 0.28, r.bottom());
    }
    return path;
}
} // namespace

TimelineWidget::TimelineWidget(Document *doc, QWidget *parent)
    : QWidget(parent), doc_(doc) {
    setMinimumHeight(150);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::CustomContextMenu);
    in_ = 0;
    out_ = std::max(0, doc_->timeline().frameCount() - 1);
}

void TimelineWidget::refresh() { update(); }

void TimelineWidget::invalidateCells() {
    thumbs_.clear();
    update();
}

void TimelineWidget::invalidateCell(const QString &trackId, int key) {
    if (key < 0 || trackId.isEmpty()) {
        refresh();
        return;
    }
    thumbs_.remove(trackId + QLatin1Char(':') + QString::number(key));
    update();
}

void TimelineWidget::setRange(int in, int out) {
    const int last = std::max(0, doc_->timeline().frameCount() - 1);
    in_ = std::clamp(std::min(in, out), 0, last);
    out_ = std::clamp(std::max(in, out), 0, last);
    update();
}

// Programmatic selection (camera creation, menus): must emit like a click
// does, so the dock can re-point the easing graph at the new working row.
// Without the emit the graph kept showing the previous layer's curves.
void TimelineWidget::setSelectedLayers(const QSet<QString> &ids) {
    layerSel_ = ids;
    folderSel_.clear();
    if (ids.size() == 1) anchorId_ = *ids.begin();
    emit layerSelectionChanged();
    update();
}

void TimelineWidget::clearLayerSelection() {
    if (layerSel_.isEmpty() && folderSel_.isEmpty()) return;
    layerSel_.clear();
    folderSel_.clear();
    emit layerSelectionChanged();
    update();
}

void TimelineWidget::setPxPerFrame(double v, int anchorX) {
    v = std::clamp(v, 3.0, 64.0);
    int anchorFrame = (anchorX >= 0) ? frameAtX(anchorX) : -1;
    pxPerFrame_ = v;
    if (anchorFrame >= 0) {
        int nx = xAtFrame(anchorFrame);
        hScroll_ = std::max(0, hScroll_ + (nx - anchorX));
        emit scrollRequest(hScroll_, vScroll_);
    }
    emit zoomChanged(pxPerFrame_);
    update();
}

void TimelineWidget::fitToRange() {
    int laneW = std::max(80, width() - GW());
    double v = double(laneW) / double(std::max(1, doc_->timeline().frameCount()));
    pxPerFrame_ = std::clamp(v, 3.0, 64.0);
    hScroll_ = 0;
    emit scrollRequest(hScroll_, vScroll_);
    emit zoomChanged(pxPerFrame_);
    update();
}

TimelineWidget::Columns TimelineWidget::columns() const {
    const int gw = GW();
    Columns c;
    c.spine = 2;
    c.expander = 7;
    c.check = 24;   // layer-selection checkbox (gutter multi-select for grouping)
    c.thumb = 42;
    c.name = 68;
    c.eye = gw - 56;
    c.lock = gw - 30;
    c.opacityRight = gw - 62;
    return c;
}

// The checkbox is the ONLY way to toggle a layer's selection without a
// modifier, so its rect is shared by paint and hit-testing.
QRect TimelineWidget::checkBoxRect(const RowInfo &r) const {
    const int side = 14;
    return QRect(columns().check, r.y + (r.h - side) / 2, side, side);
}

bool TimelineWidget::inCheckBox(const QRect &box, int x) {
    return x >= box.left() - 2 && x < box.right() + 3; // 2px slop each side
}

void TimelineWidget::toggleLayerSelection(const QString &trackId) {
    if (layerSel_.contains(trackId)) layerSel_.remove(trackId);
    else {
        layerSel_.insert(trackId);
        anchorId_ = trackId;
    }
    folderSel_.clear();
    if (PaintLayer *l = doc_->findTrack(trackId.toStdString())) doc_->setCurrentLayer(l);
    emit layerSelectionChanged();
    update();
}

// Folder checkbox: checked when every layer inside is selected; clicking it
// selects all of them or clears all of them.
void TimelineWidget::setFolderSelection(const QString &folderId, bool on) {
    const TimelineFolder *tf = doc_->timeline().folder(folderId.toStdString());
    if (!tf) return;
    for (const auto &t : tf->tracks) {
        const QString id = QString::fromStdString(t.layerId);
        if (on) layerSel_.insert(id);
        else layerSel_.remove(id);
    }
    folderSel_ = on ? folderId : QString();
    emit layerSelectionChanged();
    update();
}

std::vector<TimelineWidget::RowInfo> TimelineWidget::layoutRows() const {
    std::vector<RowInfo> rows;
    int y = RH() - vScroll_;
    auto makeTrackRow = [&](const TimelineFolder &f, const QString &folderId, const auto &t,
                            bool detached) {
        RowInfo r;
        r.trackId = QString::fromStdString(t.layerId);
        r.folderId = folderId;
        r.label = QString::fromStdString(t.layerName);
        r.isCamera = t.isCamera;
        r.cameraScope = t.cameraScope;
        r.detachedCam = detached;
        if (t.isCamera) {
            if (CameraLayer *cam = doc_->findCamera(t.layerId))
                r.animated = cam->animated();
            else
                r.animated = false;
        } else if (PaintLayer *pl = doc_->findTrack(t.layerId)) {
            r.animated = pl->animated();
        } else {
            r.animated = false;
        }
        r.y = y;
        r.h = TH();
        rows.push_back(r);
        y += TH();
    };
    for (auto &f : doc_->timeline().folders()) {
        const QString folderId = QString::fromStdString(f.folderId);
        // Folder-scope cameras frame the whole group: their rows sit ABOVE
        // the group header (outside the block), never as sub-rows inside it.
        // Shown even when the folder is collapsed — they are not members.
        for (auto &t : f.tracks) {
            if (t.isCamera && t.cameraScope == int(CameraScope::Folder))
                makeTrackRow(f, folderId, t, true);
        }
        RowInfo h;
        h.isFolderHeader = true;
        h.folderId = folderId;
        h.label = QString::fromStdString(f.folderName);
        h.expanded = f.expanded;
        // Members only: detached group cameras paint above, not inside.
        h.childCount = 0;
        for (auto &t : f.tracks)
            if (!(t.isCamera && t.cameraScope == int(CameraScope::Folder))) ++h.childCount;
        h.y = y;
        h.h = FH();
        rows.push_back(h);
        y += FH();
        if (f.expanded) {
            for (auto &t : f.tracks) {
                if (t.isCamera && t.cameraScope == int(CameraScope::Folder)) continue;
                makeTrackRow(f, folderId, t, false);
                // No vertical parameter expansion: an adjustment layer is ONE
                // row whose lane holds its BAR. The parameters live in the
                // transform panel that opens from the bar (right-click or
                // double-click), Alight-Motion style — never as sub-rows that
                // push the whole timeline down.
            }
        }
    }
    return rows;
}

QStringList TimelineWidget::trackRowOrder() const {
    QStringList out;
    for (const RowInfo &r : layoutRows())
        out.push_back(r.isFolderHeader ? QStringLiteral("folder:") + r.folderId : r.trackId);
    return out;
}

int TimelineWidget::rowIndexAt(const std::vector<RowInfo> &rows, int y) const {
    for (size_t i = 0; i < rows.size(); ++i)
        if (y >= rows[i].y && y < rows[i].y + rows[i].h) return (int)i;
    return -1;
}

int TimelineWidget::totalRowsHeight() const {
    int y = RH();
    for (auto &f : doc_->timeline().folders()) {
        y += FH();
        if (!f.expanded) continue;
        y += (int)f.tracks.size() * TH();
    }
    return y;
}

int TimelineWidget::xAtFrame(int f) const { return GW() + int(f * pxPerFrame_) - hScroll_; }
int TimelineWidget::frameAtX(int x) const {
    int f = int((x - GW() + hScroll_) / pxPerFrame_);
    return std::clamp(f, 0, doc_->timeline().frameCount() - 1);
}

// The in handle sits on the range's first frame, the out handle on the
// exclusive right edge of its last frame. Grab zones fill the ruler height
// and never shrink below kRangeTol (they grow to half a frame when zoomed in).
QRect TimelineWidget::rangeGrabRect(int side) const {
    const int x = (side == 1) ? xAtFrame(in_) : xAtFrame(out_) + int(pxPerFrame_);
    const int tol = std::max(kRangeTol, int(pxPerFrame_ / 2));
    return QRect(x - tol, 0, 2 * tol + 1, RH());
}

// 0 = neither, 1 = in, 2 = out. When the two zones overlap (tight range, low
// zoom) the nearer stem wins, so both ends stay reachable.
int TimelineWidget::rangeHitAt(int x) const {
    const bool inHit = rangeGrabRect(1).contains(x, 0);
    const bool outHit = rangeGrabRect(2).contains(x, 0);
    if (inHit && outHit) {
        const int xi = xAtFrame(in_), xo = xAtFrame(out_) + int(pxPerFrame_);
        return std::abs(x - xi) <= std::abs(x - xo) ? 1 : 2;
    }
    if (inHit) return 1;
    if (outHit) return 2;
    return 0;
}

int TimelineWidget::contentWidth() const {
    return GW() + int(doc_->timeline().frameCount() * pxPerFrame_) + kPadR;
}
int TimelineWidget::contentHeight() const { return totalRowsHeight() + kPadB; }

void TimelineWidget::setScroll(int hx, int vy) {
    hScroll_ = std::max(0, hx);
    vScroll_ = std::max(0, vy);
    update();
}

void TimelineWidget::ensureFrameVisible(int frame) {
    int x = xAtFrame(frame) + int(pxPerFrame_ / 2);
    int nh = hScroll_;
    if (x < GW() + 8) nh = std::max(0, hScroll_ - (GW() + 8 - x));
    else if (x > width() - 8) nh = hScroll_ + (x - (width() - 8));
    if (nh != hScroll_) {
        hScroll_ = std::max(0, nh);
        emit scrollRequest(hScroll_, vScroll_);
        update();
    }
}

std::vector<TimelineWidget::HeaderBtn> TimelineWidget::headerButtons() const {
    const int gw = GW();
    std::vector<GroupLayer*> folders;
    doc_->image().collectFolders(folders);
    const bool canUngroup = !folderSel_.isEmpty() && folders.size() > 1;
    // Five buttons, right-aligned in the gutter header: the three creation
    // buttons keep their order and "new adjustment layer" joins them, so it sits
    // exactly where the layer/folder buttons already are.
    std::vector<HeaderBtn> btns = {
        {HeadBtn::AddLayer, QRect(gw - 128, 4, kHit, kHit), IconId::LayerPlus,
         QStringLiteral("New layer"), true},
        {HeadBtn::AddGroup, QRect(gw - 102, 4, kHit, kHit), IconId::GroupPlus,
         QStringLiteral("New folder"), true},
        {HeadBtn::GroupSelected, QRect(gw - 76, 4, kHit, kHit), IconId::GroupMerge,
         QStringLiteral("Group selected layers"), !layerSel_.isEmpty()},
        {HeadBtn::Ungroup, QRect(gw - 50, 4, kHit, kHit), IconId::Ungroup,
         QStringLiteral("Ungroup folder"), canUngroup},
    };
    return btns;
}

// --- keyframe row geometry ---------------------------------------------------
// Paint and hit-testing both go through these, so what is drawn is exactly what
// can be grabbed.

PaintLayer *TimelineWidget::animatedLayerOf(const QString &trackId) const {
    return doc_->findTrack(trackId.toStdString());
}

CameraLayer *TimelineWidget::cameraOf(const QString &trackId) const {
    return doc_->findCamera(trackId.toStdString());
}

QString TimelineWidget::cameraTargetName(const CameraLayer *cam) const {
    if (!cam) return {};
    if (cam->scope() == CameraScope::Global) return QStringLiteral("whole shot");
    if (cam->targetId() == 0) return QStringLiteral("nothing");
    if (Layer *t = doc_->findLayer("L" + std::to_string(cam->targetId())))
        return QString::fromStdString(t->name());
    return QStringLiteral("deleted layer");
}

const ParamBlock *TimelineWidget::paramsOf(const QString &trackId) const {
    if (PaintLayer *pl = doc_->findTrack(trackId.toStdString())) return &pl->params();
    if (CameraLayer *cam = doc_->findCamera(trackId.toStdString())) return &cam->params();
    return nullptr;
}

QRect TimelineWidget::inlineEyeRect(const RowInfo &r) const {
    return QRect(columns().eye, r.y + (r.h - kHit) / 2, kHit, kHit);
}

QRect TimelineWidget::inlineLockRect(const RowInfo &r) const {
    return QRect(columns().lock, r.y + (r.h - kHit) / 2, kHit, kHit);
}

// --- keyed range on an animated paint layer ---------------------------------
// The keys live ON the layer; this helper reports the span from the first to
// the last keyed frame, clamped to the lane. Paint, hover and drag all read
// the SAME rect, so what you see is exactly what you can grab.
QRect TimelineWidget::keyedRangeRect(const RowInfo &r) const {
    const ParamBlock *pb = paramsOf(r.trackId);
    if (!pb || pb->empty()) return {};
    const auto keys = keyFramesOf(r.trackId, -1);
    if (keys.empty()) return {};
    const int gw = GW();
    const int x0 = xAtFrame(keys.front());
    const int x1 = xAtFrame(keys.back()) + int(pxPerFrame_);
    const int inset = InkTheme::rowPad();
    const int left = std::max(x0, gw);
    const int right = std::min(x1, contentWidth());
    if (right <= left) return {};
    return QRect(left, r.y + inset, std::max(2, right - left), r.h - 2 * inset);
}

// -1 = left edge (retime in), +1 = right edge (retime out), 2 = body (move all
// keys), 0 = not on the keyed range at all. The grip band is a fixed pixel
// width, so trimming is equally easy zoomed in or out.
int TimelineWidget::keyedRangeHit(const RowInfo &r, int x) const {
    const QRect bar = keyedRangeRect(r);
    if (bar.isEmpty() || !bar.contains(x, r.y + r.h / 2)) return 0;
    // The range is DERIVED from the keys, so there is nothing to trim: the only
    // gesture is a body drag that retimes every key together. (Diamonds are
    // grabbed first — keyHitTest runs before this — so a diamond drag still
    // moves just the selected keys.)
    return 2;
}

QRect TimelineWidget::keyedRangeRect(const QString &trackId) const {
    for (const RowInfo &r : layoutRows())
        if (r.trackId == trackId && !r.isFolderHeader) return keyedRangeRect(r);
    return {};
}

int TimelineWidget::frameAtLaneX(int x) const {
    if (x < GW()) return -1;
    return frameAtX(x);
}

int TimelineWidget::keyHitTest(const RowInfo &r, const QPoint &pos, int *frameOut) const {
    if (frameOut) *frameOut = -1;
    if (pos.x() < GW() || pos.y() < r.y || pos.y() >= r.y + r.h) return 0;
    const ParamBlock *pb = paramsOf(r.trackId);
    if (!pb || pb->empty()) return 0;
    // A diamond is a keyed FRAME; selecting it selects every parameter keyed
    // there, which is what the easing graph operates on.
    // NOT restricted to the keyed range's rect. paintKeyLane draws a diamond
    // for every keyed frame — those keys are real and still editable, so a
    // painted diamond you cannot click is a paint/hit disagreement. The lane
    // is the hit area, same as the paint.
    const int f = frameAtX(pos.x());
    const int cx = xAtFrame(f) + int(pxPerFrame_ / 2);
    if (std::abs(pos.x() - cx) <= kKeyHit && pb->hasAnyKeyAt(f)) {
        if (frameOut) *frameOut = f;
        return 1;
    }
    return 0;
}

std::vector<int> TimelineWidget::keyFrames(const PaintLayer &pl, int param) const {
    if (param < 0) {
        std::vector<int> out;
        for (ParamId id : pl.params().animatedParams())
            for (int f : pl.params().keys(id)) out.push_back(f);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    return pl.params().keys((ParamId)param);
}

std::vector<int> TimelineWidget::keyFrames(const CameraLayer &cam, int param) const {
    if (param < 0) {
        std::vector<int> out;
        for (ParamId id : cam.params().animatedParams())
            for (int f : cam.params().keys(id)) out.push_back(f);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    return cam.params().keys((ParamId)param);
}

std::vector<int> TimelineWidget::keyFramesOf(const QString &trackId, int param) const {
    if (PaintLayer *pl = doc_->findTrack(trackId.toStdString())) return keyFrames(*pl, param);
    if (CameraLayer *cam = doc_->findCamera(trackId.toStdString())) return keyFrames(*cam, param);
    return {};
}

void TimelineWidget::noteAnimationChanged() {
    keySel_.clear();
    update();
}

QPixmap TimelineWidget::thumbFor(const QString &trackId, int key) {
    if (key < 0) return {};
    const QString k = trackId + QLatin1Char(':') + QString::number(key);
    auto it = thumbs_.constFind(k);
    if (it != thumbs_.constEnd()) return it.value();
    QPixmap pm;
    // The cell's layer stack is composited, so the chip/filmstrip shows what
    // the frame actually looks like — not just its active layer.
    if (PaintLayer *layer = doc_->findTrack(trackId.toStdString())) {
        PaintDevice composite;
        Rect e = doc_->image().cellComposite(*layer, key, composite);
        if (!e.isEmpty()) {
            std::vector<uint8_t> buf(size_t(e.w) * size_t(e.h) * 4);
            composite.readRect(e, buf.data());
            QImage full(buf.data(), e.w, e.h, QImage::Format_RGBA8888);
            QImage scaled = full.scaledToHeight(kThumbPx, Qt::SmoothTransformation);
            pm = QPixmap::fromImage(scaled);
        }
    }
    if (thumbs_.size() > 256) thumbs_.clear();
    thumbs_.insert(k, pm);
    return pm;
}

bool TimelineWidget::cellHasContent(const QString &trackId, int key) const {
    if (key < 0) return false;
    PaintLayer *layer = const_cast<Document*>(doc_)->findTrack(trackId.toStdString());
    if (!layer) return false;
    const Cell *c = layer->cellAt(key);
    return c && c->hasContent();
}

// ---------------------------------------------------------------- painting

void TimelineWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), InkTheme::lane());

    // --- rows (clipped under the sticky header) ---
    p.save();
    p.setClipRect(0, RH(), width(), std::max(0, height() - RH()));
    auto rows = layoutRows();
    for (auto &r : rows) {
        if (r.y + r.h < RH() || r.y > height()) continue;
        if (r.isFolderHeader) paintFolderRow(p, r);
        else {
            const QString id = r.trackId;
            bool isCurrent = doc_->currentLayer() && doc_->currentLayer()->stableId() == id.toStdString();
            paintTrackRow(p, r, isCurrent);
        }
    }
    paintRubberBand(p);
    p.restore(); // drop the row clip opened above

    // --- sticky header on top ---
    // OUTSIDE the row clip on purpose. The clip starts at y = RH() so rows
    // scroll under the header; drawing the header inside it clipped the whole
    // strip away, which silently took the "LAYERS" caption and all four
    // gutter buttons (add layer, add folder, group, ungroup) with it.
    paintHeaderRows(p);
}

void TimelineWidget::paintHeaderRows(QPainter &p) {
    const int gw = GW();
    p.fillRect(0, 0, width(), RH(), InkTheme::chrome());
    p.setPen(InkTheme::hairline());
    p.drawLine(0, RH() - 1, width(), RH() - 1);
    p.drawLine(gw - 1, 0, gw - 1, RH());

    // gutter header: caption + count + layer/group buttons
    QFont capFont = uiFont(9, true);
    capFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    p.setFont(capFont);
    p.setPen(InkTheme::faint());
    std::vector<GroupLayer*> folders;
    doc_->image().collectFolders(folders);
    int layerCount = 0;
    for (auto *g : folders) layerCount += (int)g->children().size();
    p.drawText(QRect(10, 0, 70, RH()), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("LAYERS"));
    p.setFont(uiFont(10));
    p.setPen(InkTheme::faint());
    p.drawText(60, 0, 44, RH(), Qt::AlignVCenter | Qt::AlignLeft,
               QString::number(layerCount));

    const std::vector<HeaderBtn> btns = headerButtons();
    for (size_t i = 0; i < btns.size(); ++i) {
        const HeaderBtn &b = btns[i];
        const bool hot = (hoverBtn_ == (int)i);
        if (hot && b.enabled) p.fillRect(b.rect, InkTheme::hover());
        const QColor tint = !b.enabled ? InkTheme::faint()
                                       : (hot ? InkTheme::text() : InkTheme::dim());
        drawPix(p, cachedIcon(b.icon, tint, 15), b.rect);
    }
    paintRuler(p);
}

void TimelineWidget::paintRuler(QPainter &p) {
    const int gw = GW();
    const int fc = doc_->timeline().frameCount();
    const int fps = std::max(1, doc_->timeline().fps());
    const int cur = doc_->timeline().currentFrame();
    const int px = xAtFrame(cur) + int(pxPerFrame_ / 2);
    // frame tag geometry first: ruler labels step aside for it
    const QString tag = QString::number(cur);
    const QFont tagFont = uiFont(9, true);
    const int tagW = QFontMetrics(tagFont).horizontalAdvance(tag) + 10;
    QRect tagRect(px + 8, 1, tagW, 13);
    if (tagRect.right() > width() - 2) tagRect.moveRight(px - 8);
    p.save();
    p.setClipRect(gw, 0, width() - gw, RH());

    int step = 1;
    while (step * pxPerFrame_ < 52) step *= 2;
    // rects that ruler numbers must dodge: the playhead tag and the range labels
    std::vector<QRect> dodges;
    dodges.push_back(tagRect.adjusted(-4, 0, 4, 0));
    const int xIn = xAtFrame(in_), xOut = xAtFrame(out_) + int(pxPerFrame_);
    if (xIn > gw + 22) dodges.push_back(QRect(xIn - 20, RH() - 12, 22, 10));
    if (xOut + 26 < width()) dodges.push_back(QRect(xOut + 6, RH() - 12, 26, 10));
    auto dodged = [&](int x, int y) {
        for (const QRect &r : dodges)
            if (r.contains(x, y)) return true;
        return false;
    };
    p.setFont(uiFont(9));
    // Visible frames only (was: 0..frameCount, up to 100k iterations of
    // xAtFrame+branches per paint at 60 Hz — identical pixels, ~zero work).
    const int f0 = std::max(0, frameAtX(gw - 2) - 1);
    const int f1 = std::min(fc - 1, frameAtX(width()) + 1);
    for (int f = f0; f <= f1; ++f) {
        const int x = xAtFrame(f);
        if (x < gw - 2 || x > width()) continue;
        const bool onSecond = (f % fps) == 0;
        const bool major = (f % step) == 0;
        if (!major && !onSecond && pxPerFrame_ < 6.0) continue;
        const int len = onSecond ? 7 : (major ? 6 : 3);
        p.setPen(onSecond ? InkTheme::dim() : InkTheme::tick());
        p.drawLine(x, RH() - len, x, RH() - 1);
        if ((major || onSecond) && !dodged(x + 3, RH() - 10)) {
            p.setPen(onSecond ? InkTheme::dim() : InkTheme::faint());
            p.drawText(x + 3, RH() - 8, QString::number(f));
        }
    }
    // onion skin markers relative to the playhead
    const OnionSkinConfig &on = doc_->onion();
    if (on.enabled) {
        const int cur = doc_->timeline().currentFrame();
        for (int i = 1; i <= on.prevCount; ++i) {
            int f = cur - i;
            if (f < 0) continue;
            int x = xAtFrame(f) + int(pxPerFrame_ / 2);
            QColor c = QColor::fromRgb(on.prevTint.r, on.prevTint.g, on.prevTint.b);
            c.setAlpha(120);
            p.fillRect(x, RH() - 4, std::max(1, int(pxPerFrame_ / 2)), 3, c);
        }
        for (int i = 1; i <= on.nextCount; ++i) {
            int f = cur + i;
            if (f >= fc) continue;
            int x = xAtFrame(f) + int(pxPerFrame_ / 2);
            QColor c = QColor::fromRgb(on.nextTint.r, on.nextTint.g, on.nextTint.b);
            c.setAlpha(120);
            p.fillRect(x, RH() - 4, std::max(1, int(pxPerFrame_ / 2)), 3, c);
        }
    }
    // playback range: a bright band under the ticks with grab-able ends
    {
        const int xa = xIn;
        const int xb = xOut;
        const int bandY = RH() - 4;
        QColor band = InkTheme::accent();
        band.setAlpha(hoverRange_ != 0 || dragRange_ != 0 ? 200 : 130);
        p.fillRect(QRect(std::max(gw, xa), bandY, std::max(0, std::min(width(), xb) - std::max(gw, xa)), 3),
                   band);
        for (int side : {1, 2}) {
            const int x = (side == 1) ? xAtFrame(in_) : xAtFrame(out_) + int(pxPerFrame_);
            if (x < gw - 12 || x > width() + 12) continue;
            const bool hot = (dragRange_ == side) || (hoverRange_ == side);
            p.setPen(Qt::NoPen);
            p.setBrush(hot ? InkTheme::accent() : InkTheme::dim());
            QPainterPath h;
            if (side == 1) {
                // stem on the first frame, tab pointing right
                h.moveTo(x, 4);
                h.lineTo(x + kTabW, 4);
                h.lineTo(x + kTabW, 8);
                h.lineTo(x + 2, 8);
                h.lineTo(x + 2, RH() - 1);
                h.lineTo(x, RH() - 1);
            } else {
                // stem on the last frame's right edge, tab pointing left
                h.moveTo(x, 4);
                h.lineTo(x - kTabW, 4);
                h.lineTo(x - kTabW, 8);
                h.lineTo(x - 2, 8);
                h.lineTo(x - 2, RH() - 1);
                h.lineTo(x, RH() - 1);
            }
            h.closeSubpath();
            p.drawPath(h);
        }
        p.setFont(uiFont(8, true));
        p.setPen(InkTheme::faint());
        if (xAtFrame(in_) > gw + 22) p.drawText(xAtFrame(in_) - 20, RH() - 7, QStringLiteral("IN"));
        // the out tab/band sit on the last frame's right edge
        if (xOut + 26 < width()) p.drawText(xOut + 8, RH() - 7, QStringLiteral("OUT"));
    }
    // playhead head + frame tag (geometry computed above so labels can dodge)
    p.setPen(Qt::NoPen);
    p.setBrush(InkTheme::playhead());
    QPainterPath head;
    head.moveTo(px - 5, 0);
    head.lineTo(px + 5, 0);
    head.lineTo(px, 8);
    head.closeSubpath();
    p.fillPath(head, InkTheme::playhead());
    p.setFont(tagFont);
    p.drawRoundedRect(tagRect, 3, 3);
    p.setPen(QColor(27, 28, 32));
    p.drawText(tagRect, Qt::AlignCenter, tag);
    p.restore();
}

void TimelineWidget::paintFolderRow(QPainter &p, const RowInfo &r) {
    const Columns c = columns();
    const int gw = GW();
    p.fillRect(0, r.y, width(), r.h, InkTheme::laneAlt());
    p.setPen(InkTheme::hairline());
    p.drawLine(0, r.y, width(), r.y);
    GroupLayer *g = nullptr;
    std::vector<GroupLayer*> folders;
    doc_->image().collectFolders(folders);
    for (auto *f : folders)
        if (f->stableId() == r.folderId.toStdString()) { g = f; break; }
    const TimelineFolder *tf = doc_->timeline().folder(r.folderId.toStdString());
    const int hue = tf ? tf->colorIndex : 0;
    // spine tinted to the folder hue (subtle, 2px)
    QColor spine = InkTheme::layerHue(hue);
    spine.setAlpha(150);
    p.fillRect(0, r.y + 1, c.spine, r.h - 1, spine);
    if (r.folderId == folderSel_) p.fillRect(c.spine, r.y, 1, r.h, InkTheme::accent());

    // folder checkbox: checked when every layer inside is selected; clicking it
    // selects or clears the whole folder at once.
    const int childLayers = (int)r.childCount;
    int selChildren = 0;
    if (tf)
        for (const auto &t : tf->tracks)
            if (layerSel_.contains(QString::fromStdString(t.layerId))) ++selChildren;
    const bool folderChecked = childLayers > 0 && selChildren == childLayers;
    const QRect fbox = checkBoxRect(r);
    // an empty folder has nothing to select: draw the box as disabled
    const bool canCheck = childLayers > 0;
    p.setPen(QPen(canCheck ? (folderChecked ? InkTheme::accent() : InkTheme::tick())
                           : InkTheme::hairline(), 1));
    p.setBrush(folderChecked ? InkTheme::accent()
                             : (canCheck ? QColor(0, 0, 0, 40) : QColor(0, 0, 0, 18)));
    p.drawRoundedRect(QRectF(fbox).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (folderChecked) {
        p.setPen(QPen(InkTheme::bg(), 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(fbox.left() + 3, fbox.center().y(),
                   fbox.center().x() - 1, fbox.bottom() - 4);
        p.drawLine(fbox.center().x() - 1, fbox.bottom() - 4,
                   fbox.right() - 3, fbox.top() + 4);
    }
    p.setBrush(Qt::NoBrush);

    // expander chevron
    QPainterPath ch = chevronPath(QRectF(c.expander, r.y + (r.h - 10) / 2.0, 9, 10), r.expanded);
    p.setPen(QPen(r.expanded ? InkTheme::text() : InkTheme::dim(), 1.5, Qt::SolidLine,
                  Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(ch);
    drawPix(p, cachedIcon(r.expanded ? IconId::FolderOpen : IconId::Folder, InkTheme::layerHue(hue), 15),
            QRect(c.thumb, r.y + (r.h - 15) / 2, 16, 15));

    p.setFont(uiFont(11, true));
    p.setPen(InkTheme::text());
    const int nameW = c.opacityRight - c.name;
    QString name = QFontMetrics(p.font()).elidedText(r.label, Qt::ElideRight, nameW);
    p.drawText(QRect(c.name, r.y, nameW, r.h), Qt::AlignVCenter | Qt::AlignLeft, name);
    p.setFont(uiFont(9));
    p.setPen(InkTheme::faint());
    p.drawText(QRect(c.name + QFontMetrics(uiFont(11, true)).horizontalAdvance(name) + 6, r.y, 30, r.h),
               Qt::AlignVCenter | Qt::AlignLeft, QString::number(r.childCount));

    const bool vis = g ? g->visible() : true;
    const bool locked = g ? g->locked() : false;
    drawPix(p, cachedIcon(vis ? IconId::Eye : IconId::EyeOff,
                          vis ? InkTheme::faint() : InkTheme::accentMuted(), 14),
            QRect(c.eye, r.y + (r.h - kHit) / 2, kHit, kHit));
    drawPix(p, cachedIcon(locked ? IconId::Lock : IconId::Unlock,
                          locked ? InkTheme::warn() : InkTheme::faint(), 14),
            QRect(c.lock, r.y + (r.h - kHit) / 2, kHit, kHit));

    // lane strip behind the header keeps the grid continuous
    p.fillRect(gw, r.y, width() - gw, r.h, InkTheme::laneAlt());
    p.setPen(InkTheme::hairline());
    p.drawLine(gw, r.y + r.h - 1, width(), r.y + r.h - 1);
}

void TimelineWidget::paintTrackRow(QPainter &p, const RowInfo &r, bool isCurrent) {
    const Columns c = columns();
    const int gw = GW();
    const std::string sid = r.trackId.toStdString();
    PaintLayer *layer = doc_->findTrack(sid);
    CameraLayer *cam = r.isCamera ? doc_->findCamera(sid) : nullptr;
    if (!layer && !cam && r.isCamera) cam = doc_->findCamera(sid);
    const TimelineFolder *tf = doc_->timeline().folder(r.folderId.toStdString());
    const TimelineRow *row = doc_->timeline().row(sid);
    // Default hue: the folder's hue stepped by the track's position inside it,
    // so stacked layers stay tellable apart while still reading as one group.
    int hue = tf ? tf->colorIndex : 0;
    if (tf) {
        for (size_t i = 0; i < tf->tracks.size(); ++i)
            if (tf->tracks[i].layerId == sid) { hue = (hue + (int)i) % 8; break; }
    }
    if (row && row->colorOverride >= 0) hue = row->colorOverride;

    const bool hovered = (hoverTrack_ == r.trackId);
    const bool selected = layerSel_.contains(r.trackId);
    // Camera rows sit one visual level down: a dimmer base so they read as
    // children of the layer/folder they drive, never as another paint track.
    QColor rowBg = cam ? InkTheme::laneAlt() : InkTheme::lane();
    if (isCurrent) rowBg = InkTheme::rowCurrent();
    else if (selected) rowBg = InkTheme::rowSelected();
    else if (hovered) rowBg = InkTheme::rowHover();
    p.fillRect(0, r.y, width(), r.h, rowBg);
    if (cam && !r.detachedCam) {
        // The IDE-tree elbow: a vertical guide with a tick into the row, so a
        // layer camera is visibly "plugged into" the track above it. Detached
        // group cameras paint above their group with no elbow: they frame the
        // whole block, they are not children of it.
        QColor guide = InkTheme::faint();
        guide.setAlpha(150);
        p.setPen(QPen(guide, 1));
        const int gx = c.check - 9;
        p.drawLine(gx, r.y, gx, r.y + r.h);
        const int midY = r.y + r.h / 2;
        p.drawLine(gx, midY, c.check - 1, midY);
    }
    // Selection feedback has to be unmistakable: a tinted gutter strip, the
    // checkbox below and the accent bar. The hue stays visible either way, so
    // "Layer color" always shows its effect in the gutter.
    if (selected) {
        QColor tintSel = InkTheme::accent();
        tintSel.setAlpha(30);
        p.fillRect(0, r.y, gw, r.h, tintSel);
    }
    p.fillRect(0, r.y, c.spine, r.h, InkTheme::layerHue(hue));
    if (isCurrent) p.fillRect(c.spine, r.y, 2, r.h, InkTheme::accent());
    else if (selected) p.fillRect(c.spine, r.y, 2, r.h, InkTheme::accentMuted());

    // selection checkbox (this is the control used to group layers)
    const QRect box = checkBoxRect(r);
    p.setPen(QPen(selected ? InkTheme::accent() : (hovered ? InkTheme::dim() : InkTheme::tick()), 1));
    p.setBrush(selected ? InkTheme::accent() : QColor(0, 0, 0, 40));
    p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (selected) {
        p.setPen(QPen(InkTheme::bg(), 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(box.left() + 3, box.center().y(), box.center().x() - 1, box.bottom() - 4);
        p.drawLine(box.center().x() - 1, box.bottom() - 4, box.right() - 3, box.top() + 4);
    }
    p.setBrush(Qt::NoBrush);

    // layer chip: thumbnail of the cell exposed at the playhead, or a sheet
    // glyph while nothing is exposed there. Cameras show a transform glyph.
    const QRect chip(c.thumb, r.y + (r.h - kChipH) / 2, kChipW, kChipH);
    p.fillRect(chip, QColor(0, 0, 0, 60));
    p.setPen(InkTheme::hairline());
    p.drawRect(chip.adjusted(0, 0, -1, -1));
    if (cam) {
        const qreal dpr = devicePixelRatioF();
        const int s = int(11 * dpr);
        QPixmap glyph = cachedIcon(IconId::Camera, InkTheme::dim(), s);
        glyph.setDevicePixelRatio(dpr);
        drawPix(p, glyph, chip);
    } else {
        const int key = layer ? layer->exposureAt(doc_->timeline().currentFrame()) : -1;
        const bool hasCell = (layer && key >= 0 && layer->cellAt(key) != nullptr);
        QPixmap chipPm = hasCell ? thumbFor(r.trackId, key) : QPixmap();
        if (!chipPm.isNull()) {
            QSize sz = chipPm.size().scaled(chip.size() - QSize(2, 2), Qt::KeepAspectRatio);
            p.drawPixmap(QRect(chip.center() - QPoint(sz.width() / 2, sz.height() / 2), sz),
                         chipPm);
        } else {
            const qreal dpr = devicePixelRatioF();
            const int s = int(11 * dpr);
            QPixmap glyph = cachedIcon(IconId::Cell, InkTheme::faint(), s);
            glyph.setDevicePixelRatio(dpr);
            drawPix(p, glyph, chip);
        }
    }

    p.setFont(uiFont(11));
    p.setPen(isCurrent ? InkTheme::text() : InkTheme::dim());
    const bool locked = layer ? layer->locked() : (cam ? cam->locked() : false);    // Tags to the right of the name: blend mode and opacity, so "Blend mode"
    // and "Opacity" in the layer menu visibly land on the row.
    // Cameras show a CAM + scope tag (Layer/Folder/Global) instead.
    QString blendTag;
    QString camTag;
    if (cam) {
        camTag = (cam->scope() == CameraScope::Global   ? QStringLiteral("CAM·G")
                  : cam->scope() == CameraScope::Folder ? QStringLiteral("CAM·F")
                                                        : QStringLiteral("CAM"));
    } else if (layer) {
        switch (layer->blend()) {
        case BlendMode::Multiply: blendTag = QStringLiteral("Mul"); break;
        case BlendMode::Screen: blendTag = QStringLiteral("Scr"); break;
        case BlendMode::Erase: blendTag = QStringLiteral("Ers"); break;
        default: break;
        }
    }
    const bool hasOpacity = layer && layer->opacity() < 0.999f;
    const bool animatedTag = layer ? layer->animated() : (cam && cam->animated());
    const int tagW = (!blendTag.isEmpty()   ? 26
                      : !camTag.isEmpty()    ? 52
                                             : 0) +
                     (hasOpacity ? 34 : 0) + (animatedTag ? 18 : 0);
    // Layer cameras sit one level in (the elbow's tick points at them) and
    // read italic, so the gutter itself says "child of the track above".
    // Detached group cameras sit at top level with a straight name: they
    // belong to the group below, not to any track above.
    const bool indentedCam = cam && !r.detachedCam;
    const int nameX = indentedCam ? c.name + 14 : c.name;
    if (indentedCam) {
        QFont italic = uiFont(11);
        italic.setItalic(true);
        p.setFont(italic);
    }
    const int nameW = std::max(24, c.opacityRight - nameX - tagW);
    QString name = QFontMetrics(p.font()).elidedText(r.label, Qt::ElideRight, nameW);
    p.drawText(QRect(nameX, r.y, nameW, r.h), Qt::AlignVCenter | Qt::AlignLeft, name);
    if (locked) {
        p.setFont(uiFont(9));
        p.setPen(InkTheme::warn());
        p.drawText(QRect(c.name + QFontMetrics(uiFont(11)).horizontalAdvance(name) + 5, r.y, 12, r.h),
                   Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("•"));
    }
    int tagRight = c.opacityRight;
    if (hasOpacity) {
        p.setFont(uiFont(9));
        p.setPen(InkTheme::faint());
        p.drawText(QRect(tagRight - 32, r.y, 32, r.h), Qt::AlignVCenter | Qt::AlignRight,
                   QString::number(int(layer->opacity() * 100)) + QStringLiteral("%"));
        tagRight -= 34;
    }
    if (!blendTag.isEmpty()) {
        p.setFont(uiFont(8, true));
        p.setPen(InkTheme::dim());
        p.drawText(QRect(tagRight - 24, r.y, 24, r.h), Qt::AlignVCenter | Qt::AlignRight, blendTag);
        tagRight -= 26;
    }
    if (animatedTag) {
        // The keyed-range tag: a small filled diamond, the same glyph the lane
        // draws, so the gutter and the lane speak one language.
        const int cx = tagRight - 9, cy = r.y + r.h / 2;
        QPainterPath dm = diamondAt(cx, cy, 4, false);
        p.setPen(Qt::NoPen);
        p.setBrush(InkTheme::keyFill());
        p.drawPath(dm);
    }
    if (!camTag.isEmpty()) {
        p.setFont(uiFont(8, true));
        p.setPen(InkTheme::dim());
        p.drawText(QRect(tagRight - 50, r.y, 50, r.h), Qt::AlignVCenter | Qt::AlignRight, camTag);
        tagRight -= 52;
    }
    const Layer *chromeLayer = layer ? static_cast<const Layer*>(layer) : (cam ? static_cast<const Layer*>(cam) : nullptr);
    const bool vis = chromeLayer ? chromeLayer->visible() : true;
    const QColor eyeCol = !vis ? InkTheme::accentMuted()
                              : (hovered ? InkTheme::text() : InkTheme::faint());
    const QColor lockCol = locked ? InkTheme::warn()
                                 : (hovered ? InkTheme::text() : InkTheme::faint());
    drawPix(p, cachedIcon(vis ? IconId::Eye : IconId::EyeOff, eyeCol, 14),
            QRect(c.eye, r.y + (r.h - kHit) / 2, kHit, kHit));
    drawPix(p, cachedIcon(locked ? IconId::Lock : IconId::Unlock, lockCol, 14),
            QRect(c.lock, r.y + (r.h - kHit) / 2, kHit, kHit));

    // lane
    p.setPen(InkTheme::hairline());
    p.drawLine(0, r.y + r.h - 1, width(), r.y + r.h - 1);
    p.fillRect(gw, r.y, width() - gw, r.h, InkTheme::lane());

    p.save();
    p.setClipRect(gw, r.y, width() - gw, r.h);
    if (cam) {
        paintCameraLane(p, r);
        if (cam->animated()) paintKeyLane(p, r, *cam);
    } else {
        const auto cells = doc_->timeline().cells(sid);
        if (cells.empty()) paintEmptyLane(p, r);
        else paintCells(p, r, hue);
        // Transform keys ride ON the layer's own lane (the adjustment layer is
        // gone): diamonds + segment shapes over the paper cells.
        if (layer && layer->animated()) paintKeyLane(p, r, *layer);
    }
    // out-of-range shading: frames outside the playback range recede
    if (out_ >= in_) {
        const int xIn = std::max(gw, xAtFrame(in_));
        const int xOut = xAtFrame(out_) + int(pxPerFrame_);
        QColor shade = InkTheme::outOfRange();
        shade.setAlpha(115);
        if (xIn > gw) p.fillRect(gw, r.y, xIn - gw, r.h, shade);
        if (xOut < width()) p.fillRect(xOut, r.y, width() - xOut, r.h, shade);
    }
    p.restore();
}

// --- keyframe rows -----------------------------------------------------------

// The keyframe glyph. Filled for a key, hollow for the summary of several; a
// Hold key gets square shoulders, so interpolation mode survives a monochrome
// screenshot and a colour-blind reader alike.
QPainterPath TimelineWidget::diamondAt(int cx, int cy, int radius, bool hold) const {
    QPainterPath d;
    if (hold) {
        d.moveTo(cx, cy - radius);
        d.lineTo(cx + radius, cy - radius * 0.35);
        d.lineTo(cx + radius, cy + radius * 0.35);
        d.lineTo(cx, cy + radius);
        d.lineTo(cx - radius, cy + radius * 0.35);
        d.lineTo(cx - radius, cy - radius * 0.35);
        d.closeSubpath();
        return d;
    }
    d.moveTo(cx, cy - radius);
    d.lineTo(cx + radius, cy);
    d.lineTo(cx, cy + radius);
    d.lineTo(cx - radius, cy);
    d.closeSubpath();
    return d;
}

// The keyframe diamonds that ride on an adjustment layer's BAR, plus the
// segment shape between them. There are no parameter sub-rows any more, so
// this is the summary view: one diamond per keyed FRAME (a frame where ANY
// parameter has a key), selected when the key selection holds any key there.
void TimelineWidget::paintKeyLane(QPainter &p, const RowInfo &r, const PaintLayer &layer) {
    paintKeyLaneParams(p, r, layer.params());
}

void TimelineWidget::paintKeyLane(QPainter &p, const RowInfo &r, const CameraLayer &cam) {
    paintKeyLaneParams(p, r, cam.params());
}

void TimelineWidget::paintKeyLaneParams(QPainter &p, const RowInfo &r, const ParamBlock &params) {
    const int gw = GW();
    const int cy = r.y + r.h / 2;
    std::vector<int> frames;
    for (ParamId id : params.animatedParams())
        for (int f : params.keys(id)) frames.push_back(f);
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    p.save();
    p.setClipRect(gw, r.y, width() - gw, r.h);

    // Segment shapes first, behind the keys: a straight rule for Linear, a
    // shallow S for Bezier. This is what makes the lane readable as motion
    // rather than as a row of unrelated dots.
    for (size_t i = 0; i + 1 < frames.size(); ++i) {
        const int f0 = frames[i], f1 = frames[i + 1];
        if (f1 <= f0) continue;
        const int x0 = xAtFrame(f0) + int(pxPerFrame_ / 2);
        const int x1 = xAtFrame(f1) + int(pxPerFrame_ / 2);
        if (x1 < gw || x0 > width()) continue;
        // The mode shown is the dominant one across the frame's parameters.
        Interp mode = Interp::Linear;
        bool anyBezier = false, anyHold = false;
        for (ParamId id : params.animatedParams()) {
            const ScalarChannel *ch = params.channel(id);
            if (!ch) continue;
            const Interp m = ch->segmentInterp(f0);
            if (m == Interp::Bezier) anyBezier = true;
            if (m == Interp::Hold) anyHold = true;
        }
        if (anyHold) mode = Interp::Hold;
        else if (anyBezier) mode = Interp::Bezier;
        QColor c = (mode == Interp::Bezier) ? InkTheme::segBezier() : InkTheme::segLinear();
        if (mode == Interp::Hold) c = InkTheme::keyHold();
        c.setAlpha(120);
        p.setPen(QPen(c, 1.4, mode == Interp::Hold ? Qt::DashLine : Qt::SolidLine));
        p.setBrush(Qt::NoBrush);
        QPainterPath seg;
        seg.moveTo(x0, cy);
        if (mode == Interp::Bezier && x1 - x0 > 4) {
            const double amp = r.h * 0.16;
            seg.cubicTo(x0 + (x1 - x0) * 0.35, cy + amp, x0 + (x1 - x0) * 0.65, cy - amp, x1, cy);
        } else {
            seg.lineTo(x1, cy);
        }
        p.drawPath(seg);
    }

    // The keys themselves.
    for (int f : frames) {
        const int cx = xAtFrame(f) + int(pxPerFrame_ / 2);
        if (cx < gw - kKeyR || cx > width() + kKeyR) continue;
        bool hold = true;
        for (ParamId id : params.keysAt(f)) {
            const ScalarChannel *ch = params.channel(id);
            const ScalarKey *k = ch ? ch->keyAt(f) : nullptr;
            if (k && k->interp != Interp::Hold) hold = false;
        }
        bool selected = false;
        for (const TimelineKey &tk : keySel_)
            if (tk.adjId == r.trackId && tk.frame == f) { selected = true; break; }
        const bool hot = hoverKeyFrame_ == f && hoverTrack_ == r.trackId;
        p.setPen(Qt::NoPen);
        p.setBrush(selected ? InkTheme::keySelected()
                            : (hold ? InkTheme::keyHold() : InkTheme::keyFill()));
        if (selected) p.setPen(QPen(InkTheme::keyRing(), 1.0));
        else if (hot) p.setPen(QPen(InkTheme::keySelected(), 1.0));
        p.drawPath(diamondAt(cx, cy, kKeyR, hold));
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::NoBrush);
    }
    p.restore();
}

void TimelineWidget::paintCells(QPainter &p, const RowInfo &r, int hueIdx) {
    const std::string sid = r.trackId.toStdString();
    const auto cells = doc_->timeline().cells(sid);
    if (cells.empty()) return;
    const TimelineRow *row = doc_->timeline().row(sid);
    const int top = r.y + InkTheme::rowPad();
    const int hgt = r.h - 2 * InkTheme::rowPad();
    auto cellX0 = [&](const CellSpan &c) {
        return std::clamp(xAtFrame(c.start), GW(), width());
    };
    auto cellX1 = [&](const CellSpan &c) {
        return std::clamp(xAtFrame(c.end) + int(pxPerFrame_), GW(), width());
    };

    // Cells are independent: only CONTIGUOUS cells share one paper bar (so no
    // seam appears between neighbours), while a gap stays visibly empty.
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (runs.empty() || cells[i].start != cells[runs.back().second].end + 1)
            runs.push_back({i, i});
        else
            runs.back().second = i;
    }

    for (const auto &run : runs) {
        const int bx0 = cellX0(cells[run.first]);
        const int bx1 = cellX1(cells[run.second]);
        if (bx1 - bx0 < 1) continue;
        QRectF bar(bx0, top, std::max(2, bx1 - bx0), hgt);
        QPainterPath barPath;
        barPath.addRoundedRect(bar, InkTheme::radiusCell(), InkTheme::radiusCell());
        p.fillPath(barPath, InkTheme::paper());

        // per-cell tone (blank sheet vs drawn sheet), clipped to the bar
        p.save();
        p.setClipPath(barPath);
        for (size_t i = run.first; i <= run.second; ++i) {
            if (cellHasContent(r.trackId, cells[i].start)) continue;
            const int cx0 = cellX0(cells[i]), cx1 = cellX1(cells[i]);
            if (cx1 <= cx0) continue;
            p.fillRect(QRect(cx0, top, cx1 - cx0, hgt), InkTheme::paperBlank());
        }
        // layer hue rule along the top edge
        p.fillRect(QRectF(bx0, top, std::max(2, bx1 - bx0), 2), InkTheme::layerHueBright(hueIdx));
        p.restore();
        p.setPen(QPen(InkTheme::paperEdge(), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(barPath);

        for (size_t i = run.first; i <= run.second; ++i) {
            const CellSpan &cell = cells[i];
            const int cx0 = cellX0(cell), cx1 = cellX1(cell);
            const int cellW = std::max(1, cx1 - cx0);
            const bool blank = !cellHasContent(r.trackId, cell.start);
            QRect cellRect(cx0, top, cellW, hgt);

            // divider between neighbouring cells: they share the pixel column
            if (i > run.first) {
                p.setPen(QPen(InkTheme::paperEdge(), 1));
                p.drawLine(cx0, top, cx0, top + hgt);
            }
            const bool showNum = (cellW >= 16);
            if (showNum) {
                p.setFont(uiFont(9, true));
                p.setPen(InkTheme::ink());
                p.drawText(QRect(cx0 + 3, top + 1, cellW - 5, 11),
                           Qt::AlignVCenter | Qt::AlignLeft, QString::number(cell.index + 1));
            }
            const int thumbX = cx0 + (showNum ? 15 : 4);
            if (row && row->showThumbs && !blank && hgt >= 14 && cx1 - thumbX >= 24) {
                QPixmap pm = thumbFor(r.trackId, cell.start);
                if (!pm.isNull()) {
                    const int dh = hgt - 7;
                    const int dw = std::max(6, pm.width() * dh / std::max(1, pm.height()));
                    const int step = std::max(dw + 3, int(6 * pxPerFrame_));
                    p.save();
                    p.setClipRect(QRect(thumbX, top + 2, cx1 - thumbX - 2, hgt - 4));
                    for (int x = thumbX; x + dw <= cx1 - 2; x += step)
                        p.drawPixmap(QRect(x, top + 3 + (hgt - 6 - dh) / 2, dw, dh), pm);
                    p.restore();
                }
            }

            const bool sel = selection_.contains({r.trackId, cell.start});
            const bool hov = (hoverTrack_ == r.trackId && hoverFrame_ >= cell.start &&
                              hoverFrame_ <= cell.end);
            if (sel) {
                QColor tint = InkTheme::accent();
                tint.setAlpha(60);
                p.fillRect(cellRect, tint);
                p.setPen(QPen(InkTheme::accent(), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRect(QRectF(cellRect).adjusted(0.5, 0.5, -0.5, -0.5));
            } else if (hov) {
                p.setPen(QPen(QColor(232, 232, 234, 90), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(QRectF(cellRect).adjusted(0.5, 0.5, -0.5, -0.5));
            }
            // resize grip: a brighter cap on the edge under the pointer
            const QString cellKey = r.trackId + QLatin1Char(':') + QString::number(cell.start);
            if (hoverCellKey_ == cellKey && hoverEdge_ != 0 && edgeTolFor(cellW) > 0) {
                const int gx = hoverEdge_ > 0 ? cx1 - 1 : cx0;
                QColor grip = InkTheme::accent();
                grip.setAlpha(220);
                p.fillRect(QRect(gx, top + 1, 2, hgt - 2), grip);
            }
        }
    }
}

void TimelineWidget::paintEmptyLane(QPainter &p, const RowInfo &r) {
    // No cell here: while the pointer is over this lane, preview exactly what
    // a new cell would be — ONE frame wide.
    if (hoverTrack_ != r.trackId || hoverFrame_ < 0) return;
    if (hoverFrame_ > std::max(in_, out_)) return;
    const int top = r.y + InkTheme::rowPad();
    const int hgt = r.h - 2 * InkTheme::rowPad();
    const int gx0 = xAtFrame(hoverFrame_);
    const int gx1 = xAtFrame(hoverFrame_) + std::max(7, int(pxPerFrame_));
    QColor fill = InkTheme::paper();
    fill.setAlpha(70);
    p.fillRect(QRect(gx0, top, gx1 - gx0, hgt), fill);
    QPen pen(InkTheme::paperEdge(), 1, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRect(gx0, top, gx1 - gx0 - 1, hgt - 1));
    if (hoverTrack_ == r.trackId && hoverFrame_ >= 0) {
        p.setFont(uiFont(9, true));
        p.setPen(InkTheme::inkFaint());
        if (width() - gx1 > 74)
            p.drawText(QRect(gx1 + 5, top, 70, hgt), Qt::AlignVCenter | Qt::AlignLeft,
                       QStringLiteral("+ CELL"));
    }
}

void TimelineWidget::paintCameraLane(QPainter &p, const RowInfo &r) {
    // Cameras own no cells, so there is never a +CELL preview here (that was
    // the "regular layer" read). A faint centre rule keeps the lane from
    // looking broken, and the hovered frame gets a ghost diamond: exactly
    // what a double-click would key.
    const int cy = r.y + r.h / 2;
    p.setPen(InkTheme::hairline());
    p.drawLine(GW() + 4, cy, width() - 4, cy);
    if (hoverTrack_ != r.trackId || hoverFrame_ < 0) return;
    const int cx = xAtFrame(hoverFrame_) + int(pxPerFrame_ / 2);
    if (cx < GW() + kKeyR || cx > width() - kKeyR) return;
    p.setPen(QPen(InkTheme::keyFill(), 1.2, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawPath(diamondAt(cx, cy, kKeyR, false));
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::NoBrush);
}

void TimelineWidget::paintPlayhead(QPainter &p) {
    const int cur = doc_->timeline().currentFrame();
    const int px = xAtFrame(cur) + int(pxPerFrame_ / 2);
    p.setPen(QPen(InkTheme::playhead(), 1));
    p.drawLine(px, 0, px, height());
}

void TimelineWidget::paintRubberBand(QPainter &p) {
    if (!rubberBanding_ || rubberRect_.isEmpty()) return;
    QColor fill = InkTheme::accent();
    fill.setAlpha(34);
    p.fillRect(rubberRect_.normalized(), fill);
    p.setPen(QPen(InkTheme::accent(), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rubberRect_.normalized());
}

// -------------------------------------------------------------- interaction

void TimelineWidget::setFrameFromX(int x) {
    int f = frameAtX(x);
    if (f == doc_->timeline().currentFrame()) return;
    doc_->setCurrentFrame(f);
    ensureFrameVisible(f);
    emit frameChanged(f);
}

void TimelineWidget::updateHover(const QPoint &pos) {
    const QString track = hoverTrack_;
    const int frame = hoverFrame_;
    const int btn = hoverBtn_;
    const int edge = hoverEdge_;
    const int range = hoverRange_;
    const bool check = hoverCheck_;
    const QString cellKey = hoverCellKey_;
    const int keyFrame = hoverKeyFrame_;
    const int gutterPart = hoverGutterPart_;
    hoverTrack_.clear();
    hoverFrame_ = -1;
    hoverBtn_ = -1;
    hoverEdge_ = 0;
    hoverRange_ = 0;
    hoverCheck_ = false;
    hoverKeyFrame_ = -1;
    hoverGutterPart_ = 0;
    hoverCellKey_.clear();
    if (pos.y() < RH()) {
        if (pos.x() < GW()) {
            const auto btns = headerButtons();
            for (size_t i = 0; i < btns.size(); ++i)
                if (btns[i].rect.contains(pos) && btns[i].enabled) hoverBtn_ = (int)i;
        } else {
            hoverRange_ = rangeHitAt(pos.x());
        }
    } else if (pos.x() < GW()) {
        // gutter rows: the checkbox (layer select) and the eye/lock get hover
        // feedback, so it is obvious the box is clickable
        auto rows = layoutRows();
        int ri = rowIndexAt(rows, pos.y());
        if (ri >= 0) {
            const RowInfo &row = rows[ri];
            hoverCheck_ = inCheckBox(checkBoxRect(row), pos.x());
            if (!row.isFolderHeader) hoverTrack_ = row.trackId;
        }
    } else if (pos.x() >= GW()) {
        auto rows = layoutRows();
        int ri = rowIndexAt(rows, pos.y());
        if (ri >= 0 && !rows[ri].isFolderHeader) {
            hoverTrack_ = rows[ri].trackId;
            hoverFrame_ = frameAtX(pos.x());
            {
                // The keyed range's edges retime; a diamond on the lane is
                // grabbable, and hovering it lights up so the keys read as an
                // animated object.
                int kf = -1;
                if (keyHitTest(rows[ri], pos, &kf) == 1) hoverKeyFrame_ = kf;
                else {
                    const int hit = keyedRangeHit(rows[ri], pos.x());
                    if (hit == -1) hoverEdge_ = -1;
                    else if (hit == 1) hoverEdge_ = 1;
                }
            }
            const auto cells = doc_->timeline().cells(hoverTrack_.toStdString());
            for (const auto &cell : cells) {
                if (hoverFrame_ < cell.start || hoverFrame_ > cell.end) continue;
                const int cx0 = xAtFrame(cell.start);
                const int cx1 = xAtFrame(cell.end) + int(pxPerFrame_);
                const int tol = edgeTolFor(cx1 - cx0);
                if (tol > 0) {
                    if (std::abs(pos.x() - cx1) <= tol) hoverEdge_ = 1;
                    else if (std::abs(pos.x() - cx0) <= tol && cell.start > 0) hoverEdge_ = -1;
                }
                hoverCellKey_ = hoverTrack_ + QLatin1Char(':') + QString::number(cell.start);
                break;
            }
        }
    }
    if (hoverTrack_ != track || hoverFrame_ != frame || hoverBtn_ != btn ||
        hoverEdge_ != edge || hoverRange_ != range || hoverCheck_ != check ||
        hoverCellKey_ != cellKey || hoverKeyFrame_ != keyFrame ||
        hoverGutterPart_ != gutterPart) {
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (hoverGutterPart_ == 1) shape = Qt::PointingHandCursor;
        else if (hoverKeyFrame_ >= 0) shape = Qt::SizeHorCursor;
        else if (hoverRange_ != 0) shape = Qt::SizeHorCursor;
        else if (hoverEdge_ != 0) shape = Qt::SizeHorCursor;
        else if (hoverCheck_) shape = Qt::PointingHandCursor;
        else if (!hoverTrack_.isEmpty()) shape = Qt::OpenHandCursor;
        else if (hoverBtn_ >= 0) shape = Qt::PointingHandCursor;
        setCursor(shape);
        applyTooltip(pos);
        update();
    }
}

void TimelineWidget::applyTooltip(const QPoint &pos) {
    const int gw = GW();
    QString tip;
    if (pos.y() < RH()) {
        if (pos.x() < gw) {
            for (const auto &b : headerButtons())
                if (b.rect.contains(pos)) tip = b.tip;
        } else if (hoverRange_ == 1) {
            tip = QStringLiteral("Range in: frame %1 — drag to change (I sets it at the playhead)").arg(in_);
        } else if (hoverRange_ == 2) {
            tip = QStringLiteral("Range out: frame %1 — drag to change (O sets it at the playhead)").arg(out_);
        } else {
            tip = QStringLiteral("Playback range %1–%2 · drag the tabs to change it · right-click for range actions")
                      .arg(in_).arg(out_);
        }
    } else {
        auto rows = layoutRows();
        int ri = rowIndexAt(rows, pos.y());
        if (ri >= 0) {
            const RowInfo &r = rows[ri];
            const Columns c = columns();
            if (pos.x() < gw) {
                if (inCheckBox(checkBoxRect(r), pos.x()))
                    tip = r.isFolderHeader
                              ? QStringLiteral("Select every layer in this folder (for grouping)")
                              : QStringLiteral("Select this layer \u2014 tick two or more, then "
                                               "\u201cGroup selected layers\u201d");
                else if (pos.x() >= c.eye && pos.x() < c.eye + kHit)
                    tip = r.isFolderHeader ? QStringLiteral("Show/hide folder")
                                           : QStringLiteral("Show/hide layer");
                else if (pos.x() >= c.lock && pos.x() < c.lock + kHit)
                    tip = QStringLiteral("Lock (blocks painting and edits)");
                else if (pos.x() >= c.name && pos.x() < c.opacityRight) {
                    if (CameraLayer *tipCam = doc_->findCamera(r.trackId.toStdString()))
                        tip = QStringLiteral("Camera drives %1 \u2014 double-click to rename, "
                                             "right-click for camera options")
                                  .arg(cameraTargetName(tipCam));
                    else
                        tip = r.label + QStringLiteral(" \u2014 double-click to rename, right-click for layer options");
                }
            } else if (!r.isFolderHeader) {
                if (doc_->findCamera(r.trackId.toStdString())) {
                    // Cameras own no cells: never the cell tooltip, never the
                    // "add a cell" hint. Diamonds are the whole story here.
                    if (hoverKeyFrame_ >= 0)
                        tip = QStringLiteral("Camera keyframe at frame %1 \u2014 right-click for "
                                             "easing, drag to retime, Del removes it")
                                  .arg(hoverKeyFrame_);
                    else
                        tip = QStringLiteral("Camera lane \u2014 double-click keys at the playhead, "
                                             "right-click for camera options");
                } else {
                const auto cells = doc_->timeline().cells(r.trackId.toStdString());
                for (const auto &cell : cells) {
                    if (hoverFrame_ < cell.start || hoverFrame_ > cell.end) continue;
                    PaintLayer *l = doc_->findTrack(r.trackId.toStdString());
                    const Cell *cc = l ? l->cellAt(cell.start) : nullptr;
                    const bool blank = !(cc && cc->hasContent());
                    const int len = cell.end - cell.start + 1;
                    tip = QStringLiteral("Cell %1 · frames %2–%3 (%4 frame%5) · %6")
                              .arg(cell.index + 1).arg(cell.start).arg(cell.end)
                              .arg(len).arg(len == 1 ? QString() : QStringLiteral("s"))
                              .arg(blank ? QStringLiteral("blank") : QStringLiteral("drawn"));
                    if (hoverEdge_ > 0) tip += QStringLiteral(" — drag the right edge to change exposure");
                    else if (hoverEdge_ < 0) tip += QStringLiteral(" — drag the left edge to trim");
                    else tip += QStringLiteral(" — drag to move (Alt drops over another cell), right-drag to multi-select, right-click for cell options");
                    break;
                }
                if (tip.isEmpty())
                    tip = QStringLiteral("Empty track — double-click or press N to add a cell here");
                } // else: paint lane (camera lanes return above)
            }
        }
    }
    setToolTip(tip); // Qt owns the reveal delay; we only swap the text
}

void TimelineWidget::showContextMenu(const QPoint &pos) {
    const int gw = GW();
    const int x = pos.x();
    const int y = pos.y();
    auto rows = layoutRows();
    // Right-click is contextual: the gutter is the LAYER, the lane is the
    // CELL, a folder header is the FOLDER, the ruler is the range.
    if (y < RH()) {
        if (x >= gw) emit rulerMenuRequested(mapToGlobal(pos));
        return;
    }
    int ri = rowIndexAt(rows, y);
    if (ri < 0) return;
    const RowInfo &r = rows[ri];
    // --- paint row: the keyframes live ON the layer. Right-click a diamond
    //     gets the keyframe menu; everything else — INCLUDING the keyed range,
    //     which used to hijack the click and open a second transform panel —
    //     falls through to the ordinary cell/layer menu.
    if (const ParamBlock *pb = paramsOf(r.trackId); pb && x >= gw && !pb->empty()) {
        int kf = -1;
        if (keyHitTest(r, pos, &kf) == 1) {
            keySel_.clear();
            for (ParamId id : pb->keysAt(kf))
                keySel_.insert(TimelineKey{r.trackId, (int)id, kf});
            update();
            emit keyFrameMenuRequested(mapToGlobal(pos), r.trackId, kf);
            return;
        }
    }
    if (r.isFolderHeader) {
        emit trackMenuRequested(mapToGlobal(pos), QStringLiteral("folder:") + r.folderId);
    } else if (doc_->findCamera(r.trackId.toStdString())) {
        // Cameras own no cells: gutter AND lane both open the camera options
        // (layer options, not cell options).
        emit trackMenuRequested(mapToGlobal(pos), r.trackId);
    } else if (x < gw) {
        // Right-click acts on what is under the pointer: make the clicked
        // layer the selection the menu's group ops will see.
        if (!layerSel_.contains(r.trackId)) {
            layerSel_ = {r.trackId};
            folderSel_.clear();
            anchorId_ = r.trackId;
            if (PaintLayer *l = doc_->findTrack(r.trackId.toStdString())) doc_->setCurrentLayer(l);
            emit layerSelectionChanged();
        }
        emit trackMenuRequested(mapToGlobal(pos), r.trackId);
    } else {
        // Same for the cell under the pointer, so Copy / Mirror / Delete
        // act on it instead of on a stale selection.
        const int f = frameAtX(x);
        for (const auto &cell : doc_->timeline().cells(r.trackId.toStdString())) {
            if (f < cell.start || f > cell.end) continue;
            const TimelineSel s{r.trackId, cell.start};
            if (!selection_.contains(s)) {
                selection_ = {s};
                emit selectionChanged();
            }
            break;
        }
        emit laneMenuRequested(mapToGlobal(pos), r.trackId, f);
    }
    update();
}

void TimelineWidget::mousePressEvent(QMouseEvent *e) {
    setFocus(Qt::MouseFocusReason);
    pressPos_ = e->pos();
    scrubbing_ = movingCell_ = rubberBanding_ = resizingCell_ = false;
    draggingBar_ = false;
    dragRange_ = 0;
    resizeSide_ = 0;
    resizeCellStart_ = -1;
    const int gw = GW();
    const int x = e->pos().x();
    const int y = e->pos().y();
    auto rows = layoutRows();

    if (e->button() == Qt::RightButton) {
        // Defer the menu until release: a right-drag rubber-bands (multi-
        // select from ANY cell, occupied or empty), a plain right-click
        // opens the context menu. See mouseMove/ReleaseEvent.
        rightArmed_ = true;
        rightBandActive_ = false;
        rightPressPos_ = e->pos();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    rightArmed_ = false;
    rightBandActive_ = false;

    // --- sticky header: gutter buttons, then ruler ---
    if (y < RH()) {
        if (x < gw) {
            const auto btns = headerButtons();
            for (size_t i = 0; i < btns.size(); ++i) {
                if (!btns[i].rect.contains(e->pos()) || !btns[i].enabled) continue;
                switch (btns[i].id) {
                case HeadBtn::AddLayer: emit addLayerRequested(); break;
                case HeadBtn::AddGroup: emit addGroupRequested(); break;
                case HeadBtn::GroupSelected: emit groupSelectionRequested(); break;
                case HeadBtn::Ungroup: emit ungroupRequested(); break;
                }
                return;
            }
            return;
        }
        const int hit = rangeHitAt(x);
        if (hit != 0) {
            dragRange_ = hit;
            setCursor(Qt::SizeHorCursor);
            update();
            return;
        }
        scrubbing_ = true;
        selection_.clear();
        emit selectionChanged();
        setFrameFromX(x);
        update();
        return;
    }

    int ri = rowIndexAt(rows, y);
    if (ri < 0) {
        setFrameFromX(x);
        return;
    }
    const RowInfo r = rows[ri];

    // --- folder row ---
    if (r.isFolderHeader) {
        if (x < gw) {
            const Columns c = columns();
            if (inCheckBox(checkBoxRect(r), x)) {
                const TimelineFolder *tf = doc_->timeline().folder(r.folderId.toStdString());
                bool allSel = tf && !tf->tracks.empty();
                if (tf)
                    for (const auto &t : tf->tracks)
                        if (!layerSel_.contains(QString::fromStdString(t.layerId))) { allSel = false; break; }
                setFolderSelection(r.folderId, !allSel);
                return;
            }
            if (x >= c.expander - 3 && x < c.expander + 14) {
                if (auto *f = doc_->timeline().folder(r.folderId.toStdString())) {
                    doc_->timeline().setFolderExpanded(f->folderId, !f->expanded);
                    update();
                }
                return;
            }
            GroupLayer *g = nullptr;
            std::vector<GroupLayer*> folders;
            doc_->image().collectFolders(folders);
            for (auto *f : folders)
                if (f->stableId() == r.folderId.toStdString()) { g = f; break; }
            if (g && x >= c.eye && x < c.eye + kHit) {
                auto edit = doc_->editGuard();
                g->setVisible(!g->visible());
                doc_->frameCache().invalidateAll();
                emit keyChanged();
                update();
                return;
            }
            if (g && x >= c.lock && x < c.lock + kHit) {
                auto edit = doc_->editGuard();
                g->setLocked(!g->locked());
                emit keyChanged();
                update();
                return;
            }
            folderSel_ = r.folderId;
            layerSel_.clear();
            anchorId_.clear();
            if (g) doc_->setCurrentFolder(g);
            emit layerSelectionChanged();
            update();
            return;
        }
        // folder lane: move the playhead, keep the folder selected
        folderSel_ = r.folderId;
        setFrameFromX(x);
        update();
        return;
    }

    // --- track row ---
    PaintLayer *layer = doc_->findTrack(r.trackId.toStdString());
    CameraLayer *camRow = doc_->findCamera(r.trackId.toStdString());
    Layer *chrome = layer ? static_cast<Layer*>(layer)
                          : (camRow ? static_cast<Layer*>(camRow) : nullptr);
    const Columns c = columns();
    if (x < gw) {
        // the checkbox toggles selection with no modifier: pick 2+ layers here,
        // then use "Group selected layers" (context menu or gutter header).
        if (inCheckBox(checkBoxRect(r), x)) {
            toggleLayerSelection(r.trackId);
            return;
        }
        if (chrome && x >= c.eye && x < c.eye + kHit) {
            auto edit = doc_->editGuard();
            chrome->setVisible(!chrome->visible());
            doc_->frameCache().invalidateAll();
            emit keyChanged();
            update();
            return;
        }
        if (chrome && x >= c.lock && x < c.lock + kHit) {
            auto edit = doc_->editGuard();
            chrome->setLocked(!chrome->locked());
            emit keyChanged();
            update();
            return;
        }
        // layer selection (drives grouping)
        folderSel_.clear();
        if (e->modifiers() & Qt::ControlModifier) {
            if (layerSel_.contains(r.trackId)) layerSel_.remove(r.trackId);
            else layerSel_.insert(r.trackId);
        } else if ((e->modifiers() & Qt::ShiftModifier) && !anchorId_.isEmpty()) {
            std::vector<QString> ids;
            for (const auto &row : rows)
                if (!row.isFolderHeader) ids.push_back(row.trackId);
            auto posOf = [&](const QString &id) {
                for (size_t i = 0; i < ids.size(); ++i)
                    if (ids[i] == id) return (int)i;
                return -1;
            };
            const int a = posOf(anchorId_);
            const int b = posOf(r.trackId);
            if (a >= 0 && b >= 0)
                for (int i = std::min(a, b); i <= std::max(a, b); ++i)
                    layerSel_.insert(ids[(size_t)i]);
            else
                layerSel_.insert(r.trackId);
        } else {
            layerSel_ = {r.trackId};
        }
        anchorId_ = r.trackId;
        if (layer) doc_->setCurrentLayer(layer);
        emit layerSelectionChanged();
        update();
        return;
    }

    // --- keyframes ride ON the paint row's lane: grab a diamond first, then
    //     the keyed range (retime drag with the keys travelling) ---
    //     Cameras share the same diamonds (their params live on the camera).
    if (x >= gw && !r.isFolderHeader) {
        const ParamBlock *pbLane = paramsOf(r.trackId);
        if (pbLane && !pbLane->empty()) {
            int hitFrame = -1;
            if (keyHitTest(r, e->pos(), &hitFrame) == 1) {
                // A diamond is a keyed FRAME: select every parameter keyed
                // there, which is what the easing graph edits.
                const auto ids = pbLane->keysAt(hitFrame);
                bool ctrl = e->modifiers() & Qt::ControlModifier;
                if (!ctrl) keySel_.clear();
                for (ParamId id : ids) {
                    const TimelineKey tk{r.trackId, (int)id, hitFrame};
                    if (ctrl && keySel_.contains(tk)) keySel_.remove(tk);
                    else keySel_.insert(tk);
                }
                if (!keySel_.isEmpty()) {
                    draggingKeys_ = true;
                    keyDragAdj_ = r.trackId;
                    keyDragOrigin_ = hitFrame;
                    keyDragBase_.clear();
                    for (const TimelineKey &k : keySel_)
                        if (k.adjId == r.trackId) keyDragBase_.push_back({k.param, k.frame});
                    keyMoved_ = false;
                    if (CameraLayer *camDrag = doc_->findCamera(r.trackId.toStdString()))
                        doc_->beginCameraRecord("Move camera keyframe", camDrag);
                    else if (PaintLayer *plDrag = doc_->findTrack(r.trackId.toStdString()))
                        doc_->beginParamRecord("Move keyframe", doc_->ensureAdjustmentFor(plDrag));
                }
                emit keyChanged();
                emit selectionChanged();
                update();
                return;
            }
            // --- the keyed range: drag edges/body to retime keys ---
            const int hit = keyedRangeHit(r, x);
            if (hit != 0) {
                draggingBar_ = true;
                barDragSide_ = (hit == 2) ? 0 : hit;
                barDragTrack_ = r.trackId;
                const auto keys = keyFramesOf(r.trackId, -1);
                const int firstKey = keys.empty() ? 0 : keys.front();
                const int lastKey = keys.empty() ? 0 : keys.back();
                barDragOrigStart_ = firstKey;
                barDragOrigEnd_ = lastKey;
                // Where inside the range the pointer landed, in frames: the
                // body drag keeps this offset so keys never snap to the cursor.
                barDragGrabFrame_ = std::max(0, frameAtX(x) - barDragOrigStart_);
                if (const ParamBlock *pbBase = paramsOf(r.trackId))
                    barDragBaseParams_ = *pbBase; // snapshot: idempotent dnd
                if (CameraLayer *camBar = doc_->findCamera(r.trackId.toStdString()))
                    doc_->beginCameraRecord("Move camera keyframes", camBar);
                barMoved_ = false;
                setCursor(hit == 2 ? Qt::ClosedHandCursor : Qt::SizeHorCursor);
                update();
                return;
            }
        }
    }

    // --- lane ---
    if (layer) doc_->setCurrentLayer(layer);
    if (camRow && x >= gw) {
        // Camera lanes own no cells: a lane click moves the playhead (below)
        // and makes this camera the working one, so K / prev / next / graph
        // act on the row just clicked without a detour through the gutter.
        folderSel_.clear();
        if (e->modifiers() & Qt::ControlModifier) toggleLayerSelection(r.trackId);
        else if (!layerSel_.contains(r.trackId) || layerSel_.size() != 1) {
            layerSel_ = {r.trackId};
            anchorId_ = r.trackId;
            emit layerSelectionChanged();
        }
    }
    const int f = frameAtX(x);
    const auto cells = doc_->timeline().cells(r.trackId.toStdString());
    int ci = -1;
    for (size_t i = 0; i < cells.size(); ++i)
        if (f >= cells[i].start && f <= cells[i].end) { ci = (int)i; break; }

    if (ci >= 0) {
        const CellSpan &cell = cells[(size_t)ci];
        const int cx0 = xAtFrame(cell.start);
        const int cx1 = xAtFrame(cell.end) + int(pxPerFrame_);
        const int tol = edgeTolFor(cx1 - cx0);

        // edge drag: resize this cell only (neighbours stay untouched)
        if (tol > 0 && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
            if (std::abs(x - cx1) <= tol) {
                resizingCell_ = true;
                resizeSide_ = 1;
                resizeCellStart_ = cell.start;
                dragTrack_ = r.trackId;
                selection_ = {TimelineSel{r.trackId, cell.start}};
                emit selectionChanged();
                update();
                return;
            }
            if (std::abs(x - cx0) <= tol && cell.start > 0) {
                resizingCell_ = true;
                resizeSide_ = -1;
                resizeCellStart_ = cell.start;
                dragTrack_ = r.trackId;
                selection_ = {TimelineSel{r.trackId, cell.start}};
                emit selectionChanged();
                update();
                return;
            }
        }

        if (e->modifiers() & Qt::ControlModifier) {
            TimelineSel s{r.trackId, cell.start};
            if (selection_.contains(s)) selection_.remove(s);
            else selection_.insert(s);
            emit selectionChanged();
            update();
            return;
        }
        if (e->modifiers() & Qt::ShiftModifier) {
            int a = doc_->currentFrame();
            for (const auto &c : cells)
                if (c.start >= std::min(a, cell.start) && c.start <= std::max(a, cell.start))
                    selection_.insert({r.trackId, c.start});
            setFrameFromX(x);
            emit selectionChanged();
            update();
            return;
        }
        selection_ = {TimelineSel{r.trackId, cell.start}};
        movingCell_ = true;
        dragTrack_ = r.trackId;
        dragFromKey_ = cell.start;
        dragOrigFrame_ = f;
        setCursor(Qt::ClosedHandCursor);
        setFrameFromX(x);
        emit selectionChanged();
        update();
        return;
    }

    // empty part of the lane: rubber band (multi-select) + playhead
    rubberBanding_ = true;
    rubberStart_ = e->pos();
    rubberRect_ = QRect(e->pos(), QSize());
    if (!(e->modifiers() & Qt::ShiftModifier)) selection_.clear();
    setFrameFromX(x);
    emit selectionChanged();
    update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *e) {
    updateHover(e->pos());
    // --- keyframe interactions (before anything lane-related) ---
    if (draggingBar_) {
        const int under = frameAtX(e->pos().x());
        const int last = doc_->timeline().frameCount() - 1;
        int s = barDragOrigStart_, en = barDragOrigEnd_;
        if (barDragSide_ == -1) {
            s = std::clamp(under, 0, en);
        } else if (barDragSide_ == 1) {
            en = std::clamp(under, s, last);
        } else {
            const int len = barDragOrigEnd_ - barDragOrigStart_;
            s = std::clamp(under - barDragGrabFrame_, -barDragOrigStart_,
                           std::max(0, last - barDragOrigEnd_));
            en = s + len;
        }
        const int keyDelta = s - barDragOrigStart_;
        if (CameraLayer *camBar = doc_->findCamera(barDragTrack_.toStdString())) {
            if (keyDelta != 0) {
                doc_->previewCameraBar(camBar, keyDelta, barDragBaseParams_);
                barMoved_ = true;
                emit keyChanged();
                update();
            }
        } else if (PaintLayer *pl = animatedLayerOf(barDragTrack_)) {
            if (keyDelta != 0) {
                doc_->previewAdjustmentBar(doc_->ensureAdjustmentFor(pl), s, en, keyDelta,
                                           barDragBaseParams_);
                barMoved_ = true;
                emit keyChanged();
                update();
            }
        }
        return;
    }
    if (draggingKeys_) {
        const int f = frameAtX(e->pos().x());
        const int delta = f - keyDragOrigin_;
        CameraLayer *camDrag = doc_->findCamera(keyDragAdj_.toStdString());
        PaintLayer *pl = camDrag ? nullptr : animatedLayerOf(keyDragAdj_);
        AdjustmentLayer *adj = pl ? doc_->ensureAdjustmentFor(pl) : nullptr;
        if (delta != 0 && !keyDragBase_.empty() && (camDrag || adj)) {
            // Move the whole selection by the same delta. Order matters: a retime
            // walks the keys in the direction it is travelling, so a key never
            // lands on one that has not moved out of the way yet.
            std::vector<QPair<int, int>> order = keyDragBase_;
            if (delta > 0)
                std::sort(order.begin(), order.end(),
                          [](const QPair<int, int> &a, const QPair<int, int> &b) {
                              return a.second > b.second;
                          });
            else
                std::sort(order.begin(), order.end(),
                          [](const QPair<int, int> &a, const QPair<int, int> &b) {
                              return a.second < b.second;
                          });
            for (auto &kv : order) {
                if (camDrag)
                    doc_->liveMoveCameraKey(camDrag, (ParamId)kv.first, kv.second,
                                            kv.second + delta);
                else
                    doc_->liveMoveKey(adj, (ParamId)kv.first, kv.second, kv.second + delta);
            }
            for (auto &kv : order) kv.second += delta;
            keyDragBase_ = order;
            keyDragOrigin_ = f;
            keyMoved_ = true;
            emit keyChanged();
            update();
        }
        return;
    }
    // Right-drag rubber band: starts from ANY lane position (even an occupied
    // cell), unlike left-drag which moves the cell it started on.
    if (rightArmed_ && (e->buttons() & Qt::RightButton)) {
        if (!rightBandActive_ &&
            (e->pos() - rightPressPos_).manhattanLength() >= 6) {
            rightBandActive_ = true;
            rubberBanding_ = true;
            rubberStart_ = rightPressPos_;
            rubberRect_ = QRect(rubberStart_, QSize());
            if (!(e->modifiers() & Qt::ShiftModifier)) selection_.clear();
            emit selectionChanged();
        }
        if (rightBandActive_) {
            // Fall through to the shared rubber-band selection update below.
        } else {
            return; // armed but below threshold: swallow, no menu yet
        }
    }
    if (dragRange_ != 0) {
        int f = frameAtX(e->pos().x());
        if (dragRange_ == 1) in_ = std::min(f, out_);
        else out_ = std::max(f, in_);
        emit rangeChangeRequested(in_, out_);
        update();
        return;
    }
    if (scrubbing_) {
        setFrameFromX(e->pos().x());
        update();
        return;
    }
    if (resizingCell_) {
        PaintLayer *l = doc_->findTrack(dragTrack_.toStdString());
        if (l && resizeCellStart_ >= 0) {
            const int f = frameAtX(e->pos().x());
            bool changed = false;
            if (resizeSide_ > 0) {
                changed = doc_->setCellEnd(l, resizeCellStart_, f);
            } else {
                changed = doc_->moveCellStart(l, resizeCellStart_, f);
                // follow the cell to wherever the start actually landed
                const int landed = doc_->cellStartAt(l, std::min(f, doc_->cellEnd(l, resizeCellStart_)));
                if (changed && landed >= 0) resizeCellStart_ = landed;
            }
            if (changed) {
                invalidateCells();
                emit keyChanged();
                update();
            }
        }
        return;
    }
    if (rubberBanding_) {
        rubberRect_ = QRect(rubberStart_, e->pos());
        QRect band = rubberRect_.normalized();
        selection_.clear();
        auto rows = layoutRows();
        for (auto &r : rows) {
            if (r.isFolderHeader) continue;
            QRect rowLane(GW(), r.y, width() - GW(), r.h);
            if (!band.intersects(rowLane)) continue;
            for (const auto &cell : doc_->timeline().cells(r.trackId.toStdString())) {
                int x = xAtFrame(cell.start) + int(pxPerFrame_ / 2);
                if (x >= band.left() && x <= band.right())
                    selection_.insert({r.trackId, cell.start});
            }
        }
        emit selectionChanged();
        update();
        return;
    }
    if (movingCell_) {
        int f = frameAtX(e->pos().x());
        int delta = f - dragOrigFrame_;
        int target = std::clamp(dragFromKey_ + delta, 0, doc_->timeline().frameCount() - 1);
        if (target != dragFromKey_) {
            if (PaintLayer *l = doc_->findTrack(dragTrack_.toStdString())) {
                // Alt forces the drop onto an occupied frame (replaces that cell)
                const bool force = e->modifiers() & Qt::AltModifier;
                if (l->moveDeviceContent(dragFromKey_, target, force)) {
                    doc_->touchLayer(l, target);
                    dragFromKey_ = target;
                    dragOrigFrame_ = f;
                    selection_ = {TimelineSel{dragTrack_, target}};
                    ensureFrameVisible(target);
                    invalidateCells();
                    setCursor(Qt::ClosedHandCursor);
                    emit keyChanged();
                    emit selectionChanged();
                    update();
                } else {
                    dragOrigFrame_ = f; // blocked: re-anchor to avoid jumps
                    setCursor(Qt::ForbiddenCursor);
                }
            }
        }
        return;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *e) {
    // --- keyed range retime: one undo step for the whole gesture ---
    if (draggingBar_) {
        draggingBar_ = false;
        if (CameraLayer *camBar = doc_->findCamera(barDragTrack_.toStdString())) {
            if (barMoved_) doc_->commitCameraBar(camBar, barDragBaseParams_);
            else doc_->abortCameraBar(camBar, barDragBaseParams_);
        } else if (PaintLayer *pl = animatedLayerOf(barDragTrack_)) {
            if (pl && barMoved_)
                doc_->commitAdjustmentBar(doc_->ensureAdjustmentFor(pl), barDragOrigStart_,
                                          barDragOrigEnd_, barDragBaseParams_);
        }
        barDragTrack_.clear();
        barMoved_ = false;
        emit keyChanged();
        updateHover(e->pos());
        return;
    }
    // --- keyframe interactions ---
    if (draggingKeys_) {
        draggingKeys_ = false;
        // One undo step for the whole retime; a click that never moved pushes
        // nothing.
        if (doc_->findCamera(keyDragAdj_.toStdString()))
            doc_->endCameraRecord();
        else
            doc_->endParamRecord();
        if (keyMoved_) {
            // The selection follows the keys to wherever they landed.
            QSet<TimelineKey> moved;
            for (auto &kv : keyDragBase_)
                moved.insert(TimelineKey{keyDragAdj_, kv.first, kv.second});
            keySel_ = moved;
            emit keyChanged();
        }
        keyDragBase_.clear();
        keyDragOrigin_ = -1;
        keyMoved_ = false;
        updateHover(e->pos());
        return;
    }
    if (e->button() == Qt::RightButton) {
        if (rightArmed_) {
            if (rightBandActive_ && rubberBanding_) {
                // Right-drag finished: keep the band selection, no menu.
                rubberBanding_ = false;
                rubberRect_ = QRect();
                rightArmed_ = false;
                rightBandActive_ = false;
                emit selectionChanged();
                update();
            } else {
                // Plain right-click: show the contextual menu.
                rightArmed_ = false;
                rightBandActive_ = false;
                showContextMenu(e->pos());
            }
            updateHover(e->pos());
            return;
        }
        return;
    }
    if (rubberBanding_) {
        rubberBanding_ = false;
        rubberRect_ = QRect();
        emit selectionChanged();
        update();
    }
    if (dragRange_ != 0) emit rangeChangeRequested(in_, out_);
    if (resizingCell_) emit keyChanged();
    scrubbing_ = false;
    movingCell_ = false;
    resizingCell_ = false;
    dragRange_ = 0;
    resizeSide_ = 0;
    resizeCellStart_ = -1;
    rightArmed_ = false;
    rightBandActive_ = false;
    // Re-evaluate cursor/tooltip for where the pointer actually ended up
    // (drags set ClosedHand/SizeHor cursors that would otherwise stick).
    updateHover(e->pos());
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int gw = GW();
    if (e->pos().y() < RH()) return;
    auto rows = layoutRows();
    int ri = rowIndexAt(rows, e->pos().y());
    if (ri < 0) return;
    const RowInfo &r = rows[ri];
    if (e->pos().x() < gw) {
        emit renameRequested(r.isFolderHeader ? QStringLiteral("folder:") + r.folderId : r.trackId);
        return;
    }
    if (r.isFolderHeader) return;
    const int f = frameAtX(e->pos().x());
    // Camera lanes own no cells: a double-click keys the camera at that frame
    // (instant diamond), never an add-cell request the dock would drop.
    if (CameraLayer *cam = doc_->findCamera(r.trackId.toStdString())) {
        if (doc_->keyCameraAt(cam, f) > 0) emit keyChanged();
        update();
        return;
    }
    const auto cells = doc_->timeline().cells(r.trackId.toStdString());
    for (const auto &cell : cells) {
        if (f >= cell.start && f <= cell.end) {
            // On a cell: drop a blank cell right after it and ripple the rest
            // right, so a new cell can always be wedged between two cells.
            emit insertCellRequested(r.trackId, cell.end + 1);
            return;
        }
    }
    emit addCellRequested(r.trackId, f);
}

void TimelineWidget::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double f = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        setPxPerFrame(pxPerFrame_ * f, e->position().toPoint().x());
        e->accept();
        return;
    }
    if (e->modifiers() & Qt::ShiftModifier) {
        emit scrollRequest(std::max(0, hScroll_ + (e->angleDelta().y() > 0 ? -60 : 60)), vScroll_);
    } else if (std::abs(e->angleDelta().x()) > std::abs(e->angleDelta().y())) {
        emit scrollRequest(std::max(0, hScroll_ - e->angleDelta().x()), vScroll_);
    } else {
        emit scrollRequest(hScroll_, std::max(0, vScroll_ - e->angleDelta().y()));
    }
    e->accept();
}

void TimelineWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    update();
}

void TimelineWidget::leaveEvent(QEvent *e) {
    hoverTrack_.clear();
    hoverFrame_ = -1;
    hoverBtn_ = -1;
    hoverRange_ = 0;
    hoverCheck_ = false;
    hoverKeyFrame_ = -1;
    hoverGutterPart_ = 0;
    setCursor(Qt::ArrowCursor);
    update();
    QWidget::leaveEvent(e);
}

void TimelineWidget::setSelection(const QSet<TimelineSel> &s) { selection_ = s; update(); }
void TimelineWidget::clearSelection() { selection_.clear(); update(); }

void TimelineWidget::keyPressEvent(QKeyEvent *e) {
    bool shift = e->modifiers() & Qt::ShiftModifier;
    bool ctrl = e->modifiers() & Qt::ControlModifier;
    if (e->key() == Qt::Key_Escape && draggingKeys_) {
        if (doc_->findCamera(keyDragAdj_.toStdString()))
            doc_->cancelCameraRecord();
        else
            doc_->cancelParamRecord();
        draggingKeys_ = false;
        keyDragBase_.clear();
        keyDragOrigin_ = -1;
        keyMoved_ = false;
        emit keyChanged();
        update();
        return;
    }
    // Escape mid-drag rewinds the whole gesture to the pre-drag snapshot.
    if (e->key() == Qt::Key_Escape && draggingBar_) {
        if (CameraLayer *camBar = doc_->findCamera(barDragTrack_.toStdString()))
            doc_->abortCameraBar(camBar, barDragBaseParams_);
        else if (PaintLayer *pl = animatedLayerOf(barDragTrack_))
            doc_->abortAdjustmentBar(doc_->ensureAdjustmentFor(pl), barDragOrigStart_,
                                     barDragOrigEnd_, barDragBaseParams_);
        draggingBar_ = false;
        barMoved_ = false;
        barDragTrack_.clear();
        emit keyChanged();
        update();
        return;
    }
    // Delete on selected keyframes takes priority: the two selections are
    // mutually exclusive in practice (clicking a diamond clears the cell
    // selection), so there is never a question of which one Delete means.
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && !keySel_.isEmpty()) {
        std::map<QString, std::vector<TimelineKey>> byLayer;
        for (const TimelineKey &k : keySel_) byLayer[k.adjId].push_back(k);
        for (auto &[adjId, keys] : byLayer) {
            if (CameraLayer *camDel = doc_->findCamera(adjId.toStdString())) {
                for (const TimelineKey &k : keys)
                    doc_->removeCameraKey(camDel, (ParamId)k.param, k.frame);
            } else if (PaintLayer *pl = animatedLayerOf(adjId)) {
                for (const TimelineKey &k : keys) doc_->removeParamKey(doc_->ensureAdjustmentFor(pl), (ParamId)k.param, k.frame);
            }
        }
        keySel_.clear();
        noteAnimationChanged();
        emit keyChanged();
        return;
    }
    // Delete: remove selected cells (Shift = +pull, Krita remove-and-shift)
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        if (selection_.isEmpty()) return;
        std::map<QString, std::vector<int>> byTrack;
        for (auto &s : selection_) byTrack[s.trackId].push_back(s.frame);
        for (auto &[tid, frames] : byTrack)
            if (PaintLayer *l = doc_->findTrack(tid.toStdString()))
                doc_->removeKeys(l, frames, shift);
        selection_.clear();
        invalidateCells();
        emit keyChanged();
        emit selectionChanged();
        update();
        return;
    }
    if (ctrl && e->key() == Qt::Key_C) {
        std::map<QString, std::vector<int>> byTrack;
        for (auto &s : selection_) byTrack[s.trackId].push_back(s.frame);
        for (auto &[tid, frames] : byTrack)
            if (PaintLayer *l = doc_->findTrack(tid.toStdString())) { doc_->copyKeys(l, frames); break; }
        return;
    }
    if (ctrl && e->key() == Qt::Key_X) {
        std::map<QString, std::vector<int>> byTrack;
        for (auto &s : selection_) byTrack[s.trackId].push_back(s.frame);
        for (auto &[tid, frames] : byTrack)
            if (PaintLayer *l = doc_->findTrack(tid.toStdString())) {
                doc_->copyKeys(l, frames);
                doc_->removeKeys(l, frames, false);
                break;
            }
        selection_.clear();
        invalidateCells();
        emit keyChanged();
        emit selectionChanged();
        update();
        return;
    }
    if (ctrl && e->key() == Qt::Key_V) {
        if (PaintLayer *l = doc_->currentLayer())
            doc_->pasteKeys(l, doc_->currentFrame(), false);
        invalidateCells();
        emit keyChanged();
        update();
        return;
    }
    if (e->key() == Qt::Key_H && !ctrl) {
        if (PaintLayer *l = doc_->currentLayer()) {
            if (shift) doc_->removeHold(l, doc_->currentFrame(), 1);
            else doc_->insertHold(l, doc_->currentFrame(), 1);
            emit keyChanged();
            update();
        }
        return;
    }
    // [ / ] shorten / lengthen the exposure of the cell at the playhead
    if ((e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight) && !ctrl) {
        if (PaintLayer *l = doc_->currentLayer()) {
            const int key = doc_->cellStartAt(l, doc_->currentFrame());
            if (key >= 0) {
                if (e->key() == Qt::Key_BracketRight)
                    doc_->setCellEnd(l, key, doc_->cellEnd(l, key) + 1);
                else
                    doc_->setCellEnd(l, key, doc_->cellEnd(l, key) - 1);
                invalidateCells();
                emit keyChanged();
                update();
            }
        }
        return;
    }
    // I / O drop the playback range in and out at the playhead
    if ((e->key() == Qt::Key_I || e->key() == Qt::Key_O) && !ctrl) {
        const int f = doc_->timeline().currentFrame();
        if (e->key() == Qt::Key_I) in_ = std::min(f, out_);
        else out_ = std::max(f, in_);
        emit rangeChangeRequested(in_, out_);
        update();
        return;
    }
    if (e->key() == Qt::Key_M && !ctrl) {
        std::map<QString, std::vector<int>> byTrack;
        for (auto &s : selection_) byTrack[s.trackId].push_back(s.frame);
        for (auto &[tid, frames] : byTrack)
            if (PaintLayer *l = doc_->findTrack(tid.toStdString())) {
                int lo = *std::min_element(frames.begin(), frames.end());
                int hi = *std::max_element(frames.begin(), frames.end());
                doc_->mirrorKeys(l, lo, hi);
            }
        emit keyChanged();
        update();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        selection_.clear();
        layerSel_.clear();
        folderSel_.clear();
        emit selectionChanged();
        emit layerSelectionChanged();
        update();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace ink
