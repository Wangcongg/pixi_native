import * as API from '../../../vendor/pixi.v8.20.0.mjs'
export const V8=API
export const P=globalThis.PIXI
if(P?.VERSION!=='7.4.3')throw new Error('Native Pixi 8 API requires Native Kit ABI 1')
// The official CPU algorithms use the native canvas/DOM adapter, never a browser renderer.
API.DOMAdapter.set({...API.DOMAdapter.get(),createCanvas:(w,h)=>P.settings.ADAPTER.createCanvas(w,h),getNavigator:()=>globalThis.navigator,getBaseUrl:()=>P.settings.ADAPTER.getBaseUrl(),fetch:(...args)=>globalThis.fetch(...args)})
export class UnsupportedNativeAPIError extends Error {
  constructor(api,detail=''){super('Native Pixi 8: unsupported '+api+(detail?' ('+detail+')':''));this.name='UnsupportedNativeAPIError';this.api=api}
}
export function unsupported(api,detail){throw new UnsupportedNativeAPIError(api,detail)}
export const nativeBlendModes={'normal':0,'add':1,'multiply':2,'screen':3,'overlay':4,'darken':5,'lighten':6,'color-dodge':7,'color-burn':8,'hard-light':9,'soft-light':10,'difference':11,'exclusion':12,'hue':13,'saturation':14,'color':15,'luminosity':16,'normal-npm':17,'add-npm':18,'screen-npm':19,'none':20,'erase':26,'subtract':28,'xor':29}
export function blendNumber(value){if(typeof value==='number')return value;if(!(value in nativeBlendModes)||nativeBlendModes[value]>=4&&nativeBlendModes[value]<=16)unsupported('blendMode',String(value));return nativeBlendModes[value]}
export function alphaNumber(v){if(typeof v==='number'||v===undefined)return v;const modes={'no-premultiply-alpha':0,'premultiplied-alpha':P.ALPHA_MODES.PMA,'premultiply-alpha-on-upload':P.ALPHA_MODES.UNPACK};if(!(v in modes))unsupported('TextureSource.alphaMode',v);return modes[v]}
