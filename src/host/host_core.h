#pragma once
// host_core.h — Apple 两端(mac/iOS)共享的宿主核心:JS 绑定回调、fgui/spinex/内容/资源桥与它们的状态。
// 由 game_window.mm 与 ios_app.mm 中逐字相同的函数机械抽出(2026-09-02,见 docs/structure-plan-2026-09-02.html S-1)。
// 目前是 Objective-C++(NSString/NSData 贯穿);Android 经 quickjs shim 接入前需先去 Foundation 化。
#include "host_core_common.h"   // 三端共用部分:类型/状态/函数/公共绑定表
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#import <JavaScriptCore/JavaScriptCore.h>
#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>        // NSAttributedString 属性键(NSForegroundColorAttributeName 等)在 UIKit
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>
#else
#import <AppKit/AppKit.h>      // 同上,mac 在 AppKit
#include <GLES3/gl3.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>
#include "../cmdbuf/batcher.h"
#include "../cmdbuf/decoder.h"
#include "../cmdbuf/native_animation_frame.h"
#include "../cmdbuf/native_fgui.h"
#include "../cmdbuf/plain_text_layout_contract.h"
#include "../cmdbuf/richtext_layout_contract.h"
#include "../cmdbuf/richtext_token_layout.h"
#include "../cmdbuf/richtext_token_spacing.h"
#include "../cmdbuf/scenegraph.h"
#include "../cmdbuf/spinex.h"
#include "../common/native_environment.h"
#include "../common/sha256.h"
#include "../jsc_rooted_value_vector.h"
#include "../mac/canvas2d_jsc.h"
#include "../mac/native_resource_manager.h"

// ---- 平台提供(定义在各平台宿主) ----

// ---- 平台实现、核心调用的接口(定义在各平台宿主,签名以 mac 为准) ----
id hostPlatformColor(CGFloat r, CGFloat g, CGFloat b, CGFloat a);   // NSColor(sRGB) / UIColor
id hostPlatformClearColor();
bool hostPlatformWritePNG(CGImageRef image, NSString* path);
void callJsGlobal(const char* name, int argc, const JSValueRef* args);
bool callJsGlobalChecked(const char* name, int argc, const JSValueRef* args);
NSData* loadAsset(const char* p);
spinex::DataStore::Entry* spinexEnsureBuilt(NSString* key,int preparedHandle,std::string* outError);
NSDictionary<NSString*, NSData*>* zipParseAll(NSData* data);

