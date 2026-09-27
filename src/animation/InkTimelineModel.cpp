#include "animation/InkTimelineModel.h"

namespace ink {

std::string TimelineModel::addFolder(const std::string &name) {
    std::string id = "F" + std::to_string(nextFolder_++);
    int color = (int)folders_.size() % 8;
    folders_.push_back({id, name.empty() ? ("Folder " + std::to_string(nextFolder_ - 1)) : name,
                        true, color, {}});
    flatDirty_ = true;
    return id;
}

bool TimelineModel::removeFolder(const std::string &folderId) {
    for (auto it = folders_.begin(); it != folders_.end(); ++it)
        if (it->folderId == folderId) { folders_.erase(it); flatDirty_ = true; markIndexDirty(); return true; }
    return false;
}

bool TimelineModel::renameFolder(const std::string &folderId, const std::string &name) {
    if (auto *f = folder(folderId)) { f->folderName = name; return true; }
    return false;
}

void TimelineModel::setFolderExpanded(const std::string &folderId, bool expanded) {
    if (auto *f = folder(folderId)) f->expanded = expanded;
}

TimelineFolder *TimelineModel::folder(const std::string &id) {
    for (auto &f : folders_) if (f.folderId == id) return &f;
    return nullptr;
}
const TimelineFolder *TimelineModel::folder(const std::string &id) const {
    for (auto &f : folders_) if (f.folderId == id) return &f;
    return nullptr;
}

std::string TimelineModel::defaultFolderId() const {
    return folders_.empty() ? std::string{} : folders_.front().folderId;
}

int TimelineModel::visibleRowCount() const {
    int n = 0;
    for (auto &f : folders_) {
        n += 1; // folder header always visible (mandatory)
        if (f.expanded) n += (int)f.tracks.size();
    }
    return n;
}

void TimelineModel::addLayer(const std::string &id, const std::string &name) {
    std::string fid = defaultFolderId();
    if (fid.empty()) fid = addFolder("Folder 1");
    addTrack(fid, id, name);
}

void TimelineModel::addTrack(const std::string &folderId, const std::string &id,
                             const std::string &name) {
    if (row(id)) return;
    TimelineFolder *f = folder(folderId);
    if (!f) {
        std::string fid = defaultFolderId();
        if (fid.empty()) fid = addFolder("Folder 1");
        f = folder(fid);
    }
    TimelineRow row;
    row.layerId = id;
    row.layerName = name;
    f->tracks.push_back(std::move(row));
    flatDirty_ = true;
    markIndexDirty();
}

bool TimelineModel::removeLayer(const std::string &id) {
    for (auto &f : folders_)
        for (auto it = f.tracks.begin(); it != f.tracks.end(); ++it)
            if (it->layerId == id) { f.tracks.erase(it); flatDirty_ = true; markIndexDirty(); return true; }
    return false;
}

bool TimelineModel::moveTrack(const std::string &id, const std::string &toFolderId, int toIndex) {
    TimelineRow saved;
    bool found = false;
    for (auto &f : folders_)
        for (auto it = f.tracks.begin(); it != f.tracks.end(); ++it)
            if (it->layerId == id) { saved = *it; f.tracks.erase(it); found = true; break; }
    if (!found) return false;
    TimelineFolder *dst = folder(toFolderId);
    if (!dst) return false;
    if (toIndex < 0 || toIndex > (int)dst->tracks.size())
        dst->tracks.push_back(saved);
    else
        dst->tracks.insert(dst->tracks.begin() + toIndex, saved);
    flatDirty_ = true;
    markIndexDirty();
    return true;
}

void TimelineModel::rebuildIndex() const {
    rowIndex_.clear();
    for (size_t fi = 0; fi < folders_.size(); ++fi)
        for (size_t ti = 0; ti < folders_[fi].tracks.size(); ++ti)
            rowIndex_[folders_[fi].tracks[ti].layerId] = {(int)fi, (int)ti};
    indexDirty_ = false;
}

const TimelineRow *TimelineModel::findRow(const std::string &id) const {
    if (indexDirty_) rebuildIndex();
    auto it = rowIndex_.find(id);
    if (it != rowIndex_.end()) {
        const auto [fi, ti] = it->second;
        if (fi >= 0 && fi < (int)folders_.size() && ti >= 0 &&
            ti < (int)folders_[(size_t)fi].tracks.size()) {
            const TimelineRow *r = &folders_[(size_t)fi].tracks[(size_t)ti];
            if (r->layerId == id) return r;
        }
        // Stale entry (structure was edited past the index): fall through to
        // the slow path rather than ever returning the wrong row.
    }
    for (size_t fi = 0; fi < folders_.size(); ++fi)
        for (size_t ti = 0; ti < folders_[fi].tracks.size(); ++ti)
            if (folders_[fi].tracks[ti].layerId == id) {
                rowIndex_[id] = {(int)fi, (int)ti};
                return &folders_[fi].tracks[ti];
            }
    return nullptr;
}

TimelineRow *TimelineModel::findRow(const std::string &id) {
    return const_cast<TimelineRow *>(
        const_cast<const TimelineModel *>(this)->findRow(id));
}

TimelineRow *TimelineModel::row(const std::string &id) { return findRow(id); }
const TimelineRow *TimelineModel::row(const std::string &id) const { return findRow(id); }

std::string TimelineModel::folderOfTrack(const std::string &trackId) const {
    if (indexDirty_) rebuildIndex();
    auto it = rowIndex_.find(trackId);
    if (it != rowIndex_.end()) {
        const auto [fi, ti] = it->second;
        if (fi >= 0 && fi < (int)folders_.size() && ti >= 0 &&
            ti < (int)folders_[(size_t)fi].tracks.size() &&
            folders_[(size_t)fi].tracks[(size_t)ti].layerId == trackId)
            return folders_[(size_t)fi].folderId;
    }
    for (auto &f : folders_)
        for (auto &r : f.tracks)
            if (r.layerId == trackId) return f.folderId;
    return {};
}

void TimelineModel::setCurrentFrame(int f) { current_ = std::clamp(f, 0, frameCount_-1); }
void TimelineModel::setKey(const std::string &id, int f) {
    if (auto *r = row(id)) {
        const int k = std::clamp(f, 0, frameCount_ - 1);
        r->frames.setKey(k);
        r->spanEnd[k] = -1; // a fresh cell exposes to the last frame
    }
}
void TimelineModel::clearKey(const std::string &id, int f) {
    if (auto *r = row(id)) {
        r->frames.removeKey(f);
        r->spanEnd.erase(f);
    }
}
bool TimelineModel::moveKey(const std::string &id, int from, int to) {
    auto *r = row(id);
    if (!r || !r->frames.hasKey(from)) return false;
    to = std::clamp(to, 0, frameCount_ - 1);
    if (from == to) return true;
    const int end = cellEnd(id, from);
    r->frames.removeKey(from);
    r->spanEnd.erase(from);
    r->frames.setKey(to);
    r->spanEnd[to] = (end < 0) ? -1 : to + (end - from);
    return true;
}
int TimelineModel::activeKey(const std::string &id, int f) const {
    if (auto *r = row(id)) return r->frames.activeAt(f);
    return -1;
}

std::vector<CellSpan> TimelineModel::cells(const std::string &layerId) const {
    std::vector<CellSpan> out;
    const TimelineRow *r = findRow(layerId);
    if (!r) return out;
    // One key-vector copy total (cellEnd used to re-copy it per cell: O(K^2)).
    const auto keys = r->frames.keys();
    out.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        const int s = keys[i];
        auto it = r->spanEnd.find(s);
        int e = (it != r->spanEnd.end() && it->second >= 0) ? it->second : frameCount_ - 1;
        if (i + 1 < keys.size()) e = std::min(e, keys[i + 1] - 1); // never overlap
        e = std::min(e, frameCount_ - 1);
        if (e < s) continue;
        out.push_back({s, e, (int)out.size()});
    }
    return out;
}

