#include "ui/InkLayerDock.h"
#include "ui/InkDocument.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ink {

enum { kColName = 0, kColEye, kColLock, kColOp, kColCount };
static const int kRolePtr = Qt::UserRole + 1;      // quintptr Layer*
static const int kRoleIsFolder = Qt::UserRole + 2; // bool

LayerDock::LayerDock(Document *doc, QWidget *parent)
    : QDockWidget(QStringLiteral("Layers"), parent), doc_(doc) {
    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("Card"));
    auto *lay = new QVBoxLayout(root);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    auto *head = new QWidget(root);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(4, 0, 4, 0);
    auto *title = new QLabel(QStringLiteral("Layers"), head);
    title->setStyleSheet(QStringLiteral("font-weight: 700; font-size: 14px;"));
    hl->addWidget(title);
    hl->addStretch(1);
    auto *addBtn = new QToolButton(head);
    addBtn->setIcon(InkIcons::get(IconId::Plus, 18));
    addBtn->setToolTip(QStringLiteral("New folder / layer above current"));
    addBtn->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(addBtn);
    addMenu->addAction(QStringLiteral("New folder"), this, &LayerDock::onAddFolder);
    addMenu->addAction(QStringLiteral("New layer above current"), this, &LayerDock::onAddPaint);
    addBtn->setMenu(addMenu);
    hl->addWidget(addBtn);
    lay->addWidget(head);

    tree_ = new QTreeWidget(root);
    tree_->setColumnCount(kColCount);
    tree_->header()->hide();
    tree_->setRootIsDecorated(true);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setDropIndicatorShown(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setColumnWidth(kColName, 150);
    tree_->setColumnWidth(kColEye, 28);
    tree_->setColumnWidth(kColLock, 28);
    lay->addWidget(tree_, 1);

    auto *bar = new QWidget(root);
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(0, 0, 0, 0);
    auto *addP = new QPushButton(QStringLiteral("+ Layer"), bar);
    addP->setToolTip(QStringLiteral("New layer above current (no dialog)"));
    auto *addG = new QPushButton(QStringLiteral("+ Folder"), bar);
    auto *dup = new QPushButton(QStringLiteral("Duplicate"), bar);
    auto *del = new QPushButton(QStringLiteral("Delete"), bar);
    bl->addWidget(addP); bl->addWidget(addG); bl->addWidget(dup); bl->addWidget(del);
    lay->addWidget(bar);

    // inspector: opacity + blend for current track
    auto *insp = new QWidget(root);
    auto *il = new QVBoxLayout(insp);
    il->setContentsMargins(4, 0, 4, 0);
    il->setSpacing(2);
    auto *sec = new QLabel(QStringLiteral("TRACK"), insp);
    sec->setObjectName(QStringLiteral("Section"));
    il->addWidget(sec);
    auto *opRow = new QWidget(insp);
    auto *ol = new QHBoxLayout(opRow);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->addWidget(new QLabel(QStringLiteral("Opacity"), opRow));
    opacitySlider_ = new QSlider(Qt::Horizontal, opRow);
    opacitySlider_->setRange(0, 100);
    ol->addWidget(opacitySlider_, 1);
    il->addWidget(opRow);
    auto *blRow = new QWidget(insp);
    auto *bll = new QHBoxLayout(blRow);
    bll->setContentsMargins(0, 0, 0, 0);
    bll->addWidget(new QLabel(QStringLiteral("Blend"), blRow));
    blendBox_ = new QComboBox(blRow);
    for (const char *n : {"Normal", "Multiply", "Screen", "Erase"})
        blendBox_->addItem(QString::fromLatin1(n));
    bll->addWidget(blendBox_, 1);
    il->addWidget(blRow);
    lay->addWidget(insp);
    setWidget(root);

    connect(addP, &QPushButton::clicked, this, &LayerDock::onAddPaint);
    connect(addG, &QPushButton::clicked, this, &LayerDock::onAddFolder);
    connect(dup, &QPushButton::clicked, this, &LayerDock::onDuplicate);
    connect(del, &QPushButton::clicked, this, &LayerDock::onRemove);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &LayerDock::onItemChanged);
    connect(tree_, &QTreeWidget::itemChanged, this, &LayerDock::onItemRenamed);
    connect(tree_, &QTreeWidget::itemClicked, this, &LayerDock::onCellClicked);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &LayerDock::onContextMenu);
    connect(tree_->model(), &QAbstractItemModel::rowsMoved, this, &LayerDock::onDropMove);
    connect(opacitySlider_, &QSlider::valueChanged, this, &LayerDock::onOpacitySlider);
    connect(blendBox_, &QComboBox::currentIndexChanged, this, &LayerDock::onBlendChanged);
    refresh();
    tree_->expandAll();
}

