# Fix Perspective / Warp / Liquify under keyframes

## Goal
Perspective, Warp and Liquify must behave exactly like Free on a keyframed layer:
the box outlines the stroke the user SEES, every grab lands where the cursor is,
drags stay responsive, and nothing hangs or crashes. No regressions to Free.

## Non-negotiables
- No auto-keying. RECORD stays opt-in.
- Free mode's verified behaviour is the reference; do not change its semantics.
- Release previews; Enter / Apply / double-click lands; Esc reverts (Krita model).
- One space per computation: pointer -> the space the state lives in.
- No test may derive its expectation from the value under test.

---

## Phase 0 - Reproduce (headless, deterministic)
New test `tests/ui/test_transform_record_modes.cpp` (or a section in
`test_transform_keyframe_modes.cpp`) replaying the reported gesture with a
NON-translation pose (Rotation 30 deg + ScaleX 1.2 keyed at frame 0, RECORD on):

1. For each mode: press, 12 moves, release, Apply, then switch modes and repeat.
2. Assertions per step: params finite (`std::isfinite` on every keyed channel);
   a live session after every commit; exactly one undo step per Apply;
   the drawn box within N px of the ink's DISPLAYED bounds (computed from
   `ParamBlock::matrixFor` independently, never from a tool getter);
   a grabbed handle under the cursor resolves to that grip.
3. Stress case: switch Free->Perspective->Warp->Liquify mid-record, and Apply
   during a pending drag, to surface the re-entrancy crash.
4. Timing: run 200 preview frames and report the wall clock next to the Free
   baseline; fail only on an absurd multiple, so CI stays stable.

Expected to FAIL. This phase is the reproduction, not the fix.

## Phase 1 - One display transform, not an offset (fixes Warp + Liquify + the box)
- `InkTransformTool.h`: replace `Vec2 dispOffset_` with `Mat3 dispMatrix_`
  (device -> display) and `Mat3 dispInv_` (inverse; identity when singular/absent).
- `beginSession()` (~:212-231): build the matrix from the selection's
  `documentMatrix()` when `offsetActive()`, else from the layer's
  `matrixFor(frameOr(canvas), currentFrame)` - the code already there, keeping
  the whole matrix instead of `m[2], m[5]`. Invert once; fall back to identity.
- `toDevice(p)` becomes `dispInv_.map(p)` - the single pointer-mapping entry
  point every mode already goes through, so Warp's `warpAnchorAt`/`dragWarp`
  and Liquify's brush are fixed by construction.
- Add `toDisplay(Vec2)` for drawing: `frameCorners`, `pivotShown`, the Warp
  anchors, the Warp/Liquify outlines.
- Keep `offsetForTest()`; add `displayMatrixForTest()` / `toDeviceForTest()` so
  tests can assert the mapping itself.
- Overlay: the Warp region outline becomes a POLYGON through `dispMatrix_`
  (4 corners of the mesh moved bounds), not `drawRect` + offset (:1509).
  Liquify gains the same outline - it currently draws none (:1472).

## Phase 2 - Perspective in the space its corners live in
- Record mode: corners are frame space. Map the pointer through
  `recordedAffine()`'s inverse before writing it into a corner, and capture
  `perspStart_` from `persp_.corners` directly instead of round-tripping
  through `frameCorners()` (:924).
- Pixel mode: `perspStart_` = device-space corners; `p = toDevice(...)`.
- `dragHandleStart_` for a perspective corner comes from the corners' space.
- One `Mat3 displayMatrix()` (record: `recordedAffine() o quad`; pixel:
  `dispMatrix_ o quad`) used by the drawn box, hit-testing and the overlay, so
  the cage can never disagree with the compositor. Delete the "exact while the
  pose is at rest" caveat (:620).
- `Grip::Outside` rotates about the pivot in Perspective, matching Free and the
  hit-test comment (:692); fall back to translate when the pivot is inside.
- `recordedPoseChanged()`: compare the affine channels in Perspective mode too,
  so a rotation keyed from the panel re-seeds the cage.
- Degeneracy guard: ignore a corner drag that would collapse the quad (three
  near-collinear corners), keeping the last valid quad.

## Phase 3 - Responsiveness (the hang/lag)
- Gate EVERY preview path on `host()->nowSec()` (~30fps), not just Liquify,
  reusing the existing "last move always lands" queue flag.
- While dragging, resample Bilinear regardless of the quality combo; run the
  chosen filter once on release. Add the missing release re-render for
  Warp/Perspective, which currently leaves a throttled frame on screen.
- Cap per-frame work: clamp the preview `dst` to the region the deformation can
  reach, and skip the resample when nothing changed.
- Record mode: at most one queued rebuild per frame, one key write per channel.

## Phase 4 - Remove the crash hazards
- `Document::beginParamRecord`: refuse to start while a record is open
  (close-and-push, or assert in debug) instead of silently dropping the
  in-flight undo state (InkDocument.cpp:908).
- Make the record/reseed cycle non-reentrant with a `committing_` guard, so
  `animationRecorded` -> timeline refresh cannot re-enter `beginSession()`.
- `endParamRecord`: emit `animationRecorded` after the record is closed and the
  document is consistent (queue it if a rebuild is in flight).
- Defensive finiteness check on every value the tool writes to the model
  (`pushRecordedParams`, warp anchors, liquify field): a non-finite value
  reaching `quadToQuad`/`mappedBounds` is the one path that could produce a
  garbage rect.

## Phase 5 - Tests
- Phase 0's suite, plus: bake/record parity for Perspective; box alignment per
  mode; warp/liquify on a keyed ROTATED layer deform where the pointer is;
  liquify has a region box.
- Rework the existing "displaced layer" tests to use a rotation (not just
  PositionX) and to compute expected display positions from `matrixFor`
  directly, so a wrong `dispMatrix_` fails the test.
- Whole existing suite stays green (35 Qt / 22 headless baseline).

## Files
- src/ui/tools/InkTransformTool.h / .cpp    Phases 1-4 (the bulk)
- src/ui/InkDocument.cpp                    beginParamRecord / re-entrancy
- src/ui/InkMainWindow.cpp                  animationRecorded ordering, if needed
- src/core/InkParams.cpp                    only if a guard is needed; no semantics
- tests/ui/test_transform_keyframe_modes.cpp + new test file
- tests/test_deform.cpp                     degenerate-lattice cases

## Out of scope (flagged, needs your call)
Warp and Liquify have NO keyframe representation: a mesh cannot be expressed by
the 18 transform channels, so `canRecord()` excludes them
(InkTransformTool.cpp:291) and they always bake pixels - which is what Krita
does too. Today the RECORD toggle simply HIDES itself for those modes
(:1574, :2101), so checking it and switching to Warp does nothing at all.
Phase 1 makes that honest: visible but disabled, with a tooltip and a status
line saying the deformation bakes to pixels. Making them genuinely keyframable
means storing a lattice in the ParamBlock and applying it at composite time -
a separate, much larger feature.
