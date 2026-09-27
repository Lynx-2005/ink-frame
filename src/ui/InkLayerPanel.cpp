#include "ui/InkLayerPanel.h"
#include "ui/InkDocument.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace ink {

namespace {

constexpr int kPanelW = 260;  // same family as the timeline gutter (240)
constexpr int kHit = 22;      // eye/lock hit box, same as the gutter
constexpr int kChipW = 20;    // layer thumbnail chip
constexpr int kChipH = 13;
constexpr int kThumbPx = 24;  // cached thumbnail height
constexpr int kIndent = 12;   // px per nesting level

QFont uiFont(int px, bool bold = false) {
    QFont f;
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

// Small bounded cache: painting runs on every hover move.
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

int paintCountIn(const CellLayer *l) {
    if (l->isPaint()) return 1;
    int n = 0;
    for (auto &c : l->children()) n += paintCountIn(c.get());
    return n;
}

int selectedPaintIn(const CellLayer *l, const QSet<uint64_t> &sel) {
    if (l->isPaint()) return sel.contains(l->id()) ? 1 : 0;
    int n = 0;
    for (auto &c : l->children()) n += selectedPaintIn(c.get(), sel);
    return n;
}

void collectPaintIds(const CellLayer *l, std::vector<uint64_t> &out) {
    if (l->isPaint()) {
        out.push_back(l->id());
        return;
    }
    for (auto &c : l->children()) collectPaintIds(c.get(), out);
}

// True when `ancestor` is `l` itself or sits above it in the tree.
bool containsLayer(const CellLayer *ancestor, uint64_t id) {
    if (!ancestor) return false;
    if (ancestor->id() == id) return true;
    for (auto &c : ancestor->children())
        if (containsLayer(c.get(), id)) return true;
    return false;
}

} // namespace

// ------------------------------------------------------------ the list

LayerListView::LayerListView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(40);
}

LayerListView::Columns LayerListView::columns() const {
    Columns c;
    c.expander = 8;
    c.check = 24; // selection checkbox (the control used to group layers)
    c.chip = 46;
    c.name = 70;
    c.eye = width() - 56;
    c.lock = width() - 30;
    return c;
}

QRect LayerListView::checkBoxRect(const LayerRow &r) const {
    const int side = 14;
    const int x = columns().check + r.depth * kIndent;
    return QRect(x, r.y + (r.h - side) / 2, side, side);
}

