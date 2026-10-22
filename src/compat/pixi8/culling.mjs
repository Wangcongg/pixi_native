import {V8} from './environment.mjs'
export class Culler{
  static shared=new Culler()
  cull(container,view,skipUpdateTransform=true){this._cullRecursive(container,view,skipUpdateTransform)}
  _cullRecursive(container,view,skipUpdateTransform=true){
    if(container.cullable&&container.measurable!==false&&container.includeInBuild!==false){
      if(container.cullArea){const m=container.getGlobalTransform(new V8.Matrix(),skipUpdateTransform);container.culled=!new V8.Rectangle(view.x,view.y,view.width,view.height).intersects(container.cullArea,m)}
      else{const b=container.getBounds(skipUpdateTransform);container.culled=b.x>=view.x+view.width||b.y>=view.y+view.height||b.x+b.width<=view.x||b.y+b.height<=view.y}
    }else container.culled=false
    if(container.cullableChildren===false||container.culled||!container.renderable||container.measurable===false||container.includeInBuild===false)return
    for(const child of container.children)this._cullRecursive(child,view,skipUpdateTransform)
  }
}
