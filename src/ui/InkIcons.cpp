#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

using PaintFn = void (*)(QPainter &, const QRectF &, const QColor &);

void p_brush(QPainter &p, const QRectF &r, const QColor &c) {
    // diagonal brush: handle line + ferrule + tip triangle
    p.save();
    p.translate(r.center());
    p.rotate(-45);
    double w = r.width(), h = r.height();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(-w * 0.08, -h * 0.46, w * 0.16, h * 0.42), 2, 2); // handle
    p.setBrush(c.darker(130));
    p.drawRect(QRectF(-w * 0.11, -h * 0.06, w * 0.22, h * 0.12)); // ferrule
    QPainterPath tip;
    tip.moveTo(-w * 0.11, h * 0.06);
    tip.lineTo(w * 0.11, h * 0.06);
    tip.lineTo(0, h * 0.46);
    tip.closeSubpath();
    p.fillPath(tip, c);
    p.restore();
}

void p_eraser(QPainter &p, const QRectF &r, const QColor &c) {
    p.save();
    p.translate(r.center());
    p.rotate(-20);
    double w = r.width(), h = r.height();
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(c);
    p.setOpacity(0.92);
    QPainterPath body;
    body.moveTo(-w * 0.34, -h * 0.05);
    body.lineTo(w * 0.05, -h * 0.05);
    body.lineTo(w * 0.34, h * 0.24);
    body.lineTo(-w * 0.05, h * 0.24);
    body.closeSubpath();
    p.fillPath(body, c);
    p.drawPath(body);
    p.restore();
}

void p_shape(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r.adjusted(2, 2, -2, -2));
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(r.adjusted(2, 2, -2, -2).topLeft() + QPointF(-1, -1), 2.4, 2.4);
}

void p_picker(QPainter &p, const QRectF &r, const QColor &c) {
    // eyedropper: bulb circle + stem + drop
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(r.center().x() - 2, r.center().y() - 4), 5, 5);
    p.drawLine(QPointF(r.center().x() + 2, r.center().y()), QPointF(r.center().x() + 6, r.center().y() + 7));
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    QPainterPath drop;
    QPointF dc(r.center().x() + 6, r.center().y() + 5);
    drop.moveTo(dc.x(), dc.y() - 4);
    drop.lineTo(dc.x() + 3, dc.y() + 2);
    drop.quadTo(dc.x(), dc.y() + 5, dc.x() - 3, dc.y() + 2);
    drop.closeSubpath();
    p.fillPath(drop, c);
}

void p_text(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(r.left() + 3, r.top() + 3), QPointF(r.right() - 3, r.top() + 3));
    p.drawLine(QPointF(r.center().x(), r.top() + 3), QPointF(r.center().x(), r.bottom() - 3));
    p.drawLine(QPointF(r.center().x() - 4, r.bottom() - 3), QPointF(r.center().x() + 4, r.bottom() - 3));
}

void p_note(QPainter &p, const QRectF &r, const QColor &c) {
    QRectF b = r.adjusted(3, 4, -3, -5);
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(b, 3, 3);
    QPainterPath tail;
    tail.moveTo(b.left() + 4, b.bottom());
    tail.lineTo(b.left() + 4, b.bottom() + 4);
    tail.lineTo(b.left() + 9, b.bottom());
    tail.closeSubpath();
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.fillPath(tail, c);
    p.setBrush(c);
    p.drawEllipse(QPointF(b.center().x() - 3, b.center().y()), 1.3, 1.3);
    p.drawEllipse(QPointF(b.center().x() + 3, b.center().y()), 1.3, 1.3);
}

void p_select(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 1.8, Qt::DashLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(3, 3, -3, -3));
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::SquareCap));
    for (auto pt : {r.adjusted(3, 3, -3, -3).topLeft(), r.adjusted(3, 3, -3, -3).topRight(),
                    r.adjusted(3, 3, -3, -3).bottomLeft(), r.adjusted(3, 3, -3, -3).bottomRight()}) {
        p.drawPoint(pt);
    }
}