QTreeWidgetItem *LayerDock::folderItem(GroupLayer *g) {
    auto *it = new QTreeWidgetItem(tree_);
    it->setText(kColName, QString::fromStdString(g->name()));
    it->setIcon(kColName, InkIcons::get(IconId::Folder, InkTheme::clipColor((int)(g->id() % 8)), 18));
    it->setData(kColName, kRolePtr, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(g)));
    it->setData(kColName, kRoleIsFolder, true);
    it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    it->setForeground(kColName, QBrush(InkTheme::text()));
    return it;
}

QTreeWidgetItem *LayerDock::trackItem(PaintLayer *l, QTreeWidgetItem *parent) {
    auto *it = new QTreeWidgetItem(parent);
    it->setText(kColName, QString::fromStdString(l->name()));
    it->setData(kColName, kRolePtr, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(l)));
    it->setData(kColName, kRoleIsFolder, false);
    it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
    it->setIcon(kColEye, InkIcons::get(l->visible() ? IconId::Eye : IconId::EyeOff, 16));
    it->setIcon(kColLock, InkIcons::get(l->locked() ? IconId::Lock : IconId::Unlock, 16));
    it->setText(kColOp, QString::number(int(l->opacity() * 100)) + QStringLiteral("%"));
    it->setForeground(kColOp, QBrush(InkTheme::dim()));
    it->setTextAlignment(kColOp, Qt::AlignRight);
    updateThumb(it, l);
    return it;
}

void LayerDock::updateThumb(QTreeWidgetItem *it, PaintLayer *l) {
    int key = l->exposureAt(doc_->currentFrame());
    const PaintDevice *d = (key >= 0) ? l->deviceAt(key) : nullptr;
    QImage thumb(40, 28, QImage::Format_RGBA8888);
    thumb.fill(Qt::transparent);
    if (d && !d->extent().isEmpty()) {
        Rect e = d->extent();
        std::vector<uint8_t> buf(size_t(e.w) * size_t(e.h) * 4);
        d->readRect(e, buf.data());
        QImage full(buf.data(), e.w, e.h, QImage::Format_RGBA8888);
        QImage scaled = full.scaled(40, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&thumb);
        p.drawImage((40 - scaled.width()) / 2, (28 - scaled.height()) / 2, scaled);
    }
    QPixmap pm = QPixmap::fromImage(thumb);
    // overlay thumb left of name: combine via decoration icon
    it->setData(kColName, Qt::DecorationRole, QIcon(pm));
}

QString LayerDock::selectedStableId() const {
    auto *cur = tree_->currentItem();
    if (!cur) return {};
    auto *ptr = reinterpret_cast<Layer*>(cur->data(kColName, kRolePtr).value<quintptr>());
    return QString::fromStdString(ptr->stableId());
}

void LayerDock::reselect(const QString &stableId) {
    if (stableId.isEmpty()) return;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem *it) {
        auto *ptr = reinterpret_cast<Layer*>(it->data(kColName, kRolePtr).value<quintptr>());
        if (ptr && QString::fromStdString(ptr->stableId()) == stableId) {
            tree_->setCurrentItem(it);
            return;
        }
        for (int i = 0; i < it->childCount(); ++i) walk(it->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) walk(tree_->topLevelItem(i));
}

void LayerDock::refresh() {
    QString sel = selectedStableId();
    int scroll = tree_->verticalScrollBar()->value();
    syncing_ = true;
    tree_->blockSignals(true);
    tree_->clear();
    std::vector<GroupLayer*> folders;
    doc_->image().collectFolders(folders);
    if (folders.empty()) doc_->ensureFolder();
    doc_->image().collectFolders(folders);
    for (auto *g : folders) {
        QTreeWidgetItem *fi = folderItem(g);
        for (auto &c : g->children())
            if (c->type() == LayerType::Paint)
                trackItem(static_cast<PaintLayer*>(c.get()), fi);
    }
    tree_->expandAll();
    tree_->blockSignals(false);
    syncing_ = false;
    if (!sel.isEmpty()) reselect(sel);
    tree_->verticalScrollBar()->setValue(scroll);
    syncInspector();
}

void LayerDock::onAddFolder() {
    // instant, no dialog (user choice): auto-named, inline-rename after
    GroupLayer *g = doc_->createFolder({});
    refresh();
    // select + start rename so naming stays one gesture
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto *fi = tree_->topLevelItem(i);
        auto *ptr = reinterpret_cast<Layer*>(fi->data(kColName, kRolePtr).value<quintptr>());
        if (ptr == g) { tree_->setCurrentItem(fi); tree_->editItem(fi, kColName); break; }
    }
    emit layersChanged();
}

void LayerDock::onAddPaint() {
    PaintLayer *l = doc_->createTrackAboveCurrent({});
    refresh();
    reselect(QString::fromStdString(l->stableId()));
    emit layersChanged();
}

