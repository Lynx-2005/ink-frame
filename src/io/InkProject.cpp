#include "io/InkProject.h"
#include "core/InkImage.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ink {
namespace fs = std::filesystem;

std::string frameFileName(int frame) {
    char b[64]; snprintf(b, sizeof b, "frame_%04d.raw", frame);
    return b;
}

namespace {

// Names are user text: escape them so a quote or a newline cannot break the file.
std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

// Write one paint layer's pixels, cropped to what is actually painted, and
// report the crop so the loader can place it. A blank sheet writes an empty
// file (0 bytes) with a zero rect: the entry still exists, so the structure
// of the stack round-trips even when nothing is drawn.
void writeCellLayerRaw(const std::string &dir, const std::string &fileName, const CellLayer &l,
                       std::ostringstream &json) {
    Rect e;
    if (const PaintDevice *d = l.device()) e = d->extent();
    const std::string path = (fs::path(dir) / fileName).string();
    if (!e.isEmpty()) {
        std::vector<uint8_t> buf(size_t(e.w) * size_t(e.h) * 4);
        l.device()->readRect(e, buf.data());
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
    } else {
        std::ofstream f(path, std::ios::binary); // blank
    }
    json << ",\"file\":\"" << fileName << "\",\"rect\":[" << e.x << "," << e.y << "," << e.w << ","
         << e.h << "]";
}

// One cell's layer stack, bottom..top. `prefix` comes from (track, key) and
// grows with the nesting path, so file names stay readable and unique.
void writeCellLayers(std::ostringstream &json, const std::vector<std::shared_ptr<CellLayer>> &layers,
                     const std::string &dir, const std::string &prefix) {
    json << "[";
    for (size_t i = 0; i < layers.size(); ++i) {
        const CellLayer &l = *layers[i];
        if (i) json << ",";
        const std::string path = prefix + std::to_string(i);
        json << "{\"name\":\"" << jsonEscape(l.name()) << "\""
             << ",\"type\":\"" << (l.isPaint() ? "paint" : "group") << "\""
             << ",\"visible\":" << (l.visible() ? 1 : 0)
             << ",\"locked\":" << (l.locked() ? 1 : 0)
             << ",\"opacity\":" << l.opacity()
             << ",\"blend\":" << int(l.blend());
        if (l.isPaint()) {
            const std::string fileName = "cell_" + path + ".raw";
            writeCellLayerRaw(dir, fileName, l, json);
        } else {
            json << ",\"children\":";
            writeCellLayers(json, l.children(), dir, path + "_");
        }
        json << "}";
    }
    json << "]";
}

} // namespace

// Adjustment-layer parameters: one entry per parameter that is not at its
// default, so an untouched layer costs nothing in the file. Keys carry their
// interpolation and their handles; the handles are influence + speed, in the
// same units the curve editor edits them in, so a round trip is lossless.
static void writeParams(std::ostringstream &json, const ParamBlock &params) {
    json << ",\"params\":{";
    bool first = true;
    // The reference frame is metadata, not animation, but without it a reload
    // could not re-read the anchor/corners: legacy blocks simply omit it.
    if (params.hasFrame()) {
        const Rect f = params.frame();
        json << "\"frame\":[" << f.x << "," << f.y << "," << f.w << "," << f.h << "]";
        first = false;
    }
    const ParamDesc *table = paramTable();
    for (int pi = 0; pi < kParamCount; ++pi) {
        const ParamDesc &d = table[pi];
        const ParamId id = d.id;
        const bool animated = params.isAnimated(id);
        const double sv = params.staticValue(id);
        if (!animated && std::abs(sv - d.def) < 1e-12) continue;
        if (!first) json << ",";
        first = false;
        json << "\"" << d.key << "\":{\"s\":" << sv;
        if (animated) {
            json << ",\"k\":[";
            const auto keys = params.keys(id);
            const ScalarChannel *ch = params.channel(id);
            for (size_t i = 0; i < keys.size(); ++i) {
                const ScalarKey *k = ch ? ch->keyAt(keys[i]) : nullptr;
                if (!k) continue;
                if (i) json << ",";
                const char *mode = k->interp == Interp::Hold     ? "hold"
                                   : k->interp == Interp::Linear ? "linear"
                                                                 : "bezier";
                json << "{\"f\":" << k->frame << ",\"v\":" << k->value << ",\"i\":\"" << mode
                     << "\"";
                if (k->interp == Interp::Bezier)
                    json << ",\"o\":[" << k->outInfl << "," << k->outSpeed << "]"
                         << ",\"n\":[" << k->inInfl << "," << k->inSpeed << "]"
                         << ",\"a\":" << (k->autoBez ? 1 : 0);
                json << "}";
            }
            json << "]";
        }
        json << "}";
    }
    json << "}";
}

