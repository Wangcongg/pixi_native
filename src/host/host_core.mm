// Shared Apple rendering host
#include "host_core.h"
macres::ResourceManager gNativeResources(&gDec);
bool shouldShowNativeDebugUI() {
#if defined(PIXINATIVE_PRODUCTION)
 return false;
#else
 return true;
#endif
}
// 把 fetch/script 的 URL 归一成相对路径：去协议+host、去 dev1/ 前缀、去 query/hash。
NSString* relOf(NSString* s) {
    NSRange r = [s rangeOfString:@"://"];
    if (r.location != NSNotFound) {
        NSUInteger slash = [s rangeOfString:@"/" options:0 range:NSMakeRange(r.location+3, s.length-r.location-3)].location;
        s = (slash==NSNotFound) ? @"" : [s substringFromIndex:slash+1];
    }
    if ([s hasPrefix:@"/"]) s = [s substringFromIndex:1];
    NSRange q = [s rangeOfString:@"?"]; if (q.location!=NSNotFound) s = [s substringToIndex:q.location];
    NSRange h = [s rangeOfString:@"#"]; if (h.location!=NSNotFound) s = [s substringToIndex:h.location];
    return s;
}

// Keep local/cache keys query-free but forward cache-busting queries to the CDN.
NSString* remoteQuerySuffix(NSString* s) {
    NSRange q = [s rangeOfString:@"?"];
    if (q.location == NSNotFound) return @"";
    NSUInteger end = s.length;
    NSRange h = [s rangeOfString:@"#" options:0
        range:NSMakeRange(q.location, s.length - q.location)];
    if (h.location != NSNotFound) end = h.location;
    if (end <= q.location || end - q.location > 2048) return @"";
    return [s substringWithRange:NSMakeRange(q.location, end - q.location)];
}

NativeTextSystem gNativeText;

// __nativeTextUpdate(node,text,font,fill,stroke,strokeWidth,shadow,shadowBlur,shadowX,shadowY,
//                    align,padding,resolution,anchorX,anchorY,wordWrapWidth,lineHeight,
//                    letterSpacing,leading,verticalAlign,gradientColors,gradientStops,gradientType,
//                    pixiAscent,pixiDescent,autoSize)
//                    -> metrics|null
JSValueRef cbNativeTextUpdate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<17)return JSValueMakeNull(ctx);double w=0,h=0,lh=0;int node=(int)JSValueToNumber(ctx,a[0],e);std::string text=c2d::jsStr(ctx,a[1]);
    bool ok=gNativeText.update(node,text,c2d::jsStr(ctx,a[2]),c2d::jsStr(ctx,a[3]),
        c2d::jsStr(ctx,a[4]),JSValueToNumber(ctx,a[5],e),c2d::jsStr(ctx,a[6]),JSValueToNumber(ctx,a[7],e),
        JSValueToNumber(ctx,a[8],e),JSValueToNumber(ctx,a[9],e),c2d::jsStr(ctx,a[10]),JSValueToNumber(ctx,a[11],e),
        JSValueToNumber(ctx,a[12],e),JSValueToNumber(ctx,a[13],e),JSValueToNumber(ctx,a[14],e),JSValueToNumber(ctx,a[15],e),
        JSValueToNumber(ctx,a[16],e),0,0,argc>19?c2d::jsStr(ctx,a[19]):std::string("top"),argc>17?JSValueToNumber(ctx,a[17],e):0,
        argc>18?JSValueToNumber(ctx,a[18],e):0,argc>20?c2d::jsStr(ctx,a[20]):std::string(),
        argc>21?c2d::jsStr(ctx,a[21]):std::string(),argc>22?(int)JSValueToNumber(ctx,a[22],e):0,
        argc>23?JSValueToNumber(ctx,a[23],e):0,argc>24?JSValueToNumber(ctx,a[24],e):0,w,h,lh);
    if(!ok)return JSValueMakeNull(ctx);
    // Managed FGUI nodes now remember that the JS business facade owns the latest surface. This
    // prevents a following autoSize/setSize bridge from repainting stale package XML over it.
    const std::string liveAlign=c2d::jsStr(ctx,a[10]);
    const std::string liveVerticalAlign=argc>19?c2d::jsStr(ctx,a[19]):std::string();
    const int liveAutoSize=argc>25?(int)JSValueToNumber(ctx,a[25],e):-1;
    gNativeFgui.claimTextFacadeVisual(node,text,(float)w,(float)h,(float)lh,gSG,
                                     liveAlign,liveVerticalAlign,liveAutoSize);
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);return out;
}

std::string richV2Str(JSContextRef ctx,JSObjectRef o,const char* k){
    JSStringRef s=JSStringCreateWithUTF8CString(k);JSValueRef v=JSObjectGetProperty(ctx,o,s,nullptr);JSStringRelease(s);
    if(!v||JSValueIsUndefined(ctx,v)||JSValueIsNull(ctx,v))return {};return c2d::jsStr(ctx,v);
}

// __nativeTextUpdateV2(node, payload) -> metrics|null —— 普通文本/Input 的三宽度协议结构化入口
// (docs/text-layout-contract-v2.md 第一部分)。payload 字段与 V1 位置参数一一对应,另加
// boxWidth/boxHeight 与 horizontalFrame:"owner-box"|"natural"。
// plain 无 paragraph-box(TaggedText 语义为富文本专属);natural 要求 box=0、owner-box 要求 box>0,
// 违反或未知 = 无效协议 → null(禁止静默;JS 契约层先行校验,此为二道防线)。
// update() 的行对齐基准=surface=max(natural,box),box 双态天然覆盖两种 frame,光栅零改动。
JSValueRef cbNativeTextUpdateV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[1]))return JSValueMakeNull(ctx);
    JSObjectRef p=JSValueToObject(ctx,a[1],e);if(!p)return JSValueMakeNull(ctx);
    const std::string hf=richV2Str(ctx,p,"horizontalFrame");
    const double bw=richV2Num(ctx,p,"boxWidth",0),bh=richV2Num(ctx,p,"boxHeight",0);
    const bool frameOk=(hf=="natural"&&!(bw>0))||(hf=="owner-box"&&bw>0);
    if(!frameOk){static int badPlainFrameLogLeft=8;if(badPlainFrameLogLeft-->0)std::fprintf(stderr,"[richtext-frame] plain V2 invalid horizontalFrame='%s' boxWidth=%.1f — reject\n",hf.c_str(),bw);return JSValueMakeNull(ctx);}
    const int node=(int)JSValueToNumber(ctx,a[0],e);
    const std::string text=richV2Str(ctx,p,"text");
    double w=0,h=0,lh=0;
    bool ok=gNativeText.update(node,text,richV2Str(ctx,p,"font"),richV2Str(ctx,p,"fill"),
        richV2Str(ctx,p,"stroke"),richV2Num(ctx,p,"strokeWidth",0),richV2Str(ctx,p,"shadow"),richV2Num(ctx,p,"shadowBlur",0),
        richV2Num(ctx,p,"shadowX",0),richV2Num(ctx,p,"shadowY",0),richV2Str(ctx,p,"align"),richV2Num(ctx,p,"padding",0),
        richV2Num(ctx,p,"resolution",1),richV2Num(ctx,p,"anchorX",0),richV2Num(ctx,p,"anchorY",0),
        richV2Num(ctx,p,"wrapWidth",0),richV2Num(ctx,p,"lineHeight",0),bw,bh,richV2Str(ctx,p,"vAlign"),
        richV2Num(ctx,p,"letterSpacing",0),richV2Num(ctx,p,"leading",0),
        richV2Str(ctx,p,"gradientColors"),richV2Str(ctx,p,"gradientStops"),(int)richV2Num(ctx,p,"gradientType",0),
        richV2Num(ctx,p,"ascent",0),richV2Num(ctx,p,"descent",0),w,h,lh);
    if(!ok)return JSValueMakeNull(ctx);
    // ≙ V1 cb:成功后认领 facade 视觉,阻止后续 autoSize/setSize 桥用包 XML 旧面重绘。
    gNativeFgui.claimTextFacadeVisual(node,text,(float)w,(float)h,(float)lh,gSG,
                                     richV2Str(ctx,p,"align"),richV2Str(ctx,p,"vAlign"),
                                     (int)richV2Num(ctx,p,"autoSize",-1));
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);return out;
}

// __nativeRichTextUpdate(node,markup,font,fill,stroke,strokeWidth,align,resolution,
//                        anchorX,anchorY,wordWrapWidth,lineHeight,boxWidth,boxHeight,
//                        verticalAlign) -> metrics|null
//
// TaggedText/MultiStyleText used to expand every styled run into a Pixi Text plus four
// intermediate Containers.  CoreText already owns the equivalent attributed-string renderer
// above, so expose it as one semantic scene node.  JavaScript only normalises the application's
// named tag styles into <span style="..."> markup; parsing, measurement, rasterisation and GL
// texture ownership all stay in this Mac-native surface.
JSValueRef cbNativeRichTextUpdate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<15)return JSValueMakeNull(ctx);double w=0,h=0,lh=0,contentW=0,contentX=0,contentH=0,contentY=0,textW=0,textX=0,textH=0,textY=0;
    bool ok=gNativeText.updateRich((int)JSValueToNumber(ctx,a[0],e),c2d::jsStr(ctx,a[1]),c2d::jsStr(ctx,a[2]),
        c2d::jsStr(ctx,a[3]),c2d::jsStr(ctx,a[4]),JSValueToNumber(ctx,a[5],e),c2d::jsStr(ctx,a[6]),
        JSValueToNumber(ctx,a[7],e),JSValueToNumber(ctx,a[8],e),JSValueToNumber(ctx,a[9],e),
        JSValueToNumber(ctx,a[10],e),JSValueToNumber(ctx,a[11],e),JSValueToNumber(ctx,a[12],e),
        JSValueToNumber(ctx,a[13],e),c2d::jsStr(ctx,a[14]),w,h,lh,0,false,&contentW,&contentX,&contentH,&contentY,&textW,&textX,&textH,&textY);
    if(!ok)return JSValueMakeNull(ctx);JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);setJsNumber(ctx,out,"contentWidth",contentW);setJsNumber(ctx,out,"contentX",contentX);setJsNumber(ctx,out,"contentHeight",contentH);setJsNumber(ctx,out,"contentY",contentY);setJsNumber(ctx,out,"textContentWidth",textW);setJsNumber(ctx,out,"textContentX",textX);setJsNumber(ctx,out,"textContentHeight",textH);setJsNumber(ctx,out,"textContentY",textY);return out;
}

// __nativeRichTextUpdateV2(node, payload) -> metrics|null —— 三宽度布局协议结构化入口
// (docs/text-layout-contract-v2.md 第一部分)。payload:{markup,font,fill,stroke,strokeWidth,align,
// resolution,anchorX,anchorY,wrapWidth,lineHeight,boxWidth,boxHeight,vAlign,
// horizontalFrame:"owner-box"|"paragraph-box"|"natural"|"tagged-content"}。
// horizontalFrame 缺失/未知 = 无效协议 → null(禁止静默回退成任何猜测;JS 契约层先行校验,此为二道防线)。
JSValueRef cbNativeRichTextUpdateV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[1]))return JSValueMakeNull(ctx);
    JSObjectRef p=JSValueToObject(ctx,a[1],e);if(!p)return JSValueMakeNull(ctx);
    const std::string hf=richV2Str(ctx,p,"horizontalFrame");
    const int hFrame=hf=="owner-box"?1:(hf=="paragraph-box"?2:(hf=="natural"?3:(hf=="tagged-content"?4:-1)));
    if(hFrame<0){static int badFrameLogLeft=8;if(badFrameLogLeft-->0)std::fprintf(stderr,"[richtext-frame] V2 invalid horizontalFrame='%s' — reject\n",hf.c_str());return JSValueMakeNull(ctx);}
    double w=0,h=0,lh=0,contentW=0,contentX=0,contentH=0,contentY=0,textW=0,textX=0,textH=0,textY=0;
    bool ok=gNativeText.updateRich((int)JSValueToNumber(ctx,a[0],e),richV2Str(ctx,p,"markup"),richV2Str(ctx,p,"font"),
        richV2Str(ctx,p,"fill"),richV2Str(ctx,p,"stroke"),richV2Num(ctx,p,"strokeWidth",0),richV2Str(ctx,p,"align"),
        richV2Num(ctx,p,"resolution",1),richV2Num(ctx,p,"anchorX",0),richV2Num(ctx,p,"anchorY",0),
        richV2Num(ctx,p,"wrapWidth",0),richV2Num(ctx,p,"lineHeight",0),richV2Num(ctx,p,"boxWidth",0),
        richV2Num(ctx,p,"boxHeight",0),richV2Str(ctx,p,"vAlign"),w,h,lh,hFrame,richV2Num(ctx,p,"breakWords",0)!=0,
        &contentW,&contentX,&contentH,&contentY,&textW,&textX,&textH,&textY,
        richV2Num(ctx,p,"lineVerticalOffset",0),richV2Num(ctx,p,"verticalCenterHeight",0));
    if(!ok)return JSValueMakeNull(ctx);JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);setJsNumber(ctx,out,"contentWidth",contentW);setJsNumber(ctx,out,"contentX",contentX);setJsNumber(ctx,out,"contentHeight",contentH);setJsNumber(ctx,out,"contentY",contentY);setJsNumber(ctx,out,"textContentWidth",textW);setJsNumber(ctx,out,"textContentX",textX);setJsNumber(ctx,out,"textContentHeight",textH);setJsNumber(ctx,out,"textContentY",textY);setJsNumber(ctx,out,"anchorWidth",gNativeText.richAnchorWidth((int)JSValueToNumber(ctx,a[0],e)));return out;
}

