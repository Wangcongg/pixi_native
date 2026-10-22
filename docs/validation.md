# Validation of the public runtime

Date: 2026-10-22. Baseline: `pixi.js@8.20.0`. All checks use independent examples, without a game bundle or business SDK.

| Check | Current evidence | Limits |
| --- | --- | --- |
| JavaScript contracts | `npm test`: 142/142 passed | Includes geometry layout/lifetime, dynamic Unicode atlas growth, cache disposal, native texture pooling and exhausted screen-work quotas; not every Pixi API |
| C++ core contracts | CMake/CTest: 10/10 passed | Includes cached descendants retaining input, PMA decoding and a 20,000-frame pending queue; not a long GPU-memory soak |
| Android compilation | Debug APK compiled for arm64-v8a and x86_64; JDK 21, SDK 36, NDK 27.2 | No signed release acceptance |
| Android execution | 42/42 native/framebuffer cases passed on three consecutive cold starts of an arm64 Android API 35 emulator with 16,384-byte pages | ANGLE/SwiftShader software GPU; not a physical phone, real-touch, haptic or sustained-FPS acceptance |
| iOS compilation | arm64 iOS Simulator app linked with Xcode 27 SDK, deployment target 15.0 | No available Simulator runtime/device execution; iOS runtime remains unverified |
| macOS compilation and execution | arm64 AppKit/JavaScriptCore app built and run through ANGLE/Metal; 42/42 native/framebuffer cases passed | Deployment target 12.0; Intel macOS and live system input not covered |

The sanitized per-case Android and macOS results are in [the coverage increment record](pixi8-extension-validation-2026-10-22.json). Both platforms run the same `examples/pixi8-conformance` source. Its additional cases cover generic interleaved/non-indexed Geometry updates, PerspectiveMesh, automatic Chinese/supplementary Unicode bitmap glyphs, cache opacity/freeze/refresh/resolution/empty output, filter application and child hit testing, pooled native targets/crops, native DOM adapter constructors, and 64 consecutive cache bake/consume/destroy cycles. The burst checks the final pixels and that live native handles return to their starting count; it is not a long gameplay soak.

The increment also fixes two cross-boundary issues exposed by native execution: cached proxies losing their disposal record when the legacy flag setter runs, and an exhausted screen-work quota deferring the contents of an explicit RenderTexture snapshot. Snapshot subtrees now complete synchronously while unrelated screen work retains its quota. The native offscreen producer submits GPU work and establishes a GPU/server fence dependency before the encoded GL consumer resumes, and invalidates Pixi's GL state cache immediately. There is no CPU completion wait or pixel-readback rendering workaround.

Project-owned CPU geometry implementations reuse the pinned upstream algorithms. The native backend accepts 2D float32 position/UV layouts, including stride/offset, uint16/uint32 indices and non-indexed meshes. Unsupported layouts/instancing, custom shaders, MSAA and texture-pool automatic mipmaps remain explicit limits. Dynamic bitmap pages preserve existing glyph textures and automatic font references are released with the last view; cross-batch kerning and complex script shaping remain outside this acceptance.

The [initial extraction record](macos-conformance-2026-10-22.json) remains available as historical evidence (34 macOS cases). Dependency versions and the public lockfile are unchanged from its successful clean install. Build output, raw machine logs, engine binaries, signing material, game resources and private source history are excluded from Git.

Long gameplay, sustained phone FPS, GPU-memory growth, physical haptics, iOS execution and full Pixi 8 parity remain unverified. Read [the complete support boundary](pixi8-support-boundary.zh-CN.md); a successful compile or startup is not complete compatibility acceptance.
