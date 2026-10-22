import { P, V8, unsupported } from "./environment.mjs";
import { applyOptions, installContainerAPI, installViewHitAPI } from "./container.mjs";
import { destroyOwnedTextures, withoutTextureOwnership } from "./lifetime.mjs";
const Buffer = V8.Buffer;
// Reuse pinned upstream CPU geometry. Native consumes packed buffers, never
// Pixi's JavaScript GL renderer or its renderer-specific GPU objects.
function withLayoutNotifications(Base) {
  return class extends Base {
    addAttribute(...args) {
      const result = super.addAttribute(...args);
      this.emit("update", this);
      return result;
    }
    addIndex(...args) {
      const result = super.addIndex(...args);
      this.emit("update", this);
      return result;
    }
  };
}
class Geometry extends withLayoutNotifications(V8.Geometry) {
  static [Symbol.hasInstance](value) {
    return value instanceof V8.Geometry;
  }
}
class MeshGeometry extends withLayoutNotifications(V8.MeshGeometry) {
}
class PlaneGeometry extends withLayoutNotifications(V8.PlaneGeometry) {
}
class RopeGeometry extends withLayoutNotifications(V8.RopeGeometry) {
}
class PerspectivePlaneGeometry extends withLayoutNotifications(V8.PerspectivePlaneGeometry) {
}
const modes = { "triangle-list": 4, "triangle-strip": 5, "line-list": 1, "line-strip": 3, "point-list": 0 };
const nativeGeometry = Object.getOwnPropertyDescriptor(P.Mesh.prototype, "geometry");
const prepareOwners = /* @__PURE__ */ new Set();
function readAttribute(geometry, name) {
  const a = geometry.attributes?.[name];
  if (!a) throw new TypeError("Mesh geometry requires " + name);
  if (a.instance || geometry.instanceCount !== 1) unsupported("Geometry instanced attributes");
  if (a.format !== void 0 && a.format !== "float32x2") unsupported("Geometry." + name + ".format", a.format);
  const data = a.buffer.data;
  if (!(data instanceof Float32Array)) unsupported("Geometry." + name, "requires float32x2 data");
  const stride = (a.stride ?? 8) / 4, offset = (a.offset ?? 0) / 4;
  if (!Number.isInteger(stride) || stride < 2 || !Number.isInteger(offset) || offset < 0) throw new RangeError("Invalid " + name + " stride/offset");
  if (stride === 2 && offset === 0) return data;
  const count = Math.max(0, Math.floor((data.length - offset - 2) / stride) + 1), packed = new Float32Array(count * 2);
  for (let i = 0; i < count; i++) {
    packed[i * 2] = data[offset + i * stride];
    packed[i * 2 + 1] = data[offset + i * stride + 1];
  }
  return packed;
}
function bridgeGeometry(source, owner) {
  if (source instanceof P.Geometry) return { native: source, ensure() {
  }, dispose() {
  } };
  if (!(source instanceof V8.Geometry)) throw new TypeError("Mesh requires a Geometry");
  if (!(source.topology in modes)) unsupported("Mesh geometry topology", source.topology);
  const native = new P.MeshGeometry(), listeners = [];
  let dirty = true, positionAttribute, uvAttribute, indexBuffer, topology;
  // Delay packing until native reads the geometry: upstream build() replaces
  // position, UV and index buffers in separate synchronous assignments.
  const invalidate = () => {
    dirty = true;
    globalThis.__nativeSceneRuntime.markVisual(owner);
  };
  const sync = () => {
    const positions = readAttribute(source, "aPosition"), uvs = readAttribute(source, "aUV");
    if (positions.length !== uvs.length) throw new RangeError("Geometry position and UV vertex counts differ");
    const indices = source.indexBuffer?.data ?? new Uint32Array(Array.from({ length: positions.length / 2 }, (_, i) => i));
    if (!(indices instanceof Uint16Array || indices instanceof Uint32Array)) unsupported("Geometry indexBuffer", "requires uint16/uint32 data");
    for (const [name, data] of [["aVertexPosition", positions], ["aTextureCoord", uvs]]) {
      const buffer = native.getBuffer(name);
      buffer.data = data;
      buffer.update();
    }
    native.getIndex().data = indices;
    native.getIndex().update();
    owner.drawMode = modes[source.topology];
    owner.uvs = null;
    positionAttribute = source.attributes.aPosition;
    uvAttribute = source.attributes.aUV;
    indexBuffer = source.indexBuffer;
    topology = source.topology;
    dirty = false;
  };
  const watchBuffers = () => {
    for (const [buffer, event] of listeners) buffer.off(event, invalidate);
    listeners.length = 0;
    for (const buffer of new Set(source.buffers)) {
      for (const event of ["update", "change"]) {
        buffer.on(event, invalidate);
        listeners.push([buffer, event]);
      }
    }
  };
  sync();
  watchBuffers();
  source.on("update", invalidate);
  const destroyed = () => {
    owner.geometry = null;
  };
  source.on("destroy", destroyed);
  return { native, ensure() {
    if (dirty || source.attributes?.aPosition !== positionAttribute || source.attributes?.aUV !== uvAttribute || source.indexBuffer !== indexBuffer || source.topology !== topology) {
      const structureChanged = source.attributes?.aPosition !== positionAttribute || source.attributes?.aUV !== uvAttribute || source.indexBuffer !== indexBuffer;
      sync();
      if (structureChanged) watchBuffers();
    }
  }, dispose() {
    for (const [buffer, event] of listeners) buffer.off(event, invalidate);
    source.off("update", invalidate);
    source.off("destroy", destroyed);
  } };
}
class Mesh extends P.Mesh {
  constructor(options = {}) {
    if (options.shader) unsupported("Mesh custom Shader");
    const geometry = options.geometry ?? new MeshGeometry();
    // Reject before allocating a scene handle; invalid input must not leak one.
    if (!(geometry instanceof P.Geometry)) {
      if (!(geometry instanceof V8.Geometry)) throw new TypeError("Mesh requires a Geometry");
      if (!(geometry.topology in modes)) unsupported("Mesh geometry topology", geometry.topology);
      const p = readAttribute(geometry, "aPosition"), u = readAttribute(geometry, "aUV");
      if (p.length !== u.length) throw new RangeError("Geometry position and UV vertex counts differ");
      const index = geometry.indexBuffer?.data;
      if (index && !(index instanceof Uint16Array || index instanceof Uint32Array)) unsupported("Geometry indexBuffer", "requires uint16/uint32 data");
    }
    super(geometry, new P.MeshMaterial(options.texture ?? P.Texture.EMPTY));
    this.autoUpdate = options.autoUpdate ?? true;
    applyOptions(this, options);
  }
  get geometry() {
    return this.__pixiGeometry ?? null;
  }
  set geometry(value) {
    if (value === this.__pixiGeometry) return;
    const next = value ? bridgeGeometry(value, this) : null;
    this.__geometryBridge?.dispose();
    this.__pixiGeometry = value;
    this.__geometryBridge = next;
    nativeGeometry.set.call(this, next?.native ?? null);
  }
  get __nativeMeshGeometry() {
    this.__geometryBridge?.ensure();
    return this._geometry;
  }
  get verticesBuffer() {
    return this.__nativeMeshGeometry?.buffers[0];
  }
  get uvBuffer() {
    return this.__nativeMeshGeometry?.buffers[1];
  }
  destroy(options) {
    if (this.destroyed) return;
    const texture = this.texture, geometry = this.geometry;
    prepareOwners.delete(this);
    P.Mesh.prototype.destroy.call(this, withoutTextureOwnership(options));
    if (options === true || options?.geometry) geometry?.destroy(true);
    destroyOwnedTextures([texture], options);
  }
}
installContainerAPI(Mesh.prototype);
installViewHitAPI(Mesh.prototype);
class MeshSimple extends Mesh {
  constructor(o = {}) {
    super({ ...o, geometry: new MeshGeometry({ positions: o.vertices ?? o.positions, uvs: o.uvs, indices: o.indices, topology: o.topology }) });
    prepareOwners.add(this);
  }
  get vertices() {
    return this.geometry.positions;
  }
  set vertices(value) {
    this.geometry.positions = value;
  }
  _prepareGeometry() {
    if (this.autoUpdate) this.geometry.getBuffer("aPosition").update();
  }
}
class MeshPlane extends Mesh {
  constructor(o = {}) {
    const texture = o.texture ?? P.Texture.EMPTY;
    super({ ...o, texture, geometry: new PlaneGeometry({ width: texture.width, height: texture.height, verticesX: o.verticesX ?? 10, verticesY: o.verticesY ?? 10 }) });
    this.autoResize = o.autoResize ?? true;
    prepareOwners.add(this);
  }
  textureUpdated() {
    const g = this.geometry, t = this.texture;
    if (g && this.autoResize && (g.width !== t.width || g.height !== t.height)) {
      g.width = t.width;
      g.height = t.height;
      g.build({});
    }
  }
  _prepareGeometry() {
    this.textureUpdated();
  }
}
class MeshRope extends Mesh {
  constructor(o = {}) {
    const texture = o.texture ?? P.Texture.EMPTY, textureScale = o.textureScale ?? 0;
    super({ ...o, texture, geometry: new RopeGeometry({ width: o.width ?? texture.height, points: o.points ?? [], textureScale }) });
    if (textureScale > 0) {
      texture.source.style.addressMode = "repeat";
      texture.source.style.update();
    }
    prepareOwners.add(this);
  }
  _prepareGeometry() {
    const g = this.geometry;
    if (this.autoUpdate || g._width !== this.texture.height) {
      g._width = this.texture.height;
      g.update();
    }
  }
}
class PerspectiveMesh extends Mesh {
  constructor(options = {}) {
    const o = { ...V8.PerspectiveMesh.defaultOptions, ...options }, texture = options.texture ?? P.Texture.WHITE;
    super({ ...o, texture, geometry: new PerspectivePlaneGeometry({ width: texture.width, height: texture.height, verticesX: o.verticesX, verticesY: o.verticesY }) });
    this.setCorners(o.x0, o.y0, o.x1, o.y1, o.x2, o.y2, o.x3, o.y3);
    prepareOwners.add(this);
  }
  setCorners(...corners) {
    return this.geometry.setCorners(...corners);
  }
  textureUpdated() {
    const g = this.geometry, t = this.texture;
    if (g && (g.width !== t.width || g.height !== t.height)) {
      g.width = t.width;
      g.height = t.height;
      g.updateProjection();
    }
  }
  _prepareGeometry() {
    this.textureUpdated();
  }
}
const before = globalThis.__nativePreparePixi8Render;
globalThis.__nativePreparePixi8Render = function(root) {
  before?.(root);
  for (const o of prepareOwners) {
    if (o.destroyed) {
      prepareOwners.delete(o);
      continue;
    }
    for (let p = o; p; p = p.parent) {
      if (!p.visible || !p.renderable || p.culled || p.alpha <= 0) break;
      if (p === root) {
        o._prepareGeometry();
        break;
      }
    }
  }
};
export {
  Buffer,
  Geometry,
  Mesh,
  MeshGeometry,
  MeshPlane,
  MeshRope,
  MeshSimple,
  PerspectiveMesh,
  PerspectivePlaneGeometry,
  PlaneGeometry,
  RopeGeometry
};