// __readFile(url) -> {ok,data}（本地优先，远程兜底缓存）
JSValueRef cbReadFile(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto setp = [&](const char* k, JSValueRef v){ JSStringRef s = JSStringCreateWithUTF8CString(k); JSObjectSetProperty(ctx, obj, s, v, kJSPropertyAttributeNone, nullptr); JSStringRelease(s); };
    if (argc < 1) { setp("ok", JSValueMakeBoolean(ctx, false)); return obj; }
    JSStringRef ps = JSValueToStringCopy(ctx, args[0], exc);
    size_t n = JSStringGetMaximumUTF8CStringSize(ps); std::vector<char> pb(n);
    JSStringGetUTF8CString(ps, pb.data(), n); JSStringRelease(ps);
    NSData* d = loadAsset(pb.data());
    if (!d) { setp("ok", JSValueMakeBoolean(ctx, false)); return obj; }
    size_t len = d.length; void* buf = malloc(len); memcpy(buf, d.bytes, len);
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, len, [](void* p, void*){ free(p); }, nullptr, exc);
    setp("ok", JSValueMakeBoolean(ctx, true)); setp("data", ab);
    return obj;
}

// __evalScript(rel)：读 gGameDir/<rel>（远程兜底）并在全局执行；顶层 var 挂全局（同浏览器 <script>）。
// JS 异常通过 *exc 传播回 game_host 的 try/catch。
JSValueRef cbEvalScript(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    JSStringRef ps = JSValueToStringCopy(ctx, args[0], exc);
    size_t n = JSStringGetMaximumUTF8CStringSize(ps); std::vector<char> pb(n);
    JSStringGetUTF8CString(ps, pb.data(), n); JSStringRelease(ps);
    NSData* d = loadAsset(pb.data());
    if (!d) { JSStringRef m = JSStringCreateWithUTF8CString((std::string("__evalScript 找不到: ")+pb.data()).c_str());
        if (exc) *exc = JSValueMakeString(ctx, m); JSStringRelease(m); return JSValueMakeUndefined(ctx); }
    NSString* src = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    if (!src) { fprintf(stderr,"[game] __evalScript 非 UTF8: %s\n", pb.data()); return JSValueMakeUndefined(ctx); }
    JSStringRef js = JSStringCreateWithUTF8CString([src UTF8String]);
    JSStringRef url = JSStringCreateWithUTF8CString(pb.data());
    JSEvaluateScript(gCtx, js, nullptr, url, 1, exc);   // *exc 设置后由调用方 JS 捕获
    JSStringRelease(js); JSStringRelease(url);
    return JSValueMakeUndefined(ctx);
}

// ---- timer 桥（JSC 无原生 setTimeout/setInterval；游戏重度依赖）----
int gTimerId = 0;

NSMutableDictionary<NSNumber*, NSTimer*>* gTimers = nil;

// id → protected JS callback. Every JSValueProtect in cbSetTimer must be balanced on ALL exit
// paths (fire, clearTimer, reload) or the callback closure — and through it the whole old JS
// object graph on reload — stays reachable forever.
NSMutableDictionary<NSNumber*, NSValue*>* gTimerFns = nil;

void unprotectTimerFn(NSNumber* key) {
    NSValue* v = gTimerFns ? gTimerFns[key] : nil;
    if (!v) return;
    [gTimerFns removeObjectForKey:key];
    if (gCtx) JSValueUnprotect(gCtx, (JSObjectRef)v.pointerValue);
}

JSValueRef cbSetTimer(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1 || !JSValueIsObject(ctx, args[0])) return JSValueMakeNumber(ctx, 0);
    JSObjectRef fn = JSValueToObject(ctx, args[0], exc);
    if (!fn || !JSObjectIsFunction(ctx, fn)) return JSValueMakeNumber(ctx, 0);
    double ms = argc>1 ? JSValueToNumber(ctx, args[1], exc) : 0;
    bool repeat = argc>2 && JSValueToBoolean(ctx, args[2]);
    JSValueProtect(gCtx, fn);
    int id = ++gTimerId; NSNumber* key = @(id);
    if (gTimerFns) gTimerFns[key] = [NSValue valueWithPointer:fn];
    NSTimer* t = [NSTimer scheduledTimerWithTimeInterval:(ms>0?ms/1000.0:0.0) repeats:repeat
        block:^(NSTimer* timer){
            JSValueRef e = nullptr;
            JSObjectCallAsFunction(gCtx, fn, nullptr, 0, nullptr, &e);
            if (e) { JSStringRef es = JSValueToStringCopy(gCtx, e, nullptr); size_t n=JSStringGetMaximumUTF8CStringSize(es);
                std::vector<char> b(n); JSStringGetUTF8CString(es,b.data(),n); fprintf(stderr,"[timer] 异常: %s\n",b.data()); JSStringRelease(es); }
            if (!repeat) { unprotectTimerFn(key); [gTimers removeObjectForKey:key]; }
        }];
    gTimers[key] = t;
    return JSValueMakeNumber(ctx, id);
}

JSValueRef cbClearTimer(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    NSNumber* key = @((int)JSValueToNumber(ctx, args[0], exc));
    NSTimer* t = gTimers[key]; if (t) { [t invalidate]; [gTimers removeObjectForKey:key]; }
    unprotectTimerFn(key);
    return JSValueMakeUndefined(ctx);
}

// ---- XMLHttpRequest 桥（NSURLSession；游戏/axios 网络走它，连内网游戏服 192.168.8.112:9090）----
// JS: __xhrSend(reqId, method, absUrl, headersJson, body)  body: string|ArrayBuffer|TypedArray|null
// 完成时（回主线程）调 globalThis.__xhrDone(reqId, status, statusText, respHeadersJson, respBody:ArrayBuffer|string)。
NSString* jsToNSString(JSContextRef ctx, JSValueRef v) {
    JSStringRef s = JSValueToStringCopy(ctx, v, nullptr); if (!s) return @"";
    size_t n = JSStringGetMaximumUTF8CStringSize(s); std::vector<char> b(n);
    JSStringGetUTF8CString(s, b.data(), n); JSStringRelease(s);
    return [NSString stringWithUTF8String:b.data()] ?: @"";
}

// JS 值（string/ArrayBuffer/TypedArray）→ NSData（请求体，protobuf 走二进制）
NSData* jsValueToData(JSContextRef ctx, JSValueRef v, JSValueRef* exc) {
    if (JSValueIsNull(ctx, v) || JSValueIsUndefined(ctx, v)) return nil;
    if (JSValueIsString(ctx, v)) {
        JSStringRef s = JSValueToStringCopy(ctx, v, exc); size_t n = JSStringGetMaximumUTF8CStringSize(s);
        std::vector<char> b(n); size_t w = JSStringGetUTF8CString(s, b.data(), n); JSStringRelease(s);
        return [NSData dataWithBytes:b.data() length:(w>0?w-1:0)];   // w 含结尾 NUL
    }
    if (JSValueIsObject(ctx, v)) {
        JSObjectRef o = JSValueToObject(ctx, v, exc);
        JSTypedArrayType ta = JSValueGetTypedArrayType(ctx, v, exc);
        if (ta == kJSTypedArrayTypeArrayBuffer) {
            void* p = JSObjectGetArrayBufferBytesPtr(ctx, o, exc); size_t len = JSObjectGetArrayBufferByteLength(ctx, o, exc);
            return p ? [NSData dataWithBytes:p length:len] : nil;
        } else if (ta != kJSTypedArrayTypeNone) {
            void* p = JSObjectGetTypedArrayBytesPtr(ctx, o, exc); size_t len = JSObjectGetTypedArrayByteLength(ctx, o, exc);
            return p ? [NSData dataWithBytes:p length:len] : nil;
        }
    }
    return nil;
}

@implementation NativeFguiXmlDelegate
- (instancetype)init { if((self=[super init])) _stack=[NSMutableArray array]; return self; }
- (void)parser:(NSXMLParser*)parser didStartElement:(NSString*)element namespaceURI:(NSString*)uri qualifiedName:(NSString*)qName attributes:(NSDictionary<NSString*,NSString*>*)attrs {
    NSMutableDictionary* node=[@{@"name":element?:@"",@"attrs":[attrs mutableCopy]?:[NSMutableDictionary dictionary],@"children":[NSMutableArray array],@"text":[NSMutableString string]} mutableCopy];
    if(self.stack.count)[self.stack.lastObject[@"children"] addObject:node];else self.root=node;[self.stack addObject:node];
}
- (void)parser:(NSXMLParser*)parser foundCharacters:(NSString*)string { if(self.stack.count&&string.length)[self.stack.lastObject[@"text"] appendString:string]; }
- (void)parser:(NSXMLParser*)parser didEndElement:(NSString*)element namespaceURI:(NSString*)uri qualifiedName:(NSString*)qName { if(self.stack.count)[self.stack removeLastObject]; }
@end

@implementation NativeFguiPreparsed
@end

NativeFguiPreparseCounters gFguiPreparseStats;

NSMutableDictionary<NSString*,NativeFguiPreparsed*>* gFguiPreparseCache=nil;

NSMutableArray<NSString*>* gFguiPreparseLru=nil;

NSMutableDictionary<NSNumber*,NSMutableArray<NSDictionary*>*>* gFguiXmlPrewarm=nil;

// Main-thread only. The asset workers never touch these maps directly.
std::unordered_map<std::string,std::vector<NativeFguiPreparseWaiter>> gFguiPreparseInflight;

NSDictionary* parseNativeFguiXml(NSString* source){
    if(!source.length)return nil;NSData* data=[source dataUsingEncoding:NSUTF8StringEncoding];
    NativeFguiXmlDelegate* delegate=[NativeFguiXmlDelegate new];NSXMLParser* parser=[[NSXMLParser alloc] initWithData:data];
    parser.delegate=delegate;parser.shouldProcessNamespaces=NO;parser.shouldResolveExternalEntities=NO;
    return [parser parse]&&delegate.root?delegate.root:nil;
}

// FairyGUI legacy package is raw-deflate followed by `name|UTF16-length|content`. NSString ranges
// deliberately match JavaScript substring lengths for non-ASCII localized component XML.
NativeFguiPreparsed* parseNativeFguiArchive(NSData* input,bool warmXml,const std::string& packageKey){
    if(!input.length)return nil;double start=nowUs();z_stream zs;memset(&zs,0,sizeof(zs));
    zs.next_in=(Bytef*)input.bytes;zs.avail_in=(uInt)input.length;if(inflateInit2(&zs,-15)!=Z_OK)return nil;
    NSMutableData* output=[NSMutableData dataWithLength:std::max((NSUInteger)65536,input.length*4)];int result=Z_OK;
    if(!output||!output.mutableBytes){inflateEnd(&zs);return nil;}
    while(result==Z_OK){if(zs.total_out>=output.length)[output increaseLengthBy:std::max((NSUInteger)65536,output.length)];zs.next_out=(Bytef*)output.mutableBytes+zs.total_out;zs.avail_out=(uInt)(output.length-zs.total_out);result=inflate(&zs,Z_NO_FLUSH);}
    size_t produced=zs.total_out;inflateEnd(&zs);if(result!=Z_STREAM_END)return nil;output.length=produced;
    NSString* source=[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding];if(!source)return nil;
    NativeFguiPreparsed* parsed=[NativeFguiPreparsed new];parsed.compressedBytes=input.length;
    parsed.compressedCrc=(uint32_t)crc32(0,(const Bytef*)input.bytes,(uInt)input.length);
    NSMutableArray<NSDictionary*>* xmlSources=warmXml?[NSMutableArray array]:nil;
    NSUInteger cursor=0,length=source.length;
    while(cursor<length){
        NSRange r1=[source rangeOfString:@"|" options:0 range:NSMakeRange(cursor,length-cursor)];if(r1.location==NSNotFound)break;
        NSString* name=[source substringWithRange:NSMakeRange(cursor,r1.location-cursor)];cursor=NSMaxRange(r1);
        NSRange r2=[source rangeOfString:@"|" options:0 range:NSMakeRange(cursor,length-cursor)];if(r2.location==NSNotFound)break;
        NSInteger size=[[source substringWithRange:NSMakeRange(cursor,r2.location-cursor)] integerValue];cursor=NSMaxRange(r2);
        if(size<0||cursor+(NSUInteger)size>length)break;NSString* value=[source substringWithRange:NSMakeRange(cursor,(NSUInteger)size)];cursor+=(NSUInteger)size;
        parsed->entries.emplace(name.UTF8String?:"",value.UTF8String?:"");
        if(warmXml&&[name.pathExtension caseInsensitiveCompare:@"xml"]==NSOrderedSame)[xmlSources addObject:@{@"source":value}];
    }
    parsed.entryCount=parsed->entries.size();parsed.inflateMs=(nowUs()-start)/1000.0;
    if(parsed->entries.empty())return nil;
    if(warmXml&&xmlSources.count){
        double xmlStart=nowUs();NSMutableDictionary<NSNumber*,NSMutableArray<NSDictionary*>*>* buckets=[NSMutableDictionary dictionary];
        for(NSDictionary* record in xmlSources){NSString* xml=record[@"source"];NSDictionary* tree=parseNativeFguiXml(xml);if(!tree)continue;NSNumber* hash=@(xml.hash);NSMutableArray* bucket=buckets[hash];if(!bucket){bucket=[NSMutableArray array];buckets[hash]=bucket;}[bucket addObject:@{@"source":xml,@"tree":tree}];}
        parsed.xmlTrees=buckets;parsed.xmlMs=(nowUs()-xmlStart)/1000.0;
    }
    // This is the expensive package/component XML walk. It is intentionally performed while
    // this function is on assetQueue; the live Runtime maps are not touched until publication.
    double blueprintStart=nowUs();parsed->blueprint=nfgui::Runtime::buildPackage(packageKey,parsed->entries);parsed.blueprintMs=(nowUs()-blueprintStart)/1000.0;
    return parsed;
}

void mergeNativeFguiXmlPrewarm(NativeFguiPreparsed* parsed){
    if(!parsed.xmlTrees.count)return;if(!gFguiXmlPrewarm)gFguiXmlPrewarm=[NSMutableDictionary dictionary];
    for(NSNumber* hash in parsed.xmlTrees){NSMutableArray* bucket=gFguiXmlPrewarm[hash];if(!bucket){bucket=[NSMutableArray array];gFguiXmlPrewarm[hash]=bucket;}NSArray* incoming=parsed.xmlTrees[hash];[bucket addObjectsFromArray:incoming];gFguiPreparseStats.xmlWarmed+=incoming.count;}
    parsed.xmlTrees=nil;
}