void p_eye(QPainter &p, const QRectF &r, const QColor &c) {
    QPainterPath eye;
    QPointF ctr = r.center();
    eye.moveTo(ctr.x() - 8, ctr.y());
    eye.quadTo(ctr.x(), ctr.y() - 8, ctr.x() + 8, ctr.y());
    eye.quadTo(ctr.x(), ctr.y() + 8, ctr.x() - 8, ctr.y());
    eye.closeSubpath();
    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(eye);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(ctr, 2.6, 2.6);
}

void p_eyeOff(QPainter &p, const QRectF &r, const QColor &c) {
    p_eye(p, r, c);
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(r.adjusted(3, 3, -3, -3).bottomLeft(), r.adjusted(3, 3, -3, -3).topRight());
}

void p_lock(QPainter &p, const QRectF &r, const QColor &c) {
    QRectF body(r.center().x() - 5, r.center().y() - 1, 10, 8);
    p.setPen(QPen(c, 1.8));
    p.setBrush(c);
    p.drawRoundedRect(body, 2, 2);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(r.center().x() - 3.5, r.center().y() - 7, 7, 7), 0, 180 * 16);
}

void p_unlock(QPainter &p, const QRectF &r, const QColor &c) {
    QRectF body(r.center().x() - 5, r.center().y() - 1, 10, 8);
    p.setPen(QPen(c, 1.8));
    p.setBrush(c);
    p.drawRoundedRect(body, 2, 2);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(r.center().x() - 3.5, r.center().y() - 7, 7, 7), 20 * 16, 140 * 16);
}

void p_folder(QPainter &p, const QRectF &r, const QColor &c) {
    QPainterPath f;
    QRectF b = r.adjusted(2, 5, -2, -3);
    f.moveTo(b.left(), b.top() + 3);
    f.lineTo(b.left(), b.bottom() - 2);
    f.quadTo(b.left(), b.bottom(), b.left() + 2, b.bottom());
    f.lineTo(b.right() - 2, b.bottom());
    f.quadTo(b.right(), b.bottom(), b.right(), b.bottom() - 2);
    f.lineTo(b.right(), b.top() + 3);
    f.quadTo(b.right(), b.top() + 1, b.right() - 2, b.top() + 1);
    f.lineTo(b.left() + 7, b.top() + 1);
    f.lineTo(b.left() + 5, b.top() - 2);
    f.quadTo(b.left() + 5, b.top() - 3, b.left() + 6, b.top() - 3);
    f.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.fillPath(f, c);
}

void p_play(QPainter &p, const QRectF &r, const QColor &c) {
    QPainterPath t;
    t.moveTo(r.center().x() - 4, r.center().y() - 6);
    t.lineTo(r.center().x() + 6, r.center().y());
    t.lineTo(r.center().x() - 4, r.center().y() + 6);
    t.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.fillPath(t, c);
}

void p_pause(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(r.center().x() - 5, r.center().y() - 6, 3.6, 12), 1, 1);
    p.drawRoundedRect(QRectF(r.center().x() + 1.5, r.center().y() - 6, 3.6, 12), 1, 1);
}

void p_prevKey(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(r.center().x() - 5, r.center().y()), QPointF(r.center().x() + 1, r.center().y() - 5));
    p.drawLine(QPointF(r.center().x() - 5, r.center().y()), QPointF(r.center().x() + 1, r.center().y() + 5));
    p.drawLine(QPointF(r.center().x() + 5, r.center().y() - 6), QPointF(r.center().x() + 5, r.center().y() + 6));
}

void p_nextKey(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(r.center().x() + 5, r.center().y()), QPointF(r.center().x() - 1, r.center().y() - 5));
    p.drawLine(QPointF(r.center().x() + 5, r.center().y()), QPointF(r.center().x() - 1, r.center().y() + 5));
    p.drawLine(QPointF(r.center().x() - 5, r.center().y() - 6), QPointF(r.center().x() - 5, r.center().y() + 6));
}

