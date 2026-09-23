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
        if (it->folderId == folderId) { folders_.erase(it); flatDirty_ = true; return true; }
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
}

bool TimelineModel::removeLayer(const std::string &id) {
    for (auto &f : folders_)
        for (auto it = f.tracks.begin(); it != f.tracks.end(); ++it)
            if (it->layerId == id) { f.tracks.erase(it); flatDirty_ = true; return true; }
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
    return true;
}

TimelineRow *TimelineModel::row(const std::string &id) {
    for (auto &f : folders_)
        for (auto &r : f.tracks)
            if (r.layerId == id) return &r;
    return nullptr;
}
const TimelineRow *TimelineModel::row(const std::string &id) const {
    for (auto &f : folders_)
        for (auto &r : f.tracks)
            if (r.layerId == id) return &r;
    return nullptr;
}

std::string TimelineModel::folderOfTrack(const std::string &trackId) const {
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
    const TimelineRow *r = row(layerId);
    if (!r) return out;
    const auto keys = r->frames.keys();
    for (size_t i = 0; i < keys.size(); ++i) {
        const int s = keys[i];
        const int e = cellEnd(layerId, s);
        if (e < s) continue;
        out.push_back({s, e, (int)out.size()});
    }
    return out;
}

int TimelineModel::cellCount(const std::string &layerId) const {
    const TimelineRow *r = row(layerId);
    return r ? (int)r->frames.keys().size() : 0;
}

int TimelineModel::cellEnd(const std::string &layerId, int start) const {
    const TimelineRow *r = row(layerId);
    if (!r) return -1;
    auto it = r->spanEnd.find(start);
    const int stored = (it != r->spanEnd.end()) ? it->second : -1;
    int end = (stored < 0) ? frameCount_ - 1 : stored;
    auto next = r->frames.keys();
    for (int k : next)
        if (k > start) { end = std::min(end, k - 1); break; }
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
    if (!row(layerId) || !row(layerId)->frames.hasKey(start)) return 0;
    return cellEnd(layerId, start) - start + 1;
}

int TimelineModel::cellStartAt(const std::string &layerId, int frame) const {
    const TimelineRow *r = row(layerId);
    if (!r) return -1;
    auto it = r->frames.keys();
    for (auto k = it.rbegin(); k != it.rend(); ++k) {
        if (*k > frame) continue;
        return (cellEnd(layerId, *k) >= frame) ? *k : -1; // gap -> nothing exposed
    }
    return -1;
}

int TimelineModel::cellIndexAt(const std::string &layerId, int frame) const {
    const int start = cellStartAt(layerId, frame);
    if (start < 0) return -1;
    int idx = 0;
    for (int k : row(layerId)->frames.keys()) {
        if (k == start) return idx;
        ++idx;
    }
    return -1;
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