NSDictionary* takeNativeFguiXmlPrewarm(NSString* source){
    if(!source.length||!gFguiXmlPrewarm)return nil;NSNumber* hash=@(source.hash);NSMutableArray<NSDictionary*>* bucket=gFguiXmlPrewarm[hash];
    for(NSUInteger i=0;i<bucket.count;i++){NSDictionary* record=bucket[i];if([record[@"source"] isEqualToString:source]){NSDictionary* tree=record[@"tree"];[bucket removeObjectAtIndex:i];if(!bucket.count)[gFguiXmlPrewarm removeObjectForKey:hash];return tree;}}
    return nil;
}

JSValueRef jsNSString(JSContextRef ctx, NSString* s){if(!s)return JSValueMakeNull(ctx);JSStringRef k=JSStringCreateWithCFString((__bridge CFStringRef)s);JSValueRef v=JSValueMakeString(ctx,k);JSStringRelease(k);return v;}

JSObjectRef fguiXmlNodeToJs(JSContextRef ctx,NSDictionary* node,JSValueRef* exc,size_t* nodeCount){
    if(nodeCount)(*nodeCount)++;
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"__nativeXml",JSValueMakeBoolean(ctx,true));setJsProperty(ctx,out,"nodeName",jsNSString(ctx,node[@"name"]));setJsProperty(ctx,out,"type",JSValueMakeNumber(ctx,1));
    NSString* text=node[@"text"]?:@"";setJsProperty(ctx,out,"text",text.length?jsNSString(ctx,text):JSValueMakeNull(ctx));
    NSDictionary* attrs=node[@"attrs"]?:@{};JSObjectRef ao=JSObjectMake(ctx,nullptr,nullptr);for(NSString* key in attrs){JSStringRef k=JSStringCreateWithCFString((__bridge CFStringRef)key);JSObjectSetProperty(ctx,ao,k,jsNSString(ctx,attrs[key]),kJSPropertyAttributeNone,exc);JSStringRelease(k);}setJsProperty(ctx,out,"$attributes",ao);setJsProperty(ctx,out,"attributes",ao);
    NSArray* children=node[@"children"]?:@[];jscbridge::RootedValueVector values(ctx);values.reserve(children.count);for(NSDictionary* child in children)values.push_back(fguiXmlNodeToJs(ctx,child,exc,nodeCount));JSObjectRef ca=JSObjectMakeArray(ctx,values.size(),values.empty()?nullptr:values.data(),exc);setJsProperty(ctx,out,"$children",ca);setJsProperty(ctx,out,"children",ca);return out;
}

JSValueRef cbNativeFguiXmlParse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);NSString* source=jsToNSString(ctx,a[0]);if(!source.length)return JSValueMakeNull(ctx);
    NSDictionary* tree=takeNativeFguiXmlPrewarm(source);if(tree)gFguiPreparseStats.xmlHits++;else{gFguiPreparseStats.xmlFallbacks++;tree=parseNativeFguiXml(source);}
    if(!tree)return JSValueMakeNull(ctx);double bridgeStart=nowUs();size_t nodes=0;JSValueRef out=fguiXmlNodeToJs(ctx,tree,exc,&nodes);double bridgeMs=(nowUs()-bridgeStart)/1000.0;
    gFguiPreparseStats.xmlBridgeNodes+=nodes;gFguiPreparseStats.xmlBridgeMs+=bridgeMs;gFguiPreparseStats.maxXmlBridgeMs=std::max(gFguiPreparseStats.maxXmlBridgeMs,bridgeMs);return out;
}

JSValueRef nativeFguiEntriesToJs(JSContextRef ctx,NativeFguiPreparsed* parsed,NSString* resKey,bool cacheHit,JSValueRef* exc){
    if(!parsed)return JSValueMakeNull(ctx);std::string packageKey=resKey.UTF8String?:"";bool registered=gNativeFgui.package(packageKey)!=nullptr;
    if(!registered&&!parsed->blueprint&&parsed->entries.empty())return JSValueMakeNull(ctx);JSObjectRef map=JSObjectMake(ctx,nullptr,nullptr);
    if(!registered&&parsed->blueprint){double publishStart=nowUs();registered=gNativeFgui.installPackage(std::move(parsed->blueprint),&gSG);gFguiPreparseStats.blueprintPublishMs+=(nowUs()-publishStart)/1000.0;if(registered)gFguiPreparseStats.blueprintPublishes++;}
    if(!registered){double buildStart=nowUs();registered=gNativeFgui.registerPackage(packageKey,parsed->entries,&gSG);gFguiPreparseStats.syncBlueprintMs+=(nowUs()-buildStart)/1000.0;gFguiPreparseStats.syncBlueprintBuilds++;}
    // The C++ blueprint deliberately outlives JS UIPackage.removePackage. Re-adding a cached
    // package must therefore restore its exporter-id alias as well as returning the lazy token;
    // multiple machine archives legitimately reuse the same FairyGUI package id.
    if(registered)gNativeFgui.activatePackage(packageKey);
    // Once C++ accepted the immutable package, publish only a tiny lazy descriptor token. The
    // previous path copied every decompressed XML/FNT string into JSC before UIPackage.create(),
    // producing a measurable popup-open pause and retaining a second archive map.
    if(registered&&resKey.length)setJsProperty(ctx,map,"__nativePackageKey",jsNSString(ctx,resKey));
    else for(const auto& entry:parsed->entries){JSStringRef key=JSStringCreateWithUTF8CString(entry.first.c_str());JSStringRef value=JSStringCreateWithUTF8CString(entry.second.c_str());JSObjectSetProperty(ctx,map,key,JSValueMakeString(ctx,value),kJSPropertyAttributeNone,exc);JSStringRelease(value);JSStringRelease(key);}
    setJsProperty(ctx,map,"__nativeEntryCount",JSValueMakeNumber(ctx,(double)parsed.entryCount));
    setJsProperty(ctx,map,"__nativeBlueprint",JSValueMakeBoolean(ctx,registered));
    setJsProperty(ctx,map,"__nativePreparseHit",JSValueMakeBoolean(ctx,cacheHit));
    setJsProperty(ctx,map,"__nativeInflateMs",JSValueMakeNumber(ctx,parsed.inflateMs));
    // The installed Package now owns the descriptors. Drop the preparsed archive's duplicate
    // strings while retaining byte/CRC/count metadata for cache validation and later token hits.
    if(registered){std::unordered_map<std::string,std::string>().swap(parsed->entries);}return map;
}

// __nativeFguiEntry(packageKey,name) -> string|null.  The JS UIPackage facade memoises returned
// descriptors locally, so each actually-used entry crosses the JSC boundary at most once.
JSValueRef cbNativeFguiEntry(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*){
    if(argc<2)return JSValueMakeNull(ctx);NSString* packageKey=jsToNSString(ctx,a[0]),*name=jsToNSString(ctx,a[1]);gFguiPreparseStats.entryGets++;
    const std::string* value=gNativeFgui.descriptor(packageKey.UTF8String?:"",name.UTF8String?:"");if(!value){gFguiPreparseStats.entryMisses++;return JSValueMakeNull(ctx);}
    gFguiPreparseStats.entryBytes+=value->size();JSStringRef text=JSStringCreateWithUTF8CString(value->c_str());JSValueRef out=JSValueMakeString(ctx,text);JSStringRelease(text);return out;
}

JSValueRef cbNativeFguiInflate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);NSString* resKey=argc>1?jsToNSString(ctx,a[1]):@"";
    NativeFguiPreparsed* cached=resKey.length?gFguiPreparseCache[resKey]:nil;
    if(cached){gFguiPreparseStats.syncHits++;if(getenv("FGUI_PREPARSE_LOG"))fprintf(stderr,"[fgui-preparse/" HOST_PLATFORM_TAG "] HIT %s entries=%lu inflate=%.2fms\n",resKey.UTF8String,(unsigned long)cached.entryCount,cached.inflateMs);double bridgeStart=nowUs();JSValueRef out=nativeFguiEntriesToJs(ctx,cached,resKey,true,exc);gFguiPreparseStats.inflateBridgeMs+=(nowUs()-bridgeStart)/1000.0;return out;}
    gFguiPreparseStats.syncFallbacks++;NSData* input=jsValueToData(ctx,a[0],exc);if(!input.length)return JSValueMakeNull(ctx);
    std::string packageKey=resKey.UTF8String?:"";NativeFguiPreparsed* parsed=parseNativeFguiArchive(input,false,packageKey);if(!parsed){gFguiPreparseStats.failures++;return JSValueMakeNull(ctx);}
    gFguiPreparseStats.syncInflateMs+=parsed.inflateMs;if(parsed->blueprint){gFguiPreparseStats.syncBlueprintMs+=parsed.blueprintMs;gFguiPreparseStats.syncBlueprintBuilds++;}gFguiPreparseStats.lastInflateMs=parsed.inflateMs;
    if(getenv("FGUI_PREPARSE_LOG"))fprintf(stderr,"[fgui-preparse/" HOST_PLATFORM_TAG "] FALLBACK %s entries=%lu inflate=%.2fms\n",resKey.UTF8String?:"",(unsigned long)parsed.entryCount,parsed.inflateMs);
    double bridgeStart=nowUs();JSValueRef out=nativeFguiEntriesToJs(ctx,parsed,resKey,false,exc);gFguiPreparseStats.inflateBridgeMs+=(nowUs()-bridgeStart)/1000.0;return out;
}

JSValueRef cbNativeFguiSetStrings(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*){
    if(argc<1)return JSValueMakeBoolean(ctx,false);NSString* source=jsToNSString(ctx,a[0]);bool ok=gNativeFgui.setStringsSource(source.UTF8String?:"");if(ok)gNativeFgui.invalidateAllReservations(gSG);return JSValueMakeBoolean(ctx,ok);
}

// Compact immutable package metadata. UIPackage.create used to request package.xml, bridge a full
// XML object graph, split sprites.bytes and allocate every Rectangle on the JSC/render thread.
// The native package compiler has already done that work on assetQueue, so publish only the small
// PackageItem facade required by game/business code. Sprite frames stay lazy (callback below).
JSValueRef cbNativeFguiManifest(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);NSString* key=jsToNSString(ctx,a[0]);gFguiPreparseStats.manifestGets++;double started=nowUs();
    nfgui::Package* pkg=gNativeFgui.package(key.UTF8String?:"");if(!pkg){gFguiPreparseStats.manifestMisses++;return JSValueMakeNull(ctx);}
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"id",fguiJsString(ctx,pkg->id));setJsProperty(ctx,out,"name",fguiJsString(ctx,pkg->name));setJsProperty(ctx,out,"lazy",JSValueMakeBoolean(ctx,true));setJsNumber(ctx,out,"itemCount",pkg->resourceOrder.size());
    gFguiPreparseStats.manifestItems+=pkg->resourceOrder.size();double elapsed=(nowUs()-started)/1000.0;gFguiPreparseStats.manifestBridgeMs+=elapsed;gFguiPreparseStats.maxManifestBridgeMs=std::max(gFguiPreparseStats.maxManifestBridgeMs,elapsed);return out;
}

// __nativeFguiItem(package,idOrName,byName) -> compact PackageItem row|null.
JSValueRef cbNativeFguiItem(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);NSString* key=jsToNSString(ctx,a[0]),*query=jsToNSString(ctx,a[1]);bool byName=argc>2&&JSValueToBoolean(ctx,a[2]);gFguiPreparseStats.itemGets++;
    nfgui::Package* pkg=gNativeFgui.package(key.UTF8String?:"");if(!pkg){gFguiPreparseStats.itemMisses++;return JSValueMakeNull(ctx);}std::string id=query.UTF8String?:"";
    if(byName){auto named=pkg->resourceIdByName.find(id);if(named==pkg->resourceIdByName.end()){gFguiPreparseStats.itemMisses++;return JSValueMakeNull(ctx);}id=named->second;}
    auto found=pkg->resourcesById.find(id);if(found==pkg->resourcesById.end()){gFguiPreparseStats.itemMisses++;return JSValueMakeNull(ctx);}return nativeFguiResourceRow(ctx,found->second,exc);
}

// __nativeFguiItemsByType(package,type) -> ids in authored package.xml order.
JSValueRef cbNativeFguiItemsByType(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);NSString* key=jsToNSString(ctx,a[0]);int type=(int)JSValueToNumber(ctx,a[1],exc);gFguiPreparseStats.typeGets++;nfgui::Package* pkg=gNativeFgui.package(key.UTF8String?:"");if(!pkg)return JSValueMakeNull(ctx);
    jscbridge::RootedValueVector values(ctx);for(const std::string& id:pkg->resourceOrder){auto found=pkg->resourcesById.find(id);if(found!=pkg->resourcesById.end()&&found->second.packageItemType==type)values.push_back(fguiJsString(ctx,id));}return JSObjectMakeArray(ctx,values.size(),values.empty()?nullptr:values.data(),exc);
}

JSValueRef cbNativeFguiSprite(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);NSString* key=jsToNSString(ctx,a[0]),*item=jsToNSString(ctx,a[1]);gFguiPreparseStats.spriteGets++;double started=nowUs();
    const nfgui::Sprite* sprite=gNativeFgui.sprite(key.UTF8String?:"",item.UTF8String?:"");if(!sprite){gFguiPreparseStats.spriteMisses++;return JSValueMakeNull(ctx);}
    JSValueRef values[6]={fguiJsString(ctx,sprite->atlasId),JSValueMakeNumber(ctx,sprite->x),JSValueMakeNumber(ctx,sprite->y),JSValueMakeNumber(ctx,sprite->width),JSValueMakeNumber(ctx,sprite->height),JSValueMakeBoolean(ctx,sprite->rotated)};
    JSObjectRef out=JSObjectMakeArray(ctx,6,values,exc);gFguiPreparseStats.spriteBridgeMs+=(nowUs()-started)/1000.0;return out;
}