void p_plus(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(r.center().x(), r.center().y() - 6), QPointF(r.center().x(), r.center().y() + 6));
    p.drawLine(QPointF(r.center().x() - 6, r.center().y()), QPointF(r.center().x() + 6, r.center().y()));
}

void p_minus(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(r.center().x() - 6, r.center().y()), QPointF(r.center().x() + 6, r.center().y()));
}

void p_check(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF({QPointF(r.center().x() - 6, r.center().y() + 1),
                              QPointF(r.center().x() - 1.5, r.center().y() + 5),
                              QPointF(r.center().x() + 6, r.center().y() - 5)}));
}

void p_close(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(r.adjusted(5, 5, -5, -5).topLeft(), r.adjusted(5, 5, -5, -5).bottomRight());
    p.drawLine(r.adjusted(5, 5, -5, -5).topRight(), r.adjusted(5, 5, -5, -5).bottomLeft());
}

void p_onion(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r.adjusted(2, 4, -8, -4));
    p.setOpacity(0.55);
    p.drawEllipse(r.adjusted(6, 4, -4, -4));
    p.setOpacity(1.0);
}

void p_loop(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    QRectF b = r.adjusted(3, 6, -3, -6);
    p.drawArc(b, 40 * 16, 260 * 16);
    QPainterPath head;
    QPointF tip(b.right() - 1, b.center().y() - 4);
    head.moveTo(tip);
    head.lineTo(tip + QPointF(-4, -1));
    head.lineTo(tip + QPointF(-1, 4));
    head.closeSubpath();
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.fillPath(head, c);
}

void p_undo(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath arc;
    arc.moveTo(r.center().x() + 6, r.center().y() - 5);
    arc.quadTo(r.center().x() - 6, r.center().y() - 5, r.center().x() - 6, r.center().y() + 2);
    arc.lineTo(r.center().x() + 2, r.center().y() + 2);
    p.drawPath(arc);
    QPainterPath head;
    head.moveTo(r.center().x() - 6, r.center().y() + 2);
    head.lineTo(r.center().x() - 2, r.center().y() - 1);
    head.lineTo(r.center().x() - 2, r.center().y() + 5);
    head.closeSubpath();
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.fillPath(head, c);
}

void p_redo(QPainter &p, const QRectF &r, const QColor &c) {
    p.save();
    p.translate(r.center());
    p.scale(-1, 1);
    p.translate(-r.center());
    p_undo(p, r, c);
    p.restore();
}

void p_menu(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    for (double dy : {-4.5, 0.0, 4.5})
        p.drawLine(QPointF(r.center().x() - 7, r.center().y() + dy),
                   QPointF(r.center().x() + 7, r.center().y() + dy));
}

void p_fit(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QRectF b = r.adjusted(4, 4, -4, -4);
    p.drawRect(b);
    // corner ticks pulling inward: "fit to view"
    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
    const double t = 4.5;
    for (auto [p1, p2] : {std::pair<QPointF, QPointF>{{b.left(), b.top()}, {b.left() + t, b.top() + t}},
                          {{b.right(), b.top()}, {b.right() - t, b.top() + t}},
                          {{b.left(), b.bottom()}, {b.left() + t, b.bottom() - t}},
                          {{b.right(), b.bottom()}, {b.right() - t, b.bottom() - t}}}) {
        p.drawLine(p1, p2);
    }
}

void p_key(QPainter &p, const QRectF &r, const QColor &c) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    QPointF ctr = r.center();
    QPainterPath d;
    d.moveTo(ctr.x(), ctr.y() - 6);
    d.lineTo(ctr.x() + 5, ctr.y());
    d.lineTo(ctr.x(), ctr.y() + 6);
    d.lineTo(ctr.x() - 5, ctr.y());
    d.closeSubpath();
    p.fillPath(d, c);
}

