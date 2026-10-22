// 文本布局契约 V2 —— 三宽度模型(docs/text-layout-contract-v2.md 第一部分)。
//
// 三个宽度彻底拆开:wrapWidth 只管断行;horizontalFrame 决定对齐范围;surface 由 Native 取
// max(natural, boxWidth)。horizontalFrame 只能显式枚举:
//   owner-box     行在 FGUI owner 实时宽内对齐(boxWidth=owner.$width,Controller/Relations/缩放
//                 只改实时值不改 policy) —— 670/540 弹窗居中场景;
//   paragraph-box TaggedText 历史语义:行在 wordWrapWidth 内对齐(Cashback Plus 784 等契约);
//   natural       行在自然内容宽内对齐(autoSize=both / 动态 PIXI.Text)。
//   tagged-content 动态 MultiStyleText:保留段落坐标,纹理/报告宽度仍为内容宽,锚点基准独立。
//
// 决议顺序:显式契约(label.__nativeTextLayoutContract 或 owner.__nativeTextLayoutContract)优先;
// 否则走 V1→V2 翻译层 —— 把 native_richtext 的历史推断逐条显式化并标 inferred+inferenceSource,
// 运行时始终只有一个布局引擎。无效显式契约:__RICHTEXT_CONTRACT_STRICT=1(测试)抛错;
// 否则记 invalidContract 并回退翻译层(回退到 V1 历史行为,不是回退左对齐;协议事实全程入 ring,
// __dumpTextLayoutContracts() 可直接打印 kind/wrap/horizontalFrame/resolvedFrameWidth/align)。
(function(){
'use strict';
if(globalThis.__nativeTextLayoutContractModule)return;

var HFRAMES={'owner-box':1,'paragraph-box':1,'natural':1,'tagged-content':1};
var HALIGNS={'left':1,'center':1,'right':1,'end':1};
var stats=globalThis.__textLayoutContractStats||(globalThis.__textLayoutContractStats={explicit:0,inferred:0,invalidContract:0,bySource:{},v2Calls:0,v1Calls:0});
var RING_CAP=64,ring=[],ringAt=0;
function log(m){if(typeof __log==='function')__log(m);else if(typeof console!=='undefined')console.log(m);}
function record(entry){ring[ringAt]=entry;ringAt=(ringAt+1)%RING_CAP;}

function invalid(reason,contract){
  stats.invalidContract++;
  if((globalThis.__RICHTEXT_CONTRACT_STRICT|0)===1)
    throw new Error('[text-layout] invalid contract: '+reason+' '+JSON.stringify(contract));
  if(stats.invalidContract<=8)log('[text-layout] invalid contract('+reason+'),回退翻译层: '+JSON.stringify(contract));
  return null;
}
function validateExplicit(c,style){
  if(!c||typeof c!=='object')return invalid('not-object',c);
  if(c.version!==2)return invalid('version='+String(c.version),c);
  if(!HFRAMES[c.horizontalFrame])return invalid('horizontalFrame='+String(c.horizontalFrame),c);
  if(c.horizontalAlign!==undefined&&!HALIGNS[c.horizontalAlign])return invalid('horizontalAlign='+String(c.horizontalAlign),c);
  if(c.lineVerticalOffset!==undefined&&!isFinite(Number(c.lineVerticalOffset)))return invalid('lineVerticalOffset='+String(c.lineVerticalOffset),c);
  if(c.verticalCenterHeight!==undefined&&(!isFinite(Number(c.verticalCenterHeight))||Number(c.verticalCenterHeight)<0))return invalid('verticalCenterHeight='+String(c.verticalCenterHeight),c);
  if(c.horizontalFrame==='paragraph-box'||c.horizontalFrame==='tagged-content'){
    var wrapOn=!!(style&&style.wordWrap===true&&Number(style.wordWrapWidth)>0);
    var declared=!!(c.wrap&&c.wrap.enabled&&Number(c.wrap.width)>0);
    if(!wrapOn&&!declared)return invalid(c.horizontalFrame+' requires wrap',c);
  }
  return c;
}
// 统一换行宽决议:显式契约的 wrap 优先(接受即生效——被 validate 接受却丢弃是静默错排),
// 否则回落 style 的 wordWrap/wordWrapWidth。产出值贯穿 payload/缓存键/ring。
function resolveWrapWidth(c,style){
  if(c&&c.wrap&&c.wrap.enabled&&Number(c.wrap.width)>0)return Number(c.wrap.width);
  if(c&&c.wrap&&c.wrap.enabled===false)return 0;
  return style&&style.wordWrap===true?Number(style.wordWrapWidth)||0:0;
}
// V1 推断显式化 —— 逐条对应 native_richtext.js 的历史猜测链(现已删除),来源可追溯:
//   legacy-tagged-width  __nativeAttributedWidthBox && wordWrap → 按 wordWrapWidth 对齐
//   legacy-owner-fixed   固定尺寸 FGUI 字段($widthAutoSize===false) → 按 owner 实时宽对齐
//   legacy-natural       其余(动态 MultiStyleText 等) → 自然宽
function inferRich(owner,style,isFguiField,ownerWidth){
  var wrapOn=!!(style&&style.wordWrap===true),wrapW=wrapOn?Number(style.wordWrapWidth)||0:0;
  var hasWrap=wrapOn&&isFinite(wrapW)&&wrapW>0;
  if(owner&&owner.__nativeAttributedWidthBox===true&&hasWrap)
    return {horizontalFrame:'paragraph-box',inferenceSource:'legacy-tagged-width'};
  // Dynamic MultiStyleText has paragraph coordinates but content-sized bounds and texture.
  // `natural` lost the paragraph origin; it is equivalent only for center align + anchor=.5.
  // Preserve origin and anchor extent without inflating the public width used by scale-to-fit.
  if(!isFguiField&&hasWrap)
    return {horizontalFrame:'tagged-content',inferenceSource:'dynamic-tagged-wrap'};
  if(isFguiField&&owner.$widthAutoSize===false&&isFinite(ownerWidth))
    return {horizontalFrame:'owner-box',inferenceSource:'legacy-owner-fixed'};
  return {horizontalFrame:'natural',inferenceSource:'legacy-natural'};
}
// 富文本入口决议:返回 {horizontalFrame, boxWidth, inferred, inferenceSource}。
function resolveRich(label,owner,style,isFguiField,ownerWidth){
  var explicit=(label&&label.__nativeTextLayoutContract)||(owner&&owner.__nativeTextLayoutContract)||null;
  var c=explicit?validateExplicit(explicit,style):null;
  var inferred=!c;
  if(!c)c=inferRich(owner,style,isFguiField,ownerWidth);
  var wrapW=resolveWrapWidth(inferred?null:c,style);
  var frame=c.horizontalFrame,boxWidth=0;
  if(frame==='owner-box')boxWidth=isFguiField&&isFinite(ownerWidth)?Math.max(0,ownerWidth):0;
  else if(frame==='paragraph-box')boxWidth=isFinite(wrapW)&&wrapW>0?wrapW:0;
  if(inferred){stats.inferred++;var src=c.inferenceSource||'unknown';stats.bySource[src]=(stats.bySource[src]||0)+1;}
  else stats.explicit++;
  var source=inferred?(c.inferenceSource||'unknown'):'explicit';
  // `horizontalAlign` is an explicit layout effect, not a style rewrite. H5 helpers such as
  // TextWrapperUtils.setPixiTextAlignCorrect move every token (including inline images) to the
  // owner centre even when customData.default replaced the XML align with the TaggedText default
  // `left`. A flattened Native label cannot express that child mutation, so carry the equivalent
  // line alignment through the same V2 contract.
  var resolvedAlign=c.horizontalAlign!==undefined?c.horizontalAlign:((style&&style.align)||'left');
  var lineVerticalOffset=inferred?0:(Number(c.lineVerticalOffset)||0);
  var verticalCenterHeight=inferred?0:Math.max(0,Number(c.verticalCenterHeight)||0);
  record({kind:'fgui-rich',wrap:wrapW,horizontalFrame:frame,resolvedFrameWidth:(frame==='paragraph-box'||frame==='tagged-content')?wrapW:boxWidth,align:resolvedAlign,lineVerticalOffset:lineVerticalOffset,verticalCenterHeight:verticalCenterHeight,source:source});
  return {horizontalFrame:frame,boxWidth:boxWidth,resolvedWrapWidth:wrapW,resolvedAlign:resolvedAlign,lineVerticalOffset:lineVerticalOffset,verticalCenterHeight:verticalCenterHeight,inferred:inferred,inferenceSource:source};
}

// 普通文本/Input 决议(fgui-text/input/pixi-text)。现状事实的显式化:V1 绑定无 boxWidth,
// 恒 natural——fgui 的 owner 对齐由外部节点定位镜像 H5(GTextField layoutAlign),文本 surface 内
// 只做多行行对齐(Pixi 语义)。paragraph-box 是 TaggedText 富文本专属,对 plain 非法。
// 显式 owner-box(box=owner 实时宽,行在 surface 内对齐)走 V2 绑定;声明方需自行保证外部定位
// 不再叠加(显式契约的责任边界)。
function inferPlain(owner,isFguiField){
  if(isFguiField){
    var meta=owner&&owner.__nativeFguiMeta;
    var kind=meta&&meta.kind==='inputtext'?'input':'fgui-text';
    return {horizontalFrame:'natural',inferenceSource:'legacy-plain-fgui',kind:kind};
  }
  return {horizontalFrame:'natural',inferenceSource:'legacy-plain-dynamic',kind:'pixi-text'};
}
function resolvePlain(label,owner,style,isFguiField,ownerWidth){
  var explicit=(label&&label.__nativeTextLayoutContract)||(owner&&owner.__nativeTextLayoutContract)||null;
  var c=null;
  if(explicit){
    c=validateExplicit(explicit,style);
    if(c&&(c.horizontalFrame==='paragraph-box'||c.horizontalFrame==='tagged-content'))c=invalid(c.horizontalFrame+' is richtext-only',explicit);
  }
  var inferred=!c;
  var kind=inferred?null:'explicit';
  if(!c){var inf=inferPlain(owner,isFguiField);c=inf;kind=inf.kind;}
  else kind=isFguiField?((owner&&owner.__nativeFguiMeta&&owner.__nativeFguiMeta.kind==='inputtext')?'input':'fgui-text'):'pixi-text';
  var frame=c.horizontalFrame,boxWidth=0;
  if(frame==='owner-box')boxWidth=isFguiField&&isFinite(ownerWidth)?Math.max(0,ownerWidth):0;
  if(inferred){stats.inferred++;var src=c.inferenceSource||'unknown';stats.bySource[src]=(stats.bySource[src]||0)+1;}
  else stats.explicit++;
  var source=inferred?(c.inferenceSource||'unknown'):'explicit';
  var wrapW=resolveWrapWidth(inferred?null:c,style);
  record({kind:kind,wrap:wrapW,horizontalFrame:frame,resolvedFrameWidth:frame==='owner-box'?boxWidth:0,align:(style&&style.align)||'left',source:source});
  return {horizontalFrame:frame,boxWidth:boxWidth,resolvedWrapWidth:wrapW,inferred:inferred,inferenceSource:source,kind:kind};
}

// autoSize=shrink keeps its width-derived scale, even for explicitly multi-line localisation.
// When that natural block is taller than the authored height, middle/bottom alignment must keep
// the signed gap: clamping it to zero makes the block grow only downward and lets a ScrollPane
// clip the final line. Non-overflow values are identical to FairyGUI's normal layoutAlign math.
function shrinkVerticalOffset(textHeight,boxHeight,scale,verticalAlign){
  textHeight=Math.max(0,Number(textHeight)||0);boxHeight=Math.max(0,Number(boxHeight)||0);
  scale=Number(scale);if(!isFinite(scale)||scale<0)scale=1;
  var gap=boxHeight-textHeight*scale;
  if(verticalAlign===1||verticalAlign==='middle')return gap*.5;
  if(verticalAlign===2||verticalAlign==='bottom')return gap;
  return 0;
}

// BitmapText(FairyGUI BMFont):frame 由 autoSize **结构性决定**,C++ writeBitmapText 按 FairyGUI
// 语义排版(断行=owner 宽−GUTTER(4),对齐基准=widthAuto?内容宽:owner 实时宽)——无运行时猜测分支,
// 并入协议=把该结构事实显式化进同一 ring/统计。显式契约在 bitmap 上不受支持(排版由 autoSize
// 驱动,要改 frame 应改 autoSize):记 invalidContract 并按结构决议继续,strict 下抛错暴露误用。
// 资源可疑形态审计(条件经确认):bitmap + align(center/right) + widthAuto + 无水平 pivot
// + 无 center/right 水平 relation → suspectResourceContract。shrink/both 会把对齐框收缩成
// 内容宽,center/right 失去作用域(coinBoosterOffer reward_font 案例:360 框收缩到内容宽,
// 左偏 46.5 逻辑像素,H5 与 Native 行为一致)。资源侧修法:固定框内居中用 autoSize="none";
// 保留自然宽用 pivot=0.5 或 center relation(fgui setSize 按 pivot.x*diffWidth 补偿 x)。
// relations 由 C++ 自治、JS 侧通常不可读:能读到非空 relations 时保守跳过(避免误报),
// 读不到按"无 relation"计——宁可靠 pivot 判据,漏报由后续 C++ 侧审计补。
var suspectSeen=Object.create(null);
function bitmapSuspectResource(owner,align){
  var pivotX=owner&&owner.$pivot?Number(owner.$pivot.x)||0:0;
  if(pivotX>0)return;
  var rel=owner&&owner.relations;
  if(rel&&rel.items&&rel.items.length)return;
  stats.suspectResourceContract=(stats.suspectResourceContract||0)+1;
  var key=String(owner&&owner.$font||'')+'|'+String(owner&&owner.name||'?');
  if(suspectSeen[key])return;suspectSeen[key]=1;
  log('[text-layout] suspectResourceContract kind=bitmap align='+align+' widthAuto=1 pivotX=0 relation=none'
    +' name='+String(owner&&owner.name||'?')+' font='+String(owner&&owner.$font||'?')
    +' —— 对齐框被 shrink/both 收缩,center/right 失效;资源侧修:autoSize=none / pivot=0.5 / center relation');
}
function resolveBitmap(owner,align,singleLine){
  if(owner&&owner.__nativeTextLayoutContract)invalid('bitmap-explicit-unsupported',owner.__nativeTextLayoutContract);
  // shrink($autoSize===3)≠widthAuto:fgui shrink 语义=保持 authored 框+超框缩放适配(C++
  // writeBitmapText 已同步修复,对齐框=owner 宽=owner-box);仅 both 才把宽交给内容(natural)。
  var shrinkMode=!!owner&&(owner.$autoSize|0)===3;
  var widthAuto=!!(owner&&owner.$widthAutoSize)&&!shrinkMode;
  var frame=widthAuto?'natural':'owner-box';
  if(widthAuto&&(align==='center'||align==='right'))bitmapSuspectResource(owner,align);
  var ownerWidth=owner?Number(owner.$width!==undefined?owner.$width:owner.width)||0:0;
  // C++ writeBitmapText 的断行条件是 !widthAuto && !singleLine:固定宽单行文本不断行,
  // ring 的 wrap 必须如实记 0,否则污染审计门槛。
  var wrapEnabled=frame==='owner-box'&&!singleLine;
  stats.inferred++;stats.bySource['legacy-bitmap-autosize']=(stats.bySource['legacy-bitmap-autosize']||0)+1;
  record({kind:'bitmap',wrap:wrapEnabled?Math.max(0,ownerWidth-4):0,horizontalFrame:frame,resolvedFrameWidth:frame==='owner-box'?ownerWidth:0,align:align||'left',source:'legacy-bitmap-autosize'});
  return {horizontalFrame:frame,boxWidth:frame==='owner-box'?ownerWidth:0,inferred:true,inferenceSource:'legacy-bitmap-autosize',kind:'bitmap'};
}

globalThis.__nativeTextLayoutContractModule={resolveRich:resolveRich,resolvePlain:resolvePlain,resolveBitmap:resolveBitmap,shrinkVerticalOffset:shrinkVerticalOffset,validateExplicit:validateExplicit,HFRAMES:HFRAMES,HALIGNS:HALIGNS};
globalThis.__dumpTextLayoutContracts=function(){
  var out=[];for(var i=0;i<RING_CAP;i++){var e=ring[(ringAt+i)%RING_CAP];if(e)out.push(e);}
  log('[text-layout] stats '+JSON.stringify(stats));
  for(var j=Math.max(0,out.length-24);j<out.length;j++)log('[text-layout] '+JSON.stringify(out[j]));
  return {stats:stats,recent:out};
};
})();
