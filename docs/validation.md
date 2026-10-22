# Validation of the public source snapshot

Date: 2026-10-22. Only independent examples were used; no game bundle or business SDK is part of this acceptance.

| Check | Evidence | Limits |
| --- | --- | --- |
| Clean dependency install | `npm ci --offline` succeeded with the locked public dependencies | Local npm cache was available; the lockfile describes the public registry dependencies |
| JavaScript contracts | `npm test`: 130/130 passed | API and lifetime contracts, not every Pixi feature or native GPU path |
| C++ core contracts | CMake/CTest: 10/10 passed | Includes PMA fixture decoding and a 20,000-frame pending queue test; not a long GPU-memory soak |
| Android build | Debug APK compiled for arm64-v8a and x86_64, JDK 21, SDK 36, NDK 27.2 | No Android device or emulator was connected for this public snapshot; not run on Android |
| iOS build | arm64 iOS Simulator app linked successfully with Xcode 27 SDK, deployment target 15.0, CMake Unix Makefiles | No iOS Simulator/device run; unsigned simulator output only |
| macOS build | arm64 AppKit/JavaScriptCore app linked, deployment target 12.0, external ANGLE/Metal | Intel macOS not built or run in this checkpoint |
| macOS runtime | Hello Pixi 8 cold-started, rendered text and a textured rotating sprite, and exited after a framebuffer screenshot | No live user mouse/keyboard interaction or extended gameplay acceptance |
| macOS Pixi 8 conformance | 34/34 independent native/framebuffer cases passed | No Android system-input probe; no full Spine/third-party-plugin acceptance |

The machine-readable macOS result and individual case names are in [macos-conformance-2026-10-22.json](macos-conformance-2026-10-22.json). The public extraction also fixed `Texture.from`/`Sprite.from` with an already-loaded native texture, macOS background-color forwarding and PMA source metadata/decoding; these were found during actual native startup and pixel comparison.

Long gameplay, sustained phone FPS, GPU-memory growth, mobile haptics, iOS runtime behavior and full Pixi 8 parity remain unverified. The presence of source or a successful build must not be described as real-device acceptance. Read [the complete support boundary](pixi8-support-boundary.zh-CN.md).

Build output, raw machine logs, downloaded engine binaries, signing material, game resources and the private repository history are excluded from the public Git snapshot. Local build and npm caches were used only as build prerequisites; all committed build inputs reference public upstreams or the project itself.