JSValueRef cbNativeFguiBindAtlas(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<6)return JSValueMakeBoolean(ctx,false);NSString* pkg=jsToNSString(ctx,a[0]),*atlas=jsToNSString(ctx,a[1]);int tex=(int)JSValueToNumber(ctx,a[2],exc);gNativeFgui.invalidateReservationsForPackage(pkg.UTF8String?:"",gSG);gNativeFgui.bindAtlas(pkg.UTF8String?:"",atlas.UTF8String?:"",tex,(float)JSValueToNumber(ctx,a[3],exc),(float)JSValueToNumber(ctx,a[4],exc),(float)JSValueToNumber(ctx,a[5],exc),&gSG);return JSValueMakeBoolean(ctx,tex>0);
}

// __nativeFguiReserve(package, component, targetCount) prepares detached inactive instances.
// They have no JS facade and do not enter the live Runtime table until __nativeFguiCreate claims
// one, so neither MovieClip nor auto-play transition time can advance behind the loading screen.
JSValueRef cbNativeFguiReserve(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeNumber(ctx,0);NSString* pkg=jsToNSString(ctx,a[0]),*component=jsToNSString(ctx,a[1]);
    double requested=JSValueToNumber(ctx,a[2],exc);size_t target=(size_t)std::max(0.0,std::min(64.0,requested));
    size_t available=gNativeFgui.reserve(pkg.UTF8String?:"",component.UTF8String?:"",target,gSG,(nowUs()-gStartUs)/1000.0,gNativeBatchSerial);
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"available",available);setJsProperty(ctx,out,"block",fguiJsString(ctx,gNativeFgui.reservationBlock()));return out;
}

JSValueRef cbNativeFguiClearReservations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*){
    if(argc<1)gNativeFgui.invalidateAllReservations(gSG);else{NSString* pkg=jsToNSString(ctx,a[0]);gNativeFgui.invalidateReservationsForPackage(pkg.UTF8String?:"",gSG);}return JSValueMakeUndefined(ctx);
}

JSValueRef cbNativeFguiCreate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);NSString* pkg=jsToNSString(ctx,a[0]),*component=jsToNSString(ctx,a[1]);std::string expectedSchema;if(argc>2&&!JSValueIsNull(ctx,a[2])&&!JSValueIsUndefined(ctx,a[2])){NSString* expected=jsToNSString(ctx,a[2]);expectedSchema=expected.UTF8String?:"";}double t0=nowUs(),ageNowMs=(t0-gStartUs)/1000.0;bool claimedReservation=false;int id=gNativeFgui.create(pkg.UTF8String?:"",component.UTF8String?:"",gSG,ageNowMs,&claimedReservation,gNativeBatchSerial);JSObjectRef out=fguiInstanceResultToJs(ctx,id,(nowUs()-t0)/1000.0,ageNowMs,claimedReservation,false,exc,expectedSchema.empty()?nullptr:&expectedSchema);if(out)return out;JSObjectRef failure=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,failure,"compatible",JSValueMakeBoolean(ctx,false));setJsProperty(ctx,failure,"fallbackReason",fguiJsString(ctx,gNativeFgui.createFailure().empty()?"create-unavailable":gNativeFgui.createFailure()));return failure;
}

// __nativeFguiCheckoutBatch is intentionally exact: it is not a general-purpose hidden factory.
// It only checks out already-reserved purchase rewards and returns an empty array unless the whole
// requested batch can be claimed. The enclosing SceneGraph command batch publishes only after
// promote, which makes this purchase-reward path a safe exception to ordinary reservation aging.


bool fguiJsStringArray(JSContextRef ctx,JSValueRef value,size_t exact,
                              std::vector<std::string>& out,JSValueRef* exc){
    if(!JSValueIsArray(ctx,value))return false;JSObjectRef array=JSValueToObject(ctx,value,exc);if(!array)return false;
    JSStringRef key=JSStringCreateWithUTF8CString("length");double rawLength=JSValueToNumber(ctx,JSObjectGetProperty(ctx,array,key,exc),exc);JSStringRelease(key);
    if(!std::isfinite(rawLength)||rawLength<0||std::floor(rawLength)!=rawLength||rawLength>4096)return false;
    size_t length=(size_t)rawLength;if(exact!=std::numeric_limits<size_t>::max()&&length!=exact)return false;
    out.clear();out.reserve(length);for(size_t i=0;i<length;++i){JSValueRef item=JSObjectGetPropertyAtIndex(ctx,array,(unsigned)i,exc);if(!JSValueIsString(ctx,item))return false;std::string text=c2d::jsStr(ctx,item);if(text.size()>65536)return false;out.push_back(std::move(text));}
    return !(exc&&*exc);
}

// One exact store transaction replaces 18 promote crossings, 18 reparents, controller setters and
// 18 attributed-text raster bridge calls. Every structural operand is validated before CoreText;
// structure is published only after all fresh text surfaces exist successfully.


// Exact live update for a num_con surface created by the direct checkout commit.  Ordinary live
// fixed style arrays as commit and receive four unboxed metrics (width/height/lineHeight/scale).


JSValueRef cbNativeFguiChild(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeNull(ctx);int inst=(int)JSValueToNumber(ctx,a[0],exc),parent=(int)JSValueToNumber(ctx,a[1],exc);nfgui::InstanceNode* n=nullptr;if(JSValueIsNumber(ctx,a[2]))n=gNativeFgui.child(inst,parent,"",(int)JSValueToNumber(ctx,a[2],exc));else{NSString* key=jsToNSString(ctx,a[2]);n=gNativeFgui.child(inst,parent,key.UTF8String?:"");}JSObjectRef out=fguiNodeToJs(ctx,n,exc);return out?out:JSValueMakeNull(ctx);
}

// Exact lightweight lookup for a metadata schema already validated by fguiInstanceSchemaToken.
// Returning only the numeric slot avoids rebuilding attrs/controller/transition objects for the
// same hot-store path eight more times. The pristine bit, expectedId and owner check make any
// runtime-mutated facade state fail closed; JavaScript then requests authoritative full metadata.


// Capability-checked lightweight text mutation used by closed FGUI refs. Both bitmap and CoreText
// paths live in Runtime so semantic attrs, ownership and metrics commit or roll back together.
JSValueRef cbNativeFguiSetTextMutation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                              const JSValueRef a[],JSValueRef* exc){
    if(argc!=4||!JSValueIsNumber(ctx,a[0])||!JSValueIsNumber(ctx,a[1])||
       !JSValueIsString(ctx,a[2])||!JSValueIsNumber(ctx,a[3]))return JSValueMakeBoolean(ctx,false);
    double rawInstance=JSValueToNumber(ctx,a[0],exc),rawNode=JSValueToNumber(ctx,a[1],exc),
           rawBold=JSValueToNumber(ctx,a[3],exc);
    if((exc&&*exc)||!std::isfinite(rawInstance)||!std::isfinite(rawNode)||
       !std::isfinite(rawBold)||rawInstance<=0||rawInstance>INT_MAX||rawNode<=0||
       rawNode>INT_MAX||std::floor(rawInstance)!=rawInstance||std::floor(rawNode)!=rawNode||
       (rawBold!=0&&rawBold!=1))return JSValueMakeBoolean(ctx,false);
    std::string text=c2d::jsStr(ctx,a[2]);
    return JSValueMakeBoolean(ctx,gNativeFgui.setTextMutation((int)rawInstance,(int)rawNode,
                                                              text,(int)rawBold,gSG));
}

// One synchronous bridge publishes last-write-wins text mutations from any compatible FGUI
// transaction. Runtime preflights capability/ownership for every row and rolls earlier rows back
// if a later surface fails; JavaScript replays singles on a false result.
JSValueRef cbNativeFguiCommitTextMutations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                                  const JSValueRef a[],JSValueRef* exc){
    const double started=nowUs();++gFguiTextMutationCalls;
    auto finish=[&](bool ok,size_t rows){
        gFguiTextMutationRows+=rows;if(!ok)++gFguiTextMutationFailures;
        gFguiTextMutationMs+=(nowUs()-started)/1000.0;return JSValueMakeBoolean(ctx,ok);
    };
    if(argc!=2||!JSValueIsObject(ctx,a[0])||!JSValueIsArray(ctx,a[1]))return finish(false,0);
    JSObjectRef words=JSValueToObject(ctx,a[0],exc);
    if(!words||(exc&&*exc)||JSValueGetTypedArrayType(ctx,a[0],exc)!=kJSTypedArrayTypeInt32Array)
        return finish(false,0);
    const size_t wordCount=JSObjectGetTypedArrayLength(ctx,words,exc);
    const int32_t* raw=(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,words,exc);
    if((exc&&*exc)||!raw||wordCount<3||wordCount>512*3||wordCount%3)return finish(false,0);
    const size_t rowCount=wordCount/3;
    std::vector<std::string> texts;
    if(!fguiJsStringArray(ctx,a[1],rowCount,texts,exc))return finish(false,rowCount);
    std::vector<nfgui::TextMutationWrite> writes;writes.reserve(rowCount);
    for(size_t i=0;i<rowCount;++i){
        const int instance=raw[i*3],node=raw[i*3+1],bold=raw[i*3+2];
        if(instance<=0||node<=0||(bold!=0&&bold!=1))return finish(false,rowCount);
        nfgui::TextMutationWrite write;write.instanceId=instance;write.nativeId=node;
        write.boldMode=bold;write.text=std::move(texts[i]);writes.push_back(std::move(write));
    }
    return finish(gNativeFgui.commitTextMutations(writes,gSG),rowCount);
}

JSValueRef cbNativeFguiSetController(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]);bool ok=gNativeFgui.setController((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",(int)JSValueToNumber(ctx,a[3],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

// Fixed row layout: Int32[kind,instance,target,value0,value1,value2], Float64[number0,number1],
// and one controller-name string per row.  Keeping all three arrays exact-sized lets C++ reject a
// torn/malformed transaction before Runtime sees any live mutation.
JSValueRef cbNativeFguiCommitMutations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                               const JSValueRef a[],JSValueRef* exc){
    const double started=nowUs();++gFguiOrderedMutationCalls;
    auto finish=[&](bool ok,size_t rows){
        gFguiOrderedMutationRows+=rows;if(!ok)++gFguiOrderedMutationFailures;
        gFguiOrderedMutationMs+=(nowUs()-started)/1000.0;
        // 成功返回整批的传播计数(除各行目标外被标记的其它节点数),失败返回 false;JS 侧 ok = res!==false
        return ok?JSValueMakeNumber(ctx,gNativeFgui.lastWriteFanout):JSValueMakeBoolean(ctx,false);
    };
    if(argc!=3||!JSValueIsObject(ctx,a[0])||!JSValueIsObject(ctx,a[1])||
       !JSValueIsArray(ctx,a[2]))return finish(false,0);
    if(JSValueGetTypedArrayType(ctx,a[0],exc)!=kJSTypedArrayTypeInt32Array||
       JSValueGetTypedArrayType(ctx,a[1],exc)!=kJSTypedArrayTypeFloat64Array)
        return finish(false,0);
    JSObjectRef wordObject=JSValueToObject(ctx,a[0],exc),numberObject=JSValueToObject(ctx,a[1],exc);
    const size_t wordCount=wordObject?JSObjectGetTypedArrayLength(ctx,wordObject,exc):0,
                 numberCount=numberObject?JSObjectGetTypedArrayLength(ctx,numberObject,exc):0;
    const int32_t* words=wordObject?(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,wordObject,exc):nullptr;
    const double* numbers=numberObject?(const double*)JSObjectGetTypedArrayBytesPtr(ctx,numberObject,exc):nullptr;
    if((exc&&*exc)||!words||!numbers||wordCount<6||wordCount>2048*6||wordCount%6)
        return finish(false,0);
    const size_t rowCount=wordCount/6;
    if(numberCount!=rowCount*2)return finish(false,rowCount);
    std::vector<std::string> names;
    if(!fguiJsStringArray(ctx,a[2],rowCount,names,exc))return finish(false,rowCount);
    std::vector<nfgui::MutationWrite> writes;writes.reserve(rowCount);
    for(size_t i=0;i<rowCount;++i){
        const int kind=words[i*6],instance=words[i*6+1],target=words[i*6+2];
        if(kind<(int)nfgui::MutationKind::Controller||kind>(int)nfgui::MutationKind::DisplayRotation||
           instance<=0||target<=0||!std::isfinite(numbers[i*2])||!std::isfinite(numbers[i*2+1]))
            return finish(false,rowCount);
        nfgui::MutationWrite write;write.kind=(nfgui::MutationKind)kind;write.instanceId=instance;
        write.targetId=target;write.value0=words[i*6+3];write.value1=words[i*6+4];
        write.value2=words[i*6+5];write.number0=(float)numbers[i*2];
        write.number1=(float)numbers[i*2+1];write.name=std::move(names[i]);
        writes.push_back(std::move(write));
    }
    return finish(gNativeFgui.commitMutations(writes,gSG),rowCount);
}

JSValueRef cbNativeFguiSetBitmapText(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<11)return JSValueMakeNull(ctx);NSString* text=jsToNSString(ctx,a[2]),*align=jsToNSString(ctx,a[7]);int vertical=(int)JSValueToNumber(ctx,a[8],exc);const char* verticalAlign=vertical==1?"middle":(vertical==2?"bottom":"top");nfgui::BitmapTextMetrics metrics;float localOffsetY=argc>12?(float)JSValueToNumber(ctx,a[12],exc):std::numeric_limits<float>::quiet_NaN();
    bool ok=gNativeFgui.setBitmapText((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),text.UTF8String?:"",(float)JSValueToNumber(ctx,a[3],exc),(uint32_t)JSValueToNumber(ctx,a[4],exc),(float)JSValueToNumber(ctx,a[5],exc),(float)JSValueToNumber(ctx,a[6],exc),align.UTF8String?:"left",verticalAlign,JSValueToBoolean(ctx,a[9]),(int)JSValueToNumber(ctx,a[10],exc),gSG,&metrics,argc>11&&JSValueToBoolean(ctx,a[11]),localOffsetY);
    if(!ok)return JSValueMakeNull(ctx);JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));setJsNumber(ctx,out,"layoutVersion",1);setJsNumber(ctx,out,"textWidth",metrics.textWidth);setJsNumber(ctx,out,"textHeight",metrics.textHeight);setJsNumber(ctx,out,"width",metrics.width);setJsNumber(ctx,out,"height",metrics.height);setJsNumber(ctx,out,"glyphCount",metrics.glyphCount);setJsProperty(ctx,out,"trusted",JSValueMakeBoolean(ctx,metrics.trusted));setJsProperty(ctx,out,"wrote",JSValueMakeBoolean(ctx,metrics.wrote));return out;
}