void LayerListView::drawCheckBox(QPainter &p, const QRect &box, bool checked,
                                 bool enabled) const {
    // Mirrors the timeline gutter's checkbox so the two read as one control.
    p.setPen(QPen(checked ? InkTheme::accent()
                          : (enabled ? InkTheme::tick() : InkTheme::hairline()),
                  1));
    p.setBrush(checked ? InkTheme::accent()
                       : (enabled ? QColor(0, 0, 0, 40) : QColor(0, 0, 0, 18)));
    p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (checked) {
        p.setPen(QPen(InkTheme::bg(), 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(box.left() + 3, box.center().y(), box.center().x() - 1, box.bottom() - 4);
        p.drawLine(box.center().x() - 1, box.bottom() - 4, box.right() - 3, box.top() + 4);
    }
    p.setBrush(Qt::NoBrush);
}

void LayerListView::setRows(const std::vector<LayerRow> &rows, uint64_t currentId,
                            const QSet<uint64_t> &sel) {
    rows_ = rows;
    currentId_ = currentId;
    sel_ = sel;
    updateGeometry();
    update();
}

void LayerListView::setEmptyText(const QString &title, const QString &sub) {
    emptyTitle_ = title;
    emptySub_ = sub;
    update();
}

QSize LayerListView::sizeHint() const {
    int h = 0;
    for (const auto &r : rows_) h += r.h;
    return QSize(200, std::max(80, h));
}

QRect LayerListView::rowRect(uint64_t id) const {
    for (const auto &r : rows_)
        if (r.id == id) return QRect(0, r.y, width(), r.h);
    return {};
}

QRect LayerListView::nameRect(uint64_t id) const {
    const Columns c = columns();
    for (const auto &r : rows_) {
        if (r.id != id) continue;
        const int x = c.name + r.depth * kIndent;
        const int right = c.eye - 8;
        return QRect(x, r.y + 3, std::max(40, right - x), r.h - 6);
    }
    return {};
}

uint64_t LayerListView::idAt(const QPoint &pos) const {
    uint64_t id = 0;
    partAt(pos, id);
    return id;
}

int LayerListView::partAt(const QPoint &pos, uint64_t &id) const {
    id = 0;
    const Columns c = columns();
    for (const auto &r : rows_) {
        if (pos.y() < r.y || pos.y() >= r.y + r.h) continue;
        id = r.id;
        const int ind = r.depth * kIndent;
        if (r.isGroup && pos.x() >= c.expander + ind - 3 && pos.x() < c.expander + ind + 14)
            return 1;
        const QRect box = checkBoxRect(r);
        if (pos.x() >= box.left() - 2 && pos.x() < box.right() + 3) return 2; // 2px slop
        if (pos.x() >= c.eye && pos.x() < c.eye + kHit) return 3;
        if (pos.x() >= c.lock && pos.x() < c.lock + kHit) return 4;
        return 0;
    }
    return 0;
}

QPixmap LayerListView::thumbFor(const CellLayer *layer) {
    if (!layer) return {};
    auto it = thumbs_.constFind(layer->id());
    if (it != thumbs_.constEnd()) return it.value();
    QPixmap pm;
    if (const PaintDevice *d = layer->device()) {
        Rect e = d->extent();
        if (!e.isEmpty()) {
            std::vector<uint8_t> buf(size_t(e.w) * size_t(e.h) * 4);
            d->readRect(e, buf.data());
            QImage full(buf.data(), e.w, e.h, QImage::Format_RGBA8888);
            pm = QPixmap::fromImage(full.scaledToHeight(kThumbPx, Qt::SmoothTransformation));
        }
    }
    if (thumbs_.size() > 256) thumbs_.clear();
    thumbs_.insert(layer->id(), pm);
    return pm;
}

void LayerListView::drawChevron(QPainter &p, const QRectF &r, bool open, const QColor &c) const {
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
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void LayerListView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Columns c = columns();
    const qreal dpr = devicePixelRatioF();

    if (rows_.empty()) {
        // Teaching empty state: say what belongs here and how to fill it.
        QRect area = rect().adjusted(18, 0, -18, 0);
        const bool two = !emptySub_.isEmpty();
        const int titleH = 20;
        const int subH = two ? 34 : 0; // two wrapped lines fit at 10px
        const int blockH = titleH + (two ? 4 : 0) + subH;
        const int top = area.center().y() - blockH / 2;
        p.setFont(uiFont(12));
        p.setPen(InkTheme::dim());
        p.drawText(QRect(area.left(), top, area.width(), titleH), Qt::AlignCenter, emptyTitle_);
        if (two) {
            p.setFont(uiFont(10));
            p.setPen(InkTheme::faint());
            p.drawText(QRect(area.left(), top + titleH + 4, area.width(), subH),
                       Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, emptySub_);
        }
    }

    for (const auto &r : rows_) {
        const QRect row(0, r.y, width(), r.h);
        const bool isCurrent = (r.id == currentId_);
        const bool selected = sel_.contains(r.id);
        const bool hovered = (r.id == hoverId_);
        QColor bg = isCurrent ? InkTheme::rowCurrent()
                   : selected ? InkTheme::rowSelected()
                   : hovered ? InkTheme::rowHover() : QColor(Qt::transparent);
        if (r.isGroup && !isCurrent && !selected && !hovered) bg = QColor(0, 0, 0, 36);
        if (bg.alpha() > 0) p.fillRect(row, bg);
        if (selected) {
            QColor a = InkTheme::accent();
            a.setAlpha(30);
            p.fillRect(row, a);
        }

        const int ind = r.depth * kIndent;
        // hue spine for groups (id-derived: stable across reordering)
        if (r.isGroup) {
            QColor hue = InkTheme::layerHue(int(r.id % 8));
            hue.setAlpha(150);
            p.fillRect(QRect(2 + ind, row.y() + 3, 2, row.height() - 6), hue);
        }
        if (isCurrent || selected)
            p.fillRect(QRect(4 + ind, row.y() + 3, 2, row.height() - 6),
                       isCurrent ? InkTheme::accent() : InkTheme::accentMuted());

        // a layer a group hides/locks still shows its own flags, just dimmed
        if (r.hiddenByGroup) p.setOpacity(0.45);

        // selection checkbox (tick two or more, then Group) — the same control
        // as the timeline gutter
        drawCheckBox(p, checkBoxRect(r), r.checked, r.checkEnabled);

        // expander (groups)
        if (r.isGroup)
            drawChevron(p, QRectF(c.expander + ind, row.y() + (row.height() - 10) / 2.0, 9, 10),
                        r.expanded, r.expanded ? InkTheme::text() : InkTheme::dim());

        // chip: group glyph, or the layer's own pixels
        const QRect chip(c.chip + ind, row.y() + (row.height() - kChipH) / 2, kChipW, kChipH);
        if (r.isGroup) {
            const int s = int(13 * dpr);
            QPixmap glyph = cachedIcon(r.expanded ? IconId::FolderOpen : IconId::Folder,
                                       InkTheme::layerHue(int(r.id % 8)), s);
            glyph.setDevicePixelRatio(dpr);
            drawPix(p, glyph, chip);
        } else {
            p.fillRect(chip, QColor(0, 0, 0, 60));
            p.setPen(InkTheme::hairline());
            p.setBrush(Qt::NoBrush);
            p.drawRect(chip.adjusted(0, 0, -1, -1));
            const QPixmap pm = thumbFor(r.layer);
            if (!pm.isNull()) {
                const QSize sz = pm.size().scaled(chip.size() - QSize(2, 2), Qt::KeepAspectRatio);
                p.drawPixmap(QRect(chip.center() - QPoint(sz.width() / 2, sz.height() / 2), sz), pm);
            } else {
                const int s = int(10 * dpr);
                QPixmap glyph = cachedIcon(IconId::Cell, InkTheme::faint(), s);
                glyph.setDevicePixelRatio(dpr);
                drawPix(p, glyph, chip);
            }
        }

        // name (elided; full name lives in the tooltip)
        const QRect nameRc = nameRect(r.id);
        p.setFont(uiFont(11, r.isGroup));
        p.setPen((isCurrent || r.isGroup) ? InkTheme::text() : InkTheme::dim());
        QString label = QString::fromStdString(r.layer->name());
        if (r.isGroup && r.childCount > 0) label += QStringLiteral("  (%1)").arg(r.childCount);
        p.drawText(nameRc, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(label, Qt::ElideRight, nameRc.width()));

        // eye + lock
        const bool locked = r.layer->locked();
        const bool gateLocked = locked || r.lockedByGroup; // a locked group locks too
        QColor lockCol = locked ? InkTheme::warn()
                         : (r.lockedByGroup ? InkTheme::warn() : (hovered ? InkTheme::text()
                                                                         : InkTheme::faint()));
        if (r.lockedByGroup) lockCol.setAlpha(140); // inherited, not its own lock
        const QColor eyeCol = !r.layer->visible() ? InkTheme::accentMuted()
                             : (r.hiddenByGroup ? InkTheme::faint()
                                                : (hovered ? InkTheme::text() : InkTheme::faint()));
        const int s = int(14 * dpr);
        QPixmap eye = cachedIcon(r.layer->visible() ? IconId::Eye : IconId::EyeOff, eyeCol, s);
        eye.setDevicePixelRatio(dpr);
        QPixmap lk = cachedIcon(gateLocked ? IconId::Lock : IconId::Unlock, lockCol, s);
        lk.setDevicePixelRatio(dpr);
        drawPix(p, eye, QRect(c.eye, row.y() + (row.height() - kHit) / 2, kHit, kHit));
        drawPix(p, lk, QRect(c.lock, row.y() + (row.height() - kHit) / 2, kHit, kHit));
        p.setOpacity(1.0);

        p.setPen(InkTheme::hairline());
        p.drawLine(row.left(), row.bottom(), row.right(), row.bottom());
    }

    // drag & drop feedback
    if (dragging_ && dropValid_) {
        if (dropIntoRect_.isValid()) {
            QColor a = InkTheme::accent();
            a.setAlpha(60);
            p.fillRect(dropIntoRect_, a);
            p.setPen(InkTheme::accent());
            p.setBrush(Qt::NoBrush);
            p.drawRect(dropIntoRect_.adjusted(0, 0, -1, -1));
        } else if (dropIndicator_.isValid()) {
            p.fillRect(dropIndicator_, InkTheme::accent());
        }
    }

    // keyboard focus ring (the list is reachable with Tab)
    if (hasFocus() && !rows_.empty()) {
        p.setPen(InkTheme::accent());
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void LayerListView::mousePressEvent(QMouseEvent *e) {
    if (editor_) commitRename();
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    uint64_t id = 0;
    const int part = partAt(e->pos(), id);
    if (!id) return;
    setFocus(Qt::MouseFocusReason);
    if (part == 1) {
        emit expandToggled(id);
        return;
    }
    if (part == 2) {
        emit checkToggled(id);
        return;
    }
    if (part == 3) {
        emit visibilityToggled(id);
        return;
    }
    if (part == 4) {
        emit lockToggled(id);
        return;
    }
    pressId_ = id;
    pressPos_ = e->pos();
    dragging_ = false;
    emit selectRequested(id, e->modifiers());
}

void LayerListView::mouseMoveEvent(QMouseEvent *e) {
    uint64_t id = 0;
    const int part = partAt(e->pos(), id);
    if (id != hoverId_ || part != hoverPart_) {
        hoverId_ = id;
        hoverPart_ = part;
        if (part == 1) setToolTip(QStringLiteral("Expand / collapse this group"));
        else if (part == 2) {
            for (const auto &r : rows_)
                if (r.id == id) {
                    setToolTip(r.isGroup
                                   ? QStringLiteral("Select every layer in this group "
                                                    "(for grouping, moving, deleting)")
                                   : QStringLiteral("Select this layer — tick two or more, "
                                                    "then Group (Ctrl+G)"));
                    break;
                }
        } else if (part == 3) setToolTip(QStringLiteral("Show / hide this layer"));
        else if (part == 4) setToolTip(QStringLiteral("Lock (blocks painting)"));
        else if (id) {
            for (const auto &r : rows_)
                if (r.id == id) {
                    setToolTip(QString::fromStdString(r.layer->name()) +
                               QStringLiteral(" — click to select, double-click to rename, "
                                              "drag to reorder, right-click for layer options"));
                    break;
                }
        } else setToolTip(QString());
        setCursor(part == 0 ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
    }

    if (!(e->buttons() & Qt::LeftButton) || !pressId_ || rows_.empty()) return;
    if (!dragging_ && (e->pos() - pressPos_).manhattanLength() > 6) dragging_ = true;
    if (!dragging_) return;

    dropValid_ = false;
    dropIndicator_ = QRect();
    dropIntoRect_ = QRect();
    const int y = e->pos().y();
    const LayerRow &first = rows_.front();
    const LayerRow &last = rows_.back();
    if (y < first.y) {
        dropParent_ = first.parentId; // above the front-most row
        dropIndex_ = first.index + 1;
        dropValid_ = true;
        dropIndicator_ = QRect(0, first.y, width(), 2);
    } else if (y >= last.y + last.h) {
        dropParent_ = last.parentId; // below the back-most row
        dropIndex_ = last.index;
        dropValid_ = true;
        dropIndicator_ = QRect(0, last.y + last.h - 2, width(), 2);
    } else {
        for (const auto &r : rows_) {
            if (y < r.y || y >= r.y + r.h) continue;
            const bool midBand = r.isGroup && y >= r.y + r.h * 0.32 && y <= r.y + r.h * 0.68;
            if (midBand) {
                dropParent_ = r.id; // into the group, on top
                dropIndex_ = r.childCount;
                dropValid_ = true;
                dropIntoRect_ = QRect(0, r.y, width(), r.h);
            } else if (y < r.y + r.h / 2) {
                dropParent_ = r.parentId; // above this row in the panel = higher in the stack
                dropIndex_ = r.index + 1;
                dropValid_ = true;
                dropIndicator_ = QRect(0, r.y, width(), 2);
            } else {
                dropParent_ = r.parentId; // below this row in the panel
                dropIndex_ = r.index;
                dropValid_ = true;
                dropIndicator_ = QRect(0, r.y + r.h - 2, width(), 2);
            }
            break;
        }
    }
    // never drop a layer into itself or into its own subtree
    if (dropValid_ && dropParent_ != 0) {
        for (const auto &r : rows_)
            if (r.id == pressId_ && containsLayer(r.layer, dropParent_)) dropValid_ = false;
    }
    update();
}

void LayerListView::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        if (dragging_ && dropValid_) emit moveRequested(pressId_, dropParent_, dropIndex_);
        dragging_ = false;
        dropValid_ = false;
        pressId_ = 0;
        dropIndicator_ = QRect();
        dropIntoRect_ = QRect();
        update();
    }
    QWidget::mouseReleaseEvent(e);
}

void LayerListView::mouseDoubleClickEvent(QMouseEvent *e) {
    uint64_t id = 0;
    const int part = partAt(e->pos(), id);
    if (id && part == 0) {
        for (const auto &r : rows_)
            if (r.id == id) {
                beginRename(id, QString::fromStdString(r.layer->name()));
                break;
            }
    }
    QWidget::mouseDoubleClickEvent(e);
}

void LayerListView::contextMenuEvent(QContextMenuEvent *e) {
    uint64_t id = 0;
    partAt(e->pos(), id);
    // A right-click on a row that is already part of the selection keeps that
    // selection: the menu then acts on the whole ticked set (group, delete).
    // Only an unselected row becomes the selection.
    if (id && !sel_.contains(id)) emit selectRequested(id, Qt::NoModifier);
    emit menuRequested(id, e->globalPos());
}

void LayerListView::leaveEvent(QEvent *e) {
    hoverId_ = 0;
    hoverPart_ = 0;
    setToolTip(QString());
    update();
    QWidget::leaveEvent(e);
}

void LayerListView::keyPressEvent(QKeyEvent *e) {
    if (editor_) {
        QWidget::keyPressEvent(e);
        return;
    }
    const int n = int(rows_.size());
    auto rowIndexOf = [&](uint64_t id) {
        for (int i = 0; i < n; ++i)
            if (rows_[size_t(i)].id == id) return i;
        return -1;
    };
    int cur = rowIndexOf(currentId_);
    if (cur < 0 && !sel_.isEmpty()) cur = rowIndexOf(*sel_.begin());
    uint64_t firstSel = sel_.isEmpty() ? 0 : *sel_.begin();
    switch (e->key()) {
        case Qt::Key_Up:
            if (n) emit selectRequested(rows_[size_t(std::max(0, cur < 0 ? 0 : cur - 1))].id,
                                        Qt::NoModifier);
            break;
        case Qt::Key_Down:
            if (n) emit selectRequested(rows_[size_t(std::min(n - 1, cur < 0 ? 0 : cur + 1))].id,
                                        Qt::NoModifier);
            break;
        case Qt::Key_F2: {
            const uint64_t id = currentId_ ? currentId_ : firstSel;
            for (const auto &r : rows_)
                if (r.id == id) {
                    beginRename(id, QString::fromStdString(r.layer->name()));
                    break;
                }
            break;
        }
        case Qt::Key_Delete:
            if (currentId_ || firstSel) emit deleteRequested(currentId_ ? currentId_ : firstSel);
            break;
        case Qt::Key_G:
            if (e->modifiers() & Qt::ControlModifier) {
                if (e->modifiers() & Qt::ShiftModifier) emit ungroupRequested();
                else emit groupRequested();
                break;
            }
            QWidget::keyPressEvent(e);
            return;
        case Qt::Key_Space: {
            const uint64_t id = currentId_ ? currentId_ : firstSel;
            if (id) emit checkToggled(id);
            break;
        }
        case Qt::Key_Escape:
            emit selectRequested(0, Qt::NoModifier);
            break;
        default:
            QWidget::keyPressEvent(e);
            return;
    }
}

void LayerListView::beginRename(uint64_t id, const QString &text) {
    const QRect rc = nameRect(id);
    if (rc.isNull()) return;
    if (!editor_) {
        editor_ = new QLineEdit(this);
        editor_->setObjectName(QStringLiteral("LayerRename"));
        editor_->setFrame(false);
        connect(editor_, &QLineEdit::editingFinished, this, &LayerListView::commitRename);
    }
    editId_ = id;
    editCancelled_ = false;
    editor_->setText(text);
    editor_->setGeometry(rc);
    editor_->selectAll();
    editor_->show();
    editor_->raise();
    editor_->setFocus(Qt::OtherFocusReason);
}

void LayerListView::cancelRename() {
    if (!editor_) return;
    editCancelled_ = true;
    editor_->hide();
    setFocus(Qt::OtherFocusReason);
}

void LayerListView::commitRename() {
    if (!editor_) return;
    const uint64_t id = editId_;
    const QString text = editor_->text().trimmed();
    editor_->hide();
    editId_ = 0;
    if (editCancelled_) {
        editCancelled_ = false;
        return;
    }
    if (id && !text.isEmpty()) emit renameCommitted(id, text);
}

// ----------------------------------------------------------- the panel

LayerPanel::LayerPanel(Document *doc, QWidget *parent) : QWidget(parent), doc_(doc) {
    setObjectName(QStringLiteral("LayerPanel"));
    setFixedWidth(kPanelW);

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("LayerPanelCard"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(root);

    auto *lay = new QVBoxLayout(root);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);

    // --- header: title, context line, actions ---
    auto *head = new QWidget(root);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(2);
    title_ = new QLabel(QStringLiteral("Layer"), head);
    title_->setObjectName(QStringLiteral("PanelTitle"));
    hl->addWidget(title_);
    hl->addStretch(1);
    addBtn_ = new QToolButton(head);
    addBtn_->setIcon(InkIcons::get(IconId::Plus, 16));
    addBtn_->setToolTip(QStringLiteral("Add a layer or a group"));
    addBtn_->setPopupMode(QToolButton::InstantPopup);
    {
        auto *m = new QMenu(addBtn_);
        m->addAction(QStringLiteral("New layer"), this, [this] { opAddLayer(); });
        m->addAction(QStringLiteral("New group"), this, [this] { opAddGroup(); });
        m->addSeparator();
        m->addAction(QStringLiteral("Duplicate layer"), this, [this] {
            auto sel = selectedLayers();
            if (sel.size() == 1) opDuplicate(sel.front()->id());
            else {
                transient_ = QStringLiteral("Select one layer to duplicate");
                afterOp();
            }
        });
        addBtn_->setMenu(m);
    }
    groupBtn_ = new QToolButton(head);
    groupBtn_->setIcon(InkIcons::get(IconId::GroupMerge, 16));
    groupBtn_->setToolTip(QStringLiteral("Group selected layers (Ctrl+G)"));
    ungroupBtn_ = new QToolButton(head);
    ungroupBtn_->setIcon(InkIcons::get(IconId::Ungroup, 16));
    ungroupBtn_->setToolTip(QStringLiteral("Ungroup the selected group (Ctrl+Shift+G)"));
    hl->addWidget(addBtn_);
    hl->addWidget(groupBtn_);
    hl->addWidget(ungroupBtn_);
    lay->addWidget(head);

    context_ = new QLabel(root);
    context_->setObjectName(QStringLiteral("PanelContext"));
    context_->setTextFormat(Qt::PlainText);
    context_->setFixedHeight(14);
    lay->addWidget(context_);

    // --- the list ---
    list_ = new LayerListView(root);
    scroll_ = new QScrollArea(root);
    scroll_->setWidget(list_);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lay->addWidget(scroll_, 1);

    // --- inspector: the selected layer's opacity + blend ---
    auto *insp = new QWidget(root);
    auto *il = new QVBoxLayout(insp);
    il->setContentsMargins(0, 2, 0, 0);
    il->setSpacing(4);
    auto *opRow = new QWidget(insp);
    auto *ol = new QHBoxLayout(opRow);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->setSpacing(6);
    auto *opLab = new QLabel(QStringLiteral("Opacity"), opRow);
    opLab->setObjectName(QStringLiteral("Section"));
    opLab->setFixedWidth(52);
    opacity_ = new QSlider(Qt::Horizontal, opRow);
    opacity_->setRange(0, 100);
    opacity_->setValue(100);
    opacity_->setToolTip(QStringLiteral("Opacity of the selected layer"));
    opacityValue_ = new QLabel(QStringLiteral("100%"), opRow);
    opacityValue_->setObjectName(QStringLiteral("OptValue"));
    opacityValue_->setFixedWidth(38);
    opacityValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ol->addWidget(opLab);
    ol->addWidget(opacity_, 1);
    ol->addWidget(opacityValue_);
    auto *blRow = new QWidget(insp);
    auto *bl = new QHBoxLayout(blRow);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(6);
    auto *blLab = new QLabel(QStringLiteral("Blend"), blRow);
    blLab->setObjectName(QStringLiteral("Section"));
    blLab->setFixedWidth(52);
    blend_ = new QComboBox(blRow);
    blend_->addItems({QStringLiteral("Normal"), QStringLiteral("Multiply"), QStringLiteral("Screen"),
                      QStringLiteral("Erase")});
    blend_->setToolTip(QStringLiteral("Blend mode of the selected layer"));
    bl->addWidget(blLab);
    bl->addWidget(blend_, 1);
    il->addWidget(opRow);
    il->addWidget(blRow);
    lay->addWidget(insp);

    // --- wiring ---
    connect(opacity_, &QSlider::valueChanged, this, [this](int v) {
        if (syncing_) return;
        CellLayer *l = singleSelection();
        if (!l) return;
        doc_->setCellLayerOpacity(l, float(v) / 100.0f);
        opacityValue_->setText(QStringLiteral("%1%").arg(v));
        afterOp();
    });
    connect(blend_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (syncing_) return;
        CellLayer *l = singleSelection();
        if (!l) return;
        doc_->setCellLayerBlend(l, BlendMode(std::clamp(idx, 0, 3)));
        afterOp();
    });
    connect(groupBtn_, &QToolButton::clicked, this, &LayerPanel::opGroup);
    connect(ungroupBtn_, &QToolButton::clicked, this, &LayerPanel::opUngroup);
    connect(list_, &LayerListView::selectRequested, this, &LayerPanel::onSelect);
    connect(list_, &LayerListView::checkToggled, this, &LayerPanel::onCheck);
    connect(list_, &LayerListView::visibilityToggled, this, [this](uint64_t id) {
        Cell *c = cell();
        if (!c) return;
        if (CellLayer *l = c->find(id)) {
            doc_->setCellLayerVisible(l, !l->visible());
            afterOp();
        }
    });
    connect(list_, &LayerListView::lockToggled, this, [this](uint64_t id) {
        Cell *c = cell();
        if (!c) return;
        if (CellLayer *l = c->find(id)) {
            doc_->setCellLayerLocked(l, !l->locked());
            afterOp();
        }
    });
    connect(list_, &LayerListView::expandToggled, this, [this](uint64_t id) {
        if (collapsed_.contains(id)) collapsed_.remove(id);
        else collapsed_.insert(id);
        refresh();
    });
    connect(list_, &LayerListView::renameCommitted, this, [this](uint64_t id, const QString &name) {
        Cell *c = cell();
        if (!c) return;
        if (CellLayer *l = c->find(id)) doc_->renameCellLayer(l, name.toStdString());
        afterOp();
    });
    connect(list_, &LayerListView::menuRequested, this, &LayerPanel::showContextMenu);
    connect(list_, &LayerListView::moveRequested, this, &LayerPanel::onMove);
    connect(list_, &LayerListView::deleteRequested, this, [this](uint64_t id) {
        if (sel_.size() > 1) opDeleteSelected(); // Delete acts on the ticked set
        else opDelete(id);
    });
    connect(list_, &LayerListView::groupRequested, this, &LayerPanel::opGroup);
    connect(list_, &LayerListView::ungroupRequested, this, &LayerPanel::opUngroup);

    refresh();
}

QWidget *LayerPanel::listWidget() const { return list_; }

Cell *LayerPanel::cell() const { return doc_->currentCell(); }

std::vector<CellLayer*> LayerPanel::selectedLayers() const {
    std::vector<CellLayer*> out;
    for (const auto &r : rows_)
        if (sel_.contains(r.id)) out.push_back(r.layer);
    return out;
}

CellLayer *LayerPanel::singleSelection() const {
    Cell *c = cell();
    if (!c) return nullptr;
    if (sel_.size() == 1)
        if (CellLayer *l = c->find(*sel_.begin())) return l;
    // Nothing (or several) selected: the inspector edits the paint target.
    return c->activePaintLayer();
}

void LayerPanel::buildRows() {
    rows_.clear();
    Cell *c = cell();
    hasCell_ = c != nullptr;
    if (!c) return;
    int y = 0;
    std::function<void(const std::vector<std::shared_ptr<CellLayer>>&, uint64_t, int)> walk =
        [&](const std::vector<std::shared_ptr<CellLayer>> &vec, uint64_t parentId, int depth) {
        // FRONT-most first: the panel reads like every other layer palette.
        for (int i = int(vec.size()) - 1; i >= 0; --i) {
            const auto &l = vec[size_t(i)];
            LayerRow row;
            row.layer = l.get();
            row.id = l->id();
            row.parentId = parentId;
            row.index = i;
            row.childCount = int(l->children().size());
            row.depth = depth;
            row.isGroup = !l->isPaint();
            row.expanded = !collapsed_.contains(l->id());
            if (row.isGroup) {
                const int total = paintCountIn(row.layer);
                row.checkEnabled = total > 0;
                row.checked = total > 0 && selectedPaintIn(row.layer, sel_) == total;
            } else {
                row.checked = sel_.contains(row.id);
            }
            // A group's hide/lock reaches every layer under it.
            row.hiddenByGroup = l->visible() && !c->visibleThrough(l.get());
            row.lockedByGroup = !l->locked() && c->lockedThrough(l.get());
            row.y = y;
            row.h = row.isGroup ? InkTheme::folderH() : InkTheme::trackH();
            y += row.h;
            rows_.push_back(row);
            if (row.isGroup && row.expanded) walk(l->children(), l->id(), depth + 1);
        }
    };
    walk(c->layers(), 0, 0);
}

void LayerPanel::refresh() {
    buildRows();
    // prune selection that no longer exists (deleted, or another cell now)
    QSet<uint64_t> keep;
    for (const auto &r : rows_)
        if (sel_.contains(r.id)) keep.insert(r.id);
    sel_ = keep;
    if (anchor_ && !keep.contains(anchor_)) anchor_ = 0;

    Cell *c = cell();
    const CellLayer *active = c ? c->activePaintLayer() : nullptr;
    list_->setRows(rows_, active ? active->id() : 0, sel_);

    if (!hasCell_) {
        if (doc_->autoKeyEnabled())
            list_->setEmptyText(QStringLiteral("No cell at this frame"),
                                QStringLiteral("Press + to add the first layer — the cell is "
                                               "created together with it."));
        else
            list_->setEmptyText(QStringLiteral("No cell at this frame"),
                                QStringLiteral("Auto-key is off. Turn it on in the timeline, or "
                                               "add a cell there first."));
    }

    // context line: which cell is being edited, and whether painting is blocked
    QString ctx;
    if (hasCell_) {
        PaintLayer *t = doc_->currentLayer();
        const int key = doc_->currentCellKey();
        const int idx = doc_->timeline().cellIndexAt(t->stableId(), key);
        const int end = doc_->cellEnd(t, key);
        ctx = QStringLiteral("%1 · cell %2 · frames %3–%4")
                  .arg(QString::fromStdString(t->name()))
                  .arg(idx + 1)
                  .arg(key)
                  .arg(end);
        if (doc_->paintBlockedByLock()) ctx += QStringLiteral(" · locked");
    } else {
        ctx = QStringLiteral("%1 · no cell here")
                  .arg(doc_->currentLayer() ? QString::fromStdString(doc_->currentLayer()->name())
                                            : QStringLiteral("no track"));
    }
    if (!transient_.isEmpty()) {
        ctx = transient_;
        transient_.clear();
    }
    context_->setText(ctx);
    const bool blocked = doc_->paintBlockedByLock();
    context_->setStyleSheet(blocked ? QStringLiteral("color:#D9A34A;") : QString());

    syncInspector();
    syncButtons();
}

void LayerPanel::invalidateThumbs() {
    list_->invalidateThumbs();
    list_->update();
}

void LayerPanel::invalidateThumb(uint64_t id) {
    list_->invalidateThumb(id);
}

void LayerPanel::syncInspector() {
    CellLayer *l = singleSelection();
    syncing_ = true;
    const bool on = l != nullptr;
    opacity_->setEnabled(on);
    blend_->setEnabled(on);
    if (on) {
        opacity_->setValue(int(l->opacity() * 100.0f + 0.5f));
        opacityValue_->setText(QStringLiteral("%1%").arg(opacity_->value()));
        blend_->setCurrentIndex(int(l->blend()));
    } else {
        // Nothing to inspect: show an empty inspector rather than a
        // meaningless "100% / Normal".
        opacity_->setValue(0);
        opacityValue_->setText(QStringLiteral("—"));
        blend_->setCurrentIndex(-1);
    }
    syncing_ = false;
}

void LayerPanel::syncButtons() {
    addBtn_->setEnabled(true);
    const auto sel = selectedLayers();
    groupBtn_->setEnabled(sel.size() >= 2);
    ungroupBtn_->setEnabled(sel.size() == 1 && !sel.front()->isPaint());
}

void LayerPanel::onSelect(uint64_t id, Qt::KeyboardModifiers mods) {
    Cell *c = cell();
    if (!c) return;
    if (id == 0) {
        sel_.clear();
        anchor_ = 0;
        refresh();
        return;
    }
    CellLayer *l = c->find(id);
    if (!l) return;
    if (mods & Qt::ControlModifier) {
        if (sel_.contains(id)) sel_.remove(id);
        else sel_.insert(id);
        anchor_ = id;
    } else if ((mods & Qt::ShiftModifier) && anchor_) {
        int a = -1, b = -1;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].id == anchor_) a = int(i);
            if (rows_[i].id == id) b = int(i);
        }
        if (a >= 0 && b >= 0) {
            sel_.clear();
            for (int i = std::min(a, b); i <= std::max(a, b); ++i) sel_.insert(rows_[size_t(i)].id);
        } else {
            sel_ = {id};
        }
    } else {
        sel_ = {id};
        anchor_ = id;
    }
    // clicking a paint layer makes it the paint target (tools follow it)
    if (l->isPaint()) doc_->setCellLayerActive(l);
    refresh();
}

