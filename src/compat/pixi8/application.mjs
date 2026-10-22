import {P,V8,unsupported} from './environment.mjs'
import {Container} from './container.mjs'
export const Ticker=V8.Ticker,UPDATE_PRIORITY=V8.UPDATE_PRIORITY
export function adaptRenderer(renderer){
  const render=renderer.render,resize=renderer.resize,generate=renderer.generateTexture
  Object.defineProperty(renderer,'canvas',{get(){return this.view}})
  renderer.render=function(options,...args){if(options?.container)return render.call(this,options.container,{...options,renderTexture:options.target??options.renderTexture});return render.call(this,options,...args)}
  renderer.resize=function(w,h,resolution){if(resolution!==undefined)this.resolution=resolution;return resize.call(this,w,h)}
  renderer.generateTexture=function(options){if(options?.target){const {target,frame,resolution,antialias}=options;if(antialias)unsupported('generateTexture.antialias');return generate.call(this,target,{region:frame,resolution,multisample:0})}return generate.apply(this,arguments)}
  const extract=renderer.extract
  if(extract){renderer.extract={pixels(options){const target=options?.target??options,frame=options?.frame,data=extract.pixels(target,frame),rect=frame||target?.frame||renderer.screen,resolution=target?.source?.resolution??renderer.resolution??1;return {pixels:data,width:Math.round(rect.width*resolution),height:Math.round(rect.height*resolution)}},canvas(options){return extract.canvas(options?.target??options,options?.frame)},image(options){return extract.image(options?.target??options,options?.format,options?.quality,options?.frame)},base64(options){return extract.base64(options?.target??options,options?.format,options?.quality,options?.frame)}}}
  return renderer
}
export class Application{
  static _plugins=V8.Application._plugins
  constructor(options){this.stage=new Container();this.stage.__nativeApplicationStage=true;const stats=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});stats.nativeApplicationStages=(stats.nativeApplicationStages||0)+1;this.renderer=null;this._initialized=false;if(options!==undefined)this.initialize(options)}
  initialize(options={}){if(this._initialized)throw new Error('Application is already initialized');if(options.preference==='webgpu')unsupported('WebGPU renderer');const o={...options,view:options.canvas??options.view,backgroundColor:options.background??options.backgroundColor};this.renderer=adaptRenderer(new P.Renderer(o));this._initialized=true;globalThis.__PIXI_APP__=this;for(const plugin of Application._plugins)plugin.init.call(this,options)}
  async init(options){this.initialize(options)}
  render(){if(!this.renderer)throw new Error('Application.init() must finish before render');this.renderer.render({container:this.stage})}
  get canvas(){return this.renderer?.canvas}get view(){return this.canvas}get screen(){return this.renderer?.screen}
  destroy(rendererOptions=false,options=false){if(!this._initialized){this.stage.destroy(options);this.stage=null;return}for(const plugin of Application._plugins.slice().reverse())plugin.destroy.call(this);this.stage.destroy(options);this.stage=null;this.renderer.destroy(typeof rendererOptions==='object'?!!rendererOptions.removeView:!!rendererOptions);this.renderer=null;this._initialized=false;if(globalThis.__PIXI_APP__===this)globalThis.__PIXI_APP__=null}
}
export async function autoDetectRenderer(options={}){if(options.preference==='webgpu')unsupported('WebGPU renderer');return adaptRenderer(new P.Renderer({...options,view:options.canvas,backgroundColor:options.background??options.backgroundColor}))}