int TimelineModel::cellCount(const std::string &layerId) const {
    const TimelineRow *r = findRow(layerId);
    return r ? (int)r->frames.keys().size() : 0;
}

int TimelineModel::cellEnd(const std::string &layerId, int start) const {
    const TimelineRow *r = findRow(layerId);
    if (!r) return -1;
    auto it = r->spanEnd.find(start);
    int end = (it != r->spanEnd.end() && it->second >= 0) ? it->second : frameCount_ - 1;
    // Clip to the next key without copying the key vector (was O(K) per call).
    const int nxt = r->frames.nextKey(start);
    if (nxt > start) end = std::min(end, nxt - 1);
    return std::min(end, frameCount_ - 1);
}

void TimelineModel::setCellEnd(const std::string &layerId, int start, int end) {
    auto *r = row(layerId);
    if (!r || !r->frames.hasKey(start)) return;
    const bool open = (end < 0) || (end >= frameCount_ - 1);
    if (open) {
        r->spanEnd[start] = -1;
        return;
    }
    r->spanEnd[start] = std::max(start, end);
}

int TimelineModel::cellLength(const std::string &layerId, int start) const {
    const TimelineRow *r = findRow(layerId);
    if (!r || !r->frames.hasKey(start)) return 0;
    return cellEnd(layerId, start) - start + 1;
}

int TimelineModel::cellStartAt(const std::string &layerId, int frame) const {
    const TimelineRow *r = findRow(layerId);
    if (!r) return -1;
    // The holding key is the only candidate (was: copy all keys, then
    // cellEnd() per candidate — O(K^2) per mouse move).
    const int hold = r->frames.activeAt(frame);
    if (hold < 0) return -1;
    return (cellEnd(layerId, hold) >= frame) ? hold : -1; // gap -> nothing exposed
}

int TimelineModel::cellIndexAt(const std::string &layerId, int frame) const {
    const int start = cellStartAt(layerId, frame);
    if (start < 0) return -1;
    const TimelineRow *r = findRow(layerId);
    return r ? r->frames.rankOf(start) : -1;
}

void TimelineModel::rebuildFlat() const {
    flatCache_.clear();
    for (auto &f : folders_)
        for (auto &r : f.tracks) flatCache_.push_back(r);
    flatDirty_ = false;
}

const std::vector<TimelineRow> &TimelineModel::rows() const {
    if (flatDirty_) rebuildFlat();
    return flatCache_;
}

} // namespace ink
