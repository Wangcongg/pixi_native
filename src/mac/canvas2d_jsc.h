// S8: 最小原生 Canvas2D（pixi Text 用的子集）。CoreText 绘字 + CoreGraphics 光栅化。
// 暴露 __c2d* 给 JS 2d context 垫片调用。坐标系：JS canvas 左上原点 y 向下 → 内部转 CG 左下原点。
// 像素：premultiplied RGBA8（与 Image 路径一致，纹理上传无需再处理）；__c2dGetData 翻成左上行优先。
#pragma once
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>   // 仅 macOS；本文件实际只用 CoreGraphics/CoreText/Foundation，iOS 下无需 AppKit
#else
#import <CoreGraphics/CoreGraphics.h>
#endif
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <JavaScriptCore/JavaScriptCore.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <map>
#include <algorithm>

// 游戏靠 FontFace API 加载(JSC 无此 API → 失败)，这里改由宿主注册文件并建映射，makeFont 据此解析。
inline std::map<std::string,std::string> gFontAlias;   // inline:host_core.mm 与平台宿主两个 TU 共享同一份

namespace c2d {

struct Canvas {
    int w=1,h=1;
    unsigned char* buf=nullptr;
    CGContextRef cg=nullptr;
    CGAffineTransform transform=CGAffineTransformIdentity;
    std::vector<CGAffineTransform> transformStack;
};
inline std::vector<Canvas> gC;   // inline:同上,画布池必须是单实例

static CGColorSpaceRef cs() { static CGColorSpaceRef s = CGColorSpaceCreateDeviceRGB(); return s; }
static void freeC(Canvas& c){ if(c.cg){CGContextRelease(c.cg);c.cg=nullptr;} if(c.buf){free(c.buf);c.buf=nullptr;} }
static void allocC(Canvas& c, int w, int h){
    freeC(c); c.w = w<1?1:w; c.h = h<1?1:h;
    c.transform=CGAffineTransformIdentity; c.transformStack.clear();
    c.buf = (unsigned char*)calloc((size_t)c.w*c.h*4, 1);
    c.cg = CGBitmapContextCreate(c.buf, c.w, c.h, 8, c.w*4, cs(), kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    if (c.cg){ CGContextTranslateCTM(c.cg, 0, c.h); CGContextScaleCTM(c.cg, 1, -1); }  // 翻成左上原点(canvas 约定)，内存即左上行优先
}

static std::string jsStr(JSContextRef ctx, JSValueRef v){
    JSStringRef s = JSValueToStringCopy(ctx, v, nullptr);
    size_t n = JSStringGetMaximumUTF8CStringSize(s); std::string r(n, 0);
    JSStringGetUTF8CString(s, &r[0], n); r.resize(strlen(r.c_str())); JSStringRelease(s); return r;
}
static double jsNum(JSContextRef ctx, JSValueRef v){ return JSValueToNumber(ctx, v, nullptr); }

// CSS 颜色 → rgba[0..1]
static void parseColor(const std::string& s, CGFloat out[4]){
    out[0]=out[1]=out[2]=0; out[3]=1;
    if (s.empty()) return;
    if (s[0]=='#'){
        unsigned r=0,g=0,b=0,a=255;
        if (s.size()>=7){ sscanf(s.c_str()+1,"%02x%02x%02x",&r,&g,&b); if(s.size()>=9) sscanf(s.c_str()+7,"%02x",&a); }
        else if (s.size()>=4){ unsigned rr=0,gg=0,bb=0,aa=15; sscanf(s.c_str()+1,"%1x%1x%1x",&rr,&gg,&bb); if(s.size()>=5) sscanf(s.c_str()+4,"%1x",&aa); r=rr*17;g=gg*17;b=bb*17;a=aa*17; }
        out[0]=r/255.0; out[1]=g/255.0; out[2]=b/255.0; out[3]=a/255.0; return;
    }
    if (s.compare(0,4,"rgba")==0){ float r=0,g=0,b=0,a=1; sscanf(s.c_str(),"rgba(%f,%f,%f,%f",&r,&g,&b,&a); out[0]=r/255.0;out[1]=g/255.0;out[2]=b/255.0;out[3]=a; return; }
    if (s.compare(0,3,"rgb")==0){ float r=0,g=0,b=0; sscanf(s.c_str(),"rgb(%f,%f,%f",&r,&g,&b); out[0]=r/255.0;out[1]=g/255.0;out[2]=b/255.0; return; }
    if (s=="white"){ out[0]=out[1]=out[2]=1; } else if (s=="red"){ out[0]=1; }
    else if (s=="green"){ out[1]=1; } else if (s=="blue"){ out[2]=1; }
    else if (s=="transparent") out[3]=0;
    // 其余默认黑
}

static std::vector<std::string> splitTextGradientField(const std::string& source,char separator){
    std::vector<std::string> out;size_t pos=0;
    while(pos<=source.size()){size_t end=source.find(separator,pos);if(end==std::string::npos)end=source.size();out.push_back(source.substr(pos,end-pos));if(end==source.size())break;pos=end+1;}
    return out;
}

// JS sends CSS colours separately from offsets so rgba(...) remains intact. Convert that compact
// bridge representation to the flat stop vector already consumed by drawTextGradient.
static std::vector<double> textGradientStops(const std::string& encodedColors,const std::string& encodedStops){
    if(encodedColors.empty()||encodedColors=="none")return {};
    std::vector<std::string> colors=splitTextGradientField(encodedColors,'|');
    std::vector<std::string> locations=splitTextGradientField(encodedStops,',');
    if(colors.size()<2)return {};
    std::vector<double> out;out.reserve(colors.size()*5);double previous=0;
    for(size_t i=0;i<colors.size();++i){
        double location=i<locations.size()?std::strtod(locations[i].c_str(),nullptr):(colors.size()==1?0:(double)i/(double)(colors.size()-1));
        if(!std::isfinite(location))location=previous;location=std::max(previous,std::min(1.0,std::max(0.0,location)));previous=location;
        CGFloat rgba[4];parseColor(colors[i],rgba);out.push_back(location);for(int component=0;component<4;component++)out.push_back(rgba[component]);
    }
    return out;
}

// CSS font: "[style] [variant] [weight] <size>px <family[, ...]>"
static CTFontRef makeFont(const std::string& font){
    double size=10; bool bold=false, italic=false;
    size_t pxp = font.find("px");
    if (pxp!=std::string::npos){ size_t b=pxp; while(b>0 && (isdigit((unsigned char)font[b-1])||font[b-1]=='.')) b--; size=atof(font.substr(b,pxp-b).c_str()); }
    if (font.find("bold")!=std::string::npos||font.find("Bold")!=std::string::npos||font.find(" 700")!=std::string::npos) bold=true;
    if (font.find("italic")!=std::string::npos) italic=true;
    std::string fam="Helvetica";
    if (pxp!=std::string::npos){ size_t fs=font.find(' ', pxp); if(fs!=std::string::npos) fam=font.substr(fs+1); }
    size_t comma=fam.find(','); if(comma!=std::string::npos) fam=fam.substr(0,comma);
    while(!fam.empty()&&(fam[0]==' '||fam[0]=='\''||fam[0]=='"')) fam.erase(0,1);
    while(!fam.empty()&&(fam.back()==' '||fam.back()=='\''||fam.back()=='"')) fam.pop_back();
    if (fam.empty()) fam="Helvetica";
    { auto it=gFontAlias.find(fam); if(it!=gFontAlias.end()) fam=it->second;
      // CoreText resolves Arial to ArialMT but logs a performance note on every call.
      // Use the same face's PostScript name for repeated text measurement/drawing.
      else if(fam=="Arial") fam="ArialMT"; }   // 游戏自定义名 → 实际 PostScript 名
    CFStringRef cfFam=CFStringCreateWithCString(NULL,fam.c_str(),kCFStringEncodingUTF8);
    CTFontRef base=CTFontCreateWithName(cfFam,size,NULL); CFRelease(cfFam);
    if (bold||italic){
        CTFontSymbolicTraits tr=0; if(bold)tr|=kCTFontTraitBold; if(italic)tr|=kCTFontTraitItalic;
        CTFontRef styled=CTFontCreateCopyWithSymbolicTraits(base,size,NULL,tr,tr);
        if(styled){ CFRelease(base); base=styled; }
    }
    return base;
}

static double alignedX(CTLineRef line, double x, const std::string& align){
    double width=CTLineGetTypographicBounds(line,nullptr,nullptr,nullptr);
    if(align=="center") return x-width*0.5;
    if(align=="right"||align=="end") return x-width;
    return x;
}

static void applyShadow(CGContextRef cg, const std::string& css, double blur, double offsetX, double offsetY, double alpha){
    CGFloat shadow[4]; parseColor(css,shadow); shadow[3]*=alpha;
    if(shadow[3]<=0 || (blur<=0 && offsetX==0 && offsetY==0)) return;
    CGColorRef color=CGColorCreate(cs(),shadow);
    // CoreGraphics shadow offset 用 default user space(y 向上)；Canvas 用 y 向下。
    CGContextSetShadowWithColor(cg,CGSizeMake(offsetX,-offsetY),std::max(0.0,blur),color);
    CGColorRelease(color);
}

static CGLineJoin parseLineJoin(const std::string& join){
    if(join=="round") return kCGLineJoinRound;
    if(join=="bevel") return kCGLineJoinBevel;
    return kCGLineJoinMiter;
}

static void drawText(Canvas& c, const std::string& str, double x, double y, const std::string& fontStr,
                     CGFloat color[4], const std::string& baseline, const std::string& align,
                     bool stroke, double lineWidth, const std::string& lineJoin, double miterLimit,
                     const std::string& shadowCss, double shadowBlur, double shadowX, double shadowY, double alpha,
                     double letterSpacing=0){
    CTFontRef font=makeFont(fontStr);
    CFStringRef cfStr=CFStringCreateWithCString(NULL,str.c_str(),kCFStringEncodingUTF8);
    if(!cfStr){ CFRelease(font); return; }
    double ascent=CTFontGetAscent(font), descent=CTFontGetDescent(font);
    double baseY=y;                                   // alphabetic：y 即基线
    if (baseline=="top") baseY=y+ascent;
    else if (baseline=="middle") baseY=y+ascent-(ascent+descent)/2;
    else if (baseline=="bottom") baseY=y-descent;
    else if (baseline=="hanging") baseY=y+ascent*0.8;
    CGColorRef col=CGColorCreate(cs(),color);
    CFStringRef keys[3]={kCTFontAttributeName,kCTForegroundColorAttributeName,kCTKernAttributeName};
    CFTypeRef vals[3]={font,col,nullptr};int attrCount=2;CFNumberRef kern=nullptr;
    if(std::fabs(letterSpacing)>0.0001){CGFloat value=(CGFloat)letterSpacing;kern=CFNumberCreate(NULL,kCFNumberCGFloatType,&value);vals[attrCount++]=kern;}
    CFDictionaryRef attrs=CFDictionaryCreate(NULL,(const void**)keys,(const void**)vals,attrCount,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as=CFAttributedStringCreate(NULL,cfStr,attrs);
    CTLineRef line=CTLineCreateWithAttributedString(as);
    x=alignedX(line,x,align);
    CGContextSaveGState(c.cg);
    CGContextConcatCTM(c.cg,c.transform);
    applyShadow(c.cg,shadowCss,shadowBlur,shadowX,shadowY,alpha);
    if (stroke){ CGContextSetTextDrawingMode(c.cg,kCGTextStroke); CGContextSetStrokeColorWithColor(c.cg,col); CGContextSetLineWidth(c.cg,lineWidth); CGContextSetLineJoin(c.cg,parseLineJoin(lineJoin)); CGContextSetMiterLimit(c.cg,miterLimit); }
    else CGContextSetTextDrawingMode(c.cg,kCGTextFill);
    // 上下文已翻成左上原点；文字在该空间会上下镜像 → 在基线点局部再反翻一次使字形正立
    CGContextTranslateCTM(c.cg, x, baseY);
    CGContextScaleCTM(c.cg, 1, -1);
    CGContextSetTextPosition(c.cg, 0, 0);
    CTLineDraw(line,c.cg);
    CGContextRestoreGState(c.cg);
    CFRelease(line);CFRelease(as);CFRelease(attrs);if(kern)CFRelease(kern);CGColorRelease(col);CFRelease(cfStr);CFRelease(font);
}

// 渐变填充文字：用字形作剪裁区(kCGTextClip)，再在其上画线性渐变。stops 为扁平数组 [offset,r,g,b,a, ...]。
static void drawTextGradient(Canvas& c, const std::string& str, double x, double y, const std::string& fontStr,
                             double x0,double y0,double x1,double y1,
                             const std::vector<double>& stops, const std::string& baseline, const std::string& align,
                             double alpha, const std::string& shadowCss, double shadowBlur, double shadowX, double shadowY,
                             double letterSpacing=0){
    if (stops.size() < 10) return;   // 至少 2 个 stop
    CTFontRef font=makeFont(fontStr);
    CFStringRef cfStr=CFStringCreateWithCString(NULL,str.c_str(),kCFStringEncodingUTF8);
    if(!cfStr){ CFRelease(font); return; }
    double ascent=CTFontGetAscent(font), descent=CTFontGetDescent(font);
    double baseY=y;
    if (baseline=="top") baseY=y+ascent;
    else if (baseline=="middle") baseY=y+ascent-(ascent+descent)/2;
    else if (baseline=="bottom") baseY=y-descent;
    else if (baseline=="hanging") baseY=y+ascent*0.8;
    CFStringRef keys[2]={kCTFontAttributeName,kCTKernAttributeName}; CFTypeRef vals[2]={font,nullptr};int attrCount=1;CFNumberRef kern=nullptr;
    if(std::fabs(letterSpacing)>0.0001){CGFloat value=(CGFloat)letterSpacing;kern=CFNumberCreate(NULL,kCFNumberCGFloatType,&value);vals[attrCount++]=kern;}
    CFDictionaryRef attrs=CFDictionaryCreate(NULL,(const void**)keys,(const void**)vals,attrCount,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as=CFAttributedStringCreate(NULL,cfStr,attrs);
    CTLineRef line=CTLineCreateWithAttributedString(as);
    x=alignedX(line,x,align);
    size_t n=stops.size()/5;
    std::vector<CGFloat> comps(n*4), locs(n);
    for(size_t i=0;i<n;i++){ locs[i]=stops[i*5]; comps[i*4]=stops[i*5+1]; comps[i*4+1]=stops[i*5+2]; comps[i*4+2]=stops[i*5+3]; comps[i*4+3]=stops[i*5+4]*alpha; }
    CGGradientRef grad=CGGradientCreateWithColorComponents(cs(), comps.data(), locs.data(), n);
    CGContextSaveGState(c.cg);
    CGContextConcatCTM(c.cg,c.transform);
    applyShadow(c.cg,shadowCss,shadowBlur,shadowX,shadowY,alpha);
    // 局部翻转使字形正立(同 drawText)，以字形设剪裁区
    CGContextTranslateCTM(c.cg, x, baseY); CGContextScaleCTM(c.cg, 1, -1);
    CGContextSetTextDrawingMode(c.cg, kCGTextClip);
    CGContextSetTextPosition(c.cg, 0, 0);
    CTLineDraw(line, c.cg);
    // 撤销局部变换回画布坐标系(剪裁区已在设备空间保留)，用 pixi 给的画布坐标画渐变
    CGContextScaleCTM(c.cg, 1, -1); CGContextTranslateCTM(c.cg, -x, -baseY);
    CGContextDrawLinearGradient(c.cg, grad, CGPointMake(x0,y0), CGPointMake(x1,y1),
        kCGGradientDrawsBeforeStartLocation|kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(c.cg);
    CGGradientRelease(grad);
    CFRelease(line);CFRelease(as);CFRelease(attrs);if(kern)CFRelease(kern);CFRelease(cfStr);CFRelease(font);
}

// Draw custom attributed-string gradient runs after CoreText has painted their stroke. Gradient
// runs use a transparent foreground colour, then their glyph paths become a CoreGraphics clip.
// The context must already be in the same bottom-left CoreText coordinate space as CTFrameDraw.
// line 级渐变绘制:token 级排版(richtext_token_layout.h)下每个 token 是独立 CTLine,
// origin 为该 line 的 baseline 原点(bottom-up 坐标)。frame 版在下面按行调用同一逻辑。
static void drawLineTextGradients(CTLineRef line,CGPoint origin,CGContextRef cg,CFStringRef attributeName){
    if(!line||!cg||!attributeName)return;
    CFArrayRef runs=CTLineGetGlyphRuns(line);CFIndex runCount=runs?CFArrayGetCount(runs):0;
    for(CFIndex runIndex=0;runIndex<runCount;runIndex++){
        CTRunRef run=(CTRunRef)CFArrayGetValueAtIndex(runs,runIndex);CFDictionaryRef attrs=CTRunGetAttributes(run);if(!attrs)continue;
        CFTypeRef raw=CFDictionaryGetValue(attrs,attributeName);if(!raw||CFGetTypeID(raw)!=CFStringGetTypeID())continue;
        CFStringRef descriptor=(CFStringRef)raw;CFIndex length=CFStringGetLength(descriptor);CFIndex bytes=CFStringGetMaximumSizeForEncoding(length,kCFStringEncodingUTF8)+1;
        std::string encoded((size_t)std::max<CFIndex>(1,bytes),'\0');if(!CFStringGetCString(descriptor,&encoded[0],bytes,kCFStringEncodingUTF8))continue;encoded.resize(std::strlen(encoded.c_str()));
        size_t first=encoded.find('\n'),second=first==std::string::npos?std::string::npos:encoded.find('\n',first+1);if(first==std::string::npos||second==std::string::npos)continue;
        std::vector<double> stops=textGradientStops(encoded.substr(0,first),encoded.substr(first+1,second-first-1));if(stops.size()<10)continue;
        int type=std::atoi(encoded.substr(second+1).c_str());CFIndex glyphCount=CTRunGetGlyphCount(run);if(glyphCount<=0)continue;
        CTFontRef font=(CTFontRef)CFDictionaryGetValue(attrs,kCTFontAttributeName);if(!font)continue;
        std::vector<CGGlyph> glyphs((size_t)glyphCount);std::vector<CGPoint> positions((size_t)glyphCount);CTRunGetGlyphs(run,CFRangeMake(0,0),glyphs.data());CTRunGetPositions(run,CFRangeMake(0,0),positions.data());
        CGMutablePathRef path=CGPathCreateMutable();
        for(CFIndex glyphIndex=0;glyphIndex<glyphCount;glyphIndex++){
            CGPathRef glyph=CTFontCreatePathForGlyph(font,glyphs[(size_t)glyphIndex],nullptr);if(!glyph)continue;
            CGAffineTransform transform=CGAffineTransformMakeTranslation(origin.x+positions[(size_t)glyphIndex].x,origin.y+positions[(size_t)glyphIndex].y);
            CGPathAddPath(path,&transform,glyph);CGPathRelease(glyph);
        }
        CGRect bounds=CGPathGetBoundingBox(path);if(CGPathIsEmpty(path)||CGRectIsEmpty(bounds)){CGPathRelease(path);continue;}
        size_t count=stops.size()/5;std::vector<CGFloat> components(count*4),locations(count);
        for(size_t i=0;i<count;i++){locations[i]=(CGFloat)stops[i*5];for(int component=0;component<4;component++)components[i*4+(size_t)component]=(CGFloat)stops[i*5+1+(size_t)component];}
        CGGradientRef gradient=CGGradientCreateWithColorComponents(cs(),components.data(),locations.data(),count);if(!gradient){CGPathRelease(path);continue;}
        CGPoint start=type==1?CGPointMake(CGRectGetMinX(bounds),CGRectGetMidY(bounds)):CGPointMake(CGRectGetMidX(bounds),CGRectGetMaxY(bounds));
        CGPoint end=type==1?CGPointMake(CGRectGetMaxX(bounds),CGRectGetMidY(bounds)):CGPointMake(CGRectGetMidX(bounds),CGRectGetMinY(bounds));
        CGContextSaveGState(cg);CGContextAddPath(cg,path);CGContextClip(cg);CGContextDrawLinearGradient(cg,gradient,start,end,kCGGradientDrawsBeforeStartLocation|kCGGradientDrawsAfterEndLocation);CGContextRestoreGState(cg);
        CGGradientRelease(gradient);CGPathRelease(path);
    }
}
static void drawFrameTextGradients(CTFrameRef frame,CGContextRef cg,CFStringRef attributeName){
    if(!frame||!cg||!attributeName)return;
    CFArrayRef lines=CTFrameGetLines(frame);CFIndex lineCount=lines?CFArrayGetCount(lines):0;if(lineCount<=0)return;
    std::vector<CGPoint> origins((size_t)lineCount);CTFrameGetLineOrigins(frame,CFRangeMake(0,0),origins.data());
    for(CFIndex lineIndex=0;lineIndex<lineCount;lineIndex++)
        drawLineTextGradients((CTLineRef)CFArrayGetValueAtIndex(lines,lineIndex),origins[(size_t)lineIndex],cg,attributeName);
}

static double measureWidth(const std::string& str, const std::string& fontStr){
    CTFontRef font=makeFont(fontStr);
    CFStringRef cfStr=CFStringCreateWithCString(NULL,str.c_str(),kCFStringEncodingUTF8);
    if(!cfStr){CFRelease(font);return 0;}
    CFStringRef keys[1]={kCTFontAttributeName}; CFTypeRef vals[1]={font};
    CFDictionaryRef attrs=CFDictionaryCreate(NULL,(const void**)keys,(const void**)vals,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as=CFAttributedStringCreate(NULL,cfStr,attrs);
    CTLineRef line=CTLineCreateWithAttributedString(as);
    double w=CTLineGetTypographicBounds(line,NULL,NULL,NULL);
    CFRelease(line);CFRelease(as);CFRelease(attrs);CFRelease(cfStr);CFRelease(font);
    return w;
}

// ---- JSC 回调 ----
static JSValueRef cbCreate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int w=argc>0?(int)jsNum(ctx,a[0]):1, h=argc>1?(int)jsNum(ctx,a[1]):1;
    Canvas c; allocC(c,w,h); gC.push_back(c);
    return JSValueMakeNumber(ctx,(double)(gC.size()-1));
}
static JSValueRef cbResize(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    allocC(gC[id],(int)jsNum(ctx,a[1]),(int)jsNum(ctx,a[2])); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbSave(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id>=0&&id<(int)gC.size()) gC[id].transformStack.push_back(gC[id].transform);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbRestore(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]);
    if(id>=0&&id<(int)gC.size()&&!gC[id].transformStack.empty()){
        gC[id].transform=gC[id].transformStack.back(); gC[id].transformStack.pop_back();
    }
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbScale(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id>=0&&id<(int)gC.size()) gC[id].transform=CGAffineTransformScale(gC[id].transform,jsNum(ctx,a[1]),jsNum(ctx,a[2]));
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbTranslate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id>=0&&id<(int)gC.size()) gC[id].transform=CGAffineTransformTranslate(gC[id].transform,jsNum(ctx,a[1]),jsNum(ctx,a[2]));
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbRotate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id>=0&&id<(int)gC.size()) gC[id].transform=CGAffineTransformRotate(gC[id].transform,jsNum(ctx,a[1]));
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbTransform(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]);
    if(id>=0&&id<(int)gC.size()) gC[id].transform=CGAffineTransformConcat(gC[id].transform,
        CGAffineTransformMake(jsNum(ctx,a[1]),jsNum(ctx,a[2]),jsNum(ctx,a[3]),jsNum(ctx,a[4]),jsNum(ctx,a[5]),jsNum(ctx,a[6])));
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbSetTransform(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    gC[id].transform=argc>=7 ? CGAffineTransformMake(jsNum(ctx,a[1]),jsNum(ctx,a[2]),jsNum(ctx,a[3]),jsNum(ctx,a[4]),jsNum(ctx,a[5]),jsNum(ctx,a[6])) : CGAffineTransformIdentity;
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbResetTransform(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id>=0&&id<(int)gC.size()) gC[id].transform=CGAffineTransformIdentity;
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbMeasure(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    return JSValueMakeNumber(ctx, measureWidth(jsStr(ctx,a[1]), jsStr(ctx,a[2])));
}
// __c2dFillText(id,str,x,y,font,fillCss,baseline,alpha,align,shadowCss,shadowBlur,shadowX,shadowY)
static JSValueRef cbFillText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    double alpha=argc>7?jsNum(ctx,a[7]):1.0;
    CGFloat col[4]; parseColor(jsStr(ctx,a[5]),col); col[3]*=alpha;
    drawText(gC[id], jsStr(ctx,a[1]), jsNum(ctx,a[2]), jsNum(ctx,a[3]), jsStr(ctx,a[4]), col, jsStr(ctx,a[6]),
             argc>8?jsStr(ctx,a[8]):"left", false, 0, "miter", 10,
             argc>9?jsStr(ctx,a[9]):"transparent", argc>10?jsNum(ctx,a[10]):0,
             argc>11?jsNum(ctx,a[11]):0, argc>12?jsNum(ctx,a[12]):0, alpha);
    return JSValueMakeUndefined(ctx);
}
// __c2dFillTextGrad(id,str,x,y,font, x0,y0,x1,y1, stopsJson[扁平 offset,r,g,b,a...], baseline, alpha)
static JSValueRef cbFillTextGrad(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    std::string sj=jsStr(ctx,a[9]); std::vector<double> stops;
    for(const char* p=sj.c_str(); *p; ){ if((*p>='0'&&*p<='9')||*p=='-'||*p=='.'){ char* e; stops.push_back(strtod(p,&e)); p=e; } else ++p; }
    double alpha=argc>11?jsNum(ctx,a[11]):1.0;
    drawTextGradient(gC[id], jsStr(ctx,a[1]), jsNum(ctx,a[2]), jsNum(ctx,a[3]), jsStr(ctx,a[4]),
                     jsNum(ctx,a[5]),jsNum(ctx,a[6]),jsNum(ctx,a[7]),jsNum(ctx,a[8]), stops, jsStr(ctx,a[10]),
                     argc>12?jsStr(ctx,a[12]):"left", alpha, argc>13?jsStr(ctx,a[13]):"transparent",
                     argc>14?jsNum(ctx,a[14]):0, argc>15?jsNum(ctx,a[15]):0, argc>16?jsNum(ctx,a[16]):0);
    return JSValueMakeUndefined(ctx);
}
// __c2dStrokeText(id,str,x,y,font,strokeCss,lineWidth,baseline,alpha,align,lineJoin,miterLimit,shadowCss,shadowBlur,shadowX,shadowY)
static JSValueRef cbStrokeText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    double alpha=argc>8?jsNum(ctx,a[8]):1.0;
    CGFloat col[4]; parseColor(jsStr(ctx,a[5]),col); col[3]*=alpha;
    drawText(gC[id], jsStr(ctx,a[1]), jsNum(ctx,a[2]), jsNum(ctx,a[3]), jsStr(ctx,a[4]), col, jsStr(ctx,a[7]),
             argc>9?jsStr(ctx,a[9]):"left", true, jsNum(ctx,a[6]), argc>10?jsStr(ctx,a[10]):"miter",
             argc>11?jsNum(ctx,a[11]):10, argc>12?jsStr(ctx,a[12]):"transparent",
             argc>13?jsNum(ctx,a[13]):0, argc>14?jsNum(ctx,a[14]):0, argc>15?jsNum(ctx,a[15]):0, alpha);
    return JSValueMakeUndefined(ctx);
}
// __c2dFillRect(id,x,y,w,h,fillCss,alpha)
static JSValueRef cbFillRect(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    Canvas& c=gC[id]; double x=jsNum(ctx,a[1]),y=jsNum(ctx,a[2]),w=jsNum(ctx,a[3]),h=jsNum(ctx,a[4]);
    CGFloat col[4]; parseColor(jsStr(ctx,a[5]),col); col[3]*=(argc>6?jsNum(ctx,a[6]):1.0);
    CGContextSaveGState(c.cg); CGContextConcatCTM(c.cg,c.transform);
    CGContextSetRGBFillColor(c.cg,col[0],col[1],col[2],col[3]);
    CGContextFillRect(c.cg, CGRectMake(x, y, w, h));   // 上下文已左上原点
    CGContextRestoreGState(c.cg);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbClearRect(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    Canvas& c=gC[id]; double x=jsNum(ctx,a[1]),y=jsNum(ctx,a[2]),w=jsNum(ctx,a[3]),h=jsNum(ctx,a[4]);
    CGContextSaveGState(c.cg); CGContextConcatCTM(c.cg,c.transform);
    CGContextClearRect(c.cg, CGRectMake(x, y, w, h)); CGContextRestoreGState(c.cg);
    return JSValueMakeUndefined(ctx);
}
// __c2dGetData(id) -> ArrayBuffer（左上行优先 premultiplied RGBA）
static JSValueRef cbGetData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* exc){
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeNull(ctx);
    Canvas& c=gC[id]; size_t bytes=(size_t)c.w*c.h*4; unsigned char* out=(unsigned char*)malloc(bytes);
    memcpy(out, c.buf, bytes);   // CTM 已翻转 → buf 即左上行优先，无需再翻
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx,out,bytes,[](void* p,void*){ free(p); },nullptr,exc);
}

