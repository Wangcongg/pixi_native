import {V8} from './environment.mjs'
// A gradient is a generated image resource. Rasterize its analytic color field into a
// BufferImageSource; the native renderer owns the resulting texture and all drawing.
// This also avoids depending on a partial browser Canvas2D implementation.
function stopsOf(stops){for(const s of stops)if(!Number.isFinite(s.offset)||s.offset<0||s.offset>1)throw new RangeError('Gradient color stop offset must be in [0, 1]');return (stops.length?stops:[{offset:0,color:'white'},{offset:1,color:'black'}]).map((s,i)=>({offset:s.offset,color:new V8.Color(s.color).toArray(),i})).sort((a,b)=>a.offset-b.offset||a.i-b.i)}
function write(data,index,t,stops){if(!Number.isFinite(t))return;let a=stops[0],b=a;for(const stop of stops){if(stop.offset<=t)a=stop;else{b=stop;break}b=a}const mix=b.offset===a.offset?0:Math.min(1,Math.max(0,(t-a.offset)/(b.offset-a.offset)));for(let c=0;c<4;c++)data[index+c]=Math.round((a.color[c]+(b.color[c]-a.color[c])*mix)*255)}
function texture(data,width,height,wrap){return new V8.Texture({source:new V8.BufferImageSource({resource:data,width,height,format:'rgba8unorm',alphaMode:'premultiply-alpha-on-upload',addressMode:wrap})})}
export class FillGradient extends V8.FillGradient{
  buildLinearGradient(){
    if(this.texture)return
    let {x:x0,y:y0}=this.start,{x:x1,y:y1}=this.end,dx=x1-x0,dy=y1-y0
    const flip=dx<0||dy<0
    if(this._wrapMode==='clamp-to-edge'){if(dx<0){[x0,x1]=[x1,x0];dx=-dx}if(dy<0){[y0,y1]=[y1,y0];dy=-dy}}
    const size=this._textureSize,data=new Uint8Array(size*4),stops=stopsOf(this.colorStops)
    for(let x=0;x<size;x++)write(data,x*4,flip?1-(x+.5)/size:(x+.5)/size,stops)
    this.texture=texture(data,size,1,this._wrapMode)
    this.transform=new V8.Matrix().scale(Math.hypot(dx,dy)/size,1).rotate(Math.atan2(dy,dx)).translate(x0,y0)
    if(this.textureSpace==='local')this.transform.scale(size,size)
  }
  buildRadialGradient(){
    if(this.texture)return
    const size=this._textureSize,{x:x0,y:y0}=this.center,{x:x1,y:y1}=this.outerCenter,r0=this.innerRadius,r1=this.outerRadius,ox=x1-r1,oy=y1-r1,s=size/(2*r1),cx=(x0-ox)*s,cy=(y0-oy)*s,ex=(x1-ox)*s,ey=(y1-oy)*s,ir=r0*s,er=r1*s,dx=ex-cx,dy=ey-cy,dr=er-ir,A=dx*dx+dy*dy-dr*dr,cos=Math.cos(this.rotation),sin=Math.sin(this.rotation),stops=stopsOf(this.colorStops),data=new Uint8Array(size*size*4)
    for(let y=0;y<size;y++)for(let x=0;x<size;x++){
      const offset=(y*size+x)*4,background=stops[stops.length-1].color;for(let c=0;c<4;c++)data[offset+c]=Math.round(background[c]*255);
      const px=x+.5-cx,py=y+.5-cy,qx=cos*px+sin*py,qy=(-sin*px+cos*py)/this.scale,B=-2*(qx*dx+qy*dy+ir*dr),C=qx*qx+qy*qy-ir*ir
      if(qx+cx<0||qx+cx>=size||qy+cy<0||qy+cy>=size)continue
      let t=NaN
      if(Math.abs(A)<1e-9){if(Math.abs(B)>1e-9){const root=-C/B;if(ir+root*dr>=0)t=root}}
      else{const D=B*B-4*A*C;if(D>=0){const d=Math.sqrt(D),roots=[(-B-d)/(2*A),(-B+d)/(2*A)].filter(v=>ir+v*dr>=0);if(roots.length)t=Math.max(...roots)}}
      const foreground=new Uint8Array(4);write(foreground,0,t,stops);const alpha=foreground[3]/255,bgAlpha=background[3],outAlpha=alpha+bgAlpha*(1-alpha);for(let c=0;c<3;c++)data[offset+c]=outAlpha?Math.round((foreground[c]*alpha+background[c]*255*bgAlpha*(1-alpha))/outAlpha):0;data[offset+3]=Math.round(outAlpha*255)
    }
    this.texture=texture(data,size,size,this._wrapMode)
    this.transform=new V8.Matrix().scale(this.textureSpace==='local'?2*r1:1/s,this.textureSpace==='local'?2*r1:1/s).translate(ox,oy)
  }
}
