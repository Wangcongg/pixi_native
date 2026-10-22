// Public Pixi 8.20 API; scene handles and drawing are owned by Native Kit, never a JS renderer.
export {UnsupportedNativeAPIError} from './pixi8/environment.mjs'
export * from './pixi8/application.mjs'
export * from './pixi8/container.mjs'
export * from './pixi8/texture.mjs'
export * from './pixi8/texture-pool.mjs'
export * from './pixi8/sprite.mjs'
export * from './pixi8/graphics.mjs'
export * from './pixi8/text.mjs'
export * from './pixi8/mesh.mjs'
export * from './pixi8/filters.mjs'
export * from './pixi8/particle.mjs'
export * from './pixi8/culling.mjs'
export {FederatedEvent,FederatedPointerEvent,FederatedMouseEvent,FederatedWheelEvent,EventBoundary} from './pixi8/events.mjs'
export {VERSION,Color,Matrix,Point,ObservablePoint,Rectangle,Circle,Ellipse,Polygon,RoundedRectangle,Triangle,Bounds,groupD8,DEG_TO_RAD,RAD_TO_DEG,PI_2,ExtensionType,extensions,EventEmitter,BufferUsage} from '../../vendor/pixi.v8.20.0.mjs'
// These algorithms manage CPU data only; native scene/GPU systems are never
// replaced by the official JavaScript renderer infrastructure.
export {Transform,ViewableBuffer,TextureUvs,TextureMatrix,Pool,PoolGroupClass,DOMAdapter,earcut,isPow2,nextPow2,log2,removeItems,uid} from '../../vendor/pixi.v8.20.0.mjs'
import {P} from './pixi8/environment.mjs'
export const loadTextures=P.loadTextures