// __c2dPutData(id, ArrayBuffer(RGBA straight alpha), w, h, dx, dy):写入像素(预乘进 CG buf)。pixi Extract 截图用。
static JSValueRef cbPutData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* exc){
    if (argc < 4) return JSValueMakeUndefined(ctx);
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()) return JSValueMakeUndefined(ctx);
    Canvas& c=gC[id];
    JSObjectRef bufObj = JSValueToObject(ctx, a[1], exc); if(!bufObj) return JSValueMakeUndefined(ctx);
    unsigned char* src = (unsigned char*)JSObjectGetArrayBufferBytesPtr(ctx, bufObj, exc);
    size_t srcLen = JSObjectGetArrayBufferByteLength(ctx, bufObj, exc);
    int w=(int)jsNum(ctx,a[2]), h=(int)jsNum(ctx,a[3]);
    int dx=argc>4?(int)jsNum(ctx,a[4]):0, dy=argc>5?(int)jsNum(ctx,a[5]):0;
    if (!src || w<=0 || h<=0 || srcLen < (size_t)w*h*4) return JSValueMakeUndefined(ctx);
    for (int row=0; row<h; row++){
        int ty = dy+row; if (ty<0 || ty>=c.h) continue;
        for (int col=0; col<w; col++){
            int tx = dx+col; if (tx<0 || tx>=c.w) continue;
            const unsigned char* s = src + ((size_t)row*w+col)*4;
            unsigned char* d = c.buf + ((size_t)ty*c.w+tx)*4;
            unsigned a8 = s[3];   // straight → premultiplied(buf 是 kCGImageAlphaPremultipliedLast)
            d[0]=(unsigned char)((s[0]*a8+127)/255); d[1]=(unsigned char)((s[1]*a8+127)/255);
            d[2]=(unsigned char)((s[2]*a8+127)/255); d[3]=(unsigned char)a8;
        }
    }
    return JSValueMakeUndefined(ctx);
}
// __c2dToDataURL(id, "image/png"|"image/jpeg", quality) -> "data:image/png;base64,..."
static JSValueRef cbToDataURL(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*){
    auto empty = [&]{ JSStringRef s=JSStringCreateWithUTF8CString("data:,"); JSValueRef v=JSValueMakeString(ctx,s); JSStringRelease(s); return v; };
    int id=(int)jsNum(ctx,a[0]); if(id<0||id>=(int)gC.size()||!gC[id].cg) return empty();
    std::string fmt = argc>1 ? jsStr(ctx,a[1]) : "image/png";
    double quality = argc>2 ? jsNum(ctx,a[2]) : 0.92;
    bool jpeg = fmt.find("jpeg")!=std::string::npos || fmt.find("jpg")!=std::string::npos;
    CGImageRef img = CGBitmapContextCreateImage(gC[id].cg);
    if (!img) return empty();
    NSMutableData* data = [NSMutableData data];
    CGImageDestinationRef dest = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data,
                                     jpeg ? CFSTR("public.jpeg") : CFSTR("public.png"), 1, NULL);
    if (!dest){ CGImageRelease(img); return empty(); }
    NSDictionary* opts = jpeg ? @{ (__bridge NSString*)kCGImageDestinationLossyCompressionQuality: @(quality) } : @{};
    CGImageDestinationAddImage(dest, img, (__bridge CFDictionaryRef)opts);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest); CGImageRelease(img);
    if (!ok || !data.length) return empty();
    NSString* url = [NSString stringWithFormat:@"data:%s;base64,%@", jpeg?"image/jpeg":"image/png",
                     [data base64EncodedStringWithOptions:0]];
    JSStringRef s = JSStringCreateWithUTF8CString(url.UTF8String);
    JSValueRef v = JSValueMakeString(ctx, s); JSStringRelease(s);
    return v;
}