// ---- 共享状态(定义在 host_core.mm)与共享类型,按原 game_window.mm 顺序 ----
extern macres::ResourceManager gNativeResources;
class NativeTextSystem {
    struct Entry {
        GLuint gl=0; uint32_t cid=0; std::string key;
        std::string richRequestKey;
        double richWidth=0,richHeight=0,richLineHeight=0;
        double richContentX=0,richContentY=0,richContentWidth=0,richContentHeight=0;
        double richTextContentX=0,richTextContentY=0,richTextContentWidth=0,richTextContentHeight=0;
        double richSurfaceOriginX=0,richSurfaceOriginY=0;
        double richAnchorWidth=0;
        // Unanchored composite quads: the CoreText surface followed by any FairyGUI inline
        // images. Empty means the rich label is still the ordinary one-sprite fast path.
        std::vector<sg::NativeQuad> richQuads;
    };
    std::unordered_map<int, Entry> nodes_;
    uint32_t nextCid_ = 8000000; // exactly representable in scenegraph Float32 and clear of WebGL ids
public:
    ~NativeTextSystem(){ clear(); }
    void clear(){ for(auto& kv:nodes_) release(kv.second); nodes_.clear(); }
    bool hasEntry(int node) const { return nodes_.find(node)!=nodes_.end(); }
    double richAnchorWidth(int node) const { auto it=nodes_.find(node);return it==nodes_.end()?0:it->second.richAnchorWidth; }
    void detach(int node){ auto it=nodes_.find(node);if(it==nodes_.end())return;release(it->second);nodes_.erase(it); }
    void clearFresh(int node){
        detach(node);if(node<=0||node>=sg::NODE_CAP||!gSGBuf)return;
        gSG.clearNativeQuads(node);float* s=gSGBuf+(size_t)node*sg::STRIDE;
        s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;s[sg::RX]=s[sg::RY]=s[sg::RW]=s[sg::RH]=0;
        s[sg::QUADCNT]=0;s[sg::PREMUL]=0;s[sg::TINT]=(float)0xFFFFFF;gSG.markBoundsDirty(node);
    }
    bool update(int node,const std::string& text,const std::string& font,const std::string& fill,
                const std::string& stroke,double strokeWidth,const std::string& shadow,double shadowBlur,
                double shadowX,double shadowY,const std::string& align,double padding,double resolution,
                double anchorX,double anchorY,double wrapWidth,double requestedLineHeight,
                double boxWidth,double boxHeight,const std::string& verticalAlign,double letterSpacing,double leading,
                const std::string& fillGradientColors,const std::string& fillGradientStops,int fillGradientType,
                double pixiAscent,double pixiDescent,
                double& width,double& height,double& lineHeight){
        if(node<=0||node>=sg::NODE_CAP||!gSGBuf)return false;
        float* s=gSGBuf+(size_t)node*sg::STRIDE;
        std::vector<std::string> lines;std::vector<double> lineWidths;
        double glyphTopOffset=0;
        layout(text,font,wrapWidth,requestedLineHeight,strokeWidth,letterSpacing,leading,pixiAscent,pixiDescent,
               lines,lineWidths,width,height,lineHeight,glyphTopOffset);
        double naturalWidth=width,naturalHeight=height;padding=std::max(0.0,padding);
        // ≙ H5 TextMetrics.measureText(pixi.v7.4.3:20348-20352):width=maxLine+stroke,dropShadow 开启时
        // **不论角度**再 +distance;canvas=ceil(width+2pad)。texture.orig(=pixi .width 与 fgui
        // UITextField.textWidth 的共同口径,:7863)=canvas−2pad。扩边全落在右/下,字形基准仍在内容域
        // (:21218 linePositionX=stroke/2+…)。此前 native 不扩不 ceil:RedTip 气泡数字("10"@26px)
        // .width=24.28≤25 逃过业务 changeScale(H5 27>25 缩 0.926),且 anchor(0.5) 位移少 shadow/2
        // +ceil 差→数字比 H5 偏右 ~2px;阴影(shadowY=2)还可能被下缘裁掉。
        const double shadowDist=(shadowX!=0||shadowY!=0)?std::hypot(shadowX,shadowY):0.0;
        const double naturalSurfaceWidth=std::ceil(width+shadowDist+padding*2);
        const double naturalSurfaceHeight=std::ceil(height+shadowDist+padding*2);
        width=std::max(naturalSurfaceWidth,boxWidth);
        height=std::max(naturalSurfaceHeight,boxHeight);
        if(std::getenv("NATIVE_TEXT_SURFACE_TRACE")){
            static int textTraceLeft=1024;if(textTraceLeft-->0)std::fprintf(stderr,"[native-text-surface] node=%d local=%.1f,%.1f text=%.80s font=%.80s natural=%.1fx%.1f surface=%.1fx%.1f line=%.1f wrap=%.1f box=%.1fx%.1f kern=%.1f leading=%.1f stroke=%.1f\n",
                node,s[sg::X],s[sg::Y],text.c_str(),font.c_str(),naturalWidth,naturalHeight,width,height,lineHeight,wrapWidth,boxWidth,boxHeight,letterSpacing,leading,strokeWidth);
        }
        if(text.empty()||width<=0||height<=0){detach(node);gSG.clearNativeQuads(node);s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;return true;}
        resolution=std::max(1.0,std::min(3.0,resolution));
        // Same guards as updateRich: unwrapped marquee/localised strings can blow past the GL
        // texture limit, and float→int on a huge width*resolution is UB before it even uploads.
        if(!std::isfinite(width)||!std::isfinite(height)||width>16384||height>16384)return false;
        int pw=std::max(1,(int)ceil(width*resolution)),ph=std::max(1,(int)ceil(height*resolution));
        GLint maxTexture=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTexture);
        if(pw>maxTexture||ph>maxTexture||(uint64_t)pw*(uint64_t)ph>64u*1024u*1024u)return false;
        // The entry is owned per scene node, but that node can be restyled in place.  Treat every
        // input which affects either raster pixels or the sprite descriptor as part of the cache
        // identity; equal pixel dimensions alone do not imply equal wrapping/alignment/shadows.
        std::string key=text;
        auto keyPart=[&](const std::string& value){key.push_back('\x1f');key+=value;};
        keyPart(font);keyPart(fill);keyPart(stroke);keyPart(shadow);keyPart(align);keyPart(verticalAlign);
        keyPart(fillGradientColors);keyPart(fillGradientStops);keyPart(std::to_string(fillGradientType));
        keyPart(std::to_string(pw));keyPart(std::to_string(ph));keyPart(std::to_string(strokeWidth));
        keyPart(std::to_string(shadowBlur));keyPart(std::to_string(shadowX));keyPart(std::to_string(shadowY));
        keyPart(std::to_string(padding));keyPart(std::to_string(resolution));keyPart(std::to_string(wrapWidth));
        keyPart(std::to_string(requestedLineHeight));
        keyPart(std::to_string(letterSpacing));keyPart(std::to_string(leading));
        keyPart(std::to_string(pixiAscent));keyPart(std::to_string(pixiDescent));
        keyPart(std::to_string(boxWidth));keyPart(std::to_string(boxHeight));keyPart(std::to_string(width));
        keyPart(std::to_string(height));keyPart(std::to_string(lineHeight));
        Entry& en=nodes_[node];
        // A node may switch between the ordinary and attributed text surfaces. Do not let an
        // earlier rich request masquerade as a hit after the ordinary texture replaced it.
        en.richRequestKey.clear();en.richQuads.clear();gSG.clearNativeQuads(node);
        if(en.key!=key){
            c2d::Canvas c;c2d::allocC(c,pw,ph);
            if(!c.buf||!c.cg){c2d::freeC(c);return false;}
            c.transform=CGAffineTransformMakeScale(resolution,resolution);
            const std::vector<double> gradient=c2d::textGradientStops(fillGradientColors,fillGradientStops);
            for(size_t li=0;li<lines.size();++li){const std::string& line=lines[li];
                double strokeInset=std::max(0.0,strokeWidth)*0.5;
                // 无 fgui 盒(动态文本)时 center/right 基准=内容域(≙ H5 :21218 以 maxLineWidth 定位,
                // shadow/ceil 扩边不参与)——用 surface 会把字形拉进扩边,墨迹中心偏移≠H5。
                // fgui 盒(boxWidth>0)保持 surface 域对齐(owner-box 契约面,不受 shadow 影响)。
                double x=padding+strokeInset;
                if(align=="center")x=boxWidth>0?width*0.5:padding+naturalWidth*0.5;
                else if(align=="right"||align=="end")x=boxWidth>0?width-padding-strokeInset:padding+naturalWidth-strokeInset;
                double top=padding;if(verticalAlign=="middle")top=std::max(padding,(height-naturalHeight)*0.5);else if(verticalAlign=="bottom")top=std::max(padding,height-padding-naturalHeight);
                double y=top+glyphTopOffset+li*lineHeight;
                if(strokeWidth>0&&stroke!="transparent"){
                    CGFloat sc[4];c2d::parseColor(stroke,sc);
                    c2d::drawText(c,line,x,y,font,sc,"top",align,true,strokeWidth,"round",10,"transparent",0,0,0,1,letterSpacing);
                }
                if(gradient.size()>=10){
                    const double gradientHeight=std::max(1.0,lineHeight-leading);
                    const double x0=fillGradientType==1?padding:width*.5,y0=fillGradientType==1?y+gradientHeight*.5:y;
                    const double x1=fillGradientType==1?std::max(padding,width-padding):width*.5,y1=fillGradientType==1?y+gradientHeight*.5:y+gradientHeight;
                    c2d::drawTextGradient(c,line,x,y,font,x0,y0,x1,y1,gradient,"top",align,1,shadow,shadowBlur,shadowX,shadowY,letterSpacing);
                }else{
                    CGFloat fc[4];c2d::parseColor(fill,fc);
                    c2d::drawText(c,line,x,y,font,fc,"top",align,false,0,"miter",10,shadow,shadowBlur,shadowX,shadowY,1,letterSpacing);
                }
            }
            if(std::getenv("NATIVE_TEXT_DUMP")&&(text=="$99.99"||text=="20,000"||text=="YOUR ESTIMATED PRIZE"||text.find("NO COUPONS")!=std::string::npos)){
                CGImageRef image=CGBitmapContextCreateImage(c.cg);if(image){NSString* path=[NSString stringWithFormat:@"/tmp/native_text_%d.png",node];hostPlatformWritePNG(image,path);CGImageRelease(image);std::fprintf(stderr,"[native-text-dump] node=%d text=%.80s path=%s\n",node,text.c_str(),path.UTF8String);}
            }
            GLint old=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&old);
            if(!en.gl){glGenTextures(1,&en.gl);en.cid=nextCid_++;gDec.registerExternalTexture(en.cid,en.gl);}
            glBindTexture(GL_TEXTURE_2D,en.gl);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,pw,ph,0,GL_RGBA,GL_UNSIGNED_BYTE,c.buf);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D,(GLuint)old);c2d::freeC(c);en.key=key;
        }
        s[sg::TYPE]=sg::T_SPRITE;s[sg::TEXID]=(float)en.cid;s[sg::RX]=(float)(-anchorX*width);s[sg::RY]=(float)(-anchorY*height);
        s[sg::RW]=(float)width;s[sg::RH]=(float)height;
        const float uv[8]={0,0,1,0,1,1,0,1};for(int i=0;i<8;i++)s[sg::U0+i]=uv[i];
        s[sg::TINT]=(float)0xFFFFFF;s[sg::PREMUL]=0;s[sg::RENDERABLE]=1;gSG.markBoundsDirty(node);return true;
    }
    // horizontalFrame(三宽度布局协议,docs/text-layout-contract-v2.md 第一部分):
    //   0=legacy(V1 调用,对齐基准=surface=max(natural,box) —— JS 端历史上恒传 box==wrap,
    //     故 natural≤wrap 时与 paragraph-box 等价,语义由 JS 猜测决定);
    //   1=owner-box(box=FGUI owner 实时宽,行严格在 owner 内对齐;超宽负 x 由 surface origin 表示,
    //     wrap 只管断行);
    //   2=paragraph-box(TaggedText 历史语义:行在 wrapWidth 内对齐,段落块贴左);
    //   3=natural(行在自然内容宽内对齐,box 应传 0)。
    //   4=tagged-content(段落坐标 + 内容大小的纹理,保留原点与独立 anchorWidth)。
    bool updateRich(int node,const std::string& markup,const std::string& font,const std::string& fill,
                    const std::string& stroke,double strokeWidth,const std::string& align,double resolution,
                    double anchorX,double anchorY,double wrapWidth,double requestedLineHeight,
                    double boxWidth,double boxHeight,const std::string& verticalAlign,
                    double& width,double& height,double& lineHeight,int horizontalFrame=0,bool breakWords=false,
                    double* contentWidthOut=nullptr,double* contentXOut=nullptr,
                    double* contentHeightOut=nullptr,double* contentYOut=nullptr,
                    double* textContentWidthOut=nullptr,double* textContentXOut=nullptr,
                    double* textContentHeightOut=nullptr,double* textContentYOut=nullptr,
                    double lineVerticalOffset=0,double verticalCenterHeight=0){
        if(node<=0||node>=sg::NODE_CAP||!gSGBuf)return false;
        float* s=gSGBuf+(size_t)node*sg::STRIDE;
        resolution=std::max(1.0,std::min(3.0,resolution));
        std::string requestKey=markup+'\x1f'+font+'\x1f'+fill+'\x1f'+stroke+'\x1f'+align+"|rich-request|"
            +std::to_string(strokeWidth)+'|'+std::to_string(resolution)+'|'+std::to_string(wrapWidth)+'|'
            +std::to_string(requestedLineHeight)+'|'+std::to_string(boxWidth)+'x'+std::to_string(boxHeight)+'|'+verticalAlign
            +"|hf"+std::to_string(horizontalFrame)+(breakWords?"|bw":"")
            +"|lineY"+std::to_string(lineVerticalOffset)+"|centerY"+std::to_string(verticalCenterHeight)
            +'|'+richImageSignature(node,markup);
        Entry& en=nodes_[node];
        if(en.gl&&en.richRequestKey==requestKey){
            width=en.richWidth;height=en.richHeight;lineHeight=en.richLineHeight;
            if(contentWidthOut)*contentWidthOut=en.richContentWidth;
            if(contentXOut)*contentXOut=en.richContentX;
            if(contentHeightOut)*contentHeightOut=en.richContentHeight;
            if(contentYOut)*contentYOut=en.richContentY;
            if(textContentWidthOut)*textContentWidthOut=en.richTextContentWidth;
            if(textContentXOut)*textContentXOut=en.richTextContentX;
            if(textContentHeightOut)*textContentHeightOut=en.richTextContentHeight;
            if(textContentYOut)*textContentYOut=en.richTextContentY;
            publishRich(node,en,s,width,height,anchorX,anchorY);return true;
        }
        NSString* raw=[[NSString alloc] initWithUTF8String:markup.c_str()];if(!raw)raw=@"";
        if(raw.length==0){detach(node);gSG.clearNativeQuads(node);s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;width=height=lineHeight=0;
            if(contentWidthOut)*contentWidthOut=0;if(contentXOut)*contentXOut=0;if(contentHeightOut)*contentHeightOut=0;if(contentYOut)*contentYOut=0;
            if(textContentWidthOut)*textContentWidthOut=0;if(textContentXOut)*textContentXOut=0;if(textContentHeightOut)*textContentHeightOut=0;if(textContentYOut)*textContentYOut=0;return true;}
        CTFontRef baseFont=c2d::makeFont(font);if(!baseFont)return false;CGFloat baseSize=CTFontGetSize(baseFont);
        NSMutableAttributedString* attr=richAttributed(node,markup,font,fill,baseSize,stroke,strokeWidth,verticalAlign);if(!attr){CFRelease(baseFont);return false;}
        // ---- token 级排版(修法候选 A,docs/text-layout-contract-v2.md):CoreText 只测量/绘制单 token
        // (无 kern 属性;token 内保留字体 pair kerning ≙ 浏览器 canvas,token 间无 ≙ H5 独立纹理)。
        // 断行/字距/对齐由 richtext_token_layout.h 按 H5 oracle 语义线性累计,负 kern 永不钳制——
        // CoreText 整行排版的 per-glyph max(0,advance+kern) 钳制正是"丢空格"根因。
        // 语义对数:tests/mac_coretext_kern_compare.js(--sim 全等;current 记录旧实现钳制行为)。----
        const double naturalLineHeight=std::ceil(CTFontGetAscent(baseFont)+CTFontGetDescent(baseFont)+CTFontGetLeading(baseFont));
        lineHeight=naturalLineHeight;
        if(requestedLineHeight>0)lineHeight=std::max(naturalLineHeight,requestedLineHeight);
        bool hasStroke=false;NSUInteger strokeIndex=0;
        while(strokeIndex<attr.length){NSRange range=NSMakeRange(strokeIndex,1);id value=[attr attribute:NSStrokeWidthAttributeName atIndex:strokeIndex effectiveRange:&range];if(value&&std::fabs([value doubleValue])>0.0001){hasStroke=true;break;}strokeIndex=range.length?NSMaxRange(range):strokeIndex+1;}
        NSString* full=attr.string;NSCharacterSet* wsSet=[NSCharacterSet whitespaceAndNewlineCharacterSet];
        struct TokenDraw{NSRange range;richtext::TokenInput input;NSDictionary* image;CTLineRef line;CTLineRef fillLine;};
        std::vector<TokenDraw> draws;
        {NSUInteger pos=0;
         while(pos<full.length){
            // longestEffectiveRange:词不能被同属性的相邻 append 段任意拆开(拆开=各 +1×letterSpacing)。
            NSRange runRange=NSMakeRange(pos,0);
            NSDictionary* runAttrs=[attr attributesAtIndex:pos longestEffectiveRange:&runRange inRange:NSMakeRange(pos,full.length-pos)];
            NSUInteger runEnd=NSMaxRange(runRange);if(runEnd<=pos)runEnd=pos+1;
            const double runKern=[runAttrs[richTokenKernAttribute()] doubleValue];
            const double runStrokeAdv=[runAttrs[richTokenStrokeAdvanceAttribute()] doubleValue];
            NSDictionary* imgInfo=runAttrs[richImageAttribute()];
            if([imgInfo isKindOfClass:[NSDictionary class]]){
                // 逐字符:内容相同的相邻 <img> 属性字典 isEqual 会被 longestEffective 合并成一个 range
                for(NSUInteger i=pos;i<runEnd;i++){
                    TokenDraw d{};d.range=NSMakeRange(i,1);d.image=imgInfo;
                    d.input.kind=richtext::TokenKind::Image;d.input.nakedWidth=[imgInfo[@"width"] doubleValue];
                    d.input.ascent=[imgInfo[@"ascent"] doubleValue];d.input.descent=[imgInfo[@"descent"] doubleValue];
                    d.input.naturalLineHeight=d.input.ascent+d.input.descent;
                    d.input.blockImage=[imgInfo[@"block"] boolValue];   // imgDisplay=block → 布局器强制换行
                    draws.push_back(d);
                }
                pos=runEnd;continue;
            }
            NSUInteger i=pos;
            while(i<runEnd){
                unichar c0=[full characterAtIndex:i];
                if([wsSet characterIsMember:c0]){
                    // 空白逐字符成 token(≙ TaggedText splitAroundWhitespace 的 /\s/ 单字符切分)
                    NSRange cluster=[full rangeOfComposedCharacterSequenceAtIndex:i];if(NSMaxRange(cluster)>runEnd)cluster.length=runEnd-cluster.location;
                    TokenDraw d{};d.range=cluster;
                    d.input.kind=c0=='\n'?richtext::TokenKind::Newline:richtext::TokenKind::Whitespace;
                    d.input.graphemeCount=1;d.input.letterSpacing=runKern;d.input.strokeAdvance=0;
                    draws.push_back(d);i=NSMaxRange(cluster);
                }else{
                    NSUInteger start=i;int graphemes=0;
                    while(i<runEnd&&![wsSet characterIsMember:[full characterAtIndex:i]]){
                        NSRange cl=[full rangeOfComposedCharacterSequenceAtIndex:i];
                        NSUInteger nx=NSMaxRange(cl);if(nx>runEnd)nx=runEnd;if(nx<=i)nx=i+1;
                        graphemes++;i=nx;
                    }
                    TokenDraw d{};d.range=NSMakeRange(start,i-start);
                    d.input.kind=richtext::TokenKind::Word;d.input.graphemeCount=std::max(1,graphemes);
                    d.input.letterSpacing=runKern;d.input.strokeAdvance=runStrokeAdv;
                    // default style 级 breakWords(业务唯一用法,zh-TW):相连 segment 词逐个成词、
                    // 可在 segment 边界断行。oracle 铁证:CJK 单 token 不因此获得词内断行(H5 同溢出)。
                    d.input.breakWords=breakWords;
                    draws.push_back(d);
                }
            }
            pos=runEnd;
         }}
        for(auto& d:draws){
            if(d.input.kind==richtext::TokenKind::Image)continue;
            if(d.input.kind==richtext::TokenKind::Newline){d.input.naturalLineHeight=0;continue;}
            NSAttributedString* sub=[attr attributedSubstringFromRange:d.range];
            d.line=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)sub);
            CGFloat la=0,ld=0,ll=0;d.input.nakedWidth=CTLineGetTypographicBounds(d.line,&la,&ld,&ll);
            d.input.ascent=la;d.input.descent=ld;d.input.naturalLineHeight=std::ceil(la+ld+ll);
        }
        auto releaseDraws=[&](){for(auto& d:draws){if(d.line)CFRelease(d.line);if(d.fillLine)CFRelease(d.fillLine);}};
        std::vector<richtext::TokenInput> inputs;inputs.reserve(draws.size());
        for(const auto& d:draws)inputs.push_back(d.input);
        // 断行仅由 wordWrapWidth 约束(FGUI box 只定 surface/对齐,不参与断行——历史契约保留)。
        richtext::TokenLayoutResult laid=richtext::layoutTokens(inputs,wrapWidth,std::numeric_limits<double>::infinity(),richtext::TokenAlign::Left,requestedLineHeight,naturalLineHeight,CTFontGetAscent(baseFont));
        const double surfaceOriginY=richtext::applyLineVerticalCorrection(laid,lineVerticalOffset,verticalCenterHeight);
        const double naturalW=std::ceil(laid.naturalWidth);
        width=naturalW;height=std::ceil(laid.naturalHeight);double naturalHeight=height;width=std::max(width,boxWidth);height=std::max(height,boxHeight);
        if(!std::isfinite(width)||!std::isfinite(height)||width<0||height<0||width>16384||height>16384){releaseDraws();CFRelease(baseFont);return false;}
        // frame policy 决议对齐基准(三宽度协议)。owner-box 必须严格使用 owner 宽，而不是
        // max(natural, owner) 的光栅宽；否则超宽行的 align=center 会退化为 x=0，正是带大图
        // 多语言提示向右溢出的根因。负 x 由下方 surface-origin 重基表示，不再改业务坐标。
        double alignWidth=width;
        if(horizontalFrame==1)alignWidth=boxWidth>0?boxWidth:naturalW;
        else if(horizontalFrame==2||horizontalFrame==4)alignWidth=wrapWidth>0?wrapWidth:naturalW;
        else if(horizontalFrame==3)alignWidth=naturalW;
        richtext::applyLineAlignOffsets(laid,alignWidth,align=="center"?richtext::TokenAlign::Center:((align=="right"||align=="end")?richtext::TokenAlign::Right:richtext::TokenAlign::Left));
        double surfaceOriginX=0;
        if(horizontalFrame==1)surfaceOriginX=richtext::rebaseNegativeLineOrigins(laid);
        else if(horizontalFrame==4)surfaceOriginX=richtext::compactLineOrigins(laid);
        // paragraph-box 的超宽行(首词不可断)会产生负行偏移;单 surface 无法向左溢出绘制,
        // 显式钳 0 并记录——禁止静默(H5 是独立 token 纹理可溢出,此为已知实现差,业务无此形态)。
        for(size_t li=0;horizontalFrame==2&&li<laid.lines.size();++li){
            if(laid.lines[li].x>=0)continue;
            static int overflowLogLeft=8;
            if(overflowLogLeft-->0)std::fprintf(stderr,"[richtext-frame] 行 %zu 偏移 %.1f<0(alignWidth=%.1f<行宽),钳 0 —— paragraph-box 超宽行\n",li,laid.lines[li].x,alignWidth);
            const double shift=-laid.lines[li].x;
            laid.lines[li].x=0;
            for(auto& tp:laid.tokens)if((size_t)tp.line==li)tp.x+=shift;
        }
        const richtext::HorizontalContentBounds contentBounds=richtext::horizontalContentBounds(laid,surfaceOriginX);
        double top=0;if(verticalAlign=="middle")top=std::max(0.0,(height-naturalHeight)*.5);else if(verticalAlign=="bottom")top=std::max(0.0,height-naturalHeight);
        const richtext::ContentBounds fullTokenBounds=richtext::tokenContentBounds(laid,inputs,surfaceOriginX,surfaceOriginY,top,false);
        const richtext::ContentBounds textTokenBounds=richtext::tokenContentBounds(laid,inputs,surfaceOriginX,surfaceOriginY,top,true);
        if(std::getenv("NATIVE_RICHTEXT_LINE_TRACE")){
            static int lineTraceLeft=256;
            if(lineTraceLeft-->0){
                std::fprintf(stderr,"[richtext-frame] node=%d hFrame=%d wrap=%.1f alignWidth=%.1f surface=%.1fx%.1f natural=%.1fx%.1f content=%.1f,%.1f %.1fx%.1f text=%.1f,%.1f %.1fx%.1f surfaceOrigin=%.1f,%.1f lineY=%.1f centerY=%.1f align=%s lines=%zu nodeX=%.1f nodeY=%.1f worldX=%.1f worldY=%.1f anchorX=%.2f RX=%.1f\n",
                    node,horizontalFrame,wrapWidth,alignWidth,width,height,
                    naturalW,naturalHeight,contentBounds.x,fullTokenBounds.y,contentBounds.width,fullTokenBounds.height,
                    textTokenBounds.x,textTokenBounds.y,textTokenBounds.width,textTokenBounds.height,
                    surfaceOriginX,surfaceOriginY,lineVerticalOffset,verticalCenterHeight,align.c_str(),laid.lines.size(),
                    s[sg::X],s[sg::Y],s[sg::WTX],s[sg::WTY],anchorX,surfaceOriginX-anchorX*width);
                for(size_t li=0;li<laid.lines.size();++li)
                    std::fprintf(stderr,"[richtext-frame]   line[%zu] x=%.2f width=%.2f center=%.2f\n",li,laid.lines[li].x,laid.lines[li].width,laid.lines[li].x+laid.lines[li].width/2);
            }
        }
        int pw=std::max(1,(int)std::ceil(width*resolution)),ph=std::max(1,(int)std::ceil(height*resolution));
        GLint maxTexture=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTexture);if(pw>maxTexture||ph>maxTexture||(uint64_t)pw*(uint64_t)ph>64u*1024u*1024u){releaseDraws();CFRelease(baseFont);return false;}
        // Keep the raster cache at least as discriminating as the request cache. Equal output
        // dimensions do not imply equal line breaks or vertical alignment.
        std::string key=requestKey+"|pixels|"+std::to_string(pw)+"x"+std::to_string(ph);
        if(en.key!=key){
            c2d::Canvas c;c2d::allocC(c,pw,ph);if(!c.buf||!c.cg){c2d::freeC(c);releaseDraws();CFRelease(baseFont);return false;}c.transform=CGAffineTransformMakeScale(resolution,resolution);
            CGContextSaveGState(c.cg);CGContextConcatCTM(c.cg,c.transform);CGContextTranslateCTM(c.cg,0,height);CGContextScaleCTM(c.cg,1,-1);
            CGContextSetTextMatrix(c.cg,CGAffineTransformIdentity);
            // token baseline:top-down = top + line.y + line.ascent;翻转坐标下 y = height - 该值
            auto baselineUp=[&](size_t k){const auto& tp=laid.tokens[k];const auto& ln=laid.lines[(size_t)tp.line];return height-(top+ln.y+ln.ascent);};
            std::vector<sg::NativeQuad> imageQuads;
            // CoreText's combined fill-and-stroke mode can discard the foreground colour for
            // attributed runs. Match Pixi's canvas renderer: stroke behind, fill above.
            // 渐变 token 保持整词绘制:drawLineTextGradients 按整词 run 几何建渐变,逐字符会把
            // 渐变切成字符级小段;渐变+letterSpacing 组合业务不存在,存在时 ls 堆积让位于渐变正确。
            auto tokenHasGradient=[&](NSRange r)->bool{
                __block bool has=false;
                [attr enumerateAttribute:richGradientAttribute() inRange:r options:0 usingBlock:^(id v,NSRange rr,BOOL* stop){(void)rr;if(v){has=true;*stop=YES;}}];
                return has;
            };
            auto perCharToken=[&](const TokenDraw& td)->bool{
                return td.input.letterSpacing!=0&&td.input.kind==richtext::TokenKind::Word&&!tokenHasGradient(td.range);
            };
            if(hasStroke)for(size_t k=0;k<draws.size();++k){
                if(!draws[k].line)continue;
                if(perCharToken(draws[k])){drawTokenLetterSpaced([attr attributedSubstringFromRange:draws[k].range],c.cg,laid.tokens[k].x,baselineUp(k),draws[k].input.letterSpacing);}
                else{CGContextSetTextPosition(c.cg,laid.tokens[k].x,baselineUp(k));CTLineDraw(draws[k].line,c.cg);}
            }
            for(size_t k=0;k<draws.size();++k){
                TokenDraw& d=draws[k];
                if(d.image){
                    float iw=[d.image[@"width"] floatValue],ih=[d.image[@"height"] floatValue],pa=[d.image[@"placementAscent"] floatValue];
                    if(iw>0&&ih>0){const auto& ln=laid.lines[(size_t)laid.tokens[k].line];float qx=(float)laid.tokens[k].x,qy=(float)(top+ln.y+ln.ascent-pa);
                        sg::NativeQuad q{};q[0]=[d.image[@"texture"] floatValue];q[1]=qx;q[2]=qy;q[3]=qx+iw;q[4]=qy;q[5]=qx+iw;q[6]=qy+ih;q[7]=qx;q[8]=qy+ih;q[9]=[d.image[@"u0"] floatValue];q[10]=[d.image[@"v0"] floatValue];q[11]=[d.image[@"u1"] floatValue];q[12]=[d.image[@"v1"] floatValue];q[13]=[d.image[@"u2"] floatValue];q[14]=[d.image[@"v2"] floatValue];q[15]=[d.image[@"u3"] floatValue];q[16]=[d.image[@"v3"] floatValue];q[17]=(float)0xFFFFFF;q[18]=1;q[19]=-1;q[20]=-1;q[22]=[d.image[@"premul"] floatValue];imageQuads.push_back(q);}
                    continue;
                }
                if(!d.line)continue;
                NSAttributedString* fillTok=nil;
                if(hasStroke){
                    NSMutableAttributedString* fillSub=[[attr attributedSubstringFromRange:d.range] mutableCopy];
                    NSRange all=NSMakeRange(0,fillSub.length);
                    [fillSub removeAttribute:NSStrokeWidthAttributeName range:all];[fillSub removeAttribute:NSStrokeColorAttributeName range:all];
                    d.fillLine=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)fillSub);
                    fillTok=fillSub;
                }
                CGPoint origin=CGPointMake(laid.tokens[k].x,baselineUp(k));
                if(perCharToken(d)){
                    drawTokenLetterSpaced(fillTok?fillTok:[attr attributedSubstringFromRange:d.range],c.cg,origin.x,origin.y,d.input.letterSpacing);
                }else{
                    CTLineRef drawLine=d.fillLine?d.fillLine:d.line;
                    CGContextSetTextPosition(c.cg,origin.x,origin.y);CTLineDraw(drawLine,c.cg);
                }
                c2d::drawLineTextGradients(d.line,origin,c.cg,(__bridge CFStringRef)richGradientAttribute());
            }
            CGContextRestoreGState(c.cg);
            GLint old=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&old);if(!en.gl){glGenTextures(1,&en.gl);if(!en.gl){c2d::freeC(c);releaseDraws();CFRelease(baseFont);return false;}en.cid=nextCid_++;gDec.registerExternalTexture(en.cid,en.gl);}glBindTexture(GL_TEXTURE_2D,en.gl);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,pw,ph,0,GL_RGBA,GL_UNSIGNED_BYTE,c.buf);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glBindTexture(GL_TEXTURE_2D,(GLuint)old);
            en.richQuads.clear();if(!imageQuads.empty()){sg::NativeQuad textQuad{};textQuad[0]=(float)en.cid;textQuad[1]=0;textQuad[2]=0;textQuad[3]=(float)width;textQuad[4]=0;textQuad[5]=(float)width;textQuad[6]=(float)height;textQuad[7]=0;textQuad[8]=(float)height;const float uv[8]={0,0,1,0,1,1,0,1};for(int i=0;i<8;i++)textQuad[9+i]=uv[i];textQuad[17]=(float)0xFFFFFF;textQuad[18]=1;textQuad[19]=-1;textQuad[20]=-1;textQuad[22]=0;en.richQuads.push_back(textQuad);en.richQuads.insert(en.richQuads.end(),imageQuads.begin(),imageQuads.end());}
            c2d::freeC(c);en.key=key;
        }
        releaseDraws();
        en.richRequestKey=requestKey;en.richWidth=width;en.richHeight=height;en.richLineHeight=lineHeight;
        en.richContentX=contentBounds.x;en.richContentY=fullTokenBounds.y;en.richContentWidth=contentBounds.width;en.richContentHeight=fullTokenBounds.height;
        en.richTextContentX=textTokenBounds.x;en.richTextContentY=textTokenBounds.y;en.richTextContentWidth=textTokenBounds.width;en.richTextContentHeight=textTokenBounds.height;
        en.richSurfaceOriginX=surfaceOriginX;en.richSurfaceOriginY=surfaceOriginY;
        en.richAnchorWidth=horizontalFrame==4?richtext::taggedAnchorWidth(contentBounds.x,contentBounds.width,
            align=="center"?richtext::TokenAlign::Center:((align=="right"||align=="end")?richtext::TokenAlign::Right:richtext::TokenAlign::Left)):width;
        if(contentWidthOut)*contentWidthOut=en.richContentWidth;
        if(contentXOut)*contentXOut=en.richContentX;
        if(contentHeightOut)*contentHeightOut=en.richContentHeight;
        if(contentYOut)*contentYOut=en.richContentY;
        if(textContentWidthOut)*textContentWidthOut=en.richTextContentWidth;
        if(textContentXOut)*textContentXOut=en.richTextContentX;
        if(textContentHeightOut)*textContentHeightOut=en.richTextContentHeight;
        if(textContentYOut)*textContentYOut=en.richTextContentY;
        CFRelease(baseFont);publishRich(node,en,s,width,height,anchorX,anchorY);return true;
    }