void LayerPanel::onCheck(uint64_t id) {
    Cell *c = cell();
    if (!c) return;
    CellLayer *l = c->find(id);
    if (!l) return;
    if (l->isPaint()) {
        // Ticking a layer also makes it the paint target, like the gutter.
        if (sel_.contains(id)) sel_.remove(id);
        else {
            sel_.insert(id);
            anchor_ = id;
        }
        doc_->setCellLayerActive(l);
    } else {
        const int total = paintCountIn(l);
        if (total <= 0) return;
        const bool all = selectedPaintIn(l, sel_) == total;
        std::vector<uint64_t> ids;
        collectPaintIds(l, ids);
        for (uint64_t pid : ids) {
            if (all) sel_.remove(pid);
            else sel_.insert(pid);
        }
        anchor_ = id;
    }
    refresh();
}

void LayerPanel::opDeleteSelected() {
    Cell *c = cell();
    if (!c) return;
    std::vector<uint64_t> ids;
    for (const auto &r : rows_)
        if (sel_.contains(r.id)) ids.push_back(r.id);
    if (ids.empty()) return;
    int removed = 0;
    for (uint64_t id : ids) {
        CellLayer *l = c->find(id);
        if (!l) continue;               // its group was deleted already
        if (!c->canRemove(l)) continue; // a cell keeps one paint layer
        if (doc_->deleteCellLayer(l)) {
            sel_.remove(id);
            ++removed;
        }
    }
    if (removed == 0) transient_ = QStringLiteral("A cell needs at least one paint layer");
    afterOp();
}

