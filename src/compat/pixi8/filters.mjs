import {P,unsupported} from './environment.mjs'
export class BlurFilter extends P.BlurFilter{constructor(o={}){super(o.strength??8,o.quality??4,o.resolution??1,o.kernelSize??5);if(o.strengthX!==undefined)this.strengthX=o.strengthX;if(o.strengthY!==undefined)this.strengthY=o.strengthY;if(o.padding!==undefined)this.padding=o.padding}get strength(){if(this.blurX!==this.blurY)throw new Error("BlurFilter strengthX and strengthY are different");return this.blurX}set strength(v){this.blur=v}get strengthX(){return this.blurX}set strengthX(v){this.blurX=v}get strengthY(){return this.blurY}set strengthY(v){this.blurY=v}}
export class ColorMatrixFilter extends P.ColorMatrixFilter{}
export class AlphaFilter extends P.AlphaFilter{constructor(o={}){super(typeof o==='number'?o:o.alpha??1)}}
export class NoiseFilter extends P.NoiseFilter{constructor(o={}){super(o.noise??.5,o.seed??Math.random())}}
export class DisplacementFilter extends P.DisplacementFilter{constructor(o,legacyScale){if(o instanceof P.Sprite)o={sprite:o,scale:legacyScale};const scale=o.scale??20;super(o.sprite,typeof scale==='number'?scale:20);if(typeof scale==='object')this.scale.copyFrom(scale)}}
export class Filter{constructor(){unsupported('Filter custom shader')}}
export class Shader{constructor(){unsupported('Shader custom GPU program')}}
export class GlProgram{constructor(){unsupported('GlProgram')}static from(){unsupported('GlProgram.from')}}
export class GpuProgram{constructor(){unsupported('GpuProgram')}static from(){unsupported('GpuProgram.from')}}
