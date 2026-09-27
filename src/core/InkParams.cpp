#include "core/InkParams.h"
#include <cmath>
#include <cstring>

namespace ink {

namespace {

const ParamDesc kTable[kParamCount] = {
    {ParamId::AnchorX, "anchorX", "Anchor X", "", 0.5, -4.0, 5.0, false, ParamGroup::Transform},
    {ParamId::AnchorY, "anchorY", "Anchor Y", "", 0.5, -4.0, 5.0, false, ParamGroup::Transform},
    {ParamId::PositionX, "posX", "Position X", "px", 0.0, -20000.0, 20000.0, false,
     ParamGroup::Transform},
    {ParamId::PositionY, "posY", "Position Y", "px", 0.0, -20000.0, 20000.0, false,
     ParamGroup::Transform},
    {ParamId::ScaleX, "scaleX", "Scale X", "%", 1.0, -20.0, 20.0, false, ParamGroup::Transform},
    {ParamId::ScaleY, "scaleY", "Scale Y", "%", 1.0, -20.0, 20.0, false, ParamGroup::Transform},
    {ParamId::Rotation, "rotation", "Rotation", "deg", 0.0, -3600.0, 3600.0, true,
     ParamGroup::Transform},
    {ParamId::ShearX, "shearX", "Shear X", "", 0.0, -5.0, 5.0, false, ParamGroup::Transform},
    {ParamId::ShearY, "shearY", "Shear Y", "", 0.0, -5.0, 5.0, false, ParamGroup::Transform},
    {ParamId::Opacity, "opacity", "Opacity", "%", 1.0, 0.0, 1.0, false, ParamGroup::Transform},
    {ParamId::Corner0X, "corner0X", "Corner 1 X", "", 0.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner0Y, "corner0Y", "Corner 1 Y", "", 0.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner1X, "corner1X", "Corner 2 X", "", 1.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner1Y, "corner1Y", "Corner 2 Y", "", 0.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner2X, "corner2X", "Corner 3 X", "", 1.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner2Y, "corner2Y", "Corner 3 Y", "", 1.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner3X, "corner3X", "Corner 4 X", "", 0.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
    {ParamId::Corner3Y, "corner3Y", "Corner 4 Y", "", 1.0, -10.0, 11.0, false,
     ParamGroup::Perspective},
};

const ParamId kCornerIds[4][2] = {
    {ParamId::Corner0X, ParamId::Corner0Y},
    {ParamId::Corner1X, ParamId::Corner1Y},
    {ParamId::Corner2X, ParamId::Corner2Y},
    {ParamId::Corner3X, ParamId::Corner3Y},
};

constexpr double kEps = 1e-9;

} // namespace

const ParamDesc *paramTable() { return kTable; }

const ParamDesc *paramFromKey(const std::string &key) {
    for (int i = 0; i < kParamCount; ++i)
        if (key == kTable[i].key) return &kTable[i];
    return nullptr;
}

const std::vector<ParamId> &defaultKeySet() {
    // The WHOLE transform: every Add-keyframe must carry the full pose,
    // including shear and the perspective corners. A partial set is what used
    // to freeze perspective between two keys — the corners had a key at one
    // end only, and valueAt() holds a lone key backwards, so nothing moved.
    // Rest-pose keys stay free: the compositor skips identity frames.
    static const std::vector<ParamId> kSet = {
        ParamId::AnchorX, ParamId::AnchorY,     ParamId::PositionX, ParamId::PositionY,
        ParamId::ScaleX,  ParamId::ScaleY,      ParamId::Rotation,  ParamId::ShearX,
        ParamId::ShearY,  ParamId::Opacity,     ParamId::Corner0X,  ParamId::Corner0Y,
        ParamId::Corner1X, ParamId::Corner1Y,   ParamId::Corner2X,  ParamId::Corner2Y,
        ParamId::Corner3X, ParamId::Corner3Y,
    };
    return kSet;
}

// --- keys --------------------------------------------------------------------

bool ParamBlock::setKey(ParamId id, int frame, double value, Interp interp, bool updateExisting,
                        bool deriveHandles) {
    ScalarChannel &ch = ch_[idx(id)];
    const bool existed = ch.hasKey(frame);
    if (existed && !updateExisting) return false;
    ch.setKey(frame, float(value), interp);
    if (!existed) {
        ch.setAutoBez(frame, true);
        if (deriveHandles && interp == Interp::Bezier) refreshAutoHandlesNear(id, frame);
    }
    return true;
}

bool ParamBlock::removeKey(ParamId id, int frame) {
    ScalarChannel &ch = ch_[idx(id)];
    if (!ch.hasKey(frame)) return false;
    // The neighbours' tangents were shaped by the key that just left; only
    // the two keys beside the gap can change (their neighbour set changed).
    const int p = ch.prevKey(frame);
    const int n = ch.nextKey(frame);
    const bool ok = ch.removeKey(frame);
    if (!ok) return false;
    // (Empty shells stay in place; every query treats them as absent.)
    refreshOneKey(id, p);
    refreshOneKey(id, n);
    return true;
}

int ParamBlock::removeKeysAt(int frame) {
    std::vector<ParamId> ids = keysAt(frame);
    for (ParamId id : ids) removeKey(id, frame);
    return (int)ids.size();
}

bool ParamBlock::hasKey(ParamId id, int frame) const {
    return ch_[idx(id)].hasKey(frame);
}

bool ParamBlock::hasAnyKeyAt(int frame) const { return !keysAt(frame).empty(); }

std::vector<ParamId> ParamBlock::keysAt(int frame) const {
    std::vector<ParamId> out;
    for (int i = 0; i < kParamCount; ++i) {
        const ParamId id = (ParamId)i;
        if (hasKey(id, frame)) out.push_back(id);
    }
    return out;
}

bool ParamBlock::moveKey(ParamId id, int from, int to) {
    ScalarChannel &ch = ch_[idx(id)];
    if (!ch.hasKey(from)) return false;
    if (from == to) return true;
    // Union of the removal neighbourhood (oldPrev/oldNext become adjacent)
    // and the insertion neighbourhood (newPrev/newNext gain a neighbour).
    // prevKey/nextKey skip the endpoints themselves, so capturing them up
    // front stays valid past the occupant removal and the move; frames that
    // no longer exist are skipped by refreshOneKey.
    const int oldP = ch.prevKey(from), oldN = ch.nextKey(from);
    if (ch.hasKey(to)) ch.removeKey(to);
    const int newP = ch.prevKey(to), newN = ch.nextKey(to);
    ch.moveKey(from, to);
    refreshOneKey(id, oldP);
    refreshOneKey(id, oldN);
    refreshOneKey(id, newP);
    refreshOneKey(id, newN);
    refreshOneKey(id, to);
    return true;
}

bool ParamBlock::setKeyValue(ParamId id, int frame, double value) {
    ScalarChannel &ch = ch_[idx(id)];
    const ScalarKey *k0 = ch.keyAt(frame);
    if (!k0) return false;
    ScalarKey k = *k0;
    ch.setKey(frame, float(value), k.interp);
    ch.setHandles(frame, k.outInfl, k.outSpeed, k.inInfl, k.inSpeed);
    ch.setAutoBez(frame, k.autoBez);
    // A value change moves the neighbours' auto tangents too (their chord
    // runs through this value); the key's own tangent only reads its
    // neighbours, but recomputing it is harmless and keeps one code path.
    refreshAutoHandlesNear(id, frame);
    return true;
}

bool ParamBlock::setInterp(ParamId id, int frame, Interp i) {
    ScalarChannel &ch = ch_[idx(id)];
    if (!ch.setInterp(frame, i)) return false;
    // Speeds never depend on interp, but a key flipped TO Bezier becomes due
    // for its auto tangent (a Linear-born key still holds zero speeds).
    refreshOneKey(id, frame);
    return true;
}

bool ParamBlock::setHandles(ParamId id, int frame, double outInfl, double outSpeed, double inInfl,
                            double inSpeed) {
    ScalarChannel &ch = ch_[idx(id)];
    if (!ch.setHandles(frame, outInfl, outSpeed, inInfl, inSpeed)) return false;
    // Hand-setting a handle makes it FREE: from here on it is the user's shape,
    // not the neighbours'.
    ch.setAutoBez(frame, false);
    return true;
}

bool ParamBlock::autoHandles(ParamId id, int frame) {
    ScalarChannel &ch = ch_[idx(id)];
    if (!ch.autoHandles(frame)) return false;
    ch.setAutoBez(frame, true);
    return true;
}

void ParamBlock::refreshOneKey(ParamId id, int frame) {
    if (frame < 0) return;
    ScalarChannel &ch = ch_[idx(id)];
    const ScalarKey *k = ch.keyAt(frame);
    if (k && k->autoBez && k->interp == Interp::Bezier) ch.autoHandles(frame);
}

void ParamBlock::refreshAutoHandlesNear(ParamId id, int frame) {
    ScalarChannel &ch = ch_[idx(id)];
    refreshOneKey(id, ch.prevKey(frame));
    refreshOneKey(id, frame);
    refreshOneKey(id, ch.nextKey(frame));
}

void ParamBlock::refreshAutoHandles(ParamId id) {
    ScalarChannel &ch = ch_[idx(id)];
    for (int f : ch.keys()) {
        const ScalarKey *k = ch.keyAt(f);
        if (k && k->autoBez && k->interp == Interp::Bezier) ch.autoHandles(f);
    }
}

void ParamBlock::refreshAllAutoHandles() {
    for (int i = 0; i < kParamCount; ++i) refreshAutoHandles((ParamId)i);
}

// --- queries -----------------------------------------------------------------

const ScalarChannel *ParamBlock::channel(ParamId id) const {
    const ScalarChannel &ch = ch_[idx(id)];
    return ch.empty() ? nullptr : &ch;
}

bool ParamBlock::isAnimated(ParamId id) const { return !ch_[idx(id)].empty(); }

std::vector<ParamId> ParamBlock::animatedParams() const {
    std::vector<ParamId> out;
    for (int i = 0; i < kParamCount; ++i) {
        const ParamId id = (ParamId)i;
        if (isAnimated(id)) out.push_back(id);
    }
    return out;
}

std::vector<int> ParamBlock::keys(ParamId id) const { return ch_[idx(id)].keys(); }

int ParamBlock::keyCount() const {
    int n = 0;
    for (int i = 0; i < kParamCount; ++i) n += ch_[i].keyCount();
    return n;
}

int ParamBlock::prevKey(ParamId id, int frame) const { return ch_[idx(id)].prevKey(frame); }

int ParamBlock::nextKey(ParamId id, int frame) const { return ch_[idx(id)].nextKey(frame); }

int ParamBlock::firstKey(ParamId id) const { return ch_[idx(id)].firstKey(); }

int ParamBlock::lastKey(ParamId id) const { return ch_[idx(id)].lastKey(); }

// --- values ------------------------------------------------------------------

double ParamBlock::staticValue(ParamId id) const {
    return hasStat_[idx(id)] ? stat_[idx(id)] : paramDesc(id).def;
}

void ParamBlock::setStaticValue(ParamId id, double v) {
    stat_[idx(id)] = v;
    hasStat_[idx(id)] = true;
}

double ParamBlock::valueAt(ParamId id, double frame) const {
    const ScalarChannel &ch = ch_[idx(id)];
    if (!ch.empty()) return ch.valueAtF(frame, staticValue(id));
    return staticValue(id);
}

void ParamBlock::clearParam(ParamId id) {
    ch_[idx(id)].clear();
    hasStat_[idx(id)] = false;
}

bool ParamBlock::empty() const {
    for (int i = 0; i < kParamCount; ++i)
        if (!ch_[i].empty()) return false;
    for (int i = 0; i < kParamCount; ++i)
        if (hasStat_[i] && std::abs(stat_[i] - paramDesc((ParamId)i).def) > 1e-12) return false;
    return true;
}

bool ParamBlock::isIdentityAt(double frame) const {
    // Evaluate EVERY parameter — keyed or static — at this instant and compare
    // against its default. A block can be non-empty (it has keys) and still
    // resolve to the identity at the playhead: a curve passing through rest,
    // or keys that only exist on other parameters. Callers use this to skip
    // the resample entirely on frames where the animation has nothing to show.
    // The tolerance is generous (1e-6) because the values round-trip through
    // float ScalarKeys and json doubles; a genuine parameter step is orders of
    // magnitude larger.
    for (int i = 0; i < kParamCount; ++i) {
        const ParamId id = (ParamId)i;
        if (std::abs(valueAt(id, frame) - paramDesc(id).def) > 1e-6) return false;
    }
    return true;
}

// --- resolution --------------------------------------------------------------

Mat3 ParamBlock::matrixFor(const Rect &src, double frame, float *opacityOut) const {
    const double op = valueAt(ParamId::Opacity, frame);
    if (opacityOut) *opacityOut = float(std::clamp(op, 0.0, 1.0));
    if (empty()) return Mat3::identity();

    const Vec2 pivot{src.x + valueAt(ParamId::AnchorX, frame) * src.w,
                     src.y + valueAt(ParamId::AnchorY, frame) * src.h};
    // Same composition order as FreeTransformParams::matrix so a keyed
    // transform and the transform tool's live preview are the same map.
    Mat3 m = Mat3::translation(valueAt(ParamId::PositionX, frame),
                               valueAt(ParamId::PositionY, frame))
                 .multiply(Mat3::translation(pivot.x, pivot.y))
                 .multiply(Mat3::rotation(valueAt(ParamId::Rotation, frame)))
                 .multiply(Mat3::shear(valueAt(ParamId::ShearX, frame),
                                       valueAt(ParamId::ShearY, frame)))
                 .multiply(Mat3::scale(valueAt(ParamId::ScaleX, frame),
                                       valueAt(ParamId::ScaleY, frame)))
                 .multiply(Mat3::translation(-pivot.x, -pivot.y));

    // Perspective corners are a projective override on the SOURCE, so they act
    // before everything else (same rule as the tool's hasQuad path).
    bool anyCorner = false;
    for (int i = 0; i < 4 && !anyCorner; ++i)
        for (int k = 0; k < 2; ++k) {
            const ParamId id = kCornerIds[i][k];
            if (std::abs(valueAt(id, frame) - paramDesc(id).def) > kEps) anyCorner = true;
        }
    if (anyCorner) {
        Vec2 from[4], to[4];
        rectCorners(src, from);
        for (int i = 0; i < 4; ++i)
            to[i] = {src.x + valueAt(kCornerIds[i][0], frame) * src.w,
                     src.y + valueAt(kCornerIds[i][1], frame) * src.h};
        Mat3 q;
        if (quadToQuad(from, to, q)) m = m.multiply(q);
    }
    return m;
}

void ParamBlock::shiftKeys(int from, int delta, int maxFrame) {
    for (int i = 0; i < kParamCount; ++i) ch_[i].shiftFrom(from, delta, maxFrame);
}

int ParamBlock::mirrorRange(int lo, int hi) {
    int n = 0;
    for (int i = 0; i < kParamCount; ++i) n += ch_[i].mirrorRange(lo, hi);
    return n;
}

bool ParamBlock::operator==(const ParamBlock &o) const {
    for (int i = 0; i < kParamCount; ++i) {
        const bool hs = hasStat_[i], ho = o.hasStat_[i];
        if (hs != ho) return false;
        if (hs && stat_[i] != o.stat_[i]) return false;
    }
    if (hasFrame() != o.hasFrame()) return false;
    if (hasFrame() &&
        (frame_.x != o.frame_.x || frame_.y != o.frame_.y || frame_.w != o.frame_.w ||
         frame_.h != o.frame_.h))
        return false;
    // Compare channels by their key content, ignoring empty shells.
    for (int i = 0; i < kParamCount; ++i) {
        const ScalarChannel &ch = ch_[i];
        if (!ch.empty()) {
            const ScalarChannel *other = o.channel((ParamId)i);
            if (!other || other->empty()) return false;
            if (other->keys() != ch.keys()) return false;
            for (int f : ch.keys()) {
                const ScalarKey *a = ch.keyAt(f);
                const ScalarKey *b = other->keyAt(f);
                if (!a || !b) return false;
                if (a->value != b->value || a->interp != b->interp) return false;
                if (a->outInfl != b->outInfl || a->outSpeed != b->outSpeed) return false;
                if (a->inInfl != b->inInfl || a->inSpeed != b->inSpeed) return false;
                if (a->autoBez != b->autoBez) return false;
            }
        } else if (o.isAnimated((ParamId)i)) {
            return false;
        }
    }
    return true;
}

} // namespace ink