void p_folderOpen(QPainter &p, const QRectF &r, const QColor &c) {
    QPainterPath f;
    QRectF b = r.adjusted(2, 6, -2, -3);
    f.moveTo(b.left(), b.top() + 3);
    f.lineTo(b.left(), b.bottom() - 2);
    f.quadTo(b.left(), b.bottom(), b.left() + 2, b.bottom());
    f.lineTo(b.right() - 3, b.bottom());
    f.lineTo(b.right(), b.top() + 5);
    f.lineTo(b.left() + 5, b.top() + 5);
    f.lineTo(b.left() + 3, b.top() - 1);
    f.quadTo(b.left() + 3, b.top() - 2, b.left() + 4, b.top() - 2);
    f.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.fillPath(f, c);
}

void p_skipStart(QPainter &p, const QRectF &r, const QColor &c) {
    QPointF ctr = r.center();
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(ctr.x() - 6, ctr.y() - 5.5), QPointF(ctr.x() - 6, ctr.y() + 5.5));
    for (double o : {0.0, 4.5}) {
        p.drawLine(QPointF(ctr.x() + 1.5 + o, ctr.y() - 5), QPointF(ctr.x() - 3.5 + o, ctr.y()));
        p.drawLine(QPointF(ctr.x() + 1.5 + o, ctr.y() + 5), QPointF(ctr.x() - 3.5 + o, ctr.y()));
    }
}

void p_skipEnd(QPainter &p, const QRectF &r, const QColor &c) {
    p.save();
    p.translate(r.center());
    p.scale(-1, 1);
    p.translate(-r.center());
    p_skipStart(p, r, c);
    p.restore();
}

void p_once(QPainter &p, const QRectF &r, const QColor &c) {
    QPointF ctr = r.center();
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(ctr.x() - 7, ctr.y() - 5), QPointF(ctr.x() + 4, ctr.y() - 5));
    p.drawLine(QPointF(ctr.x() + 4, ctr.y() - 5), QPointF(ctr.x() + 4, ctr.y() + 5));
    p.drawLine(QPointF(ctr.x() + 4, ctr.y() + 5), QPointF(ctr.x() - 7, ctr.y() + 5));
    QPainterPath head;
    head.moveTo(ctr.x() - 7, ctr.y() + 5);
    head.lineTo(ctr.x() - 3, ctr.y() + 1.5);
    head.lineTo(ctr.x() - 3, ctr.y() + 8.5);
    head.closeSubpath();
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.fillPath(head, c);
}

void p_pingPong(QPainter &p, const QRectF &r, const QColor &c) {
    QPointF ctr = r.center();
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(ctr.x() - 6, ctr.y() - 5), QPointF(ctr.x() + 6, ctr.y() - 5));
    p.drawLine(QPointF(ctr.x() - 6, ctr.y() + 5), QPointF(ctr.x() + 6, ctr.y() + 5));
    for (double s : {-1.0, 1.0}) {
        QPainterPath head;
        head.moveTo(ctr.x() + 6 * s, ctr.y());
        head.lineTo(ctr.x() + 1.5 * s, ctr.y() - 3.5);
        head.lineTo(ctr.x() + 1.5 * s, ctr.y() + 3.5);
        head.closeSubpath();
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.fillPath(head, c);
    }
}

void p_followHead(QPainter &p, const QRectF &r, const QColor &c) {
    QPointF ctr = r.center();
    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(ctr.x(), ctr.y() - 7), QPointF(ctr.x(), ctr.y() + 7));
    QPainterPath head;
    head.moveTo(ctr.x() - 4, ctr.y() - 6.5);
    head.lineTo(ctr.x() + 4, ctr.y() - 6.5);
    head.lineTo(ctr.x(), ctr.y() - 1.5);
    head.closeSubpath();
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.fillPath(head, c);
}