JSValueRef cbNativeFguiTransitionPlay(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<7)return JSValueMakeNumber(ctx,0);NSString* name=jsToNSString(ctx,a[2]);int token=gNativeFgui.playTransition((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",JSValueToNumber(ctx,a[3],exc),(int)JSValueToNumber(ctx,a[4],exc),JSValueToNumber(ctx,a[5],exc),JSValueToBoolean(ctx,a[6]),gSG);return JSValueMakeNumber(ctx,token);
}

JSValueRef cbNativeFguiTransitionStop(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<5)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]);bool ok=gNativeFgui.stopTransition((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",JSValueToBoolean(ctx,a[3]),JSValueToBoolean(ctx,a[4]),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiTransitionSetValue(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]),*label=jsToNSString(ctx,a[3]);std::vector<std::string> values;for(size_t i=4;i<argc;i++){NSString* value=jsToNSString(ctx,a[i]);values.push_back(value.UTF8String?:"");}return JSValueMakeBoolean(ctx,gNativeFgui.setTransitionValue((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",label.UTF8String?:"",values));
}

JSValueRef cbNativeFguiTransitionSetTarget(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<5)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]),*label=jsToNSString(ctx,a[3]),*target=jsToNSString(ctx,a[4]);return JSValueMakeBoolean(ctx,gNativeFgui.setTransitionTarget((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",label.UTF8String?:"",target.UTF8String?:"",gSG));}

JSValueRef cbNativeFguiTransitionSetDuration(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<5)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]),*label=jsToNSString(ctx,a[3]);return JSValueMakeBoolean(ctx,gNativeFgui.setTransitionDuration((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",label.UTF8String?:"",(float)JSValueToNumber(ctx,a[4],exc)));}

JSValueRef cbNativeFguiTransitionUpdateRelations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<6)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]),*target=jsToNSString(ctx,a[3]);return JSValueMakeBoolean(ctx,gNativeFgui.updateTransitionFromRelations((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",target.UTF8String?:"",(float)JSValueToNumber(ctx,a[4],exc),(float)JSValueToNumber(ctx,a[5],exc)));}

JSValueRef cbNativeFguiTransitionChangeRepeat(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<4)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]);return JSValueMakeBoolean(ctx,gNativeFgui.changeTransitionRepeat((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:"",(int)JSValueToNumber(ctx,a[3],exc)));}

JSValueRef cbNativeFguiTransitionPlaying(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<3)return JSValueMakeBoolean(ctx,false);NSString* name=jsToNSString(ctx,a[2]);return JSValueMakeBoolean(ctx,gNativeFgui.isTransitionPlaying((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.UTF8String?:""));}

dispatch_semaphore_t assetCpuSlots() {
    static dispatch_semaphore_t slots;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ slots = dispatch_semaphore_create(2); });
    return slots;
}

void notifyNativeFguiPreparse(const NativeFguiPreparseWaiter& waiter,bool ok,NativeFguiPreparsed* parsed){
    if(!gCtx||gCtx!=waiter.context)return;JSValueRef args[5]={JSValueMakeNumber(gCtx,waiter.token),JSValueMakeBoolean(gCtx,ok),JSValueMakeNumber(gCtx,parsed?parsed.inflateMs:0),JSValueMakeNumber(gCtx,parsed?parsed.xmlMs:0),JSValueMakeNumber(gCtx,parsed?(double)parsed.entryCount:0)};callJsGlobal("__nativeFguiPreparseDone",5,args);
}

void cacheNativeFguiPreparse(NSString* resKey,NativeFguiPreparsed* parsed){
    if(!resKey.length||!parsed)return;if(!gFguiPreparseCache){gFguiPreparseCache=[NSMutableDictionary dictionary];gFguiPreparseLru=[NSMutableArray array];}
    gFguiPreparseCache[resKey]=parsed;[gFguiPreparseLru removeObject:resKey];[gFguiPreparseLru addObject:resKey];mergeNativeFguiXmlPrewarm(parsed);
    while(gFguiPreparseLru.count>24){NSString* stale=gFguiPreparseLru.firstObject;[gFguiPreparseLru removeObjectAtIndex:0];[gFguiPreparseCache removeObjectForKey:stale];}
}

JSValueRef cbNativeFguiPreparseStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"requests",gFguiPreparseStats.requests);setJsNumber(ctx,out,"jobs",gFguiPreparseStats.jobs);setJsNumber(ctx,out,"deduped",gFguiPreparseStats.deduped);setJsNumber(ctx,out,"preparseCacheHits",gFguiPreparseStats.preparseCacheHits);setJsNumber(ctx,out,"syncHits",gFguiPreparseStats.syncHits);setJsNumber(ctx,out,"syncFallbacks",gFguiPreparseStats.syncFallbacks);setJsNumber(ctx,out,"failures",gFguiPreparseStats.failures);setJsNumber(ctx,out,"cacheEntries",gFguiPreparseCache.count);setJsNumber(ctx,out,"inflight",gFguiPreparseInflight.size());setJsNumber(ctx,out,"xmlWarmed",gFguiPreparseStats.xmlWarmed);setJsNumber(ctx,out,"xmlHits",gFguiPreparseStats.xmlHits);setJsNumber(ctx,out,"xmlFallbacks",gFguiPreparseStats.xmlFallbacks);setJsNumber(ctx,out,"xmlBridgeNodes",gFguiPreparseStats.xmlBridgeNodes);setJsNumber(ctx,out,"entryGets",gFguiPreparseStats.entryGets);setJsNumber(ctx,out,"entryMisses",gFguiPreparseStats.entryMisses);setJsNumber(ctx,out,"entryBytes",gFguiPreparseStats.entryBytes);setJsNumber(ctx,out,"blueprintPublishes",gFguiPreparseStats.blueprintPublishes);setJsNumber(ctx,out,"syncBlueprintBuilds",gFguiPreparseStats.syncBlueprintBuilds);setJsNumber(ctx,out,"manifestGets",gFguiPreparseStats.manifestGets);setJsNumber(ctx,out,"manifestMisses",gFguiPreparseStats.manifestMisses);setJsNumber(ctx,out,"manifestItems",gFguiPreparseStats.manifestItems);setJsNumber(ctx,out,"itemGets",gFguiPreparseStats.itemGets);setJsNumber(ctx,out,"itemMisses",gFguiPreparseStats.itemMisses);setJsNumber(ctx,out,"typeGets",gFguiPreparseStats.typeGets);setJsNumber(ctx,out,"spriteGets",gFguiPreparseStats.spriteGets);setJsNumber(ctx,out,"spriteMisses",gFguiPreparseStats.spriteMisses);setJsNumber(ctx,out,"backgroundInflateMs",gFguiPreparseStats.backgroundInflateMs);setJsNumber(ctx,out,"backgroundXmlMs",gFguiPreparseStats.backgroundXmlMs);setJsNumber(ctx,out,"backgroundBlueprintMs",gFguiPreparseStats.backgroundBlueprintMs);setJsNumber(ctx,out,"syncInflateMs",gFguiPreparseStats.syncInflateMs);setJsNumber(ctx,out,"syncBlueprintMs",gFguiPreparseStats.syncBlueprintMs);setJsNumber(ctx,out,"lastInflateMs",gFguiPreparseStats.lastInflateMs);setJsNumber(ctx,out,"blueprintPublishMs",gFguiPreparseStats.blueprintPublishMs);setJsNumber(ctx,out,"inflateBridgeMs",gFguiPreparseStats.inflateBridgeMs);setJsNumber(ctx,out,"xmlBridgeMs",gFguiPreparseStats.xmlBridgeMs);setJsNumber(ctx,out,"maxXmlBridgeMs",gFguiPreparseStats.maxXmlBridgeMs);setJsNumber(ctx,out,"manifestBridgeMs",gFguiPreparseStats.manifestBridgeMs);setJsNumber(ctx,out,"maxManifestBridgeMs",gFguiPreparseStats.maxManifestBridgeMs);setJsNumber(ctx,out,"spriteBridgeMs",gFguiPreparseStats.spriteBridgeMs);return out;
}

// Native ResourceManager completion. Pixels never enter JSC: cid names a host-owned
// external GL texture that the Pixi compatibility layer adopts as its GLTexture.
// 返回是否送达 JS:publishRef 路径的调用方据此决定退还所有权引用(送不到 = JS 永远不会
// adopt/cancel,引用必须原地 release,否则该纹理永久钉死在显存预算之外)。
bool finishNativeImageAsync(JSGlobalContextRef requestContext, int token,
                                   uint32_t cid, int width, int height) {
    if (!gCtx || gCtx != requestContext) return false;
    const bool needsPremultiply = cid && gNativeResources.needsPremultiply(cid);
    JSValueRef callbackArgs[6] = { JSValueMakeNumber(gCtx, token), JSValueMakeNumber(gCtx, width),
        JSValueMakeNumber(gCtx, height), JSValueMakeNull(gCtx), JSValueMakeNumber(gCtx, cid),
        JSValueMakeBoolean(gCtx, needsPremultiply) };
    return callJsGlobalChecked("__imgDone", 6, callbackArgs);   // 同时传递 KTX DFD alpha 语义
}

// publishRef=true 的完成回调:cid 携带一个已上账的所有权引用(软/adopt 性质),送达失败必须退还。
void deliverPublishedImage(JSGlobalContextRef requestContext, int token,
                                  uint32_t cid, int width, int height) {
    if (!finishNativeImageAsync(requestContext, token, cid, width, height) && cid)
        gNativeResources.releaseAdopt(cid);
}

// __decodeImageAsync(token, arrayBuffer|typedArray) -> accepted:boolean.
// Only the small compressed source bytes are copied while on the JSC thread;
// ImageIO decode and RGBA allocation run on the bounded native asset queue.
JSValueRef cbDecodeImageAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                     const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeBoolean(ctx, false);
    int token = (int)JSValueToNumber(ctx, args[0], exc);
    NSData* data = jsValueToData(ctx, args[1], exc);
    if (!data.length) return JSValueMakeBoolean(ctx, false);
    JSGlobalContextRef requestContext = gCtx;
    const NSUInteger byteLength = data.length;
    gNativeResources.requestImageData(data, [requestContext, token, byteLength](uint32_t cid, int width, int height) {
        if (!cid) fprintf(stderr, "[game] native blob image prepare 失败: %lu 字节\n", (unsigned long)byteLength);
        deliverPublishedImage(requestContext, token, cid, width, height);
    }, /*publishRef=*/true);
    return JSValueMakeBoolean(ctx, true);
}

// Native-only machine package download bridge. It deliberately sits below Pixi/FGUI: the lobby
// can prefetch a whole machine, while the ordinary loader later reads the same versioned URL from
// disk and follows its unchanged H5-era scene flow.


// Settles a commit whose done-callback never arrived, atomically under the cache lock:
// false = already active (JS treats the commit as succeeded and reloads), true = cancelled and
// the in-flight background commit can never activate the release any more.


// Consumes the marker only if it still holds exactly the presented text, so a rollback that
// happened while the report was in flight keeps its own event for the next attempt.


JSValueRef cbNativeResourceRetain(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                         const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.retain(cid));
}

JSValueRef cbNativeResourceAlive(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                        const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.alive(cid));
}

JSValueRef cbNativeResourceRelease(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                          const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.release(cid));
}

// adopt(软)引用的两个收尾桥:transferred=首绑时性质转硬(refs 不变),releaseAdopt=退还软引用。
JSValueRef cbNativeResourceReleaseAdopt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                               const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.releaseAdopt(cid));
}

JSValueRef cbNativeResourceTransferred(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                              const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.transferAdopt(cid));
}

JSValueRef cbNativeResourceStats(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                                        const JSValueRef[], JSValueRef*) {
    macres::Stats stats = gNativeResources.stats(); JSObjectRef out = JSObjectMake(ctx, nullptr, nullptr);
    setJsNumber(ctx,out,"byteHits",stats.byteHits);setJsNumber(ctx,out,"byteMisses",stats.byteMisses);setJsNumber(ctx,out,"byteInflightJoins",stats.byteInflightJoins);
    setJsNumber(ctx,out,"imageHits",stats.imageHits);setJsNumber(ctx,out,"imageMisses",stats.imageMisses);setJsNumber(ctx,out,"imageInflightJoins",stats.imageInflightJoins);
    setJsNumber(ctx,out,"decodedImages",stats.decodedImages);setJsNumber(ctx,out,"decodeFailures",stats.decodeFailures);setJsNumber(ctx,out,"uploadedImages",stats.uploadedImages);
    setJsNumber(ctx,out,"uploadedBytes",stats.uploadedBytes);setJsNumber(ctx,out,"byteCacheBytes",stats.byteCacheBytes);setJsNumber(ctx,out,"gpuCacheBytes",stats.gpuCacheBytes);
    setJsNumber(ctx,out,"pendingUploadBytes",stats.pendingUploadBytes);setJsNumber(ctx,out,"byteEntries",stats.byteEntries);setJsNumber(ctx,out,"imageEntries",stats.imageEntries);setJsNumber(ctx,out,"pendingUploads",stats.pendingUploads);
    setJsNumber(ctx,out,"evictedImages",stats.evictedImages);setJsNumber(ctx,out,"evictedBytes",stats.evictedBytes);setJsNumber(ctx,out,"retainMisses",stats.retainMisses);setJsNumber(ctx,out,"releaseMisses",stats.releaseMisses);
    setJsNumber(ctx,out,"renderFrame",stats.renderFrame);setJsNumber(ctx,out,"protectedImages",stats.protectedImages);setJsNumber(ctx,out,"publishedRefs",stats.publishedRefs);setJsNumber(ctx,out,"adoptRefs",stats.adoptRefs);return out;
}

