# InkFrame Architecture (production-grade, clean-room)

No Krita code is used. Concepts borrowed: tiled raster store, paintop/dab split,
keyframe channels, async frame cache, onion compositor, external-ffmpeg export.

## Threading model
- UI thread: canvas paint, timeline view, playback timer (16ms).
- Stroke path: `StrokeQueue` serialises dab jobs per stroke; each dab records
  first-touch tile snapshots into `TileMemento` (O(dirty tiles) undo).
- `FrameCache`: 1–2 workers (grows to N), generation counter invalidates stale
  renders on scrub/edit. LRU cap 64 projections; disk swap is v2.
- `Image::project` is re-entrant const (tile reads lock per-store mutex).

## Invalidation
- `TileData::version` bumps on write → canvas uploads only dirty tiles (v2 GL path;
  v1 QPainter path already uses `extent()` + dirty rects from dab/fill/blit).
- `Document::touchLayer` invalidates cache from edited frame forward.

## Canvas (src/ui/InkCanvasWidget)
- `QOpenGLWidget`, HiDPI via `devicePixelRatioF`, checkerboard, zoom 0.05–16x,
  rotate, middle-drag pan, `QTabletEvent` pressure, wheel zoom.
- v2: upload `TileShared` to GL textures keyed by `(tx,ty,version)`.

## Timeline (src/ui/InkTimelineDock + InkTimelineWidget + src/animation/InkTimelineModel)
- Headless `TimelineModel`: folders, tracks, cells, fps, frame count. A CELL is
  one key plus its own exposure span [start, spanEnd]; spans are stored per cell
  (mirrored from `PaintLayer`), never derived from the next key, so resizing or
  moving one cell can't resize or move another. A span may stop short of the
  next key, which leaves a gap with no exposure. A track with no key is an
  empty track — no cell is drawn until one is added, and a NEW cell is one
  frame long (extend it by dragging its right edge or via the Exposure menu).
- `PaintLayer` owns the spans: `spanEnd`/`setSpanEnd`/`exposureAt`/`shiftFrom`;
  `moveDeviceContent`/`copyDeviceContent` carry the exposure with the pixels.
  `Image::project` composites the cel exposed at a frame (gaps draw nothing).
- `InkTimelineWidget`: custom-painted sticky gutter (layer list, columns for
  name/eye/lock/opacity, Ctrl+Shift multi-select feeding group/ungroup) + ruler
  (ticks, seconds, onion markers, draggable In/Out band, I/O shortcuts) + paper
  cells. Contiguous cells share one paper bar; gaps stay visibly empty.
- Cell interaction: drag the body to move (Alt drops over an occupied frame),
  drag either edge to resize/trim, double-click an empty lane to add a cell,
  right-click a lane for the cell menu (exposure sizes, copy/paste/mirror,
  ripple insert/remove), right-click the gutter for the layer/folder menu
  (group, ungroup, move to folder, stack order, delete folder, opacity, blend,
  color, thumbnails). Right-click is context-specific: gutter = layer,
  lane = cell, folder header = folder, ruler = playback range.
- Transport toolbar: skip/prev/next/play-pause toggle, timecode + frame
  register, In/Out spins, fps, Auto-key, onion (toggle + right-click settings),
  loop cycle, follow-playhead, zoom. Cell ops: New / Duplicate / Delete.
- Layer structure ops live in `src/core/InkImage`: `groupTracks`,
  `ungroupFolder`, `moveTrack` (folders stay mandatory; the last one can't be
  dissolved). `Document` wraps them plus `deleteFolder`/`moveTrackInFolder` and
  resyncs the model.
- `PlaybackEngine::advance(dt)` with Loop/PingPong/Once + drop-frame via `changed`.

## Brush (src/brush)
- `AutoBrushTip` procedural mask (hardness smoothstep), `BrushDynamics` pressure
  curves, `StrokeInterpolator` spacing-accurate dab placement, `PixelBrushOp` /
  `EraserOp`, `PaintOpRegistry` for future engines (smudge, spray, MyPaint-style).
- Redo = deterministic replay of recorded `StrokeEvent`s.

## Color (src/pigment)
- `ColorSpaceRegistry`, sRGB↔linear, `composite::{normal,multiply,screen,erase}`.
- OCIO/LCMS integration point: `toLinear` is the seam for display-referred LUTs.

## I/O (src/io)
- `.inkframe/` dir + `project.json` + `layer_*_frame_*.raw` (v2: crop to extent +
  zlib + thumbnail). `buildFfmpegArgs` shells to system ffmpeg (never link libav).

## Scaling to Krita-size (why Krita is millions of lines)
Krita ships ~30 paintops, ~30 filters, 25 file formats, vector flake, HDR/OCIO,
Python API, etc. InkFrame matches that trajectory by keeping each as a plugin:
add `src/brush/SmudgeOp.cpp` or `src/io/PsdImport.cpp` without touching core.
Next verticals: transform masks, filter layers, vector overlay, audio scrub,
storyboard docker, resource SQLite sync.
