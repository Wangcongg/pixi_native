import {P} from './environment.mjs'
export const {FederatedEvent,FederatedPointerEvent,FederatedMouseEvent,FederatedWheelEvent}=P
export class EventBoundary extends P.EventBoundary{hitTestFn(target,location){if(target.__nativeV8LocalHit){const local=target.toLocal(location);return target.containsPoint(local)}return super.hitTestFn(target,location)}}