void LayerDock::onDuplicate() {
    auto *cur = tree_->currentItem();
    if (!cur || cur->data(kColName, kRoleIsFolder).toBool()) return;
    auto *t = reinterpret_cast<PaintLayer*>(cur->data(kColName, kRolePtr).value<quintptr>());
    PaintLayer *d = doc_->duplicateTrack(t);
    refresh();
    if (d) reselect(QString::fromStdString(d->stableId()));
    emit layersChanged();
}

void LayerDock::onRemove() {
    auto *cur = tree_->currentItem();
    if (!cur) return;
    bool isFolder = cur->data(kColName, kRoleIsFolder).toBool();
    auto *ptr = reinterpret_cast<Layer*>(cur->data(kColName, kRolePtr).value<quintptr>());
    if (isFolder) {
        auto *g = static_cast<GroupLayer*>(ptr);
        std::vector<GroupLayer*> folders;
        doc_->image().collectFolders(folders);
        if (folders.size() <= 1) {
            g->children().clear(); // mandatory guard: clear, don't remove
        } else {
            auto &ch = doc_->image().root().children();
            ch.erase(std::remove_if(ch.begin(), ch.end(),
                                    [&](auto &c) { return c.get() == g; }),
                     ch.end());
        }
        doc_->setCurrentFolder(doc_->ensureFolder());
    } else {
        doc_->deleteTrack(static_cast<PaintLayer*>(ptr));
    }
    doc_->frameCache().invalidateAll();
    refresh(); emit layersChanged();
}

void LayerDock::onCellClicked(QTreeWidgetItem *item, int col) {
    if (syncing_ || !item) return;
    auto *ptr = reinterpret_cast<Layer*>(item->data(kColName, kRolePtr).value<quintptr>());
    if (col == kColEye) {
        ptr->setVisible(!ptr->visible());
        doc_->frameCache().invalidateAll();
        refresh(); emit layersChanged();
    } else if (col == kColLock) {
        ptr->setLocked(!ptr->locked());
        refresh(); emit layersChanged();
    }
}

void LayerDock::onItemChanged(QTreeWidgetItem *cur, QTreeWidgetItem *) {
    if (syncing_ || !cur) return;
    bool isFolder = cur->data(kColName, kRoleIsFolder).toBool();
    auto *ptr = reinterpret_cast<Layer*>(cur->data(kColName, kRolePtr).value<quintptr>());
    if (isFolder) {
        doc_->setCurrentFolder(static_cast<GroupLayer*>(ptr));
    } else {
        auto *t = static_cast<PaintLayer*>(ptr);
        doc_->setCurrentLayer(t);
        if (auto *g = doc_->image().findFolderOf(t)) doc_->setCurrentFolder(g);
    }
    syncInspector();
}

void LayerDock::onItemRenamed(QTreeWidgetItem *item, int col) {
    if (syncing_ || !item || col != kColName) return; // only name column edits
    QString name = item->text(kColName).trimmed();
    auto *ptr = reinterpret_cast<Layer*>(item->data(kColName, kRolePtr).value<quintptr>());
    if (!name.isEmpty()) ptr->setName(name.toStdString());
    emit layersChanged();
}

void LayerDock::onDropMove() {
    // Full rebuild from UI order: folders in top-level order, tracks in
    // child order (fixes old code that ignored intra-folder reorder).
    // Keep shared_ptrs alive across the rebuild.
    std::vector<std::pair<GroupLayer*, std::vector<PaintLayer*>>> uiOrder;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto *fi = tree_->topLevelItem(i);
        if (fi->data(kColName, kRoleIsFolder).toBool()) {
            auto *g = reinterpret_cast<GroupLayer*>(fi->data(kColName, kRolePtr).value<quintptr>());
            std::vector<PaintLayer*> ts;
            for (int j = 0; j < fi->childCount(); ++j) {
                auto *ci = fi->child(j);
                if (!ci->data(kColName, kRoleIsFolder).toBool())
                    ts.push_back(reinterpret_cast<PaintLayer*>(ci->data(kColName, kRolePtr).value<quintptr>()));
            }
            uiOrder.push_back({g, ts});
        }
    }
    // pin all paint layers
    std::map<PaintLayer*, std::shared_ptr<Layer>> pinned;
    {
        std::vector<PaintLayer*> all;
        doc_->image().collectPaintLayers(all);
        std::vector<GroupLayer*> gs;
        doc_->image().collectFolders(gs);
        for (auto *g : gs)
            for (auto &c : g->children())
                if (c->type() == LayerType::Paint)
                    pinned[static_cast<PaintLayer*>(c.get())] = c;
    }
    // re-attach in UI order (cross-folder moves included)
    for (auto &[g, ts] : uiOrder) {
        g->children().clear();
        for (auto *t : ts) {
            auto it = pinned.find(t);
            if (it != pinned.end() && it->second) {
                g->addChild(it->second);
                it->second.reset();
            }
        }
    }
    // any unplaced (shouldn't happen) go back to first folder
    {
        std::vector<GroupLayer*> gs;
        doc_->image().collectFolders(gs);
        GroupLayer *dst = gs.empty() ? doc_->ensureFolder() : gs.front();
        for (auto &[t, sh] : pinned)
            if (sh) dst->addChild(sh);
    }
    // root folder order follows UI
    {
        std::map<GroupLayer*, std::shared_ptr<Layer>> pinnedF;
        auto &root = doc_->image().root().children();
        for (auto &c : root)
            if (c->type() == LayerType::Group)
                pinnedF[static_cast<GroupLayer*>(c.get())] = c;
        root.clear();
        for (auto &[g, ts] : uiOrder) {
            (void)ts;
            auto it = pinnedF.find(g);
            if (it != pinnedF.end() && it->second) { root.push_back(it->second); it->second.reset(); }
        }
        for (auto &[g, sh] : pinnedF)
            if (sh) root.push_back(sh);
    }
    doc_->frameCache().invalidateAll();
    refresh(); emit layersChanged();
}

