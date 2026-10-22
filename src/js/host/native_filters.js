// (color-overlay / drop-shadow / twist). The real packages are ESM and are intentionally
// skipped by the host loader; without these constructors the game falls back to missing
// globals or attaches unsupported Filter instances that become T_ESCAPE.
//
// Each constructor is shaped so native_scenegraph.__filterNativeKind() recognises it and
// the existing C++ post-process stack can consume it.
(function(){
'use strict';
var PIXI=globalThis.PIXI;
if(!PIXI||!PIXI.Filter)return;
var Filter=PIXI.Filter,filters=PIXI.filters||(PIXI.filters={});

function define(name, Ctor){
  if(filters[name])return;
  filters[name]=Ctor;
  if(typeof globalThis!=='undefined')globalThis[name]=Ctor;
}

// ---- ColorOverlayFilter ----------------------------------------------------
function ColorOverlayFilter(color,alpha){
  Filter.call(this,null,null,{color:new Float32Array([1,1,1]),alpha:1});
  this.padding=0;
  this.color=color===undefined?0xffffff:color;
  this.alpha=alpha===undefined?1:alpha;
}
ColorOverlayFilter.prototype=Object.create(Filter.prototype);
ColorOverlayFilter.prototype.constructor=ColorOverlayFilter;
Object.defineProperties(ColorOverlayFilter.prototype,{
  color:{
    get:function(){
      var c=this.uniforms.color||[1,1,1];
      return ((Math.round((c[0]||0)*255)<<16)|
              (Math.round((c[1]||0)*255)<<8)|
              Math.round((c[2]||0)*255))>>>0;
    },
    set:function(v){
      var n=Number(v)||0,u=this.uniforms.color||(this.uniforms.color=new Float32Array(3));
      u[0]=((n>>16)&255)/255;u[1]=((n>>8)&255)/255;u[2]=(n&255)/255;
    }
  },
  alpha:{
    get:function(){return this.uniforms.alpha===undefined?1:this.uniforms.alpha;},
    set:function(v){this.uniforms.alpha=Math.max(0,Math.min(1,Number(v)||0));}
  }
});
define('ColorOverlayFilter',ColorOverlayFilter);

// ---- DropShadowFilter (Kawase-backed shape expected by native) --------------
function DropShadowFilter(options){
  options=options||{};
  Filter.call(this,null,null,{});
  this.padding=Math.max(0,Number(options.padding)||20);
  this._alpha=options.alpha===undefined?0.5:Number(options.alpha)||0;
  this._blur=options.blur===undefined?2:Number(options.blur)||0;
  this._color=options.color===undefined?0x000000:Number(options.color)||0;
  this._distance=options.distance===undefined?5:Number(options.distance)||0;
  this._rotation=options.rotation===undefined?45:Number(options.rotation)||0;
  this._quality=Math.max(1,Math.min(8,Number(options.quality)||3));
  this._shadowOnly=!!options.shadowOnly;
  this._offset={x:0,y:0};
  this._blurFilter={
    kernels:[],_kernels:[],
    pixelSize:{x:1,y:1},_pixelSize:{x:1,y:1},
    clamp:true,_clamp:true
  };
  this._tintFilter={uniforms:{color:[0,0,0]}};
  this._syncTint();
  this._syncOffset();
  this._syncKernels();
}
DropShadowFilter.prototype=Object.create(Filter.prototype);
DropShadowFilter.prototype.constructor=DropShadowFilter;
DropShadowFilter.prototype._syncTint=function(){
  var n=this._color>>>0,c=this._tintFilter.uniforms.color;
  c[0]=((n>>16)&255)/255;c[1]=((n>>8)&255)/255;c[2]=(n&255)/255;
};
DropShadowFilter.prototype._syncOffset=function(){
  var rad=(this._rotation||0)*Math.PI/180,d=this._distance||0;
  this._offset.x=Math.cos(rad)*d;this._offset.y=Math.sin(rad)*d;
};
DropShadowFilter.prototype._syncKernels=function(){
  var q=Math.max(1,Math.min(8,Number(this._quality)||3)),b=Number(this._blur)||0,k=[];
  for(var i=0;i<q;i++)k.push(b);
  this._blurFilter.kernels=k;this._blurFilter._kernels=k;
};
Object.defineProperties(DropShadowFilter.prototype,{
  alpha:{get:function(){return this._alpha;},set:function(v){this._alpha=Number(v)||0;}},
  shadowOnly:{get:function(){return this._shadowOnly;},set:function(v){this._shadowOnly=!!v;}},
  color:{get:function(){return this._color;},set:function(v){this._color=Number(v)||0;this._syncTint();}},
  distance:{get:function(){return this._distance;},set:function(v){this._distance=Number(v)||0;this._syncOffset();}},
  rotation:{get:function(){return this._rotation;},set:function(v){this._rotation=Number(v)||0;this._syncOffset();}},
  blur:{get:function(){return this._blur;},set:function(v){this._blur=Number(v)||0;this._syncKernels();}},
  quality:{get:function(){return this._quality;},set:function(v){this._quality=Math.max(1,Math.min(8,Number(v)||3));this._syncKernels();}},
  offset:{
    get:function(){return this._offset;},
    set:function(v){
      if(!v)return;
      this._offset.x=Number(v.x)||0;this._offset.y=Number(v.y)||0;
      this._distance=Math.sqrt(this._offset.x*this._offset.x+this._offset.y*this._offset.y);
      this._rotation=Math.atan2(this._offset.y,this._offset.x)*180/Math.PI;
    }
  }
});
define('DropShadowFilter',DropShadowFilter);

// ---- TwistFilter -----------------------------------------------------------
function TwistFilter(radius,angle,padding){
  Filter.call(this,null,null,{
    radius:radius===undefined?200:Number(radius)||0,
    angle:angle===undefined?4:Number(angle)||0,
    offset:{x:0,y:0}
  });
  this.padding=Math.max(0,Number(padding)||20);
}
TwistFilter.prototype=Object.create(Filter.prototype);
TwistFilter.prototype.constructor=TwistFilter;
Object.defineProperties(TwistFilter.prototype,{
  radius:{
    get:function(){return this.uniforms.radius;},
    set:function(v){this.uniforms.radius=Math.max(0,Number(v)||0);}
  },
  angle:{
    get:function(){return this.uniforms.angle;},
    set:function(v){this.uniforms.angle=Number(v)||0;}
  },
  offset:{
    get:function(){return this.uniforms.offset;},
    set:function(v){
      if(!v)return;
      this.uniforms.offset.x=Number(v.x===undefined?v[0]:v.x)||0;
      this.uniforms.offset.y=Number(v.y===undefined?v[1]:v.y)||0;
    }
  }
});
define('TwistFilter',TwistFilter);

if(typeof __log==='function')__log('[native-filters] ColorOverlay/DropShadow/Twist shims installed');
})();