// --- cells: the paper-sheet family (folded corner) ---
void p_sheetShape(QPainter &p, const QRectF &b, const QColor &c, double pen) {
    const double f = std::min(b.width(), b.height()) * 0.42;
    QPainterPath path;
    path.moveTo(b.left() + 0.8, b.top());
    path.lineTo(b.right() - f, b.top());
    path.lineTo(b.right(), b.top() + f);
    path.lineTo(b.right(), b.bottom() - 0.8);
    path.lineTo(b.left() + 0.8, b.bottom() - 0.8);
    path.closeSubpath();
    p.setPen(QPen(c, pen, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.drawLine(QPointF(b.right() - f, b.top()), QPointF(b.right() - f, b.top() + f));
    p.drawLine(QPointF(b.right() - f, b.top() + f), QPointF(b.right(), b.top() + f));
}

void p_cell(QPainter &p, const QRectF &r, const QColor &c) {
    p_sheetShape(p, r.adjusted(3, 2, -3, -2), c, 1.6);
}

void p_cellPlus(QPainter &p, const QRectF &r, const QColor &c, bool minus) {
    const double w = r.width(), h = r.height();
    p_sheetShape(p, QRectF(r.left() + 1, r.top() + 1, w * 0.68, h * 0.68), c, 1.5);
    QPointF m(r.right() - w * 0.15, r.bottom() - h * 0.15);
    const double a = w * 0.19;
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(m.x() - a, m.y()), QPointF(m.x() + a, m.y()));
    if (!minus) p.drawLine(QPointF(m.x(), m.y() - a), QPointF(m.x(), m.y() + a));
}

void p_cellDuplicate(QPainter &p, const QRectF &r, const QColor &c) {
    const double w = r.width(), h = r.height();
    p_sheetShape(p, QRectF(r.left() + 1, r.top() + 1, w * 0.58, h * 0.58), c, 1.4);
    p_sheetShape(p, QRectF(r.left() + w * 0.34, r.top() + h * 0.34, w * 0.58, h * 0.58), c, 1.5);
}

void p_layerPlus(QPainter &p, const QRectF &r, const QColor &c) {
    const double w = r.width(), h = r.height();
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const double bh = h * 0.19;
    for (int i = 0; i < 3; ++i) {
        QRectF slab(r.left() + 1.5 + i * 1.2, r.top() + 1.5 + i * (bh + 1.6),
                    w * 0.62 - i * 1.2, bh);
        p.drawRoundedRect(slab, 1.2, 1.2);
    }
    QPointF m(r.right() - w * 0.2, r.bottom() - h * 0.22);
    const double a = w * 0.2;
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(m.x() - a, m.y()), QPointF(m.x() + a, m.y()));
    p.drawLine(QPointF(m.x(), m.y() - a), QPointF(m.x(), m.y() + a));
}

void p_folderBadge(QPainter &p, const QRectF &r, const QColor &c, bool plus, bool arrowOut) {
    const double w = r.width(), h = r.height();
    QRectF b(r.left() + 1, r.top() + h * 0.34, w * 0.64, h * 0.44);
    QPainterPath f;
    f.moveTo(b.left(), b.top() + 2);
    f.lineTo(b.left(), b.bottom() - 1);
    f.quadTo(b.left(), b.bottom(), b.left() + 1.5, b.bottom());
    f.lineTo(b.right() - 1.5, b.bottom());
    f.quadTo(b.right(), b.bottom(), b.right(), b.bottom() - 1.5);
    f.lineTo(b.right(), b.top() + 2);
    f.quadTo(b.right(), b.top(), b.right() - 2, b.top());
    f.lineTo(b.left() + 4.5, b.top());
    f.lineTo(b.left() + 3, b.top() - 2);
    f.quadTo(b.left() + 3, b.top() - 2.6, b.left() + 3.8, b.top() - 2.6);
    f.closeSubpath();
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(f);
    const QPointF m(r.right() - w * 0.21, r.bottom() - h * 0.21);
    const double a = w * 0.19;
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (plus) {
        p.drawLine(QPointF(m.x() - a, m.y()), QPointF(m.x() + a, m.y()));
        p.drawLine(QPointF(m.x(), m.y() - a), QPointF(m.x(), m.y() + a));
    } else {
        const double dy = arrowOut ? -a : a;
        p.drawLine(QPointF(m.x(), m.y() + dy), QPointF(m.x(), m.y() - dy));
        p.drawLine(QPointF(m.x(), m.y() - dy), QPointF(m.x() - a * 0.8, m.y() - dy + (arrowOut ? a * 0.8 : -a * 0.8)));
        p.drawLine(QPointF(m.x(), m.y() - dy), QPointF(m.x() + a * 0.8, m.y() - dy + (arrowOut ? a * 0.8 : -a * 0.8)));
    }
}