void LayerPanel::onMove(uint64_t id, uint64_t parentId, int index) {
    Cell *c = cell();
    if (!c) return;
    CellLayer *l = c->find(id);
    if (!l) return;
    CellLayer *group = parentId ? c->find(parentId) : nullptr;
    if (parentId && (!group || group->isPaint())) return;
    if (doc_->moveCellLayerTo(l, group, index)) afterOp();
    else refresh();
}

void LayerPanel::opAddLayer() {
    if (CellLayer *l = doc_->addCellLayer({}, nullptr)) {
        sel_ = {l->id()};
        anchor_ = l->id();
    } else {
        transient_ = QStringLiteral("Auto-key is off — enable it to create cells");
    }
    afterOp();
}

void LayerPanel::opAddGroup() {
    if (CellLayer *g = doc_->addCellGroup({}, nullptr)) {
        sel_ = {g->id()};
        anchor_ = g->id();
    } else {
        transient_ = QStringLiteral("Auto-key is off — enable it to create cells");
    }
    afterOp();
}

void LayerPanel::opDuplicate(uint64_t id) {
    Cell *c = cell();
    if (!c) return;
    CellLayer *l = c->find(id);
    if (!l) return;
    if (CellLayer *copy = doc_->duplicateCellLayer(l)) {
        sel_ = {copy->id()};
        anchor_ = copy->id();
    }
    afterOp();
}