void LayerDock::onContextMenu(const QPoint &pos) {
    auto *item = tree_->itemAt(pos);
    if (!item) return;
    bool isFolder = item->data(kColName, kRoleIsFolder).toBool();
    auto *ptr = reinterpret_cast<Layer*>(item->data(kColName, kRolePtr).value<quintptr>());
    QMenu m;
    if (isFolder) {
        auto *g = static_cast<GroupLayer*>(ptr);
        m.addAction(QStringLiteral("New layer here"), this, [this, g] {
            doc_->setCurrentFolder(g);
            onAddPaint();
        });
        m.addAction(QStringLiteral("Rename"), this, [this, item] { tree_->editItem(item, kColName); });
        QAction *v = m.addAction(g->visible() ? QStringLiteral("Hide folder") : QStringLiteral("Show folder"));
        connect(v, &QAction::triggered, this, [this, g] {
            g->setVisible(!g->visible()); doc_->frameCache().invalidateAll(); refresh(); emit layersChanged();
        });
        m.exec(tree_->viewport()->mapToGlobal(pos));
        return;
    }
    auto *t = static_cast<PaintLayer*>(ptr);
    m.addAction(QStringLiteral("New layer above"), this, [this, t] {
        doc_->setCurrentLayer(t);
        if (auto *g = doc_->image().findFolderOf(t)) doc_->setCurrentFolder(g);
        onAddPaint();
    });
    m.addAction(QStringLiteral("Duplicate"), this, [this, t] { doc_->duplicateTrack(t); refresh(); emit layersChanged(); });
    m.addAction(QStringLiteral("Rename"), this, [this, item] { tree_->editItem(item, kColName); });
    m.addSeparator();
    QAction *v = m.addAction(t->visible() ? QStringLiteral("Hide") : QStringLiteral("Show"));
    connect(v, &QAction::triggered, this, [this, t] {
        t->setVisible(!t->visible()); doc_->frameCache().invalidateAll(); refresh(); emit layersChanged();
    });
    QAction *lk = m.addAction(t->locked() ? QStringLiteral("Unlock") : QStringLiteral("Lock"));
    connect(lk, &QAction::triggered, this, [this, t] { t->setLocked(!t->locked()); refresh(); emit layersChanged(); });
    m.addSeparator();
    m.addAction(QStringLiteral("Delete"), this, [this, t] { doc_->deleteTrack(t); refresh(); emit layersChanged(); });
    m.exec(tree_->viewport()->mapToGlobal(pos));
}

void LayerDock::syncInspector() {
    opacitySlider_->blockSignals(true);
    blendBox_->blockSignals(true);
    PaintLayer *l = doc_->currentLayer();
    opacitySlider_->setEnabled(l != nullptr);
    blendBox_->setEnabled(l != nullptr);
    if (l) {
        opacitySlider_->setValue(int(l->opacity() * 100));
        blendBox_->setCurrentIndex(int(l->blend()));
    }
    opacitySlider_->blockSignals(false);
    blendBox_->blockSignals(false);
}

void LayerDock::onOpacitySlider(int v) {
    PaintLayer *l = doc_->currentLayer();
    if (!l) return;
    l->setOpacity(v / 100.0f);
    doc_->frameCache().invalidateAll();
    // light refresh of the opacity cell without losing selection
    refresh();
    emit layersChanged();
}

void LayerDock::onBlendChanged(int idx) {
    PaintLayer *l = doc_->currentLayer();
    if (!l) return;
    l->setBlend(BlendMode(idx));
    doc_->frameCache().invalidateAll();
    emit layersChanged();
}

} // namespace ink
