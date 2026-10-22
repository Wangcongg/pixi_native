# pixi_native

- This is an independent native Pixi runtime. Do not add game business code, internal endpoints, SDK credentials, account/reward/payment logic or project defaults.
- Edit original source. `game/`, `generated/`, build output and dependency caches are disposable and must not be committed.
- Consult `docs/pixi8-support-boundary.zh-CN.md` before integrating or packaging a game. Disclose unsupported APIs, partial behavior and unverified platforms. Keep the document current.
- Preserve native rendering semantics. Do not hide unsupported behavior using browser fallback rendering, substitute effects or swallowed errors.
- Run JS contract tests and applicable native builds. Report which platforms were compiled and which were actually run.
- Keep project-owned code MIT and preserve all third-party licensing notices. Spine is not MIT.