void LayerPanel::opDelete(uint64_t id) {
    Cell *c = cell();
    if (!c) return;
    CellLayer *l = c->find(id);
    if (!l) return;
    if (!doc_->deleteCellLayer(l))
        transient_ = QStringLiteral("A cell needs at least one paint layer");
    else if (sel_.contains(id)) {
        sel_.remove(id);
        if (anchor_ == id) anchor_ = 0;
    }
    afterOp();
}

void LayerPanel::opGroup() {
    const auto sel = selectedLayers();
    if (sel.size() < 2) {
        transient_ = QStringLiteral("Select two or more layers to group");
        afterOp();
        return;
    }
    if (CellLayer *g = doc_->groupCellLayers(sel)) {
        sel_ = {g->id()};
        anchor_ = g->id();
    }
    afterOp();
}

void LayerPanel::opUngroup() {
    const auto sel = selectedLayers();
    if (sel.size() != 1 || sel.front()->isPaint()) {
        transient_ = QStringLiteral("Select a group to ungroup");
        afterOp();
        return;
    }
    if (doc_->ungroupCellLayer(sel.front())) {
        sel_.clear();
        anchor_ = 0;
    }
    afterOp();
}

void LayerPanel::opMove(uint64_t id, int delta) {
    Cell *c = cell();
    if (!c) return;
    if (CellLayer *l = c->find(id)) doc_->moveCellLayer(l, delta);
    afterOp();
}