@implementation NativeSpinePrepared
@end

NSMutableDictionary<NSString*,NativeSpinePrepared*>* gSpinePreparedCache=nil;

NSMutableDictionary<NSNumber*,NativeSpinePrepared*>* gSpinePreparedByHandle=nil;

NSMutableArray<NSString*>* gSpinePreparedLru=nil;

std::unordered_map<std::string,std::vector<NativeSpinePrepareWaiter>> gSpinePrepareInflight;

int gNextSpinePreparedHandle=1;

NSString* nativeSpineCacheKey(NSString* url,NSString* key){
    return [NSString stringWithFormat:@"%@\x1f%@",url?:@"",key?:@""];
}

uint32_t nativeSpineInputCrc(NSData* data){
    if(!data.length)return 0;uLong crc=crc32(0L,Z_NULL,0);const Bytef* bytes=(const Bytef*)data.bytes;NSUInteger offset=0;
    while(offset<data.length){uInt chunk=(uInt)std::min<NSUInteger>(data.length-offset,UINT_MAX);crc=crc32(crc,bytes+offset,chunk);offset+=chunk;}return (uint32_t)crc;
}

// .skel 头部版本号。spine 二进制格式开头是 int32 lowHash + int32 highHash + 变长字符串 version
// (SkeletonBinary.cpp:112-122;readString 先读 varint 长度 L,再取 L-1 字节,L==0 表示空)。
// 包内不再留 .json 时,版本号只能从这里取——preparse 要靠它决定是否点名回退。
NSString* nativeSpineSkelVersion(NSData* data){
    if(data.length<10)return @"";
    const uint8_t* p=(const uint8_t*)data.bytes;NSUInteger i=8;               // 跳过 8 字节 hash
    int len=0,shift=0;                                                        // 7 位一组的 varint
    while(i<data.length&&shift<=28){uint8_t b=p[i++];len|=(b&0x7F)<<shift;if(!(b&0x80))break;shift+=7;}
    if(len<=1||i+(NSUInteger)(len-1)>data.length)return @"";
    return [[NSString alloc] initWithBytes:p+i length:(NSUInteger)(len-1) encoding:NSUTF8StringEncoding]?:@"";
}

NativeSpinePrepared* prepareNativeSpineArchive(NSData* input,NSString* url,NSString* key,uint32_t inputCrc){
    if(!input.length)return nil;double start=nowUs();NSDictionary<NSString*,NSData*>* entries=zipParseAll(input);
    double zipMs=(nowUs()-start)/1000.0;if(!entries.count)return nil;
    // 见下方 nativeSpineSkelVersion:包内没有 .json 时,版本号只能从 .skel 头部取。
    NSArray<NSString*>* names=[entries.allKeys sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSMutableArray<NSString*>* jsonNames=[NSMutableArray array],*atlasNames=[NSMutableArray array],*imageNames=[NSMutableArray array];NSMutableArray<NSString*>* skelNames=[NSMutableArray array];
    for(NSString* name in names){if([name containsString:@"/"])continue;NSString* ext=name.pathExtension.lowercaseString;
        if([ext isEqualToString:@"json"])[jsonNames addObject:name];else if([ext isEqualToString:@"atlas"])[atlasNames addObject:name];else if([ext isEqualToString:@"png"])[imageNames addObject:name];else if([ext isEqualToString:@"skel"])[skelNames addObject:name];}
    // 骨架来源二选一即可:.skel(优先,二进制)或 .json。两者都没有才算不成包。
    if((!jsonNames.count&&!skelNames.count)||!atlasNames.count||!imageNames.count)return nil;
    NSString* skeletonName=nil,*atlasName=atlasNames.firstObject;
    for(NSString* name in atlasNames)if([[name stringByDeletingPathExtension] caseInsensitiveCompare:key]==NSOrderedSame){atlasName=name;break;}
    double atlasStart=nowUs();NSString* atlasText=[[NSString alloc] initWithData:entries[atlasName] encoding:NSUTF8StringEncoding];double atlasMs=(nowUs()-atlasStart)/1000.0;
    if(!atlasText.length)return nil;NSMutableArray<NSString*>* orderedJson=[jsonNames mutableCopy];
    for(NSString* name in jsonNames)if([[name stringByDeletingPathExtension] caseInsensitiveCompare:key]==NSOrderedSame){[orderedJson removeObject:name];[orderedJson insertObject:name atIndex:0];break;}
    NSString* skelName=skelNames.firstObject;
    for(NSString* name in skelNames)if([[name stringByDeletingPathExtension] caseInsensitiveCompare:key]==NSOrderedSame){skelName=name;break;}
    // 有 .skel 就完全不碰 .json。spinex(C++)直接吃包内 .skel 字节,JS 侧不再构建 SkeletonData,
    // 于是 NSJSONSerialization 的产物没有任何消费方——它过去只喂 mode 2 的整棵 NSDictionary 过桥。
    // 只有纯 json 的老包才退回解析,拿 skeletonFilename 与版本号。
    double jsonStart=nowUs();id skeleton=nil;
    if(!skelName.length){
        for(NSString* name in orderedJson){NSError* jsonError=nil;id candidate=[NSJSONSerialization JSONObjectWithData:entries[name] options:0 error:&jsonError];
            if(!jsonError&&[candidate isKindOfClass:[NSDictionary class]]&&[candidate[@"skeleton"] isKindOfClass:[NSDictionary class]]){skeleton=candidate;skeletonName=name;break;}}
    }
    double jsonMs=(nowUs()-jsonStart)/1000.0;
    if(!skeleton&&!skelName.length)return nil;                       // 两种骨架都没有:不成包
    NSString* version=@"";
    if(skeleton){id header=((NSDictionary*)skeleton)[@"skeleton"];if([header isKindOfClass:[NSDictionary class]]&&[header[@"spine"] isKindOfClass:[NSString class]])version=header[@"spine"];}
    else version=nativeSpineSkelVersion(entries[skelName]);          // 有 .skel 时版本号取自其头部
    NativeSpinePrepared* parsed=[NativeSpinePrepared new];parsed.url=url?:@"";parsed.key=key?:@"";parsed.cacheKey=nativeSpineCacheKey(url,key);
    parsed.skelFilename=skelName;
    parsed.skeletonFilename=skeletonName;parsed.atlasFilename=atlasName;parsed.atlasText=atlasText;parsed.version=version;parsed.imageNames=imageNames;
    parsed.entries=entries;parsed.compressedBytes=input.length;parsed.inputCrc=inputCrc;parsed.manifestEntries=jsonNames.count+atlasNames.count+imageNames.count;
    parsed.zipMs=zipMs;parsed.atlasMs=atlasMs;parsed.jsonMs=jsonMs;return parsed;
}

void touchNativeSpinePrepared(NativeSpinePrepared* parsed){
    if(!parsed.cacheKey.length)return;[gSpinePreparedLru removeObject:parsed.cacheKey];[gSpinePreparedLru addObject:parsed.cacheKey];
}

// lease 模型:prepared 的在途保活是显式引用(activeUsers,JS 加载链 pin/unpin),不再依赖
// "cap 足够大轮不到挤"的容量巧合;留存降为小热 LRU(默认 8,重建加速)。逐出从最老起跳过
// 在途者,全在途时容忍临时超容(那是必要工作集,挤掉只会断链重来更贵);unpin 归零时也补跑
// 一次,停止加载后过期 holder 不必等下一次插入才释放。上限 SPINE_PREPARED_LRU_CAP 可调
// (压测/调参;cap=1 复现"逐出→needs-reload 回源"链路)。
void spinePreparedEvictOverflow(void){
    static int lruCap=({const char* e=getenv("SPINE_PREPARED_LRU_CAP");int v=e?atoi(e):0;v>0?v:8;});
    if(!gSpinePreparedLru)return;
    NSUInteger scan=0;
    while(gSpinePreparedLru.count>(NSUInteger)lruCap&&scan<gSpinePreparedLru.count){
        NSString* stale=gSpinePreparedLru[scan];
        NativeSpinePrepared* victim=gSpinePreparedCache[stale];
        if(victim&&victim.activeUsers>0){scan++;continue;}
        [gSpinePreparedLru removeObjectAtIndex:scan];
        if(victim)[gSpinePreparedByHandle removeObjectForKey:@(victim.handle)];
        [gSpinePreparedCache removeObjectForKey:stale];
        gSpinePrepareStats.evictions++;
    }
}

void cacheNativeSpinePrepared(NativeSpinePrepared* parsed){
    if(!parsed.cacheKey.length)return;if(!gSpinePreparedCache){gSpinePreparedCache=[NSMutableDictionary dictionary];gSpinePreparedByHandle=[NSMutableDictionary dictionary];gSpinePreparedLru=[NSMutableArray array];}
    // 同 cacheKey 换代:在途(activeUsers>0)的旧 holder 保留 byHandle 条目退休服役——旧加载链
    // 自己已非 cache 现任即自行从 byHandle 收尾。无在途者照旧立即摘除。
    NativeSpinePrepared* prior=gSpinePreparedCache[parsed.cacheKey];
    if(prior&&prior.activeUsers<=0)[gSpinePreparedByHandle removeObjectForKey:@(prior.handle)];
    parsed.handle=gNextSpinePreparedHandle++;if(gNextSpinePreparedHandle<=0)gNextSpinePreparedHandle=1;gSpinePreparedCache[parsed.cacheKey]=parsed;gSpinePreparedByHandle[@(parsed.handle)]=parsed;touchNativeSpinePrepared(parsed);
    spinePreparedEvictOverflow();
}

// 在途 lease 的引用编排:引用在 notify 排队**之前**的同步块内获取——cache-hit 分支入队前
// cached.activeUsers++,完成点在**插入前**按 waiter 数预置(新 holder 出生即带引用,插入
// 逐出免疫;旧实现 pin 在 JS 回调里,cap=1 下新 holder 插入时无引用会把自己逐掉)。notify
// 只负责交付 JS(preparseDone 记账移交加载链)或拒收退还;释放统一走 release 内核。
void spineReleasePreparedRef(NativeSpinePrepared* p){
    if(!p||p.activeUsers<=0)return;
    p.activeUsers=p.activeUsers-1;
    if(p.activeUsers==0){
        if(gSpinePreparedCache[p.cacheKey]!=p)[gSpinePreparedByHandle removeObjectForKey:@(p.handle)];   // 退休 holder(同 key 换代被顶替):收尾释放
        else spinePreparedEvictOverflow();
    }
}

// __nativeSpinePin 保留为兼容/诊断口(当前编排下 JS 不再主动 pin)。
JSValueRef cbNativeSpinePin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeBoolean(ctx,false);
    NativeSpinePrepared* p=gSpinePreparedByHandle[@((int)JSValueToNumber(ctx,args[0],exc))];
    if(!p)return JSValueMakeBoolean(ctx,false);
    p.activeUsers=p.activeUsers+1;touchNativeSpinePrepared(p);
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbNativeSpineUnpin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeUndefined(ctx);
    spineReleasePreparedRef(gSpinePreparedByHandle[@((int)JSValueToNumber(ctx,args[0],exc))]);
    return JSValueMakeUndefined(ctx);
}

void notifyNativeSpinePreparse(const NativeSpinePrepareWaiter& waiter,bool ok,NativeSpinePrepared* parsed){
    // wantsLease 的 waiter 携带一份调用方预获取的引用:交付 JS(preparseDone 按 args[7] 记账
    // 移交)或拒收即退还。非 lease waiter(legacy 链只用原始字节)无引用,args[7]=false。
    bool leaseHeld=ok&&parsed&&waiter.wantsLease;
    if(!gCtx||gCtx!=waiter.context){if(leaseHeld)spineReleasePreparedRef(parsed);return;}
    JSValueRef args[8]={JSValueMakeNumber(gCtx,waiter.token),JSValueMakeBoolean(gCtx,ok),JSValueMakeNumber(gCtx,parsed?parsed.handle:0),JSValueMakeNumber(gCtx,parsed?parsed.zipMs:0),JSValueMakeNumber(gCtx,parsed?parsed.jsonMs:0),JSValueMakeNumber(gCtx,parsed?parsed.manifestEntries:0),JSValueMakeNumber(gCtx,parsed?parsed.imageNames.count:0),JSValueMakeBoolean(gCtx,leaseHeld)};callJsGlobal("__nativeSpinePreparseDone",8,args);
}

JSValueRef cbNativeSpineManifest(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);NativeSpinePrepared* parsed=gSpinePreparedByHandle[@((int)JSValueToNumber(ctx,args[0],exc))];if(!parsed)return JSValueMakeNull(ctx);gSpinePrepareStats.manifestGets++;touchNativeSpinePrepared(parsed);
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"handle",JSValueMakeNumber(ctx,parsed.handle));setJsProperty(ctx,out,"key",jsNSString(ctx,parsed.key));setJsProperty(ctx,out,"url",jsNSString(ctx,parsed.url));setJsProperty(ctx,out,"skeleton",jsNSString(ctx,parsed.skeletonFilename));setJsProperty(ctx,out,"atlas",jsNSString(ctx,parsed.atlasFilename));setJsProperty(ctx,out,"version",jsNSString(ctx,parsed.version));
    // skel 非空时 JS 侧应走 mode 3 取二进制,省掉 mode 2 的整棵对象图过桥
    setJsProperty(ctx,out,"skel",parsed.skelFilename.length?jsNSString(ctx,parsed.skelFilename):JSValueMakeNull(ctx));
    jscbridge::RootedValueVector images(ctx);images.reserve(parsed.imageNames.count);for(NSString* name in parsed.imageNames)images.push_back(jsNSString(ctx,name));setJsProperty(ctx,out,"images",JSObjectMakeArray(ctx,images.size(),images.empty()?nullptr:images.data(),exc));setJsProperty(ctx,out,"entryCount",JSValueMakeNumber(ctx,parsed.manifestEntries));return out;
}