private:
    struct RichStyle { bool bold=false,italic=false,underline=false;double size=12,kern=0,strokeWidth=0;int gradientType=0;std::string color="#000000",strokeColor="transparent",gradientColors,gradientStops,valign="baseline"; };
    struct RichFrame { std::string name;RichStyle before; };
    struct RichAttachmentMetrics { CGFloat width=0,ascent=0,descent=0; };
    static NSString* richImageAttribute(){return @"NativeInlineFairyImage";}
    static NSString* richGradientAttribute(){return @"NativeFillGradient";}
    // token 级排版(修法候选 A)的布局输入:letterSpacing 与描边 advance 补偿不再进 kCTKern
    // (CoreText 会对负 kern 做 per-glyph max(0,·) 钳制 —— 丢空格根因;显式 0 又会禁用 pair
    // kerning),改存私有属性,由 updateRich 交给 richtext_token_layout.h 线性累计。
    static NSString* richTokenKernAttribute(){return @"NativeTokenKern";}
    // Pixi drawLetterSpacing 同构(pixi.v7.4.3:21239,experimentalLetterSpacing 默认关):
    // letterSpacing≠0 的词 token 逐 grapheme 绘制,advance_i=w(suffix_i)-w(suffix_{i+1})+ls
    // (后缀差分,pair kerning 随差分保留)。整词 CTLineDraw 的墨迹不含词内 ls——布局 advance 含、
    // 墨迹不含时,(n-1)×ls 全堆在词右侧,词间视觉间隙随前词字符数增大(Confirmation 弹窗
    // "空格比 H5 宽"根因:H5 gap 恒=空格 advance,Native 实测 11~20px 与词长正相关)。
    static void drawTokenLetterSpaced(NSAttributedString* tok,CGContextRef cg,double x,double y,double ls){
        NSString* str=tok.string;
        CTLineRef whole=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)tok);
        double prevW=whole?CTLineGetTypographicBounds(whole,nullptr,nullptr,nullptr):0;if(whole)CFRelease(whole);
        __block double pos=x;__block double prev=prevW;
        [str enumerateSubstringsInRange:NSMakeRange(0,str.length) options:NSStringEnumerationByComposedCharacterSequences
                             usingBlock:^(NSString* sub,NSRange subRange,NSRange enclosing,BOOL* stop){
            (void)sub;(void)enclosing;(void)stop;
            NSAttributedString* one=[tok attributedSubstringFromRange:subRange];
            CTLineRef cl=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)one);
            if(cl){CGContextSetTextPosition(cg,pos,y);CTLineDraw(cl,cg);CFRelease(cl);}
            NSUInteger nx=NSMaxRange(subRange);
            double cur=0;
            if(nx<str.length){
                NSAttributedString* suf=[tok attributedSubstringFromRange:NSMakeRange(nx,str.length-nx)];
                CTLineRef sl=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)suf);
                if(sl){cur=CTLineGetTypographicBounds(sl,nullptr,nullptr,nullptr);CFRelease(sl);}
            }
            pos+=prev-cur+ls;prev=cur;
        }];
    }
    static NSString* richTokenStrokeAdvanceAttribute(){return @"NativeTokenStrokeAdvance";}
    static void richAttachmentDealloc(void* ref){delete static_cast<RichAttachmentMetrics*>(ref);}
    static CGFloat richAttachmentAscent(void* ref){return static_cast<RichAttachmentMetrics*>(ref)->ascent;}
    static CGFloat richAttachmentDescent(void* ref){return static_cast<RichAttachmentMetrics*>(ref)->descent;}
    static CGFloat richAttachmentWidth(void* ref){return static_cast<RichAttachmentMetrics*>(ref)->width;}
    static std::string lower(std::string s){for(char& c:s)c=(char)std::tolower((unsigned char)c);return s;}
    static std::string trim(std::string s){size_t a=0,b=s.size();while(a<b&&std::isspace((unsigned char)s[a]))a++;while(b>a&&std::isspace((unsigned char)s[b-1]))b--;return s.substr(a,b-a);}
    static std::string tagAttr(const std::string& source,const std::string& wanted){
        std::string lo=lower(source),key=lower(wanted);size_t p=0;
        while((p=lo.find(key,p))!=std::string::npos){if(p>0&&(std::isalnum((unsigned char)lo[p-1])||lo[p-1]=='-'||lo[p-1]=='_')){p+=key.size();continue;}size_t q=p+key.size();while(q<lo.size()&&std::isspace((unsigned char)lo[q]))q++;if(q>=lo.size()||lo[q]!='='){p=q;continue;}q++;while(q<source.size()&&std::isspace((unsigned char)source[q]))q++;if(q>=source.size())return {};char quote=source[q];if(quote=='\''||quote=='"'){size_t e=source.find(quote,q+1);return e==std::string::npos?source.substr(q+1):source.substr(q+1,e-q-1);}size_t e=q;while(e<source.size()&&!std::isspace((unsigned char)source[e])&&source[e]!='>')e++;return source.substr(q,e-q);}
        return {};
    }
    static std::string richImageSignature(int node,const std::string& markup){
        // Include the live atlas descriptor in the raster cache. Rebinding/reinstalling a package
        // can keep the same ui:// URL while changing its texture id or frame.
        std::string signature,lo=lower(markup);size_t pos=0;
        while((pos=lo.find("<img",pos))!=std::string::npos){size_t gt=markup.find('>',pos+4);if(gt==std::string::npos)break;std::string body=markup.substr(pos+1,gt-pos-1),src=tagAttr(body,"src");if(src.empty())src=tagAttr(body,"imgSrc");nfgui::InlineImageDescriptor image;
            signature.push_back('|');signature+=src;if(richImageDescriptor(node,body,image)){signature+=':';signature+=std::to_string(image.textureId)+':'+std::to_string(image.width)+'x'+std::to_string(image.height)+':'+std::to_string(image.premul);for(float uv:image.uv){signature.push_back(',');signature+=std::to_string(uv);}}else signature+="!missing";pos=gt+1;}
        return signature;
    }
    static bool richImageDescriptor(int node,const std::string& body,nfgui::InlineImageDescriptor& image){
        std::string src=tagAttr(body,"src");if(src.empty())src=tagAttr(body,"imgSrc");if(!src.empty()&&gNativeFgui.inlineImageForNode(node,src,image))return true;
        int texture=std::atoi(tagAttr(body,"nativeTexture").c_str());float width=std::strtof(tagAttr(body,"nativeWidth").c_str(),nullptr),height=std::strtof(tagAttr(body,"nativeHeight").c_str(),nullptr);
        std::string encoded=tagAttr(body,"nativeUv");if(texture<=0||width<=0||height<=0||encoded.empty())return false;
        const char* cursor=encoded.c_str();char* end=nullptr;float uv[8];for(int i=0;i<8;i++){uv[i]=std::strtof(cursor,&end);if(end==cursor)return false;cursor=end;if(i<7){if(*cursor!=',')return false;cursor++;}}
        image={};image.textureId=texture;image.width=width;image.height=height;image.premul=std::strtof(tagAttr(body,"nativePremul").c_str(),nullptr);for(int i=0;i<8;i++)image.uv[i]=uv[i];return true;
    }
    static void publishRich(int node,const Entry& en,float* s,double width,double height,double anchorX,double anchorY){
        const float ox=(float)(en.richSurfaceOriginX-anchorX*en.richAnchorWidth),oy=(float)(en.richSurfaceOriginY-anchorY*height);
        if(!en.richQuads.empty()){std::vector<sg::NativeQuad> quads=en.richQuads;for(auto& q:quads)for(int p=0;p<4;p++){q[1+p*2]+=ox;q[2+p*2]+=oy;}gSG.setNativeQuads(node,std::move(quads));s[sg::TEXID]=0;}
        else{gSG.clearNativeQuads(node);s[sg::TYPE]=sg::T_SPRITE;s[sg::TEXID]=(float)en.cid;s[sg::RX]=ox;s[sg::RY]=oy;const float uv[8]={0,0,1,0,1,1,0,1};for(int i=0;i<8;i++)s[sg::U0+i]=uv[i];s[sg::PREMUL]=0;}
        s[sg::RW]=(float)width;s[sg::RH]=(float)height;s[sg::TINT]=(float)0xFFFFFF;s[sg::RENDERABLE]=1;gSG.markBoundsDirty(node);
    }
    static std::string entityDecode(const std::string& source){
        std::string out;out.reserve(source.size());for(size_t i=0;i<source.size();++i){if(source[i]!='&'){out.push_back(source[i]);continue;}size_t e=source.find(';',i+1);if(e==std::string::npos||e-i>12){out.push_back('&');continue;}std::string k=source.substr(i+1,e-i-1);if(k=="lt")out.push_back('<');else if(k=="gt")out.push_back('>');else if(k=="amp")out.push_back('&');else if(k=="quot")out.push_back('"');else if(k=="apos"||k=="#39")out.push_back('\'');else if(k=="nbsp")out.push_back(' ');else if(k.size()>1&&k[0]=='#'){unsigned long cp=std::strtoul(k.c_str()+(k[1]=='x'||k[1]=='X'?2:1),nullptr,(k[1]=='x'||k[1]=='X')?16:10);if(cp<0x80)out.push_back((char)cp);else if(cp<0x800){out.push_back((char)(0xC0|(cp>>6)));out.push_back((char)(0x80|(cp&63)));}else if(cp<0x10000){out.push_back((char)(0xE0|(cp>>12)));out.push_back((char)(0x80|((cp>>6)&63)));out.push_back((char)(0x80|(cp&63)));}else{out.push_back((char)(0xF0|(cp>>18)));out.push_back((char)(0x80|((cp>>12)&63)));out.push_back((char)(0x80|((cp>>6)&63)));out.push_back((char)(0x80|(cp&63)));}}else out+=source.substr(i,e-i+1);i=e;}return out;
    }
    static void cssStyle(const std::string& css,RichStyle& style){
        size_t p=0;while(p<=css.size()){
            size_t e=css.find(';',p);if(e==std::string::npos)e=css.size();std::string one=css.substr(p,e-p);size_t c=one.find(':');
            if(c!=std::string::npos){
                std::string k=lower(trim(one.substr(0,c))),v=trim(one.substr(c+1)),lv=lower(v);
                if(k=="color"){style.color=v;style.gradientColors.clear();style.gradientStops.clear();}
                else if(k=="font-size")style.size=std::max(1.0,std::strtod(v.c_str(),nullptr));
                else if(k=="font-weight"&&(lv=="bold"||std::atoi(v.c_str())>=600))style.bold=true;
                else if(k=="font-style"&&lv.find("italic")!=std::string::npos)style.italic=true;
                else if(k=="text-decoration"&&lv.find("underline")!=std::string::npos)style.underline=true;
                else if(k=="letter-spacing")style.kern=std::strtod(v.c_str(),nullptr);
                else if(k=="-native-valign"&&(lv=="top"||lv=="middle"||lv=="bottom"||lv=="baseline"))style.valign=lv;
                else if(k=="-native-stroke-color")style.strokeColor=v;
                else if(k=="-native-stroke-width")style.strokeWidth=std::max(0.0,std::strtod(v.c_str(),nullptr));
                else if(k=="-native-fill-gradient-colors"){style.gradientColors=lv=="none"?std::string():v;if(style.gradientColors.empty())style.gradientStops.clear();}
                else if(k=="-native-fill-gradient-stops")style.gradientStops=v;
                else if(k=="-native-fill-gradient-type")style.gradientType=std::atoi(v.c_str())==1?1:0;
            }
            if(e==css.size())break;p=e+1;
        }
    }
    static void appendRich(NSMutableAttributedString* out,const std::string& source,const RichStyle& style,const std::string& family){
        std::string decoded=entityDecode(source);if(decoded.empty())return;NSString* text=[[NSString alloc] initWithUTF8String:decoded.c_str()];if(!text)return;
        std::string font=(style.italic?"italic ":"")+(style.bold?std::string("bold "):std::string())+std::to_string(style.size)+"px "+family;CTFontRef ct=c2d::makeFont(font);CGFloat rgba[4];c2d::parseColor(style.color,rgba);
        const bool hasGradient=c2d::textGradientStops(style.gradientColors,style.gradientStops).size()>=10;
        id color=hasGradient?hostPlatformClearColor():hostPlatformColor(rgba[0],rgba[1],rgba[2],rgba[3]);
        NSMutableDictionary* attrs=[@{(__bridge NSString*)kCTFontAttributeName:(__bridge id)ct,NSForegroundColorAttributeName:color} mutableCopy];
        if(hasGradient){std::string descriptor=style.gradientColors+'\n'+style.gradientStops+'\n'+std::to_string(style.gradientType);attrs[richGradientAttribute()]=[[NSString alloc] initWithUTF8String:descriptor.c_str()];}
        if(style.underline)attrs[NSUnderlineStyleAttributeName]=@(NSUnderlineStyleSingle);bool visibleStroke=style.strokeWidth>0&&style.strokeColor!="transparent";if(visibleStroke){CGFloat sc[4];c2d::parseColor(style.strokeColor,sc);attrs[NSStrokeColorAttributeName]=hostPlatformColor(sc[0],sc[1],sc[2],sc[3]);attrs[NSStrokeWidthAttributeName]=@(100.0*std::abs(style.strokeWidth)/std::max(1.0,style.size));}
        // token 级排版:letterSpacing/描边补偿不进 kCTKern(钳制与 kern=0 语义,见属性名注释),
        // 存私有属性交给 updateRich 的布局器;token 内保留字体 pair kerning(≙ 浏览器 canvas)。
        attrs[richTokenKernAttribute()]=@(richtext::tokenInternalGapKern(style.kern));
        attrs[richTokenStrokeAdvanceAttribute()]=@(richtext::tokenStrokeAdvanceCorrection(style.strokeWidth,false,visibleStroke));
        NSMutableAttributedString* attributed=[[NSMutableAttributedString alloc] initWithString:text attributes:attrs];
        [out appendAttributedString:attributed];CFRelease(ct);
    }
    static bool appendRichImage(int node,NSMutableAttributedString* out,const std::string& body,const RichStyle& style,const std::string& family){
        nfgui::InlineImageDescriptor image;if(!richImageDescriptor(node,body,image))return false;
        double width=image.width,height=image.height;const double authoredWidth=std::strtod(tagAttr(body,"width").c_str(),nullptr),authoredHeight=std::strtod(tagAttr(body,"height").c_str(),nullptr);
        if(authoredWidth>0&&authoredHeight>0){width=authoredWidth;height=authoredHeight;}
        else if(authoredWidth>0){width=authoredWidth;height=image.height*(authoredWidth/std::max(1.0f,image.width));}
        else if(authoredHeight>0){height=authoredHeight;width=image.width*(authoredHeight/std::max(1.0f,image.height));}
        if(lower(tagAttr(body,"imgDisplay"))=="icon"){
            std::string font=(style.italic?"italic ":"")+(style.bold?std::string("bold "):std::string())+std::to_string(style.size)+"px "+family;CTFontRef ct=c2d::makeFont(font);
            double scale=0.8*CTFontGetAscent(ct)/std::max(1.0,height),iconScale=std::strtod(tagAttr(body,"iconScale").c_str(),nullptr);if(iconScale>0)scale*=iconScale;width*=scale;height*=scale;CFRelease(ct);
            double sx=std::strtod(tagAttr(body,"fontScaleWidth").c_str(),nullptr),sy=std::strtod(tagAttr(body,"fontScaleHeight").c_str(),nullptr);if(sx>0)width*=sx;if(sy>0)height*=sy;
            if(std::fabs(style.kern)>0.0001)width+=style.kern;
        }
        std::string metricFontName=(style.italic?"italic ":"")+(style.bold?std::string("bold "):std::string())+std::to_string(style.size)+"px "+family;
        CTFontRef metricFont=c2d::makeFont(metricFontName);if(!metricFont)return false;
        const auto aligned=richtext::inlineAttachmentMetrics(height,CTFontGetAscent(metricFont),CTFontGetDescent(metricFont),style.valign);CFRelease(metricFont);
        auto* metrics=new RichAttachmentMetrics();metrics->width=(CGFloat)std::max(0.0,width);metrics->ascent=(CGFloat)aligned.ascent;metrics->descent=(CGFloat)aligned.descent;
        CTRunDelegateCallbacks callbacks{};callbacks.version=kCTRunDelegateVersion1;callbacks.dealloc=richAttachmentDealloc;callbacks.getAscent=richAttachmentAscent;callbacks.getDescent=richAttachmentDescent;callbacks.getWidth=richAttachmentWidth;
        CTRunDelegateRef delegate=CTRunDelegateCreate(&callbacks,metrics);if(!delegate){delete metrics;return false;}
        NSDictionary* info=@{@"texture":@(image.textureId),@"width":@(metrics->width),@"height":@(height),@"placementAscent":@(aligned.placementAscent),@"ascent":@(aligned.ascent),@"descent":@(aligned.descent),@"block":@(lower(tagAttr(body,"imgDisplay"))=="block"),@"premul":@(image.premul),
            @"u0":@(image.uv[0]),@"v0":@(image.uv[1]),@"u1":@(image.uv[2]),@"v1":@(image.uv[3]),@"u2":@(image.uv[4]),@"v2":@(image.uv[5]),@"u3":@(image.uv[6]),@"v3":@(image.uv[7])};
        unichar marker=0xFFFC;NSString* value=[NSString stringWithCharacters:&marker length:1];NSMutableDictionary* attrs=[@{(__bridge NSString*)kCTRunDelegateAttributeName:(__bridge id)delegate,NSForegroundColorAttributeName:hostPlatformClearColor(),richImageAttribute():info} mutableCopy];
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:value attributes:attrs]];CFRelease(delegate);return true;
    }
    static NSMutableAttributedString* richAttributed(int node,const std::string& markup,const std::string& font,const std::string& fill,double baseSize,const std::string& stroke,double strokeWidth,const std::string& verticalAlign){
        std::string family=font;size_t px=lower(family).find("px ");if(px!=std::string::npos)family=trim(family.substr(px+3));RichStyle style;style.size=baseSize;style.color=fill;style.strokeColor=stroke;style.strokeWidth=std::max(0.0,strokeWidth);if(verticalAlign=="top"||verticalAlign=="middle"||verticalAlign=="bottom"||verticalAlign=="baseline")style.valign=verticalAlign;std::vector<RichFrame> stack;NSMutableAttributedString* out=[[NSMutableAttributedString alloc] init];size_t pos=0;
        while(pos<markup.size()){size_t lt=markup.find('<',pos);if(lt==std::string::npos){appendRich(out,markup.substr(pos),style,family);break;}if(lt>pos)appendRich(out,markup.substr(pos,lt-pos),style,family);size_t gt=markup.find('>',lt+1);if(gt==std::string::npos){appendRich(out,markup.substr(lt),style,family);break;}std::string body=trim(markup.substr(lt+1,gt-lt-1)),lo=lower(body);bool closing=!lo.empty()&&lo[0]=='/';if(closing)lo=trim(lo.substr(1));size_t sp=lo.find_first_of(" \t/");std::string name=sp==std::string::npos?lo:lo.substr(0,sp);
            if(closing){if(name=="p"&&out.length&&[[out string] characterAtIndex:out.length-1]!='\n')appendRich(out,"\n",style,family);for(size_t i=stack.size();i>0;--i)if(stack[i-1].name==name){style=stack[i-1].before;stack.resize(i-1);break;}}
            else if(name=="br"){appendRich(out,"\n",style,family);}else if(name=="img"){if(appendRichImage(node,out,body,style,family)){pos=gt+1;continue;}std::string alt=tagAttr(body,"alt");if(alt.empty())alt=tagAttr(body,"src");if(!alt.empty()){// Non-FairyGUI URLs retain the prior textual fallback.
                size_t slash=alt.find_last_of("/\\");if(slash!=std::string::npos)alt=alt.substr(slash+1);appendRich(out,alt,style,family);}
            }else if(name=="sup"||name=="sub"){RichStyle before=style;style.size=std::max(1.0,style.size*0.75);stack.push_back({name,before});}
            else{RichStyle before=style;if(name=="b"||name=="strong")style.bold=true;else if(name=="i"||name=="em")style.italic=true;else if(name=="u")style.underline=true;else if(name=="a"){style.underline=true;std::string color=tagAttr(body,"color");if(!color.empty()){style.color=color;style.gradientColors.clear();style.gradientStops.clear();}}else if(name=="font"){// FairyGUI C++ UBBParser emits <font color/size/face>.
                std::string color=tagAttr(body,"color"),size=tagAttr(body,"size"),face=tagAttr(body,"face");std::string styleAttr=tagAttr(body,"style");if(!styleAttr.empty())cssStyle(styleAttr,style);if(!color.empty()){style.color=color;style.gradientColors.clear();style.gradientStops.clear();}if(!size.empty())style.size=std::max(1.0,std::strtod(size.c_str(),nullptr));if(!face.empty()){/* face is applied via family override on append */} }else if(name=="span")cssStyle(tagAttr(body,"style"),style);else if((name=="p"||name=="div")&&out.length&&[[out string] characterAtIndex:out.length-1]!='\n')appendRich(out,"\n",style,family);stack.push_back({name,before});}
            pos=gt+1;
        }
        return out;
    }
    void layout(const std::string& text,const std::string& fontStr,double wrap,double requested,double strokeWidth,
                double letterSpacing,double leading,double pixiAscent,double pixiDescent,
                std::vector<std::string>& lines,std::vector<double>& widths,double& width,double& height,double& lineHeight,
                double& glyphTopOffset){
        CTFontRef font=c2d::makeFont(fontStr);
        const double coreAscent=CTFontGetAscent(font),coreDescent=CTFontGetDescent(font);
        const auto metrics=plaintext::verticalFontMetrics(coreAscent,coreDescent,pixiAscent,pixiDescent);
        const double fontHeight=metrics.height;
        const double stroke=std::max(0.0,strokeWidth);
        // Match PIXI.TextMetrics: an authored lineHeight controls baseline advance, but it must
        // never shrink the texture below the actual glyph plus its outside stroke.  The old
        // lineHeight * lineCount surface clipped multi-line labels whenever lineHeight < fontSize.
        const double baseLineHeight=requested>0?requested:plaintext::defaultLineHeight(metrics,stroke);
        lineHeight=baseLineHeight+leading;
        glyphTopOffset=plaintext::drawTopOffset(coreAscent,metrics,baseLineHeight,stroke,leading);
        width=0;
        NSString* all=[[NSString alloc] initWithUTF8String:text.c_str()];if(!all)all=@"";
        NSArray<NSString*>* paragraphs=[all componentsSeparatedByString:@"\n"];
        for(NSString* para in paragraphs){
            CFStringRef keys[2]={kCTFontAttributeName,kCTKernAttributeName};CFTypeRef vals[2]={font,nullptr};int attrCount=1;CFNumberRef kern=nullptr;
            if(std::fabs(letterSpacing)>0.0001){CGFloat value=(CGFloat)letterSpacing;kern=CFNumberCreate(NULL,kCFNumberCGFloatType,&value);vals[attrCount++]=kern;}
            CFDictionaryRef attrs=CFDictionaryCreate(NULL,(const void**)keys,(const void**)vals,attrCount,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            CFAttributedStringRef as=CFAttributedStringCreate(NULL,(__bridge CFStringRef)para,attrs);
            CTTypesetterRef ts=CTTypesetterCreateWithAttributedString(as);CFIndex len=para.length,pos=0;
            if(len==0){lines.emplace_back();widths.push_back(0);}
            while(pos<len){CFIndex take=wrap>0?CTTypesetterSuggestLineBreak(ts,pos,wrap):(len-pos);if(take<=0)take=1;
                CTLineRef line=CTTypesetterCreateLine(ts,CFRangeMake(pos,take));double lw=CTLineGetTypographicBounds(line,nullptr,nullptr,nullptr);
                NSString* part=[para substringWithRange:NSMakeRange((NSUInteger)pos,(NSUInteger)take)];
                lines.emplace_back(part.UTF8String?:"");widths.push_back(lw);width=std::max(width,lw);CFRelease(line);pos+=take;}
            CFRelease(ts);CFRelease(as);CFRelease(attrs);if(kern)CFRelease(kern);
        }
        if(lines.empty()){lines.emplace_back();widths.push_back(0);}
        width+=stroke;
        height=std::max(baseLineHeight,fontHeight+stroke*2)+leading+(lines.size()-1)*lineHeight;
        CFRelease(font);
    }
    void release(Entry& e){if(e.cid)gDec.unregisterExternalTexture(e.cid);if(e.gl)glDeleteTextures(1,&e.gl);e={};}
};
extern NativeTextSystem gNativeText;
extern int gTimerId;
extern NSMutableDictionary<NSNumber*, NSTimer*>* gTimers;
extern NSMutableDictionary<NSNumber*, NSValue*>* gTimerFns;
@interface NativeFguiXmlDelegate : NSObject <NSXMLParserDelegate>
@property(nonatomic,strong) NSMutableArray<NSMutableDictionary*>* stack;
@property(nonatomic,strong) NSMutableDictionary* root;
@end
@interface NativeFguiPreparsed : NSObject {
@public
    std::unordered_map<std::string,std::string> entries;
    std::unique_ptr<nfgui::Package> blueprint;
}
@property(nonatomic,assign) NSUInteger compressedBytes;
@property(nonatomic,assign) uint32_t compressedCrc;
@property(nonatomic,assign) NSUInteger entryCount;
@property(nonatomic,assign) double inflateMs;
@property(nonatomic,assign) double xmlMs;
@property(nonatomic,assign) double blueprintMs;
@property(nonatomic,strong) NSDictionary<NSNumber*,NSArray<NSDictionary*>*>* xmlTrees;
@end
extern NativeFguiPreparseCounters gFguiPreparseStats;
extern NSMutableDictionary<NSString*,NativeFguiPreparsed*>* gFguiPreparseCache;
extern NSMutableArray<NSString*>* gFguiPreparseLru;
extern NSMutableDictionary<NSNumber*,NSMutableArray<NSDictionary*>*>* gFguiXmlPrewarm;
extern std::unordered_map<std::string,std::vector<NativeFguiPreparseWaiter>> gFguiPreparseInflight;
@interface NativeSpinePrepared : NSObject
@property(nonatomic,assign) int handle;
@property(nonatomic,copy) NSString* cacheKey;
@property(nonatomic,copy) NSString* url;
@property(nonatomic,copy) NSString* key;
@property(nonatomic,copy) NSString* skeletonFilename;
@property(nonatomic,copy) NSString* skelFilename;   // 同包内的 .skel 二进制(可选;有则 spinex 优先用它)
@property(nonatomic,copy) NSString* atlasFilename;
@property(nonatomic,copy) NSString* atlasText;
@property(nonatomic,copy) NSString* version;
@property(nonatomic,strong) NSArray<NSString*>* imageNames;
@property(nonatomic,strong) NSDictionary<NSString*,NSData*>* entries;
@property(nonatomic,assign) NSUInteger compressedBytes;
@property(nonatomic,assign) uint32_t inputCrc;
@property(nonatomic,assign) NSUInteger manifestEntries;
@property(nonatomic,assign) double zipMs;
@property(nonatomic,assign) double atlasMs;
@property(nonatomic,assign) double jsonMs;
@property(nonatomic,assign) int activeUsers;   // 在途 lease:JS 加载链 pin/unpin;>0 禁止 LRU 逐出
@end
extern NSMutableDictionary<NSString*,NativeSpinePrepared*>* gSpinePreparedCache;
extern NSMutableDictionary<NSNumber*,NativeSpinePrepared*>* gSpinePreparedByHandle;
extern NSMutableArray<NSString*>* gSpinePreparedLru;
extern std::unordered_map<std::string,std::vector<NativeSpinePrepareWaiter>> gSpinePrepareInflight;
extern int gNextSpinePreparedHandle;
extern bool gSpinexHostInit;

