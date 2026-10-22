import { V8, unsupported } from "./environment.mjs";
import { RenderTexture, TextureStyle } from "./texture.mjs";
let poolTextureId = 0;
class TexturePoolClass {
  constructor(textureOptions = {}) {
    this.textureOptions = textureOptions;
    this.textureStyle = new TextureStyle(textureOptions);
    this.enableFullScreen = false;
    this._texturePool = /* @__PURE__ */ new Map();
    this._leases = /* @__PURE__ */ new WeakMap();
  }
  createTexture(pixelWidth, pixelHeight, antialias = false, autoGenerateMipmaps = false) {
    if (antialias) unsupported("TexturePool antialias");
    if (autoGenerateMipmaps) unsupported("TexturePool autoGenerateMipmaps");
    const texture = RenderTexture.create({ ...this.textureOptions, width: pixelWidth, height: pixelHeight, resolution: 1 });
    texture.label = "texturePool_" + ++poolTextureId;
    return texture;
  }
  getOptimalTexture(frameWidth, frameHeight, resolution = 1, antialias = false, autoGenerateMipmaps = false) {
    if (![frameWidth, frameHeight, resolution].every((v) => Number.isFinite(v) && v > 0)) throw new RangeError("TexturePool dimensions and resolution must be positive");
    if (antialias) unsupported("TexturePool antialias");
    if (autoGenerateMipmaps) unsupported("TexturePool autoGenerateMipmaps");
    const width = V8.nextPow2(Math.ceil(frameWidth * resolution - 1e-6)), height = V8.nextPow2(Math.ceil(frameHeight * resolution - 1e-6)), key = width + "x" + height;
    const free = this._texturePool.get(key) ?? [];
    let texture;
    while (free.length && !texture) {
      const candidate = free.pop();
      if (!candidate.destroyed) texture = candidate;
    }
    texture ??= this.createTexture(width, height, false, false);
    texture.source.resize(width / resolution, height / resolution, resolution);
    // Explicit frame assignment exits auto-sized/noFrame mode. Mutating the
    // old frame in place would let a later FBO source update erase the crop.
    texture.frame = new V8.Rectangle(0, 0, frameWidth, frameHeight);
    this._texturePool.set(key, free);
    this._leases.set(texture, key);
    return texture;
  }
  getSameSizeTexture(texture, antialias = false) {
    return this.getOptimalTexture(texture.width, texture.height, texture.source.resolution, antialias);
  }
  returnTexture(texture, resetStyle = false) {
    const key = this._leases.get(texture);
    if (!key) throw new Error("Texture is not checked out from this pool");
    if (texture.destroyed) throw new Error("Cannot return a destroyed texture");
    if (resetStyle) texture.source.style = this.textureStyle;
    const free = this._texturePool.get(key) ?? [];
    free.push(texture);
    this._texturePool.set(key, free);
    this._leases.delete(texture);
  }
  clear(destroyTextures = true) {
    if (destroyTextures) for (const free of this._texturePool.values()) for (const texture of free) texture.destroy(true);
    this._texturePool.clear();
  }
}
const TexturePool = new TexturePoolClass();
export {
  TexturePool,
  TexturePoolClass
};
