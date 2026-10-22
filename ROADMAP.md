# Roadmap

The following are open work areas, not promised delivery dates.

- Expand Pixi 8 semantic coverage: RenderGroup/RenderLayer, complete event behavior, advanced text and texture APIs.
- Define explicit native extension interfaces for Shader/Filter pipelines without browser fallback rendering.
- Make Spine and auxiliary UI bridges optional native extensions with clear version and licensing boundaries.
- Add reproducible, pinned macOS ANGLE dependency provisioning and broader pixel conformance coverage.
- Extend Android and Apple real-device validation, lifecycle cancellation and multi-touch tests.
- Measure long gameplay sessions, native/GPU memory and sustained frame pacing on real devices.
- Add signed Android/iOS/macOS release workflows using contributor-owned signing configuration, never repository defaults.
- Improve documentation and small, independent examples for each validated API path.

The first coverage increment now implements generic float32x2 Geometry, official Plane/Rope/Perspective CPU geometry on native meshes, Unicode bitmap atlas growth, native cacheAsTexture refresh and RenderTexture pooling. These features share their JavaScript and C++ implementation across Android, iOS and macOS; platform evidence and remaining semantic limits are recorded in docs/validation.md and docs/pixi8-support-boundary.zh-CN.md.