JSValueRef cbNativeSpineGet(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeNull(ctx);NativeSpinePrepared* parsed=gSpinePreparedByHandle[@((int)JSValueToNumber(ctx,args[0],exc))];if(!parsed)return JSValueMakeNull(ctx);NSString* name=jsToNSString(ctx,args[1]);int mode=(int)JSValueToNumber(ctx,args[2],exc);touchNativeSpinePrepared(parsed);
    if(mode==1){if(![name isEqualToString:parsed.atlasFilename])return JSValueMakeNull(ctx);gSpinePrepareStats.atlasGets++;return jsNSString(ctx,parsed.atlasText);}
    NSData* data=parsed.entries[name];if(!data)return JSValueMakeNull(ctx);gSpinePrepareStats.imageGets++;void* bytes=malloc(data.length);if(!bytes)return JSValueMakeNull(ctx);memcpy(bytes,data.bytes,data.length);
    JSObjectRef buffer=JSObjectMakeArrayBufferWithBytesNoCopy(ctx,bytes,data.length,[](void* p,void*){free(p);},nullptr,exc);return buffer?(JSValueRef)buffer:JSValueMakeNull(ctx);
}

JSValueRef cbNativeSpinePreparseStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"requests",gSpinePrepareStats.requests);setJsNumber(ctx,out,"jobs",gSpinePrepareStats.jobs);setJsNumber(ctx,out,"deduped",gSpinePrepareStats.deduped);setJsNumber(ctx,out,"cacheHits",gSpinePrepareStats.cacheHits);setJsNumber(ctx,out,"ready",gSpinePrepareStats.ready);setJsNumber(ctx,out,"failures",gSpinePrepareStats.failures);setJsNumber(ctx,out,"fallbacks",gSpinePrepareStats.fallbacks);setJsNumber(ctx,out,"cacheEntries",gSpinePreparedCache.count);setJsNumber(ctx,out,"inflight",gSpinePrepareInflight.size());setJsNumber(ctx,out,"evictions",gSpinePrepareStats.evictions);
    {NSUInteger pinnedN=0;for(NativeSpinePrepared* hp in gSpinePreparedByHandle.allValues)if(hp.activeUsers>0)pinnedN++;setJsNumber(ctx,out,"pinned",pinnedN);}   // 在途 lease 观测:泄漏=不回落
    setJsNumber(ctx,out,"manifestGets",gSpinePrepareStats.manifestGets);setJsNumber(ctx,out,"imageGets",gSpinePrepareStats.imageGets);setJsNumber(ctx,out,"directImageRequests",gSpinePrepareStats.directImageRequests);setJsNumber(ctx,out,"directImageMisses",gSpinePrepareStats.directImageMisses);setJsNumber(ctx,out,"atlasGets",gSpinePrepareStats.atlasGets);setJsNumber(ctx,out,"jsonGets",gSpinePrepareStats.jsonGets);setJsNumber(ctx,out,"preparedLoads",gSpinePrepareStats.preparedLoads);setJsNumber(ctx,out,"loadFailures",gSpinePrepareStats.loadFailures);setJsNumber(ctx,out,"skeletonCalls",gSpinePrepareStats.skeletonCalls);setJsNumber(ctx,out,"compatSkeletonCalls",gSpinePrepareStats.compatSkeletonCalls);
    setJsNumber(ctx,out,"backgroundZipMs",gSpinePrepareStats.backgroundZipMs);setJsNumber(ctx,out,"backgroundAtlasMs",gSpinePrepareStats.backgroundAtlasMs);setJsNumber(ctx,out,"backgroundJsonMs",gSpinePrepareStats.backgroundJsonMs);setJsNumber(ctx,out,"jsonBridgeMs",gSpinePrepareStats.jsonBridgeMs);setJsNumber(ctx,out,"atlasMainMs",gSpinePrepareStats.atlasMainMs);setJsNumber(ctx,out,"skeletonMainMs",gSpinePrepareStats.skeletonMainMs);setJsNumber(ctx,out,"compatSkeletonMainMs",gSpinePrepareStats.compatSkeletonMainMs);setJsNumber(ctx,out,"maxSkeletonMainMs",gSpinePrepareStats.maxSkeletonMainMs);return out;
}

// ===== spinex（spine 全量 native 化）P0 对拍桥 + P1a 实例/驱动/事件桥 =====
// __spxBuild(key)：从 preparse 缓存（NativeSpinePrepared）取 atlas 文本 + skeleton JSON，用官方
// spine-cpp 构建 SkeletonData 进 gSpinexRt.store。key 与 JS 侧 Spine.skeletonCache['Skeleton_'+key]
// native 的 NativeSpinePrepared.key 是 __nativeSpinePreparse 收到的同一个 __spineResKey(url)。
// 返回整型 sdHandle + 动画/事件/槽/皮肤名字表（此后 JS 门面全部 index 通信）；build=retain，
// __spxRelease 对称释放。纹理接线：Atlas 解析期对每页发起 stableName→requestImageDataNamed
// 异步解码（照抄 cbLoadSpinePreparedImage 的 stableName 组装），cid 就绪前实例跳过发射。
// __spxProbe(key,anim,t)：临时 Instance 推到时刻 t，回骨骼世界矩阵数组（纯调试，不进热路径）。
bool gSpinexHostInit=false;

void spinexEnsureHost(){
    if(gSpinexHostInit)return;
    gSpinexHostInit=true;
    gSpinexRt.store.setTexRelease([](uint32_t cid){ gNativeResources.release(cid); });
    gSpinexRt.setDynamicTextureLifetime(
        [](uint32_t cid){ return gDec.retainTexture(cid) || gNativeResources.retain(cid); },
        [](uint32_t cid){ if(!gDec.releaseTexture(cid))gNativeResources.release(cid); });
}

NativeSpinePrepared* spinexFindPrepared(NSString* key){
    if(!key.length||!gSpinePreparedCache)return nil;
    // 时被当作"可复用"直接返回(M117 拿到 M90003 的 FS1 → Animation not found: sym_hit →
    // 符号 initView reject → 轮面空格子,且每帧 tick 异常拖垮 FPS)。
    for(NSInteger i=(NSInteger)gSpinePreparedLru.count-1;i>=0;i--){
        NSString* cacheKey=gSpinePreparedLru[(NSUInteger)i];
        NativeSpinePrepared* parsed=gSpinePreparedCache[cacheKey];
        if(parsed&&([parsed.key isEqualToString:key]||[cacheKey isEqualToString:key]))return parsed;
    }
    return nil;
}

JSValueRef cbSpinexBuild(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    NSString* key=argc>0?jsToNSString(ctx,args[0]):nil;
    int preparedHandle=argc>1?(int)JSValueToNumber(ctx,args[1],exc):0;   // 本次加载的精确身份(可选)
    std::string error;double t0=nowUs();
    spinex::DataStore::Entry* entry=key.length?spinexEnsureBuilt(key,preparedHandle,&error):nullptr;
    if(!entry){
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        setJsProperty(ctx,out,"error",jsNSString(ctx,[NSString stringWithUTF8String:error.empty()?"bad key":error.c_str()]));
        // 可回源失败:两层皆空(miss)或精确身份已被 LRU 逐出(evicted)=数据真没了,但按
        // __spineKeyUrl 登记的原始 url 重载即可自愈——重载完成信号带**新** preparedHandle 走
        // crc 裁决,不经短名猜测,与 evicted 拒猜防的错配无冲突(区别于解析失败等不可回源
        // 失败)。JS 据此触发 __spxRequestReload 并给 pending 门面,而不是锁死走 legacy。
        if(error=="prepared entry not found (preparse cache miss)"
           ||error=="prepared handle evicted (refusing short-key guess)")
            setJsProperty(ctx,out,"needsReload",JSValueMakeBoolean(ctx,true));
        return out;
    }
    gSpinexRt.store.retain(entry);   // build=retain；JS 侧 __spxRelease(sdHandle) 对称释放
    spine::SkeletonData* data=entry->data;
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"sdHandle",(double)entry->handle);
    setJsNumber(ctx,out,"refs",(double)entry->refs);
    setJsNumber(ctx,out,"inputCrc",(double)entry->inputCrc);
    setJsNumber(ctx,out,"bones",(double)data->getBones().size());      // 数值字段保持 P0 形状(对拍脚本在用)
    setJsNumber(ctx,out,"slots",(double)data->getSlots().size());
    setJsNumber(ctx,out,"skins",(double)data->getSkins().size());
    setJsNumber(ctx,out,"buildMs",(nowUs()-t0)/1000.0);
    // 名字表一次性下发：JS 门面之后全部 index 通信，杜绝每帧字符串桥。
    setJsProperty(ctx,out,"animations",spinexNameArray(ctx,data->getAnimations(),exc,
        [](spine::Animation* a){return a?a->getName().buffer():"";}));
    setJsProperty(ctx,out,"events",spinexNameArray(ctx,data->getEvents(),exc,
        [](spine::EventData* e){return e?e->getName().buffer():"";}));
    setJsProperty(ctx,out,"slotNames",spinexNameArray(ctx,data->getSlots(),exc,
        [](spine::SlotData* s){return s?s->getName().buffer():"";}));
    setJsProperty(ctx,out,"skinNames",spinexNameArray(ctx,data->getSkins(),exc,
        [](spine::Skin* s){return s?s->getName().buffer():"";}));
    // P2 查询面：durations(每动画秒数,下标对齐 animations)+ boneNames(findBone 轻对象用)。
    {
        spine::Vector<spine::Animation*>& anims=data->getAnimations();
        std::vector<JSValueRef> ds;ds.reserve(anims.size());
        for(size_t i=0;i<anims.size();++i)ds.push_back(JSValueMakeNumber(ctx,anims[i]?anims[i]->getDuration():0));
        setJsProperty(ctx,out,"durations",JSObjectMakeArray(ctx,ds.size(),ds.empty()?nullptr:ds.data(),exc));
    }
    setJsProperty(ctx,out,"boneNames",spinexNameArray(ctx,data->getBones(),exc,
        [](spine::BoneData* b){return b?b->getName().buffer():"";}));
    {
        spine::Vector<spine::SlotData*>& slots=data->getSlots();
        std::vector<JSValueRef> indices;indices.reserve(slots.size());
        for(size_t i=0;i<slots.size();++i)indices.push_back(JSValueMakeNumber(ctx,slots[i]->getBoneData().getIndex()));
        setJsProperty(ctx,out,"slotBoneIndices",JSObjectMakeArray(ctx,indices.size(),indices.empty()?nullptr:indices.data(),exc));
    }
    return out;
}

JSValueRef cbSpinexProbe(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    auto fail=[&](const char* why)->JSValueRef{
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        setJsProperty(ctx,out,"error",jsNSString(ctx,[NSString stringWithUTF8String:why?:"fail"]));
        return out;
    };
    if(argc<3)return fail("need (key, animName, timeSec)");
    NSString* key=jsToNSString(ctx,args[0]),*anim=jsToNSString(ctx,args[1]);
    double t=JSValueToNumber(ctx,args[2],exc);
    if(!key.length||!anim.length||!(t>=0))return fail("bad args");
    std::string error;
    spinex::DataStore::Entry* entry=spinexEnsureBuilt(key,/*preparedHandle=*/0,&error);   // 调试探针:短 key 即可
    if(!entry)return fail(error.empty()?"build failed":error.c_str());
    spinex::Instance instance(entry->data);   // 临时实例：探针即建即弃
    if(!instance.setAnimation(0,anim.UTF8String?:"",true))return fail("animation not found");
    instance.update((float)t);
    spine::Vector<spine::Bone*>& bones=instance.skeleton()->getBones();
    jscbridge::RootedValueVector rows(ctx);rows.reserve(bones.size());
    for(size_t i=0;i<bones.size();++i){
        spine::Bone* bone=bones[i];JSObjectRef row=JSObjectMake(ctx,nullptr,nullptr);
        JSStringRef s=JSStringCreateWithUTF8CString(bone->getData().getName().buffer()?:"");
        setJsProperty(ctx,row,"name",JSValueMakeString(ctx,s));JSStringRelease(s);
        setJsNumber(ctx,row,"a",bone->getA());setJsNumber(ctx,row,"b",bone->getB());
        setJsNumber(ctx,row,"c",bone->getC());setJsNumber(ctx,row,"d",bone->getD());
        setJsNumber(ctx,row,"worldX",bone->getWorldX());setJsNumber(ctx,row,"worldY",bone->getWorldY());
        rows.push_back(row);
    }
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"count",(double)rows.size());
    setJsProperty(ctx,out,"bones",JSObjectMakeArray(ctx,rows.size(),rows.empty()?nullptr:rows.data(),exc));
    return out;
}

// __spxAttachmentInfo(instId,slotIdx[,attachmentName])
// 无名时取 slot 当前 attachment；传名时调 Skeleton::getAttachment，语义与
// spine-ts Skeleton.getAttachment(slotIndex,name) 对齐（当前 skin → default skin）。
// 返回 {ok,type,name,x,y,scaleX,scaleY,rotation,width,height}，低频查询桥的字符串开销可忽略。
JSValueRef cbSpinexAttachmentInfo(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    spine::Slot* slot=nullptr;
    int slotIdx=-1;
    if(inst){
        slotIdx=(int)JSValueToNumber(ctx,args[1],exc);
        spine::Vector<spine::Slot*>& slots=inst->skeleton()->getSlots();
        if(slotIdx>=0&&(size_t)slotIdx<slots.size())slot=slots[slotIdx];
    }
    spine::Attachment* att=nullptr;
    if(slot){
        if(argc>2&&!JSValueIsNull(ctx,args[2])&&!JSValueIsUndefined(ctx,args[2])){
            NSString* name=jsToNSString(ctx,args[2]);
            att=inst->skeleton()->getAttachment(slotIdx,name.UTF8String?:"");
        }else att=slot->getAttachment();
    }
    if(!att){setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));return out;}
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    const char* type="other";
    if(att->getRTTI().isExactly(spine::RegionAttachment::rtti)){
        type="region";
        spine::RegionAttachment* ra=(spine::RegionAttachment*)att;
        setJsNumber(ctx,out,"x",ra->getX());setJsNumber(ctx,out,"y",ra->getY());
        setJsNumber(ctx,out,"scaleX",ra->getScaleX());setJsNumber(ctx,out,"scaleY",ra->getScaleY());
        setJsNumber(ctx,out,"rotation",ra->getRotation());
        setJsNumber(ctx,out,"width",ra->getWidth());setJsNumber(ctx,out,"height",ra->getHeight());
    }
    else if(att->getRTTI().isExactly(spine::MeshAttachment::rtti))type="mesh";
    else if(att->getRTTI().isExactly(spine::ClippingAttachment::rtti))type="clipping";
    JSStringRef ts=JSStringCreateWithUTF8CString(type);
    setJsProperty(ctx,out,"type",JSValueMakeString(ctx,ts));JSStringRelease(ts);
    JSStringRef ns=JSStringCreateWithUTF8CString(att->getName().buffer()?:"");
    setJsProperty(ctx,out,"name",JSValueMakeString(ctx,ns));JSStringRelease(ns);
    return out;
}

