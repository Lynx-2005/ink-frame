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

ProjectSaveResult saveProject(const Image &img, const std::string &dirPath) {
    try {
        fs::create_directories(dirPath);
        std::vector<const PaintLayer*> layers;
        img.collectPaintLayers(layers);
        std::ostringstream json;
        json << "{\"width\":" << img.width() << ",\"height\":" << img.height()
             << ",\"fps\":" << img.fps() << ",\"frameCount\":" << img.frameCount()
             << ",\"layers\":[";
        for (size_t li = 0; li < layers.size(); ++li) {
            const auto *l = layers[li];
            if (li) json << ",";
            json << "{\"name\":\"" << l->name() << "\",\"opacity\":" << l->opacity()
                 << ",\"visible\":" << (l->visible()?1:0) << ",\"keys\":[";
            auto keys = l->keys();
            for (size_t ki = 0; ki < keys.size(); ++ki) {
                if (ki) json << ",";
                json << keys[ki];
                // write raw RGBA8 canvas-sized (sparse optimization v2: crop to extent)
                const PaintDevice *d = l->deviceAt(keys[ki]);
                std::string path = (fs::path(dirPath) / ("layer_" + std::to_string(li) +
                                    "_" + frameFileName(keys[ki]))).string();
                std::ofstream f(path, std::ios::binary);
                int w = img.width(), h = img.height();
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        PixelRGBA8 p = d ? d->pixel(x,y) : PixelRGBA8{};
                        f.put((char)p.r); f.put((char)p.g); f.put((char)p.b); f.put((char)p.a);
                    }
            }
            json << "]}";
        }
        json << "]}";
        std::ofstream jf(fs::path(dirPath)/"project.json");
        jf << json.str();
        return {true, {}};
    } catch (std::exception &e) { return {false, e.what()}; }
}

ProjectLoadResult loadProject(Image &img, const std::string &dirPath) {
    // v1: validate presence of project.json; full parse in UI phase with Qt JSON.
    if (!fs::exists(fs::path(dirPath)/"project.json"))
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
