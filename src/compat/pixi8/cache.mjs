import { unsupported } from "./environment.mjs";
const runtime = () => globalThis.__nativeSceneRuntime;
function disposeCacheTexture(node) {
  const data = node._cacheData;
  // The ABI 1 cache flag setter can clear its cache record while publishing the
  // live view. Detach the record first so we retain and release both resources.
  node._cacheData = null;
  node.__nativeCachedTextureActive = false;
  node.__nativeV8Cache = false;
  node._cacheAsBitmap = false;
  if (data) {
    data.sprite?.destroy();
    data.__nativeRT?.destroy(true);
  }
  runtime().releaseVisual(node);
  runtime().writeHitMeta(node);
  runtime().markVisual(node);
}
function setCacheAsTexture(node, value = true) {
  if (value === false) {
    if (node.__nativeV8Cache) disposeCacheTexture(node);
    return;
  }
  const options = typeof value === "object" ? value : {};
  if (options.antialias) unsupported("cacheAsTexture.antialias");
  const resolution = options.resolution ?? null;
  if (resolution !== null && (!Number.isFinite(resolution) || resolution <= 0)) throw new RangeError("cacheAsTexture resolution must be positive");
  if (node.__nativeV8Cache) disposeCacheTexture(node);
  node._cacheData = { __nativeBakeDirty: 1 };
  node.__nativeV8Cache = true;
  node.__nativeDisposeCacheTexture = () => disposeCacheTexture(node);
  node._cacheAsBitmapResolution = resolution;
  node._cacheAsBitmap = true;
  runtime().markVisual(node);
}
function updateCacheTexture(node) {
  if (!node.__nativeV8Cache) return;
  node._cacheData.__nativeBakeDirty = 1;
  node.__nativeCachedTextureActive = false;
  runtime().writeHitMeta(node);
  runtime().markVisual(node);
}
export {
  disposeCacheTexture,
  setCacheAsTexture,
  updateCacheTexture
};