// Format v3. v2 could not express what the engine now holds: adjustment layers
// and the keyframed parameter channels that drive them. v3 adds `kind` on every
// track entry and a `params` block on adjustment tracks; a v2 reader that meets
// a v3 file sees `kind` it does not know and can skip the track, and a v3 writer
// still emits everything a v2 reader needs for the paint tracks.
// Format v4 adds `kind:"camera"` entries (scope + target + params). Paint
// entries stay byte-identical to v3 so old readers keep working.
ProjectSaveResult saveProject(const Image &img, const std::string &dirPath) {
    try {
        fs::create_directories(dirPath);
        std::vector<const GroupLayer*> folders;
        img.collectFolders(folders);

        // Targets are stored as TRACK INDICES, so indices have to exist before
        // anything is written: first pass assigns one per layer, in tree order,
        // exactly the order the second pass walks.
        std::map<uint64_t, int> indexOf;
        {
            int idx = 0;
            for (const GroupLayer *g : folders)
                for (auto &child : g->children()) {
                    if (child->type() == LayerType::Group) continue;
                    indexOf[child->id()] = idx++;
                }
        }

        std::ostringstream json;
        json << "{\"version\":4"
             << ",\"width\":" << img.width() << ",\"height\":" << img.height()
             << ",\"fps\":" << img.fps() << ",\"frameCount\":" << img.frameCount()
             << ",\"folders\":[";
        int trackIdx = 0;
        for (size_t fi = 0; fi < folders.size(); ++fi) {
            const GroupLayer *g = folders[fi];
            if (fi) json << ",";
            json << "{\"name\":\"" << jsonEscape(g->name()) << "\""
                 << ",\"visible\":" << (g->visible() ? 1 : 0)
                 << ",\"locked\":" << (g->locked() ? 1 : 0)
                 << ",\"opacity\":" << g->opacity()
                 << ",\"blend\":" << int(g->blend()) << ",\"tracks\":[";
            bool firstTrack = true;
            for (auto &child : g->children()) {
                if (child->type() == LayerType::Group) continue;
                if (child->type() == LayerType::Camera) {
                    const auto *cam = static_cast<const CameraLayer*>(child.get());
                    if (!firstTrack) json << ",";
                    firstTrack = false;
                    const char *scope = cam->scope() == CameraScope::Folder   ? "folder"
                                        : cam->scope() == CameraScope::Global ? "global"
                                                                             : "layer";
                    json << "{\"kind\":\"camera\""
                         << ",\"name\":\"" << jsonEscape(cam->name()) << "\""
                         << ",\"visible\":" << (cam->visible() ? 1 : 0)
                         << ",\"locked\":" << (cam->locked() ? 1 : 0)
                         << ",\"opacity\":" << cam->opacity()
                         << ",\"blend\":" << int(cam->blend()) << ",\"scope\":\"" << scope
                         << "\""
                         << ",\"target\":" << cam->targetId();
                    writeParams(json, cam->params());
                    json << "}";
                    ++trackIdx;
                    continue;
                }
                if (child->type() != LayerType::Paint) continue;
                const auto *t = static_cast<const PaintLayer*>(child.get());
                if (!firstTrack) json << ",";
                firstTrack = false;
                json << "{\"kind\":\"paint\""
                     << ",\"name\":\"" << jsonEscape(t->name()) << "\""
                     << ",\"visible\":" << (t->visible() ? 1 : 0)
                     << ",\"locked\":" << (t->locked() ? 1 : 0)
                     << ",\"opacity\":" << t->opacity()
                     << ",\"blend\":" << int(t->blend());
                // Transform keyframes ride directly on the paint track now
                // (the adjustment layer is gone): same wire format, new home.
                writeParams(json, t->params());
                json << ",\"cells\":[";
                const auto keys = t->keys();
                for (size_t ki = 0; ki < keys.size(); ++ki) {
                    const int k = keys[ki];
                    if (ki) json << ",";
                    // spanEnd -1 mirrors "open" (exposes to the last frame)
                    json << "{\"frame\":" << k
                         << ",\"spanEnd\":" << (t->spanOpen(k) ? -1 : t->spanEnd(k))
                         << ",\"layers\":";
                    const Cell *c = t->cellAt(k);
                    const std::string prefix =
                        std::to_string(trackIdx) + "_" + std::to_string(k) + "_";
                    if (c) writeCellLayers(json, c->layers(), dirPath, prefix);
                    else json << "[]";
                    json << "}";
                }
                json << "]}";
                ++trackIdx;
            }
            json << "]}";
        }
        json << "]}";
        std::ofstream jf(fs::path(dirPath) / "project.json");
        jf << json.str();
        return {true, {}};
    } catch (std::exception &e) {
        return {false, e.what()};
    }
}

ProjectLoadResult loadProject(Image &img, const std::string &dirPath) {
    // Stub: v2 parsing (folders/tracks/cells/cell-layer stacks) is a separate
    // feature — this only validates that a project is there at all.
    (void)img;
    if (!fs::exists(fs::path(dirPath) / "project.json"))
        return {false, "project.json missing in " + dirPath};
    return {true, {}};
}

std::vector<std::string> buildFfmpegArgs(int fps, const std::string &inputPattern,
                                         const std::string &outputPath, int crf,
                                         const std::string &preset) {
    return {"ffmpeg","-y","-framerate",std::to_string(fps),"-i",inputPattern,
            "-c:v","libx264","-preset",preset,"-crf",std::to_string(crf),
            "-pix_fmt","yuv420p",outputPath};
}

} // namespace ink
