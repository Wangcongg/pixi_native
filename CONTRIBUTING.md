# Contributing to pixi_native

欢迎一起完善 Android、iOS、macOS 的 Pixi 原生运行时。先阅读 README 和 `docs/pixi8-support-boundary.zh-CN.md`，避免把未实现的能力当成已支持。

## Report an issue

Include the Pixi version, operating system, device/CPU architecture, a minimal example, expected and actual behavior, and relevant error messages. For graphics bugs, include an original Pixi reference and native screenshot when possible. Remove tokens, account data, private game code, internal endpoints and signing material before uploading.

## Make a change

1. Fork the repository and create a focused branch.
2. Edit original source in `src/`, `android/`, `ios/`, `mac/`, `tools/` or an example. Generate runtime output with `node tools/kit.mjs prepare <config>`; never patch or commit `game/` or `generated/`.
3. Run `npm ci`, `npm test` and the CMake/CTest commands from README. Run applicable native builds for changes to platform code or rendering.
4. Update the support-boundary document when adding support, finding a limitation or expanding validation. Run `npm run audit:pixi8` after public export changes. Export presence alone is not semantic coverage.
5. Open a Pull Request describing the concrete behavior change, validation, limitations and the platforms actually run. Clearly distinguish compilation, startup, framebuffer checks and extended gameplay.

Fix the native behavior. Do not mask unsupported APIs with a browser renderer, substitute effects or swallowed errors. Keep SDK credentials, application identities, assets and backend decisions out of runtime defaults. Business SDKs and game-specific component bindings do not belong in the core.

Contribute only code and assets you have the right to share. Project-owned contributions use MIT; preserve third-party licenses and explain any new dependency. Spine is covered by its separate bundled runtime license.