void LayerPanel::opMoveToGroup(uint64_t id, uint64_t groupId) {
    Cell *c = cell();
    if (!c) return;
    CellLayer *l = c->find(id);
    if (!l) return;
    CellLayer *group = groupId ? c->find(groupId) : nullptr;
    if (groupId && (!group || group->isPaint())) return;
    doc_->moveCellLayerToGroup(l, group);
    afterOp();
}

void LayerPanel::afterOp() {
    refresh();
    emit cellLayersChanged();
}

void LayerPanel::beginRename(uint64_t id) {
    Cell *c = cell();
    if (!c) return;
    if (CellLayer *l = c->find(id))
        list_->beginRename(id, QString::fromStdString(l->name()));
}

void LayerPanel::showContextMenu(uint64_t id, const QPoint &globalPos) {
    QMenu *m = buildContextMenu(id);
    m->exec(globalPos);
    m->deleteLater();
}

// Built fresh per right-click (the actions hold the ops, and the ticked-set
// items are decided from the selection as it stands right now).
QMenu *LayerPanel::buildContextMenu(uint64_t id) {
    QMenu *m = new QMenu(this);
    Cell *c = cell();
    if (!c) return m;
    CellLayer *l = id ? c->find(id) : nullptr;
    const auto ticked = selectedLayers();
    const int nTicked = int(ticked.size());
    if (nTicked > 1) {
        // The menu follows the ticks: that is what the checkboxes are for.
        m->addAction(nTicked == 2 ? QStringLiteral("Group 2 selected layers")
                                 : QStringLiteral("Group %1 selected layers").arg(nTicked),
                    this, [this] { opGroup(); });
        m->addAction(QStringLiteral("Delete %1 selected layers").arg(nTicked), this,
                    [this] { opDeleteSelected(); });
        m->addSeparator();
        m->addAction(QStringLiteral("Clear selection"), this, [this] {
            sel_.clear();
            anchor_ = 0;
            refresh();
        });
        return m;
    }
    m->addAction(QStringLiteral("New layer"), this, [this] { opAddLayer(); });
    m->addAction(QStringLiteral("New group"), this, [this] { opAddGroup(); });
    m->addAction(QStringLiteral("Duplicate layer"), this, [this, id] { opDuplicate(id); })
        ->setEnabled(l != nullptr);
    m->addAction(QStringLiteral("Rename…"), this, [this, id] { beginRename(id); })
        ->setEnabled(l != nullptr);
    m->addAction(QStringLiteral("Delete layer"), this, [this, id] { opDelete(id); })
        ->setEnabled(l && c->canRemove(l));
    m->addSeparator();
    m->addAction(QStringLiteral("Group selected layers"), this, [this] { opGroup(); })
        ->setEnabled(nTicked >= 2);
    m->addAction(QStringLiteral("Ungroup"), this, [this] { opUngroup(); })
        ->setEnabled(l && !l->isPaint());
    if (!sel_.isEmpty()) {
        m->addAction(QStringLiteral("Clear selection"), this, [this] {
            sel_.clear();
            anchor_ = 0;
            refresh();
        });
    }
    if (l) {
        m->addSeparator();
        uint64_t parentId = 0;
        int index = 0;
        c->slotOf(l, parentId, index);
        const int siblings = parentId
                                 ? int(c->find(parentId)->children().size())
                                 : int(c->layers().size());
        m->addAction(QStringLiteral("Move up"), this, [this, id] { opMove(id, +1); })
            ->setEnabled(index + 1 < siblings);
        m->addAction(QStringLiteral("Move down"), this, [this, id] { opMove(id, -1); })
            ->setEnabled(index > 0);
        QMenu *to = m->addMenu(QStringLiteral("Move to group"));
        to->addAction(QStringLiteral("Top level"), this, [this, id] { opMoveToGroup(id, 0); })
            ->setEnabled(parentId != 0);
        std::function<void(const std::vector<std::shared_ptr<CellLayer>>&)> addGroups =
            [&](const std::vector<std::shared_ptr<CellLayer>> &vec) {
                for (auto &g : vec) {
                    if (g->isPaint()) continue;
                    if (containsLayer(l, g->id())) continue; // itself or its subtree
                    const uint64_t gid = g->id();
                    to->addAction(QString::fromStdString(g->name()), this,
                                  [this, id, gid] { opMoveToGroup(id, gid); });
                    addGroups(g->children());
                }
            };
        addGroups(c->layers());
        to->setEnabled(!to->actions().isEmpty());
    }
    return m;
}

} // namespace ink
