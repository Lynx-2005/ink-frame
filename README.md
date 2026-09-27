# InkFrame — production-grade 2D frame-by-frame animation engine (C++20 + Qt6)

Clean-room implementation. **Inspired by Krita's architecture, contains zero Krita code.**
`krita-reference/` is read-only reference and is never compiled, linked, or copied.
This keeps InkFrame proprietary-safe (Krita is GPLv3 — copying it would force GPL).

## Module map (Krita concept -> InkFrame clean-room equivalent)

| Krita concept | InkFrame module | Notes |
|---|---|---|
| `libs/image` + `tiles3/` (KisImage/Node/PaintDevice/Tile) | `src/core` (Image/Layer/PaintDevice/TileStore) | Sparse 64px tiles, COW, mementos, stroke queue |
| `libs/pigment` (KoColorSpace/CompositeOp) | `src/pigment` | sRGB/Linear registry, Normal/Multiply/Screen/Erase |
| `libs/brush` + `plugins/paintops` + DabCache | `src/brush` | Tip mask generator, sensors, dab cache, Pixel+Smudge ops |
| `kis_keyframe_channel`, `kis_time_span`, onion skin | `src/animation` | Hold/linear/**bezier** channels with influence+speed handles, onion compositor, frame cache, playback |
| `kis_transform_worker`, `kis_perspectivetransform_worker`, `kis_grid_interpolation_tools` | `src/core/InkTransform` + `src/core/InkDeform` | Affine/projective resampler (Nearest→Lanczos3), quad→quad homography, MLS point deformation, liquify displacement grid |
| `tool_transform2` (free/perspective/warp/liquify strategies) | `src/ui/tools/InkTransformTool` | One tool class, four modes, handles + options pages, one undo step per session |
| `libs/ui/canvas` + OpenGL renderer | `src/ui` (Qt6) | QOpenGLWidget tiled renderer, pan/zoom **/canvas rotate**, tablet |
| `plugins/dockers/animation` timeline/curves | `src/ui` timeline dock + `InkTransformPanel` + `InkCurveEditor` | Layer list, time ruler, paper cells, and one adjustment-layer BAR carrying keyframe diamonds; its parameters open in a transform panel and ease in a per-segment bezier graph |
| `KisAdjustmentLayer` / AE adjustment layers | `src/core/InkAdjustmentLayer` + `src/core/InkParams` | Keyframed transform + perspective corners, explicitly linked to one layer; applied after the cell composites, so every tool is animatable |
| layer palettes / per-drawing layer stacks | `src/core/InkCell` + `src/ui/InkLayerPanel` | Each cell owns its own layer stack (paint + group, opacity/blend/lock); right-side "Layer" panel (top-bar ⋮ / F7), every op undoable |
| `KisFFMpegWrapper` (external ffmpeg) | `src/io` | External `ffmpeg` binary, PNG seq, .inkframe project |
| `libs/resources` sqlite cache | `src/resources` | File-backed preset DB (SQLite via Qt in UI phase) |

## Build

Desktop (with Qt6):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
ctest --test-dir build -V
./build/inkframe_app   # canvas + timeline
```

Headless engine only (this CI machine has no Qt — UI auto-skips):
```bash
cmake -S . -B build -DINKFRAME_BUILD_UI=OFF
cmake --build build -j && ctest --test-dir build -V
```

Requires: CMake 3.21+, GCC13/Clang16+, Qt 6.4+ (for UI), `ffmpeg` binary on PATH (for video export only).

## Qt setup used on this machine (no sudo — user-local install)

```bash
pip3 install --user --break-system-packages aqtinstall
~/.local/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 --outputdir "$HOME/Qt"
# Qt lands at $HOME/Qt/6.8.3/gcc_64 (qmake, moc, CMake configs included)
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/gcc_64"
cmake --build build-qt -j$(nproc)
ctest --test-dir build-qt -V
QT_QPA_PLATFORM=offscreen ./build-qt/inkframe_app  # smoke test (headless)
./build-qt/inkframe_app                            # real run (desktop session)
```
