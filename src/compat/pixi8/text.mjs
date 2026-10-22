import {P,V8,unsupported} from './environment.mjs'
import {applyOptions,installContainerAPI,installViewHitAPI} from './container.mjs'
export const TextStyle=V8.TextStyle
const fields=['align','breakWords','fontFamily','fontSize','fontStyle','fontVariant','fontWeight','leading','letterSpacing','lineHeight','padding','textBaseline','trim','whiteSpace','wordWrap','wordWrapWidth']
function nativeStyle(style){const o={};for(const key of fields)o[key]=style[key];const fill=style._fill,stroke=style._stroke,shadow=style.dropShadow;if(fill?.fill||stroke?.fill)unsupported('TextStyle textured/gradient fill');o.fill=fill?.color??0;o.stroke=stroke?.color??0;o.strokeThickness=stroke?.width??0;o.lineJoin=stroke?.join??'miter';o.miterLimit=stroke?.miterLimit??10;o.dropShadow=!!shadow;if(shadow){o.dropShadowColor=shadow.color;o.dropShadowAlpha=shadow.alpha;o.dropShadowAngle=shadow.angle;o.dropShadowBlur=shadow.blur;o.dropShadowDistance=shadow.distance}return new P.TextStyle(o)}
const styleDescriptor=Object.getOwnPropertyDescriptor(P.Text.prototype,'style')
export class Text extends P.Text{
  constructor(options={},legacyStyle){if(typeof options!=='object')options={text:options,style:legacyStyle};super(options.text??'',{});this.style=options.style||{};if(options.resolution!==undefined)this.resolution=options.resolution;applyOptions(this,options)}
  get style(){return this.__readingNativeStyle||!this._v8Style?styleDescriptor.get.call(this):this._v8Style}
  set style(value){this._v8Style?.off('update',this.onStyleChange,this);this._v8Style=value instanceof V8.TextStyle?value:new V8.TextStyle(value instanceof P.TextStyle?Object.fromEntries(fields.map(k=>[k,value[k]])):value);this._v8Style.on('update',this.onStyleChange,this);this.onStyleChange()}
  onStyleChange(){this.__v8StyleKey=null;this._invalidateNativeText()}
  _syncNativeText(force){if(this._v8Style&&this.__v8StyleKey!==this._v8Style.styleKey){styleDescriptor.set.call(this,nativeStyle(this._v8Style));this.__v8StyleKey=this._v8Style.styleKey}this.__readingNativeStyle=true;try{return super._syncNativeText(force)}finally{this.__readingNativeStyle=false}}
  destroy(options){if(this.destroyed)return;this._v8Style?.off('update',this.onStyleChange,this);const style=this._v8Style;super.destroy(options);this._v8Style=null;if(options===true||options?.style)style?.destroy()}
}
installContainerAPI(Text.prototype);installViewHitAPI(Text.prototype)
export class BitmapText extends P.BitmapText{
  constructor(options={}){if(typeof options!=='object')options={text:options};const style=options.style instanceof TextStyle?options.style:new TextStyle({fill:0xffffff,...options.style}),family=Array.isArray(style.fontFamily)?style.fontFamily[0]:style.fontFamily;if(!P.BitmapFont.available[family])BitmapFont.install({name:family,style,chars:P.BitmapFont.ASCII});super(options.text??'',{fontName:family,fontSize:style.fontSize,align:style.align,letterSpacing:style.letterSpacing,tint:style._fill?.color??0xffffff});this._v8Style=style;style.on('update',this.onStyleChange,this);applyOptions(this,options)}
  get style(){return this._v8Style}set style(value){this._v8Style?.off('update',this.onStyleChange,this);this._v8Style=value instanceof TextStyle?value:new TextStyle(value);this._v8Style.on('update',this.onStyleChange,this);this.onStyleChange()}
  onStyleChange(){const s=this._v8Style;this.fontName=Array.isArray(s.fontFamily)?s.fontFamily[0]:s.fontFamily;this.fontSize=s.fontSize;this.align=s.align;this.letterSpacing=s.letterSpacing;this.tint=s._fill?.color??0xffffff}
  destroy(options){this._v8Style?.off('update',this.onStyleChange,this);super.destroy(options)}
}
installContainerAPI(BitmapText.prototype);installViewHitAPI(BitmapText.prototype)
export class BitmapFont{
  static install(options,...args){if(options?.name){const style=options.style instanceof TextStyle?options.style:new TextStyle(options.style);const font=P.BitmapFont.from(options.name,nativeStyle(style),Object.fromEntries(Object.entries({chars:options.chars,resolution:options.resolution,padding:options.padding,textureWidth:options.textureSize,textureHeight:options.textureSize}).filter(([,v])=>v!==undefined)));font.fontFamily=font.font;return font}const font=P.BitmapFont.install(options,...args);font.fontFamily=font.font;return font}
  static uninstall(name){return P.BitmapFont.uninstall(name)}static get available(){return P.BitmapFont.available}static get ASCII(){return P.BitmapFont.ASCII}static get ALPHA(){return P.BitmapFont.ALPHA}static get ALPHANUMERIC(){return P.BitmapFont.ALPHANUMERIC}static get NUMERIC(){return P.BitmapFont.NUMERIC}
}
