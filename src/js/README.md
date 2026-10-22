# Host JavaScript sources

`game_host` and `native_scenegraph` are ordered source fragments inside shared closures. Edit these source files and regenerate runtime output with `node tools/kit.mjs prepare <config>`.

The manifests define concatenation order. Generated `game/` output is disposable and must not be committed. The runtime loads only the configured project entry and its assets. JavaScript contract tests are in `tests/`.
