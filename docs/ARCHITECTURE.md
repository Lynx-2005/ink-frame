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
- **Tree lock** (`Image::treeMutex`, a `shared_mutex`): pixel-level races are
  handled by the tile locks, but the *structure* (layer children, per-cell
  layer stacks) is a plain container, so readers and writers must agree.
  Projections (frame-cache workers, canvas paint) take a shared lock; every
  edit takes a unique lock through `Document::EditGuard` — Document's own ops
  take one automatically (nested ops reuse the outer lock), and widgets that
  edit a layer directly (gutter eye/lock, dock opacity/blend menus) take one
  explicitly. Without it a worker can walk a cell stack while the UI is adding
  or removing layers in it.

## Invalidation
- `TileData::version` bumps on write → canvas uploads only dirty tiles (v2 GL path;
  v1 QPainter path already uses `extent()` + dirty rects from dab/fill/blit).
- `Document::touchLayer` invalidates cache from edited frame forward;
  `Document::touchCell` does the same for a per-cell layer edit.

## Per-cell layers (src/core/InkCell + src/ui/InkLayerPanel)
- A **cell** is one key plus its exposure span. Each cell owns its OWN layer
  stack (`Cell` → `CellLayer` paint/group), unique per cell: nothing is shared
  between cells, and a hold exposes the same cell (so drawing inside a hold
  edits that cell's stack).
- `CellLayer` mirrors `Layer`'s chrome (name, visible, locked, opacity, blend)
  but owns one `PaintDevice` (paint) or children (group). `PaintLayer` stores
  `map<int, unique_ptr<Cell>>`; `deviceAt()` returns the device of the cell's
  ACTIVE layer (what tools paint into), `cellAt()` the stack.
- `Image::compositeCell` composites a stack into the frame: default layers take
  a direct blit (identical to the pre-stack path), groups with their own
  opacity/blend are isolated through a scratch device. Canvas, onion skin,
  export and timeline thumbnails all read this, so they show the composite.
- `LayerPanel` (right side, opened by the top bar's three-dot button / F7)
  edits the stack of the cell under the playhead on the current track. It shows
  the FRONT-most layer at the top (the timeline gutter shows the back-most
  first — a deliberate difference). Every structural op is undoable
  (`UndoStack::pushAction`, closures holding the layers), and `Cell` refuses to
  remove the last paint layer.
- Project format v2 saves folders/tracks/spans and the whole per-cell stack
  (`cell_<track>_<key>_<path>.raw`, cropped to the painted extent).

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
  The gutter lists TRACKS (rows of the timeline); the layers *inside* a cell
  live in the right-side Layer panel (see "Per-cell layers" above).
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
  Keyframe ops sit next to follow-playhead: prev / add / next / remove,
  an interpolation preset menu, and the curve-editor toggle.
- Layer structure ops live in `src/core/InkImage`: `groupLayers`,
  `ungroupFolder`, `moveLayer` (folders stay mandatory; the last one can't be
  dissolved). `Document` wraps them plus `deleteFolder`/`moveLayerInFolder` and
  resyncs the model.
- `PlaybackEngine::advance(dt)` with Loop/PingPong/Once + drop-frame via `changed`.
  `currentTime()` exposes the sub-frame position; nothing samples it yet.

## Keyframes + adjustment layers
- `AdjustmentLayer` (src/core/InkAdjustmentLayer.h) is a `LayerType::Adjustment`
  layer that owns a `ParamBlock` and ONE `targetId`. It renders nothing itself.
  The link is EXPLICIT, not positional, so the layer can be named, grouped,
  moved and reordered anywhere in the tree without changing what it animates —
  which is what lets it behave like every other layer in the gutter. A layer in
  the gutter that appears "on top" is just where it is easiest to find; first
  match in tree order wins if two layers ever claim one target, so the result is
  deterministic rather than a fight.
- `ParamBlock` (src/core/InkParams.h) is the animatable state: one
  `ScalarChannel` per `ParamId` plus a static value for the stopwatch-off case.
  `paramTable()` is the single source of truth for names, defaults, ranges and
  units, so adding an animatable parameter is a table row, not a change to the
  compositor, the serializer or the timeline. `valueAt(id, frame)` serves both
  states, so callers never care which one a parameter is in.
- The compositor applies the animation AFTER the cell renders, inside the layer
  walk (`Image::compositeTrack`): composite the cell, `matrixFor()` the
  parameter block at that frame, `resampleRegion` the result, blit it with
  `trackOpacity * animatedOpacity`. That single placement is what makes the
  system tool-agnostic — every tool that ever painted into the cell is animated,
  and no tool needs to know keyframes exist. An empty block resolves to the
  identity matrix and opacity 1, so a layer carrying an unkeyed adjustment layer
  composites bit-identically to one carrying none.
- An adjustment layer has an ACTIVE SPAN (`barStart`/`barEnd`, inclusive), the
  AE-style layer bar. `Image::collectAdjustments(frame, …)` tests `activeAt(frame)`
  FIRST and skips everything else, so a layer trimmed away from the playhead is
  inert (identity) and costs nothing — not merely hidden. A fresh layer's span is
  resolved to `[0, frameCount()-1]` when it is created and re-clamped by
  `setFrameCount`, so the stored span is always a REAL frame range: an
  open-ended "to the end" sentinel makes the bar's rectangle millions of pixels
  wide, and its right grip is then painted, and hit-tested, thousands of pixels
  off-screen — the classic "I cannot grab the end" bug. `barEndFor(last)` exists
  for loading older files, and `adjBarRect` clamps to the drawable lane so paint,
  hover and drag all read one rect.
  The bar is the adjustment row's body in the timeline: drag the middle to slide
  the span (keys ride with it), drag either end grip to trim (keys stay), and the
  whole gesture is one undo step (`previewAdjustmentBar` rebases from a snapshot
  on every move, so it is idempotent; `commitAdjustmentBar` pushes once). A body
  drag moves the bar by the offset between the GRAB point and the cursor, never by
  the distance from the bar's own start — the latter makes the bar snap sideways
  to meet the mouse on the first pixel. Trim grips are a fixed pixel band
  (`kBarEdgeTol`), not a fraction of the bar, so trimming is equally easy at any
  zoom. Esc abandons the gesture through `abortAdjustmentBar`, which rewinds the
  span AND the keys.
- Anchors and the four perspective corners are normalized to the CANVAS, not to
  the layer's bounds: the only bounds available during a composite are the tile
  store's extent, which is tile-granular, so one extra pixel of ink would move a
  content-anchored pivot by half a tile. Keying the anchor to a normalized point
  is how you get a content-centred pivot when you want one.
- Interpolation is a cubic Bezier over time (`src/animation/InkEasing.*`), with
  the x control points clamped to [0,1] so x(t) is provably monotone: a segment
  is always a function of time and the root solve cannot fail (Newton-Raphson,
  then bisection). Handles are stored in the units animators read off the
  screen — influence (fraction of the segment the handle reaches) and speed
  (value units per frame, shown as units/second) — and are ABSOLUTE rather than
  normalized by the value delta, so a segment whose two keys hold the same value
  is still well defined.
- Keys are `auto` until a handle is dragged: an auto key's tangent is the chord
  slope through its neighbours, which reduces a collinear run of keys to an
  exactly straight line. That is what makes a recorded drag read as one clean
  move instead of wobbling, and it is why `refreshAutoHandles` runs whenever a
  channel's shape changes.
- Timeline: an adjustment layer is ALWAYS ONE ROW. It carries the link chip
  (pick-whip onto a layer row to retarget, Alt to unlink) and, in its lane, the
  layer BAR (active span) with trim grips plus one keyframe diamond per keyed
  frame. There are no parameter sub-rows: a row per animated parameter pushed the
  whole timeline down every time anything was keyed, and eighteen transform
  parameters do not fit a gutter. The diamonds are summary markers — clicking one
  selects every parameter keyed at that frame, and right-clicking one opens a
  menu that eases or deletes the whole beat at once (`applyKeyPreset` already
  broadcasts over `keysAt(frame)`, so it is one undo step). Segments between
  diamonds are drawn as their actual shape (straight for Linear, S for Bezier,
  dashed for Hold), so interpolation mode is visible without a legend. A drag
  moves the whole selection and commits as ONE undo step (`Document`'s live-edit
  session).
- `TransformPanel` (src/ui/InkTransformPanel) is where an adjustment layer's
  parameters live: right-click or double-click its BAR and it pops up beside the
  cursor as a frameless `Qt::Tool` window (which means it must install the theme
  sheet itself — a top-level window does not inherit the main window's
  stylesheet, and it would otherwise come up in the platform's light grey). One
  row per transform parameter: stopwatch, label, numeric field, keyframe diamond.
  `ParamField` is a `QLineEdit` with a drag layered on top — press-and-move
  scrubs, a click with no drag selects the text so the next keystroke replaces
  it, wheel and Up/Down step by a 1/2/5x10^n increment rounded from the range,
  and `%`/`°` are accepted on the way in. Formatting lives in
  `src/ui/InkParamFormat.h`, shared by every readout, because a painted 50% and a
  typed 50 must never disagree. A whole scrub is ONE undo step
  (`beginParamRecord`/`endParamRecord`); a typed value is one step through the
  ordinary key path. Writing follows the rule the rest of the app uses: an
  animated parameter edits its key at the playhead (auto-key lays one down), an
  un-animated one edits its static value. The panel re-resolves its layer by id
  on every refresh, so deleting the layer closes the panel instead of crashing.
- `CurveEditor` (src/ui/InkCurveEditor) is the EASING graph: one parameter, one
  segment between two of its keys, drawn as a normalised cubic Bezier from
  bottom-left to top-right in a panel docked to the RIGHT of the grid (G toggles
  it). Not a value graph — reading slopes off a multi-channel timeline and
  grabbing 6px handles at some frame position is accurate but not aimable, while
  the question an animator asks is per-move ("this move, how does it
  accelerate?"), and that is one 0..1 curve whether the move takes 3 frames or
  300. The playhead picks the segment, so the motion being shaped is the one at
  the frame you are looking at; `followPlayhead` re-locates it, and `tick()` polls
  the frame number so every path that moves the playhead (transport, scrub, key
  jumps, cell menus, playback) keeps the graph and the panel current without
  hooking twenty call sites.
- Normalised is a VIEW, not a second model. `Document::liveSetSegmentEase` is the
  single place a normalised ease becomes stored handles, derived from
  `buildSegment`'s own definitions:
  `x1 = outInfl_A`, `x2 = 1 - inInfl_B`,
  `outSpeed_A = y1 * dV / (x1 * dF)`, `inSpeed_B = (1 - y2) * dV / ((1 - x2) * dF)`.
  The B handle is measured BACK from key B, so the inverse needs `(1 - y2)`;
  dropping that reads a stored ease as its own mirror image, which the round-trip
  test in `tests/ui/test_keyframe_ui.cpp` pins. A segment with no value delta has
  no progress axis, so its handles keep their stored slope and only their TIMING
  moves, and the status line says so instead of inventing a shape.
- Every control on the graph is real, and the drawing never lies about the model:
  a channel chip switches parameter, the preset thumbnails are defined AS
  normalised control points and applied through the same live write a drag uses
  (so thumbnail, drag and undo cannot disagree), and `presetHighlight` is read
  back from the stored handles. A Linear or Hold segment ignores its handles when
  evaluating, so the graph draws the line/L it actually evaluates and GRABBING a
  handle converts the segment to Bezier first — starting from straight, so the
  curve only bends where the user dragged. `presetHighlight` lights Linear by
  MODE, never by shape: an untouched Bezier segment sits exactly on the diagonal,
  and lighting "Linear" there would wrongly claim that dragging does nothing.
  Handles are unified by default (the tangent mirrors through the shared key,
  which is what keeps a join smooth) and Alt breaks it.
- Tool recording: with Auto-key armed and a linked adjustment layer,
  `TransformTool` (Free and Perspective) and `MoveTool` write keys instead of
  baking pixels, one undo step per drag, and only for the parameters that drag
  actually moved. Warp and Liquify keep baking: their state is a mesh, not a
  handful of scalars. With no link, both tools behave exactly as before.
- Project format v3 adds `kind` to every track entry and a `params` block to
  adjustment tracks (target as a track index, one entry per non-default
  parameter, keys with their interpolation and handles). A non-default active
  span is written as `in`/`out` on the adjustment track and is absent when the
  span covers the whole project, so v3 files stay compatible.

## Brush (src/brush)
- `AutoBrushTip` procedural mask (hardness smoothstep), `BrushDynamics` pressure
  curves, `StrokeInterpolator` spacing-accurate dab placement, `PixelBrushOp` /
  `EraserOp`, `PaintOpRegistry` for future engines (smudge, spray, MyPaint-style).
- Redo = deterministic replay of recorded `StrokeEvent`s.

## Transform (src/core/InkTransform + src/core/InkDeform + src/ui/tools/InkTransformTool)
- `InkTransform`: `Mat3` projective maps, quad→quad homography (`H = B·A⁻¹`),
  inverse-mapped resampler with separable kernels (Nearest/Bilinear/Bicubic/
  Lanczos3), premultiplied so transparent neighbours cannot fringe an edge.
  Every kernel interpolates exactly at integer offsets, so identity, integer
  translation, flips and 90° turns are bit-exact; the tool detects lattice maps
  and takes the nearest path for them. Row-band threaded: 1.5 Mpx bilinear ≈ 30 ms,
  so a full-canvas preview stays interactive.
- `FreeTransformParams` holds the free transform's scalars (pivot, translate,
  scale, angle, shear) + an optional projective override from a Ctrl-corner drag;
  the matrix is composed in the engine so it is unit-tested headlessly.
- `InkDeform`: `DeformMesh` (control lattice + bilinear displacement field,
  inverted per destination pixel by fixed-point iteration), `mlsTransform`
  (Moving Least Squares in affine / similitude / rigid form) and `LiquifyField`
  (Gaussian-falloff brush, λ = exp(−½(d/σ)²), radius 3σ, build-up or wash).
  `warpRegion` / `warpCoverage` resample pixels and selection masks through it.
- `InkTransformTool`: ONE class, four modes (free, perspective, warp, liquify),
  registered as three toolbox entries (T / Shift+T / Shift+Y) with a mode
  switcher. A session lifts the pixels once (`FloatRegion`), then rebuilds the
  destination from that pristine lift on every manipulation — restore memento →
  erase source → stamp — so N dragged frames produce exactly what one Apply
  produces, and the session is a single undo memento. Enter/Apply commits, Esc
  reverts, switching tools or modes commits/reverts rather than dropping edits.
  Selection masks ride along (`Selection::resampleMask`, matrix or mesh form).
- Canvas view rotation/zoom are view-only (4/6 rotate 15°, 5 resets, 1/3 zoom,
  2 = 100 %, Ctrl+0 fits the rotated bounds) — the pixels never rotate.

## Color (src/pigment)
- `ColorSpaceRegistry`, sRGB↔linear, `composite::{normal,multiply,screen,erase}`.
- OCIO/LCMS integration point: `toLinear` is the seam for display-referred LUTs.

## I/O (src/io)
- `.inkframe/` dir + `project.json` + `layer_*_frame_*.raw` (v3: crop to extent +
  zlib + thumbnail, per-track `kind`, adjustment-layer `params`). v3 still writes
  everything a v2 reader needs for the paint tracks.
  `buildFfmpegArgs` shells to system ffmpeg (never link libav).

## Scaling to Krita-size (why Krita is millions of lines)
Krita ships ~30 paintops, ~30 filters, 25 file formats, vector flake, HDR/OCIO,
Python API, etc. InkFrame matches that trajectory by keeping each as a plugin:
add `src/brush/SmudgeOp.cpp` or `src/io/PsdImport.cpp` without touching core.
Next verticals: transform masks (they would reuse `InkTransform`/`InkDeform`
verbatim, as Krita's transform mask reuses its workers), cage and mesh transform
modes (Green coordinates / Bezier patches), ANIMATABLE mesh channels (the
parameter block already takes new `ParamId` rows; Warp and Liquify need a keyed
vertex lattice on top, which is why they still bake pixels while Free and
Perspective record), sub-frame sampling (`PlaybackEngine::currentTime()` is the
seam), filter layers, vector overlay, audio scrub, storyboard docker, resource
SQLite sync.