static void c2dSetFn(JSContextRef ctx, JSObjectRef g, const char* name, JSObjectCallAsFunctionCallback cb){
    JSStringRef s=JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx,g,s,JSObjectMakeFunctionWithCallback(ctx,s,cb),kJSPropertyAttributeNone,NULL);
    JSStringRelease(s);
}
static void registerCanvas2D(JSContextRef ctx, JSObjectRef g){
    c2dSetFn(ctx,g,"__c2dCreate",cbCreate);
    c2dSetFn(ctx,g,"__c2dResize",cbResize);
    c2dSetFn(ctx,g,"__c2dSave",cbSave);
    c2dSetFn(ctx,g,"__c2dRestore",cbRestore);
    c2dSetFn(ctx,g,"__c2dScale",cbScale);
    c2dSetFn(ctx,g,"__c2dTranslate",cbTranslate);
    c2dSetFn(ctx,g,"__c2dRotate",cbRotate);
    c2dSetFn(ctx,g,"__c2dTransform",cbTransform);
    c2dSetFn(ctx,g,"__c2dSetTransform",cbSetTransform);
    c2dSetFn(ctx,g,"__c2dResetTransform",cbResetTransform);
    c2dSetFn(ctx,g,"__c2dMeasure",cbMeasure);
    c2dSetFn(ctx,g,"__c2dFillText",cbFillText);
    c2dSetFn(ctx,g,"__c2dFillTextGrad",cbFillTextGrad);
    c2dSetFn(ctx,g,"__c2dStrokeText",cbStrokeText);
    c2dSetFn(ctx,g,"__c2dFillRect",cbFillRect);
    c2dSetFn(ctx,g,"__c2dClearRect",cbClearRect);
    c2dSetFn(ctx,g,"__c2dGetData",cbGetData);
    c2dSetFn(ctx,g,"__c2dPutData",cbPutData);
    c2dSetFn(ctx,g,"__c2dToDataURL",cbToDataURL);
}
// 注册目录下 .ttf/.otf 字体，使 CTFontCreateWithName 能按 family 名找到
static void registerFonts(const char* dir){
    NSString* d=[NSString stringWithUTF8String:dir];
    NSArray* files=[[NSFileManager defaultManager] contentsOfDirectoryAtPath:d error:nil];
    for(NSString* f in files){
        NSString* ext=f.pathExtension.lowercaseString;
        if([ext isEqualToString:@"ttf"]||[ext isEqualToString:@"otf"]){
            NSURL* url=[NSURL fileURLWithPath:[d stringByAppendingPathComponent:f]];
            CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,kCTFontManagerScopeProcess,NULL);
            fprintf(stderr,"[c2d] 注册字体 %s\n",f.UTF8String);
        }
    }
}