// ---- 共享函数(定义在 host_core.mm) ----
bool shouldShowNativeDebugUI();
bool shouldKeepBundledDebugAsset(NSString* relativePath);
NSString* relOf(NSString* s);
NSString* remoteQuerySuffix(NSString* s);
JSValueRef cbNativeTextUpdate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
std::string richV2Str(JSContextRef ctx,JSObjectRef o,const char* k);
JSValueRef cbNativeTextUpdateV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeRichTextUpdate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeRichTextUpdateV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbReadFile(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbEvalScript(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
void unprotectTimerFn(NSNumber* key);
JSValueRef cbSetTimer(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbClearTimer(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
NSString* jsToNSString(JSContextRef ctx, JSValueRef v);
NSData* jsValueToData(JSContextRef ctx, JSValueRef v, JSValueRef* exc);
NSDictionary* parseNativeFguiXml(NSString* source);
NativeFguiPreparsed* parseNativeFguiArchive(NSData* input,bool warmXml,const std::string& packageKey);
void mergeNativeFguiXmlPrewarm(NativeFguiPreparsed* parsed);
NSDictionary* takeNativeFguiXmlPrewarm(NSString* source);
JSValueRef jsNSString(JSContextRef ctx, NSString* s);
JSObjectRef fguiXmlNodeToJs(JSContextRef ctx,NSDictionary* node,JSValueRef* exc,size_t* nodeCount=nullptr);
JSValueRef cbNativeFguiXmlParse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef nativeFguiEntriesToJs(JSContextRef ctx,NativeFguiPreparsed* parsed,NSString* resKey,bool cacheHit,JSValueRef* exc);
JSValueRef cbNativeFguiEntry(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*);
JSValueRef cbNativeFguiInflate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetStrings(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*);
JSValueRef cbNativeFguiManifest(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiItem(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiItemsByType(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSprite(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiBindAtlas(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiReserve(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiClearReservations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*);
JSValueRef cbNativeFguiCreate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
bool fguiJsStringArray(JSContextRef ctx,JSValueRef value,size_t exact, std::vector<std::string>& out,JSValueRef* exc);
JSValueRef cbNativeFguiChild(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiChildId(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetTextMutation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc, const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiCommitTextMutations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc, const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetController(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiCommitMutations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc, const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetBitmapText(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionPlay(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionStop(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionSetValue(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionSetTarget(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionSetDuration(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionUpdateRelations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionChangeRepeat(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionPlaying(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
dispatch_semaphore_t assetCpuSlots();
void notifyNativeFguiPreparse(const NativeFguiPreparseWaiter& waiter,bool ok,NativeFguiPreparsed* parsed);
void cacheNativeFguiPreparse(NSString* resKey,NativeFguiPreparsed* parsed);
JSValueRef cbNativeFguiPreparseStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*);
bool finishNativeImageAsync(JSGlobalContextRef requestContext, int token, uint32_t cid, int width, int height);
void deliverPublishedImage(JSGlobalContextRef requestContext, int token, uint32_t cid, int width, int height);
JSValueRef cbDecodeImageAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNativeResourceRetain(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNativeResourceAlive(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNativeResourceRelease(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNativeResourceReleaseAdopt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNativeResourceTransferred(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNativeResourceStats(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
NSString* nativeSpineCacheKey(NSString* url,NSString* key);
uint32_t nativeSpineInputCrc(NSData* data);
NSString* nativeSpineSkelVersion(NSData* data);
NativeSpinePrepared* prepareNativeSpineArchive(NSData* input,NSString* url,NSString* key,uint32_t inputCrc);
void touchNativeSpinePrepared(NativeSpinePrepared* parsed);
void spinePreparedEvictOverflow(void);
void cacheNativeSpinePrepared(NativeSpinePrepared* parsed);
void spineReleasePreparedRef(NativeSpinePrepared* p);
JSValueRef cbNativeSpinePin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbNativeSpineUnpin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
void notifyNativeSpinePreparse(const NativeSpinePrepareWaiter& waiter,bool ok,NativeSpinePrepared* parsed);
JSValueRef cbNativeSpineManifest(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbNativeSpineGet(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbNativeSpinePreparseStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*);
void spinexEnsureHost();
NativeSpinePrepared* spinexFindPrepared(NSString* key);
JSValueRef cbSpinexBuild(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexProbe(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexAttachmentInfo(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexUpdateAttachmentRegion(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetSlotAttachment(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbUnhandledRejection(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
NSString* persistentStorageDefaultsKey(NSString* key);
JSValueRef cbPersistentStorageGet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*);
JSValueRef cbPersistentStorageSet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*);
JSValueRef cbPersistentStorageRemove(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*);
JSValueRef cbPersistentStorageClear(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
void callXhrDone(int reqId, int status, NSString* statusText, NSString* respHeadersJson, NSData* body);
void injectNativeBootConfiguration(JSGlobalContextRef ctx, JSObjectRef global);   // seal 拦截开关注入(plist/env)

// 把共享回调注册到 JS 全局对象;各平台随后注册自己的平台绑定。
void hostCoreRegisterBindings(JSContextRef ctx, JSObjectRef global);   // = hostCoreRegisterCommonBindings + Apple 表

inline void nativeLogError(NSString* domain, NSString* code, NSString* detail, BOOL fatal) {
 fprintf(stderr,"[pixi_native] %s/%s fatal=%d %s\n",domain.UTF8String,code.UTF8String,(int)fatal,detail.UTF8String);
}
