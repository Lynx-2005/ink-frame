#pragma once
// Project format + export. Production:
// - .inkframe = directory: project.json + cell_<track>_<key>_<path>.raw (RGBA8,
//   cropped to the painted extent; an empty file = a blank layer)
// - project.json v2: size, fps, frameCount, folder/track tree, per-cell
//   exposure spans, and each cell's own layer stack (paint/group, opacity,
//   blend, visibility, lock)
// - Export: PNG sequence (via Qt in UI) + MP4 via external ffmpeg binary
//   (same strategy as Krita KisFFMpegWrapper — never link libav).
#include <string>
#include <vector>

namespace ink {

class Image;

struct ProjectSaveResult { bool ok{false}; std::string error; };
struct ProjectLoadResult { bool ok{false}; std::string error; };

ProjectSaveResult saveProject(const Image &img, const std::string &dirPath);
// Stub: validates that project.json exists. Parsing the v2 tree is a separate
// feature (the app has no save/load UI yet).
ProjectLoadResult loadProject(Image &img, const std::string &dirPath);

// FFmpeg export command builder (UI shells out via QProcess).
// Returns argv for: ffmpeg -framerate fps -i frame_%04d.png -c:v libx264 -pix_fmt yuv420p out.mp4
std::vector<std::string> buildFfmpegArgs(int fps, const std::string &inputPattern,
                                         const std::string &outputPath,
                                         int crf = 18, const std::string &preset = "slow");
std::string frameFileName(int frame); // frame_0001.raw

} // namespace ink