void p_groupPlus(QPainter &p, const QRectF &r, const QColor &c) { p_folderBadge(p, r, c, true, false); }
void p_groupMerge(QPainter &p, const QRectF &r, const QColor &c) { p_folderBadge(p, r, c, false, false); }
void p_ungroup(QPainter &p, const QRectF &r, const QColor &c) { p_folderBadge(p, r, c, false, true); }

PaintFn fnFor(IconId id) {
    switch (id) {
    case IconId::Brush: return p_brush;
    case IconId::Eraser: return p_eraser;
    case IconId::Shape: return p_shape;
    case IconId::Picker: return p_picker;
    case IconId::Text: return p_text;
    case IconId::Note: return p_note;
    case IconId::Select: return p_select;
    case IconId::Eye: return p_eye;
    case IconId::EyeOff: return p_eyeOff;
    case IconId::Lock: return p_lock;
    case IconId::Unlock: return p_unlock;
    case IconId::Folder: return p_folder;
    case IconId::FolderOpen: return p_folderOpen;
    case IconId::Play: return p_play;
    case IconId::Pause: return p_pause;
    case IconId::PrevKey: return p_prevKey;
    case IconId::NextKey: return p_nextKey;
    case IconId::SkipStart: return p_skipStart;
    case IconId::SkipEnd: return p_skipEnd;
    case IconId::Plus: return p_plus;
    case IconId::Minus: return p_minus;
    case IconId::Close: return p_close;
    case IconId::Check: return p_check;
    case IconId::Onion: return p_onion;
    case IconId::Loop: return p_loop;
    case IconId::Once: return p_once;
    case IconId::PingPong: return p_pingPong;
    case IconId::Undo: return p_undo;
    case IconId::Redo: return p_redo;
    case IconId::Menu: return p_menu;
    case IconId::Fit: return p_fit;
    case IconId::Cell: return p_cell;
    case IconId::CellDuplicate: return p_cellDuplicate;
    case IconId::LayerPlus: return p_layerPlus;
    case IconId::GroupPlus: return p_groupPlus;
    case IconId::GroupMerge: return p_groupMerge;
    case IconId::Ungroup: return p_ungroup;
    case IconId::FollowHead: return p_followHead;
    case IconId::Key: return p_key;
    default: return p_plus;
    }
}

QPixmap render(IconId id, const QColor &color, int size) {
    QPixmap px(size, size);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF r(1, 1, size - 2, size - 2);
    if (id == IconId::CellPlus) {
        p_cellPlus(p, r, color, false);
    } else if (id == IconId::CellRemove) {
        p_cellPlus(p, r, color, true);
    } else {
        fnFor(id)(p, r, color);
    }
    return px;
}

} // namespace

QIcon InkIcons::get(IconId id, int size, bool active) {
    QColor c = active ? InkTheme::accent() : InkTheme::text();
    return get(id, c, size);
}

QIcon InkIcons::get(IconId id, const QColor &color, int size) {
    QIcon icon;
    icon.addPixmap(render(id, color, size), QIcon::Normal, QIcon::Off);
    QColor dis = color;
    dis.setAlpha(110);
    icon.addPixmap(render(id, dis, size), QIcon::Disabled, QIcon::Off);
    if (id == IconId::Play || id == IconId::Pause || id == IconId::Brush ||
        id == IconId::Eraser || id == IconId::Shape || id == IconId::Picker ||
        id == IconId::Text || id == IconId::Note || id == IconId::Select ||
        id == IconId::Onion) {
        icon.addPixmap(render(id, InkTheme::accent(), size), QIcon::Normal, QIcon::On);
    }
    return icon;
}

} // namespace ink