// 注册单个字体文件并取其 PostScript 名(带缓存，重复注册幂等)。
static std::string registerFontFile(const char* filePath){
    static std::map<std::string,std::string> cache;
    std::string key=filePath; auto it=cache.find(key); if(it!=cache.end()) return it->second;
    std::string ps;
    NSString* p=[NSString stringWithUTF8String:filePath];
    if([[NSFileManager defaultManager] fileExistsAtPath:p]){
        NSURL* url=[NSURL fileURLWithPath:p];
        CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,kCTFontManagerScopeProcess,NULL);
        CFArrayRef descs=CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
        if(descs){
            if(CFArrayGetCount(descs)>0){
                CTFontDescriptorRef d=(CTFontDescriptorRef)CFArrayGetValueAtIndex(descs,0);
                CFStringRef nm=(CFStringRef)CTFontDescriptorCopyAttribute(d,kCTFontNameAttribute); // PostScript 名
                if(nm){ char b[256]; if(CFStringGetCString(nm,b,sizeof(b),kCFStringEncodingUTF8)) ps=b; CFRelease(nm); }
            }
            CFRelease(descs);
        }
    }
    cache[key]=ps; return ps;
}
// 建立游戏自定义 family 名 → 字体文件真实 PostScript 名 的别名。
static void registerFontAlias(const char* gameName, const char* filePath){
    std::string ps=registerFontFile(filePath);
    if(!ps.empty()){ gFontAlias[gameName]=ps; fprintf(stderr,"[c2d] 字体别名 %s → %s\n",gameName,ps.c_str()); }
    else fprintf(stderr,"[c2d] 字体别名失败(无PS名) %s (%s)\n",gameName,filePath);
}

