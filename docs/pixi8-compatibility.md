# Pixi 8 compatibility

Baseline: `pixi.js@8.20.0`. The public entry is `src/compat/pixi8.mjs`. Official CPU math and geometry algorithms are vendored with their MIT notice; C++ owns the scene graph and OpenGL ES rendering. There is no JavaScript WebGL/WebGPU renderer fallback.

**This is an incomplete migration.** Read the [Chinese support boundary](pixi8-support-boundary.zh-CN.md), [export inventory](pixi8-api-audit.json) and [validation record](validation.md). Export presence does not establish parameter, lifetime, event or pixel parity.

Use `pixiVersion: 8` in project configuration and the `PIXI` passed to `PixiNativeApp.start`. The entry returns an initialized Application. `globalThis.PIXI` remains an internal resource bridge and should not be used as the public Pixi 8 namespace.

Run `npm run audit:pixi8` after interface changes and update the support boundary with native evidence. Public builds do not include business SDKs, game configurations or remote project defaults.
