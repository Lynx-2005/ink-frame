#pragma once
// Project format + export. Production:
// - .inkframe = directory: project.json + layer_<i>_frame_<f>.raw (RGBA8)
// - project.json: size, fps, frameCount, layers, keys
// - Export: PNG sequence (via Qt in UI) + MP4 via external ffmpeg binary
//   (same strategy as Krita KisFFMpegWrapper — never link libav).
#include <string>
#include <vector>

namespace ink {

class Image;

struct ProjectSaveResult { bool ok{false}; std::string error; };
struct ProjectLoadResult { bool ok{false}; std::string error; };

ProjectSaveResult saveProject(const Image &img, const std::string &dirPath);
ProjectLoadResult loadProject(Image &img, const std::string &dirPath);

// FFmpeg export command builder (UI shells out via QProcess).
// Returns argv for: ffmpeg -framerate fps -i frame_%04d.png -c:v libx264 -pix_fmt yuv420p out.mp4
std::vector<std::string> buildFfmpegArgs(int fps, const std::string &inputPattern,
                                         const std::string &outputPath,
                                         int crf = 18, const std::string &preset = "slow");
std::string frameFileName(int frame); // frame_0001.raw

} // namespace ink