// Project font aliases come exclusively from the prepared manifest on both Apple hosts.
static bool registerGameFonts(const char* dir, std::string& error){
    gFontAlias.clear();
    NSString* directory=[NSString stringWithUTF8String:dir];
    NSData* data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"manifest.json"]];
    if(!data){error="Project font manifest is missing";return false;}
    NSError* jsonError=nil;
    id manifest=[NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonError];
    if(![manifest isKindOfClass:[NSDictionary class]]){error=jsonError.localizedDescription.UTF8String ?: "Invalid project font manifest";return false;}
    for(NSString* family in manifest){
        id relative=manifest[family];
        if(![relative isKindOfClass:[NSString class]]){error="Invalid project font file";return false;}
        NSString* path=[[[directory stringByDeletingLastPathComponent] stringByAppendingPathComponent:relative] stringByStandardizingPath];
        NSString* prefix=[directory stringByAppendingString:@"/"];
        if(![path hasPrefix:prefix]){error="Project font escapes font directory";return false;}
        NSURL* url=[NSURL fileURLWithPath:path];CFErrorRef registrationError=nullptr;
        bool registered=CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,kCTFontManagerScopeProcess,&registrationError);
        if(!registered&&(!registrationError||CFErrorGetCode(registrationError)!=kCTFontManagerErrorAlreadyRegistered)){
            NSString* detail=registrationError?(__bridge_transfer NSString*)CFErrorCopyDescription(registrationError):@"Font registration failed";
            error=std::string(family.UTF8String)+": "+detail.UTF8String;if(registrationError)CFRelease(registrationError);return false;
        }
        if(registrationError)CFRelease(registrationError);
        std::string name=registerFontFile(path.UTF8String);
        if(name.empty()){error=std::string("Font PostScript name missing: ")+family.UTF8String;return false;}
        gFontAlias[family.UTF8String]=name;
    }
    return true;
}

} // namespace c2d
