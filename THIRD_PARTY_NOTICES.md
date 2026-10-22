# Third party components

The MIT license at the repository root applies to project-owned runtime code, tools, tests and examples. It does not relicense the components below. Preserve their notices when redistributing source or binaries.

| Component | Location | Applicable notice |
| --- | --- | --- |
| PixiJS 7.4.3 | `vendor/pixi.v7.4.3.js` | MIT, `vendor/licenses/pixi.LICENSE` |
| PixiJS 8.20.0 reference and CPU algorithms | `vendor/pixi.v8.20.0.mjs` | MIT, `vendor/pixi.v8.20.0.LICENSE` |
| Spine C++ and Spine Pixi runtime | `third_party/spine-cpp/`, `vendor/spine-pixi-v7.js` | Spine Runtimes License, `third_party/spine-cpp/LICENSE` and `vendor/licenses/spine.LICENSE` |
| stb_image | `third_party/stb/stb_image.h` | Dual licensing alternatives in the source footer |
| Zstandard decoder | `src/third_party/zstddeclib.c` | Embedded notices and `src/third_party/licenses/zstd.LICENSE` |
| Gradle wrapper | `android/gradlew*`, `android/gradle/wrapper/` | Apache-2.0, https://github.com/gradle/gradle |
| esbuild | Build dependency in `package-lock.json` | MIT, resolved from npm |
| V8 prebuilt | Downloaded during Android configuration, not committed | Pinned public upstream and SHA-256 in `tools/fetch_v8_prebuilt.sh` and `tools/v8_prebuilt.sha256`; retain upstream V8 and dependency notices |
| QuickJS | Optional source download during Android configuration | Upstream MIT notice from pinned https://github.com/bellard/quickjs |
| ANGLE | External macOS build prerequisite, not committed | Upstream ANGLE license and its dependencies, https://chromium.googlesource.com/angle/angle/ |

**Spine is not MIT.** Its runtime remains linked by the current native build even when the JavaScript Spine feature is disabled. Read the bundled Spine Runtimes License before using or redistributing a Spine-enabled build; the root MIT license does not waive its requirements. Moving Spine behind a truly optional native build extension is tracked in the roadmap.

No advertising, billing, login, analytics, Firebase or Unity SDK is included in this public runtime. Standard AndroidX and OkHttp dependencies are resolved from Google Maven/Maven Central and retain their upstream licenses.