// spine-ts MeshAttachment.region/updateRegion + Slot.setAttachment 的 native 闭环。
JSValueRef cbSpinexUpdateAttachmentRegion(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<15)return JSValueMakeBoolean(ctx,false);
    NSString* name=jsToNSString(ctx,args[2]);
    return JSValueMakeBoolean(ctx,gSpinexRt.updateAttachmentRegion(
        (int)JSValueToNumber(ctx,args[0],exc),(int)JSValueToNumber(ctx,args[1],exc),name.UTF8String?:"",
        (uint32_t)JSValueToNumber(ctx,args[3],exc),
        (float)JSValueToNumber(ctx,args[4],exc),(float)JSValueToNumber(ctx,args[5],exc),
        (float)JSValueToNumber(ctx,args[6],exc),(float)JSValueToNumber(ctx,args[7],exc),
        (int)JSValueToNumber(ctx,args[8],exc),(int)JSValueToNumber(ctx,args[9],exc),
        (int)JSValueToNumber(ctx,args[10],exc),(int)JSValueToNumber(ctx,args[11],exc),
        (int)JSValueToNumber(ctx,args[12],exc),(float)JSValueToNumber(ctx,args[13],exc),
        (float)JSValueToNumber(ctx,args[14],exc),argc>15&&JSValueToBoolean(ctx,args[15])));
}

JSValueRef cbSpinexSetSlotAttachment(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);
    NSString* name=(JSValueIsNull(ctx,args[3])||JSValueIsUndefined(ctx,args[3]))?nil:jsToNSString(ctx,args[3]);
    return JSValueMakeBoolean(ctx,gSpinexRt.setSlotAttachment(
        (int)JSValueToNumber(ctx,args[0],exc),(int)JSValueToNumber(ctx,args[1],exc),
        (int)JSValueToNumber(ctx,args[2],exc),name?name.UTF8String:nullptr));
}

JSValueRef cbUnhandledRejection(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                       const JSValueRef args[], JSValueRef* exc) {
    if (argc >= 2) {
        NSString* reason = jsToNSString(ctx, args[1]);
        NSString* stack = @"";
        if (JSValueIsObject(ctx, args[1])) {
            JSObjectRef o = JSValueToObject(ctx, args[1], nullptr);
            JSStringRef sk = JSStringCreateWithUTF8CString("stack");
            JSValueRef sv = JSObjectGetProperty(ctx, o, sk, nullptr); JSStringRelease(sk);
            if (sv && !JSValueIsUndefined(ctx, sv)) stack = jsToNSString(ctx, sv);
        }
        fprintf(stderr, "[js] [unhandledRejection] %s\n%s\n", reason.UTF8String, stack.UTF8String);
        NSString* detail = stack.length ? [NSString stringWithFormat:@"%@\n%@", reason, stack] : reason;
        nativeLogError(@"javascript_runtime", @"unhandled_rejection", detail, NO);
    }
    return JSValueMakeUndefined(ctx);
}

// Browser localStorage is synchronous, so expose a small synchronous UserDefaults bridge.
// Prefix every key to avoid touching unrelated native preferences (login channel, device ID, etc.).
NSString* persistentStorageDefaultsKey(NSString* key) {
    return [@"pixi_native.localStorage." stringByAppendingString:(key ?: @"")];
}

JSValueRef cbPersistentStorageGet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                         const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeNull(ctx);
    NSString* key = jsToNSString(ctx, args[0]);
    NSString* value = [[NSUserDefaults standardUserDefaults] stringForKey:persistentStorageDefaultsKey(key)];
    return value ? jsNSString(ctx, value) : JSValueMakeNull(ctx);
}

JSValueRef cbPersistentStorageSet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                         const JSValueRef args[], JSValueRef*) {
    if (argc < 2) return JSValueMakeBoolean(ctx, false);
    NSString* key = jsToNSString(ctx, args[0]);
    NSString* value = jsToNSString(ctx, args[1]);
    [[NSUserDefaults standardUserDefaults] setObject:(value ?: @"") forKey:persistentStorageDefaultsKey(key)];
    return JSValueMakeBoolean(ctx, true);
}

JSValueRef cbPersistentStorageRemove(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                            const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    NSString* key = jsToNSString(ctx, args[0]);
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:persistentStorageDefaultsKey(key)];
    return JSValueMakeBoolean(ctx, true);
}

JSValueRef cbPersistentStorageClear(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                                           const JSValueRef[], JSValueRef*) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSString* prefix = @"pixi_native.localStorage.";
    for (NSString* key in defaults.dictionaryRepresentation.allKeys) {
        if ([key hasPrefix:prefix]) [defaults removeObjectForKey:key];
    }
    return JSValueMakeBoolean(ctx, true);
}

// 主线程回调 JS __xhrDone
void callXhrDone(int reqId, int status, NSString* statusText, NSString* respHeadersJson, NSData* body) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fn = JSStringCreateWithUTF8CString("__xhrDone");
    JSValueRef fv = JSObjectGetProperty(gCtx, global, fn, nullptr); JSStringRelease(fn);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef f = JSValueToObject(gCtx, fv, nullptr);
    if (!f || !JSObjectIsFunction(gCtx, f)) return;
    JSValueRef bodyVal;
    if (body && body.length > 0) {
        size_t len = body.length; void* buf = malloc(len); memcpy(buf, body.bytes, len);
        JSValueRef e = nullptr;
        bodyVal = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, buf, len, [](void* p, void*){ free(p); }, nullptr, &e);
        if (e) bodyVal = JSValueMakeNull(gCtx);
    } else {
        JSStringRef empty = JSStringCreateWithUTF8CString(""); bodyVal = JSValueMakeString(gCtx, empty); JSStringRelease(empty);
    }
    JSStringRef stStr = JSStringCreateWithUTF8CString(statusText ? statusText.UTF8String : "");
    JSStringRef rhStr = JSStringCreateWithUTF8CString(respHeadersJson ? respHeadersJson.UTF8String : "{}");
    JSValueRef args[5] = { JSValueMakeNumber(gCtx, reqId), JSValueMakeNumber(gCtx, status),
                           JSValueMakeString(gCtx, stStr), JSValueMakeString(gCtx, rhStr), bodyVal };
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, f, nullptr, 5, args, &exc);
    JSStringRelease(stStr); JSStringRelease(rhStr);
    if (exc) { JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr); size_t n = JSStringGetMaximumUTF8CStringSize(es);
        std::vector<char> b(n); JSStringGetUTF8CString(es, b.data(), n); fprintf(stderr, "[xhr] __xhrDone 异常: %s\n", b.data()); JSStringRelease(es); }
}

// 上次成功解析的 Remote Config pointer(NSUserDefaults 缓存)里的 killSwitches.<name>:冷启动时在 JS 解析新 pointer 之前就生效,
// 让远程 kill 覆盖"启动到 pointer 解析完成"的窗口。name = fguiFrameSyncCoalesce(合帧)| fguiFacadePull(S4 facade 读时拉)。


// 启动契约配置:seal 拦截开关。plist NativeBootSealEnforce(CMake option NATIVE_BOOT_SEAL_ENFORCE,默认 OFF)
// 或非生产 env PIXINATIVE_BOOT_SEAL_ENFORCE=1。只在开启时写 __NATIVE_BOOT_SEAL_ENFORCE=1,不写 0——
// 让 JS 前置(spin-matrix prelude 等)仍可自行置位。
void injectNativeBootConfiguration(JSGlobalContextRef ctx, JSObjectRef global) {
    BOOL enforce = [[NSBundle.mainBundle objectForInfoDictionaryKey:@"NativeBootSealEnforce"] boolValue];
    const char* source = "bundle";
#if !defined(PIXINATIVE_PRODUCTION)
    if (const char* value = getenv("PIXINATIVE_BOOT_SEAL_ENFORCE")) { enforce = atoi(value) == 1; source = "debug-env"; }
#endif
    if (enforce) {
        JSStringRef key = JSStringCreateWithUTF8CString("__NATIVE_BOOT_SEAL_ENFORCE");
        JSObjectSetProperty(ctx, global, key, JSValueMakeNumber(ctx, 1), kJSPropertyAttributeNone, nullptr);
        JSStringRelease(key);
    }
    fprintf(stderr, "[boot-config/" HOST_PLATFORM_TAG "] sealEnforce=%s source=%s\n", enforce ? "yes" : "no", source);
}


// ---- 绑定表:两端同名同回调且回调已共享的 JS 全局函数 ----
namespace {
struct HostBinding { const char* name; JSObjectCallAsFunctionCallback cb; };
const HostBinding kHostCoreBindings[] = {
    {"__nativeTextUpdate", cbNativeTextUpdate},
    {"__nativeRichTextUpdate", cbNativeRichTextUpdate},
    {"__nativeRichTextUpdateV2", cbNativeRichTextUpdateV2},
    {"__nativeTextUpdateV2", cbNativeTextUpdateV2},
    {"__nativeFguiInflate", cbNativeFguiInflate},
    {"__nativeFguiEntry", cbNativeFguiEntry},
    {"__nativeFguiManifest", cbNativeFguiManifest},
    {"__nativeFguiItem", cbNativeFguiItem},
    {"__nativeFguiItemsByType", cbNativeFguiItemsByType},
    {"__nativeFguiSprite", cbNativeFguiSprite},
    {"__nativeFguiPreparseStats", cbNativeFguiPreparseStats},
    {"__nativeFguiSetStrings", cbNativeFguiSetStrings},
    {"__nativeFguiXmlParse", cbNativeFguiXmlParse},
    {"__nativeFguiBindAtlas", cbNativeFguiBindAtlas},
    {"__nativeFguiReserve", cbNativeFguiReserve},
    {"__nativeFguiClearReservations", cbNativeFguiClearReservations},
    {"__nativeFguiCreate", cbNativeFguiCreate},
    {"__nativeFguiChild", cbNativeFguiChild},
    {"__nativeFguiSetTextMutation", cbNativeFguiSetTextMutation},
    {"__nativeFguiCommitTextMutations", cbNativeFguiCommitTextMutations},
    {"__nativeFguiSetController", cbNativeFguiSetController},
    {"__nativeFguiCommitMutations", cbNativeFguiCommitMutations},
    {"__nativeFguiSetBitmapText", cbNativeFguiSetBitmapText},
    {"__nativeFguiTransitionPlay", cbNativeFguiTransitionPlay},
    {"__nativeFguiTransitionStop", cbNativeFguiTransitionStop},
    {"__nativeFguiTransitionSetValue", cbNativeFguiTransitionSetValue},
    {"__nativeFguiTransitionSetTarget", cbNativeFguiTransitionSetTarget},
    {"__nativeFguiTransitionSetDuration", cbNativeFguiTransitionSetDuration},
    {"__nativeFguiTransitionUpdateRelations", cbNativeFguiTransitionUpdateRelations},
    {"__nativeFguiTransitionChangeRepeat", cbNativeFguiTransitionChangeRepeat},
    {"__nativeFguiTransitionPlaying", cbNativeFguiTransitionPlaying},
    {"__persistentStorageGet", cbPersistentStorageGet},
    {"__persistentStorageSet", cbPersistentStorageSet},
    {"__persistentStorageRemove", cbPersistentStorageRemove},
    {"__persistentStorageClear", cbPersistentStorageClear},
    {"__decodeImageAsync", cbDecodeImageAsync},
    {"__nativeResourceAlive", cbNativeResourceAlive},
    {"__nativeResourceRetain", cbNativeResourceRetain},
    {"__nativeResourceRelease", cbNativeResourceRelease},
    {"__nativeResourceReleaseAdopt", cbNativeResourceReleaseAdopt},
    {"__nativeResourceTransferred", cbNativeResourceTransferred},
    {"__nativeResourceStats", cbNativeResourceStats},
    {"__nativeSpinePin", cbNativeSpinePin},
    {"__nativeSpineUnpin", cbNativeSpineUnpin},
    {"__nativeSpineManifest", cbNativeSpineManifest},
    {"__nativeSpineGet", cbNativeSpineGet},
    {"__nativeSpinePreparseStats", cbNativeSpinePreparseStats},
    {"__spxBuild", cbSpinexBuild},
    {"__spxProbe", cbSpinexProbe},
    {"__spxAttachmentInfo", cbSpinexAttachmentInfo},
    {"__spxUpdateAttachmentRegion", cbSpinexUpdateAttachmentRegion},
    {"__spxSetSlotAttachment", cbSpinexSetSlotAttachment},
    {"__readFile", cbReadFile},
    {"__evalScript", cbEvalScript},
    {"__setTimer", cbSetTimer},
    {"__clearTimer", cbClearTimer},
};
}  // namespace
void hostCoreRegisterBindings(JSContextRef ctx, JSObjectRef global) {
    hostCoreRegisterCommonBindings(ctx, global);   // 三端共用部分(host_core_common.cpp)
    for (const HostBinding& b : kHostCoreBindings) setFn(ctx, global, b.name, b.cb);
}
