#pragma once
// Shared layer vocabulary. Used by the timeline layer tree (Layer in
// InkLayer.h) and by the per-cell layer stack (CellLayer in InkCell.h). Kept
// in its own header so both can include it without a cycle.
namespace ink {

// Adjustment layers hold keyframed parameters and target ONE paint layer
// (AdjustmentLayer::targetId) — they render nothing themselves and own no
// cells, so every layer-walk site must skip them once it has contributed its
// transform.
//
// Camera layers (Clip-Studio-style 2D camera) hold keyframed parameters and
// target ONE paint layer, ONE folder, or the whole project (Global). They
// render nothing themselves and own no cells. A layer-scoped camera moves one
// track, a folder-scoped camera moves its folder as a unit, a global camera
// reframes the final composite. Invisible or identity cameras are inert.
enum class LayerType { Paint, Group, Adjustment, Camera };
// What a camera layer frames: one track, one folder, or the final image.
enum class CameraScope : int { Layer = 0, Folder = 1, Global = 2 };
enum class BlendMode { Normal, Multiply, Screen, Erase };

} // namespace ink
