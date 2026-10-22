// Native FairyGUI package compiler and scene-tree factory.
//
// The game ships the legacy FairyGUI format: a raw-deflate archive containing
// package.xml, sprites.bytes and one XML descriptor per component.  The browser
// runtime expands those descriptors into a large JavaScript object graph on the
// first UIPackage.createObject call.  This module keeps the immutable package
// blueprint and the instantiated display hierarchy in C++; JavaScript receives
// only lazy business-facing handles for objects it actually asks for.
#pragma once
#include "../common/native_diag_log.h"

#include "scenegraph.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nfgui {

struct XmlNode {
    std::string name;
    std::string text;
    std::unordered_map<std::string, std::string> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;

    const std::string& attr(const char* key) const {
        static const std::string empty;
        auto it = attrs.find(key);
        return it == attrs.end() ? empty : it->second;
    }
    const XmlNode* child(const char* wanted) const {
        for (const auto& c : children) if (c && c->name == wanted) return c.get();
        return nullptr;
    }
};

class XmlParser {
public:
    std::unique_ptr<XmlNode> parse(const std::string& source) {
        src_ = &source; pos_ = 0; error_ = false;
        while (skipMisc()) {}
        auto root = parseNode();
        if (error_) return nullptr;
        return root;
    }

private:
    const std::string* src_ = nullptr;
    size_t pos_ = 0;
    bool error_ = false;

    bool eof() const { return !src_ || pos_ >= src_->size(); }
    bool starts(const char* s) const {
        size_t n = std::char_traits<char>::length(s);
        return src_ && pos_ + n <= src_->size() && src_->compare(pos_, n, s) == 0;
    }
    void ws() { while (!eof() && std::isspace(static_cast<unsigned char>((*src_)[pos_]))) ++pos_; }
    bool skipUntil(const char* end) {
        size_t p = src_->find(end, pos_);
        if (p == std::string::npos) { error_ = true; return false; }
        pos_ = p + std::char_traits<char>::length(end); return true;
    }
    bool skipMisc() {
        ws();
        if (starts("<?")) return skipUntil("?>");
        if (starts("<!--")) return skipUntil("-->");
        if (starts("<!DOCTYPE")) return skipUntil(">");
        return false;
    }
    std::string name() {
        size_t begin = pos_;
        while (!eof()) {
            unsigned char c = static_cast<unsigned char>((*src_)[pos_]);
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.')) break;
            ++pos_;
        }
        if (begin == pos_) error_ = true;
        return src_->substr(begin, pos_ - begin);
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) { out.push_back(static_cast<char>(0xc0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 63))); }
        else if (cp <= 0xffff) { out.push_back(static_cast<char>(0xe0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63))); out.push_back(static_cast<char>(0x80 | (cp & 63))); }
        else { out.push_back(static_cast<char>(0xf0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63))); out.push_back(static_cast<char>(0x80 | (cp & 63))); }
    }
    static std::string entities(const std::string& in) {
        std::string out; out.reserve(in.size());
        for (size_t i = 0; i < in.size();) {
            if (in[i] != '&') { out.push_back(in[i++]); continue; }
            size_t semi = in.find(';', i + 1);
            if (semi == std::string::npos || semi - i > 16) { out.push_back(in[i++]); continue; }
            std::string e = in.substr(i + 1, semi - i - 1);
            if (e == "amp") out.push_back('&');
            else if (e == "lt") out.push_back('<');
            else if (e == "gt") out.push_back('>');
            else if (e == "quot") out.push_back('"');
            else if (e == "apos") out.push_back('\'');
            else if (!e.empty() && e[0] == '#') {
                char* end = nullptr; unsigned long cp = 0;
                if (e.size() > 2 && (e[1] == 'x' || e[1] == 'X')) cp = std::strtoul(e.c_str() + 2, &end, 16);
                else cp = std::strtoul(e.c_str() + 1, &end, 10);
                if (end && *end == 0 && cp <= 0x10ffff) appendUtf8(out, static_cast<uint32_t>(cp));
                else out.append(in, i, semi - i + 1);
            } else out.append(in, i, semi - i + 1);
            i = semi + 1;
        }
        return out;
    }
    std::string quoted() {
        if (eof() || ((*src_)[pos_] != '"' && (*src_)[pos_] != '\'')) { error_ = true; return {}; }
        char q = (*src_)[pos_++]; size_t begin = pos_;
        while (!eof() && (*src_)[pos_] != q) ++pos_;
        if (eof()) { error_ = true; return {}; }
        std::string value = entities(src_->substr(begin, pos_ - begin)); ++pos_; return value;
    }
    std::unique_ptr<XmlNode> parseNode() {
        while (skipMisc()) {}
        ws(); if (eof() || (*src_)[pos_] != '<' || starts("</")) { error_ = true; return nullptr; }
        ++pos_; auto node = std::make_unique<XmlNode>(); node->name = name();
        while (!error_ && !eof()) {
            ws();
            if (starts("/>")) { pos_ += 2; return node; }
            if ((*src_)[pos_] == '>') { ++pos_; break; }
            std::string key = name(); ws();
            if (eof() || (*src_)[pos_] != '=') { error_ = true; break; }
            ++pos_; ws(); node->attrs.emplace(std::move(key), quoted());
        }
        std::string body;
        while (!error_ && !eof()) {
            if (starts("</")) {
                pos_ += 2; std::string close = name(); ws();
                if (eof() || (*src_)[pos_] != '>' || close != node->name) { error_ = true; break; }
                ++pos_; node->text = entities(body); return node;
            }
            if (starts("<!--")) { if (!skipUntil("-->")) break; continue; }
            if ((*src_)[pos_] == '<') {
                auto c = parseNode(); if (!c) break; node->children.push_back(std::move(c));
            } else {
                size_t begin = pos_; while (!eof() && (*src_)[pos_] != '<') ++pos_;
                body.append(*src_, begin, pos_ - begin);
            }
        }
        error_ = true; return nullptr;
    }
};

inline float number(const std::string& s, float fallback = 0.0f) {
    if (s.empty()) return fallback;
    char* end = nullptr; float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}
inline int integer(const std::string& s, int fallback = 0) {
    if (s.empty()) return fallback;
    char* end = nullptr; long v = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? fallback : static_cast<int>(v);
}
inline std::vector<std::string> split(const std::string& s, char separator) {
    std::vector<std::string> out; size_t p = 0;
    while (p <= s.size()) { size_t q = s.find(separator, p); if (q == std::string::npos) q = s.size(); out.push_back(s.substr(p, q - p)); if (q == s.size()) break; p = q + 1; }
    return out;
}
inline void pairValue(const std::string& s, float& a, float& b, float da = 0.0f, float db = 0.0f) {
    a = da; b = db; if (s.empty()) return; auto p = split(s, ','); if (!p.empty()) a = number(p[0], da); if (p.size() > 1) b = number(p[1], db);
}

enum class ResourceKind : uint8_t { Image, MovieClip, Sound, Component, Font, Atlas, Unknown };
enum class ObjectKind : uint8_t { Component, Image, Text, RichText, InputText, Graph, Loader, List, Group, MovieClip, Unknown };

inline ResourceKind resourceKind(const std::string& tag) {
    if (tag == "image") return ResourceKind::Image;
    if (tag == "movieclip") return ResourceKind::MovieClip;
    if (tag == "sound") return ResourceKind::Sound;
    if (tag == "component") return ResourceKind::Component;
    if (tag == "font") return ResourceKind::Font;
    if (tag == "atlas") return ResourceKind::Atlas;
    return ResourceKind::Unknown;
}
// Keep the legacy FairyGUI enum value in the immutable package manifest.  ResourceKind is
// intentionally narrower (for native rendering), while the compatibility PackageItem facade
// still needs to distinguish e.g. SWF (1) from generic Misc (5).
inline int packageItemType(const std::string& tag) {
    if (tag == "image") return 0;
    if (tag == "swf") return 1;
    if (tag == "movieclip") return 2;
    if (tag == "sound") return 3;
    if (tag == "component") return 4;
    if (tag == "font") return 6;
    if (tag == "atlas") return 7;
    return 5;
}
inline ObjectKind objectKind(const std::string& tag) {
    if (tag == "component") return ObjectKind::Component;
    if (tag == "image") return ObjectKind::Image;
    if (tag == "text") return ObjectKind::Text;
    if (tag == "richtext") return ObjectKind::RichText;
    if (tag == "inputtext") return ObjectKind::InputText;
    if (tag == "graph") return ObjectKind::Graph;
    if (tag == "loader") return ObjectKind::Loader;
    if (tag == "list") return ObjectKind::List;
    if (tag == "group") return ObjectKind::Group;
    if (tag == "movieclip") return ObjectKind::MovieClip;
    return ObjectKind::Unknown;
}

struct Resource {
    struct MovieFrame { std::string spriteId; float x=0,y=0,width=0,height=0; int addDelay=0; };
    ResourceKind kind = ResourceKind::Unknown;
    int packageItemType = 5;
    std::string id, name, file;
    float width = 0, height = 0;
    bool scaleByTile = false;
    bool hasScale9 = false;
    float gridX = 0, gridY = 0, gridW = 0, gridH = 0;
    int tiledSlices = 0;
    int interval = 0, repeatDelay = 0;
    bool swing = false;
    std::vector<MovieFrame> movieFrames;
};
struct Sprite {
    std::string itemId, atlasId;
    float x = 0, y = 0, width = 0, height = 0;
    bool rotated = false;
};
struct AtlasBinding { int textureId = 0; float width = 0, height = 0, premul = 0; };
// Compact, renderer-ready description of a FairyGUI image URL. Rich text uses the same atlas
// binding as ordinary GImage/GLoader nodes, so inline images never need a GPU readback or a
// second decoded copy of the atlas.
struct InlineImageDescriptor {
    int textureId = 0;
    float width = 0, height = 0, premul = 0;
    float uv[8] = {0,0,1,0,1,1,0,1};
};
struct BitmapGlyph {
    std::string imageId;
    // `face=...` fonts store every glyph as a rectangle inside the font item's packed atlas
    // sprite. Legacy image fonts instead name a standalone PackageItem through `img=...`.
    // Retain both shapes so native rendering can follow UIPackage.loadFont without falling back
    // to an absent JavaScript display tree for atlas-backed labels.
    float x = 0, y = 0, width = 0, height = 0;
    float offsetX = 0, offsetY = 0;
    int channel = 0;
    // These are the values FairyGUI actually lays out with. Image fonts may omit xadvance; the
    // package compiler resolves that omission from common.xadvance or the image item's width.
    float advance = 0, lineHeight = 0;
};
struct BitmapFont {
    float size = 0, commonLineHeight = 0, commonAdvance = 0;
    bool resizable = false, colorable = false, ttf = false;
    std::unordered_map<uint32_t, BitmapGlyph> glyphs;
};
struct Component {
    std::string id, name, extension;
    std::unique_ptr<XmlNode> xml;
    bool hasUnsupportedStaticNode = false;
    std::string unsupportedReason;
};

struct Package {
    std::string key, id, name;
    // Reservations are built against an exact package/atlas revision.  A package can be
    // reinstalled or have its atlas table republished while an inactive component is waiting in
    // the pool, so claims must reject stale native nodes instead of displaying old textures.
    uint64_t generation = 0, resourceRevision = 0;
    // Compatibility descriptors stay native-owned.  JavaScript asks for the handful it actually
    // consumes (package.xml, sprites.bytes, a custom component XML, ...), instead of receiving a
    // complete archive-sized string map when UIPackage is registered.
    std::unordered_map<std::string, std::string> descriptors;
    std::unordered_map<std::string, Resource> resourcesById;
    // UIPackage exposes resources in package.xml order.  The native compiler otherwise stores
    // them in hash maps, so retain just the ids needed to rebuild the lightweight JS facade.
    std::vector<std::string> resourceOrder;
    std::unordered_map<std::string, std::string> resourceIdByName;
    std::unordered_map<std::string, Sprite> sprites;
    std::unordered_map<std::string, AtlasBinding> atlases;
    std::unordered_map<std::string, BitmapFont> fonts;
    std::unordered_set<std::string> requiredAtlases;
    bool resourcesPrepared = false;
    std::unordered_map<std::string, std::unique_ptr<Component>> components;
};

struct CompatibilityIssue {
    std::string packageName, componentName, reason;
};

struct ControllerActionDesc {
    std::string type, fromPage, toPage, objectId, controller, targetPage, transition;
    int repeat = 1;
    float delay = 0;
    bool stopOnExit = false;
};
struct ControllerState {
    std::string name;
    std::vector<std::string> pageIds, pageNames;
    std::vector<ControllerActionDesc> actions;
    int selectedIndex = 0;
    bool autoRadioGroupDepth = false;
};
struct GearDisplayState { std::string controller; std::vector<std::string> pages; };
struct GearXYState {
    std::string controller;
    std::vector<std::string> pages;
    std::vector<std::pair<float, float>> values;
    float defaultX = 0, defaultY = 0;
    bool present = false;
};
struct GearSizeValue { float width = 0, height = 0, scaleX = 1, scaleY = 1; };
struct GearSizeState {
    std::string controller;
    std::vector<std::string> pages;
    std::vector<GearSizeValue> values;
    GearSizeValue defaultValue;
    bool present = false;
};
struct GearLookValue { float alpha = 1, rotation = 0; bool touchable = true, grayed = false; };
struct GearLookState {
    std::string controller;
    std::vector<std::string> pages;
    std::vector<GearLookValue> values;
    GearLookValue defaultValue;
    bool present = false;
};
struct GearStringState {
    std::string controller, defaultValue;
    std::vector<std::string> pages, values;
    bool present = false;
};
struct RelationDesc {
    std::string target, sidePair;
    bool usePercent = false;
    // Relations declared on a component resource root are installed by constructInternal while
    // that root has no parent. FairyGUI therefore resolves their explicit target ids against the
    // root's own children. Relations declared on an instance descriptor are installed later and
    // resolve against the outer parent's children instead. Native builds the complete tree
    // eagerly, so retain this construction-time scope rather than inferring it from the final
    // parent pointer (which is already non-null for every nested component root).
    bool targetInOwner = false;
};
enum class TransitionKind : uint8_t {
    XY, Size, Scale, Pivot, Alpha, Rotation, Color, Animation, Visible, Sound,
    Nested, Shake, ColorFilter, Skew, Text, Icon, Unknown
};
struct TransitionItemDesc {
    TransitionKind kind = TransitionKind::Unknown;
    std::string targetId, value, startValue, endValue, label, label2, ease;
    float time = 0, duration = 0;
    int repeat = 0;
    bool tween = false, yoyo = false;
};
struct TransitionDesc {
    std::string name;
    int options = 0, autoPlayRepeat = 1;
    float autoPlayDelay = 0, maxTime = 0;
    bool autoPlay = false;
    std::vector<TransitionItemDesc> items;
};

enum class TextPrepareMode : uint8_t { AuthoredBox, FacadeNatural };
// GTextField.renderNow treats autoSize=shrink as a natural, unwrapped text surface and then
// scales that display surface to the authored width.  It does not require singleLine=true in the
// package.  Keep the decision and the post-raster placement shared by every native host so Mac,
// iOS and Android cannot silently drift back to authored-width word wrapping.
inline double plainTextWrapWidth(bool facadeNatural, bool singleLine, bool shrink,
                                 double authoredWidth) {
    return facadeNatural || singleLine || shrink ? 0.0 : std::max(0.0, authoredWidth);
}
struct PlainTextShrinkLayout {
    float x = 0, y = 0, width = 0, height = 0, scale = 1;
};
inline PlainTextShrinkLayout plainTextShrinkLayout(double surfaceWidth, double surfaceHeight,
                                                   double boxWidth, double boxHeight,
                                                   const std::string& align,
                                                   const std::string& verticalAlign) {
    PlainTextShrinkLayout out;
    if (!std::isfinite(surfaceWidth) || !std::isfinite(surfaceHeight) ||
        !std::isfinite(boxWidth) || !std::isfinite(boxHeight) ||
        surfaceWidth < 0 || surfaceHeight < 0 || boxWidth < 0 || boxHeight < 0)
        return out;
    // Match fgui.umd GTextField.shrinkTextField(): width is the sole fitting constraint.  The
    // vertical box only positions the already width-scaled surface through layoutAlign().
    const double scale = surfaceWidth > 0 && boxWidth > 0
        ? std::min(1.0, boxWidth / surfaceWidth) : 1.0;
    const double drawWidth = surfaceWidth * scale;
    const double drawHeight = surfaceHeight * scale;
    const double dx = std::max(0.0, boxWidth - drawWidth);
    // Horizontal shrink owns the font scale; authored height remains a vertical alignment frame.
    // A hard-localised line break can make the natural block taller than that frame. For
    // middle/bottom alignment retain the signed gap so the block expands around its authored
    // anchor instead of growing only downward into an enclosing ScrollPane clip.
    const double dy = boxHeight - drawHeight;
    if (align == "center") out.x = static_cast<float>(std::floor(dx * 0.5));
    else if (align == "right" || align == "end") out.x = static_cast<float>(std::floor(dx));
    if (verticalAlign == "middle") out.y = static_cast<float>(std::floor(dy * 0.5));
    else if (verticalAlign == "bottom") out.y = static_cast<float>(std::floor(dy));
    out.width = static_cast<float>(drawWidth);
    out.height = static_cast<float>(drawHeight);
    out.scale = static_cast<float>(scale);
    return out;
}
enum MutationCapability : uint32_t {
    MutationNone = 0,
    // No controller gear or authored timeline can overwrite this text between a JS setter and a
    // transaction commit. A lightweight facade may therefore keep a semantic shadow and defer it.
    MutationTextFinal = 1u << 0,
    // In addition, the text has authored fixed-box metrics (`autoSize=none`). Future generic thin
    // facades can expose width/height without synchronously rasterising the text.
    MutationTextFixedBox = 1u << 1,
};

// Mac callback and the closed JavaScript hot-store binding consumer; nested names are expressed
// as semantic paths so duplicate leaf names (final_txt/spine_box) cannot be confused.
struct TextMutationWrite {
    int instanceId = 0, nativeId = 0, boldMode = 0;
    std::string text;
};
enum class MutationKind : int32_t { Controller = 1, Size = 2, Reparent = 3, Position = 4, DisplayPosition = 5, Scale = 6, Alpha = 7, Pivot = 8, DisplayScale = 9, Rotation = 10, DisplayRotation = 11 };
struct MutationWrite {
    MutationKind kind = MutationKind::Controller;
    int instanceId = 0, targetId = 0;
    int value0 = 0, value1 = 0, value2 = 0;
    float number0 = 0, number1 = 0;
    std::string name;
};

// ---- 精确通道配置（exact channel）：C++ 侧包名/组件名/批量数唯一允许出现的地方 ----
// 与 game/host/native_fgui.js 的 EXACT_CHANNEL 一一对应，两侧必须一致。checkout 批量出库、
// 融合提交、绑定表、预约补货这些机制与具体商店无关；适用于哪个包/组件由此声明，下面的
// 给其它组件开精确通道时扩展这里与对应绑定表，不要在函数体里散落字符串比对。
// ---- fgui facade 版本化缓存协议(docs/fgui-facade-commit-protocol.md):C++ 写漏斗 ----
// 每个 facade 可见维度的变更都必须递增 facadeSeq 并置脏位:带 graph 的写点用 FacadeSeqTracker::mark 立即落槽;
// 不带 graph 的写点(InstanceNode::touchFacade / FacadeSeqTracker::note)先排队,由最外层 FacadeCommitScope 收敛时
// 统一落槽并发布 instanceCommitSeq[root]。JS 单线程:同一入口内"先后落槽"与"立即落槽"不可区分。
// scope 之外的写入(unscoped)是契约违规:计数经 __nativeFguiRuntimeStats 暴露,单测与运行时审计都要求为 0。
// S1 阶段 JS 仍走写后全量推,这些槽只被覆盖审计([fgui-seq-audit])读取;S2 起 JS 读点据此判新鲜度。
enum FacadeDirty : uint32_t {
    FD_GEOMETRY = 1u << 0, FD_LOOK = 1u << 1, FD_ATTRS = 1u << 2, FD_CONTROLLER = 1u << 3,
    FD_VISIBILITY = 1u << 4, FD_PARENT = 1u << 5, FD_CHILDREN = 1u << 6, FD_DERIVED_SIZE = 1u << 7, FD_TEXT = 1u << 8,
};
struct FacadeSeqTracker {
    // 被触达节点(publish 时按 root 归并);单线程(JS 线程)使用。static 成员:writeNode 等投影点是 static 函数。
    static inline std::vector<int> touched{};
    static inline std::vector<uint32_t> touchedBits{};   // 与 touched 平行:每次标记的位(写传播计数按几何位筛)
    static inline std::vector<std::pair<int, uint32_t>> pending{};   // 无 graph 写点排队,scope 收敛时落槽
    static inline uint64_t marks = 0, commits = 0, unscoped = 0, deferred = 0, orphans = 0, droppedDead = 0;
    static inline int scopeDepth = 0;
    static void apply(sg::SceneGraph& graph, int nativeId, uint32_t bits) {
        if (!graph.buf || nativeId <= 0 || nativeId >= sg::NODE_CAP) return;
        graph.u32(sg::FGUI_SEQ_BASE + nativeId) += 1u;
        graph.u32(sg::FGUI_DIRTY_BASE + nativeId) |= bits;
        touched.push_back(nativeId); touchedBits.push_back(bits); ++marks;
    }
    static void mark(sg::SceneGraph& graph, int nativeId, uint32_t bits) {
        if (scopeDepth == 0) ++unscoped;
        apply(graph, nativeId, bits);
    }
    static void note(int nativeId, uint32_t bits) {
        if (nativeId <= 0 || nativeId >= sg::NODE_CAP) return;
        if (scopeDepth == 0) ++unscoped;
        pending.emplace_back(nativeId, bits); ++deferred;
    }
    static void markChildren(sg::SceneGraph& graph, int parentId) {
        if (!graph.buf || parentId <= 0 || parentId >= sg::NODE_CAP) return;
        graph.u32(sg::FGUI_CHILDSEQ_BASE + parentId) += 1u;
        mark(graph, parentId, FD_CHILDREN);
    }
};

struct InstanceNode {
    int nativeId = 0;
    int parentNativeId = 0;
    // FairyGUI GGroup is a semantic sibling marker, not a display parent.  The XML `group`
    // attribute therefore cannot be represented by SceneGraph parenting: group visibility has
    // to be folded into every member's effective visibility while the authored display tree stays
    // untouched.  Resolve the sibling id once per instance and cache only the derived hidden bit.
    int groupNativeId = 0;
    // FairyGUI keeps semantic ownership on GComponent while overflow/margin routes painting
    // through an internal content container.  Keep that display edge separately so cloning,
    // clipping and lazy semantic handles never need a JavaScript children tree.
    int displayParentNativeId = 0;
    int displayContainerId = 0;
    int clipMaskId = 0;
    int ownerComponentId = 0;
    ObjectKind kind = ObjectKind::Unknown;
    std::string id, name, packageName, sourceId, extension;
    float x = 0, y = 0, width = 0, height = 0;
    // GObject.x/y and displayObject.position are not interchangeable state. In the browser the
    // latter changes only when handleXYChanged() runs; scale/size/alpha publications with a zero
    // pivot deliberately leave it untouched. GImage additionally adjusts that display position
    // for a unit negative scale. Keep the last browser-observable position for image nodes instead
    // of trying to reconstruct call history from the final semantic fields in writeNode().
    float imageDisplayX = 0, imageDisplayY = 0;
    bool imageDisplayPositionValid = false;
    // The shipped GImage constructor leaves $flip undefined. Its loose `$flip != None` check is
    // therefore true until an explicit flip attribute is assigned. -1 preserves that third state;
    // 0..3 match FairyGUI's FlipType enum.
    int imageFlipState = -1;
    bool pixelSnapping = false;
    // FairyGUI keeps three distinct size domains. source* is the immutable PackageItem/component
    // resource size, init* is the size applied by the containing instance descriptor, and
    // width/height are live layout. Native constructs the final display tree eagerly but exposes
    // semantic facades lazily, so collapsing these domains makes late extension wiring observe a
    // resized component with an authored-size child (ProgressBar/Slider then compute a negative
    // bar delta and overflow). Preserve the same construction-time facts as GObject does.
    float sourceWidth = 0, sourceHeight = 0, initWidth = 0, initHeight = 0;
    // Pixi permits business code to move a GObject's raw displayObject without changing the
    // owning GObject's semantic x/y (temporary Spine slots and guide layers rely on this). Keep
    // that display-only delta separate from FairyGUI layout so relations/reparenting cannot turn
    // a transient visual move into authoritative semantic geometry.
    float directDisplayOffsetX = 0, directDisplayOffsetY = 0;
    // Raw Pixi displayObject.scale mutations are independent from GObject's semantic scale.
    // Keep them as a display-layer override so an unrelated FairyGUI publication (position,
    // visibility, relation, controller) cannot restore the authored scale in the same frame.
    float directDisplayScaleX = 1, directDisplayScaleY = 1;
    bool directDisplayScaleOverride = false;
    // Pixi's displayObject.rotation is radians and is independent from FairyGUI's semantic
    // degree-valued GObject.rotation. Retain that raw layer across complete native publications.
    float directDisplayRotation = 0;
    bool directDisplayRotationOverride = false;
    float pivotX = 0, pivotY = 0, scaleX = 1, scaleY = 1, rotation = 0, alpha = 1;
    float skewX = 0, skewY = 0, shakeX = 0, shakeY = 0;
    bool pivotAsAnchor = false, visible = true, displayVisible = true, touchable = true, displayMask = false, authoredListItem = false, internalDisplayNode = false;
    // During XML construction Native owns EVENTMODE. Once the authored controller state is
    // complete, the materialised Pixi/FGUI facade owns it: position, size and transition writes
    // must not turn an already-interactive button back into `none`. A runtime GearLook touchable
    // change is the one Native operation that deliberately republishes the mode.
    bool runtimeEventModeOwned = false;
    // GObject.grayed: routed through the shared per-node ColorMatrix filter slot, which the
    // batcher inherits down the subtree exactly like a Pixi container filter.
    bool grayed = false;
    // gearDisplay 的页隐藏状态。必须持久在节点上并由 writeNode 合成:transition/resize 等任何
    // 后续 writeNode 若只看 visible,会把控制器隐藏中的节点重新置为可见(页外元素闪现)。
    bool gearHidden = false;
    bool groupHidden = false;
    // OPTION_IGNORE_DISPLAY_CONTROLLER is reference counted because independent or nested
    // transitions may temporarily expose the same gear-hidden target at the same time.
    int transitionDisplayLocks = 0;
    int movieFrame = 0, movieRepeated = 0;
    bool movieReversed = false;
    double movieLastMs = 0, movieElapsedMs = 0;
    float nativeTextWidth = 0, nativeTextHeight = 0, nativeTextLineHeight = 0;
    bool nativeTextMetricsValid = false, nativeTextFacadeNatural = false;
    uint32_t mutationCaps = MutationNone;
    // Once a materialised NativeTextProxy has painted this slot, its CoreText texture is newer
    // than the package XML attributes. Pure geometry changes must preserve that surface instead
    // of asking refreshVisual() to repaint the authored value over business text.
    bool nativeTextFacadeOwned = false;
    // True only after the authored/default controller state has been sealed and until the first
    // runtime mutation. Exact JS facade schemas may be shared across hot-store instances only
    // while this bit remains set; checking one byte is substantially cheaper than hashing every
    // exported attribute/controller/transition on each lazy child lookup.
    bool facadeSchemaPristine = false;
    float bitmapTextWidth = 0, bitmapTextHeight = 0;
    uint32_t bitmapGlyphCount = 0;
    bool bitmapMetricsTrusted = false;
    // GTextField.$offset.y is a display-only bitmap origin, not GObject semantic y. A finite value
    // comes from the materialised FairyGUI facade and must be applied inside glyph-local space.
    // NaN keeps the package-only/native blueprint path on its existing lifecycle approximation.
    float bitmapLocalOffsetY = std::numeric_limits<float>::quiet_NaN();
    // The shipped H5 bitmap renderer skips layoutAlign during its initial render, but a later
    // real setSize/relation/controller resize does call layoutAlign. Preserve that lifecycle
    // distinction instead of either centering every image-font label or disabling vAlign for all
    // of them. Text-only updates retain the current state, matching renderWithBitmapFont.
    bool bitmapVerticalAlignActivated = false;
    std::unordered_map<std::string, std::string> attrs;
    std::vector<RelationDesc> relations;
    std::vector<int> children;
    std::vector<ControllerState> controllers;
    GearDisplayState displayGear;
    GearXYState xyGear;
    GearSizeState sizeGear;
    GearLookState lookGear;
    GearStringState iconGear;
    GearStringState textGear;
    GearStringState colorGear;
    GearStringState animationGear;
    GearStringState fontSizeGear;
    std::vector<TransitionDesc> transitions;
    std::unordered_map<std::string, std::vector<std::string>> transitionOverrides;
    // facade 可见状态变化的节点级漏斗:置 schema 非 pristine 并排队版本递增(无需 graph,scope 收敛时落槽)。
    void touchFacade(uint32_t bits) { facadeSchemaPristine = false; FacadeSeqTracker::note(nativeId, bits); }
};

struct Instance {
    struct PackageDependency { std::string name; uint64_t generation = 0, resourceRevision = 0; };
    int id = 0, root = 0;
    bool compatible = true, checkoutOrigin = false;
    uint64_t packageGeneration = 0, resourceRevision = 0, translationRevision = 0;
    double reservedAtMs = 0;
    uint64_t reservedAtFrame = 0;
    std::string packageName, componentName, fallbackReason, reservationPolicyKey;
    std::vector<std::unique_ptr<InstanceNode>> nodes;
    std::unordered_map<int, InstanceNode*> byNativeId;
    // FGUI transition targets are authored by object id within their owning component scope.
    // Build that binding once; scanning every direct child for every timeline item was a
    // permanent O(active transitions * items * children) cost.
    std::unordered_map<int, std::unordered_map<std::string, InstanceNode*>> byOwnerAndId;
    std::vector<PackageDependency> packageDependencies;
};

struct ActiveTransition {
    int token = 0, instanceId = 0, componentId = 0, times = 1;
    std::string name;
    double startedMs = 0;
    double lastElapsed = -0.000001;
    float ownerBaseX = 0, ownerBaseY = 0;
    bool reversed = false;
    std::vector<int> displayLocks;
};

struct TransitionSignal { int token = 0, componentId = 0; std::string transitionName, label; };
struct TransitionVisibilitySignal { int instanceId = 0, nativeId = 0; bool visible = true; };
// Native timelines own the display mutation, but a materialised FairyGUI facade still caches the
// same semantic values in fields such as $alpha/$scaleX/$x.  If those shadows are not advanced
// with the timeline, a later business reset to the authored value is discarded by GObject as a
// no-op while the Native node remains at the transition's final value.  Keep this protocol generic
// across every non-visibility property that a Transition can mutate; visibility retains its
// separate signal because applying it also has to run GComponent.childStateChanged in JavaScript.
enum class TransitionPropertyKind : int32_t {
    XY = 1, Size = 2, Scale = 3, Pivot = 4, Alpha = 5, Rotation = 6, Skew = 7,
    Color = 8, Animation = 9, Text = 10, Icon = 11,
    Touchable = 12, Grayed = 13, StrokeColor = 14, FontSize = 15,
    // GearDisplay owns GObject.$internalVisible, not GObject.$visible. Display locks can
    // temporarily reconnect a page-hidden object; group visibility remains a separate ancestor
    // term in finalVisible and must never be folded into this signal.
    InternalVisible = 16,
};
struct TransitionPropertySignal {
    int instanceId = 0, nativeId = 0;
    TransitionPropertyKind kind = TransitionPropertyKind::XY;
    float value0 = 0, value1 = 0;
    std::string text;
};

struct ScheduledTask {
    int token = 0, repeat = 0;
    double delayMs = 0, dueMs = 0;
    uint64_t order = 0;
};

struct CoverageStats {
    size_t nodes = 0;
    size_t nonEmptyTextNodes = 0;
    size_t bitmapTextNodes = 0;
    size_t textFailures = 0;
    size_t bitmapTextFailures = 0;
    std::vector<std::string> failureSamples;
};

struct TemplateStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t clonedNodes = 0;
    double cloneMs = 0;
};

struct ReservationStats {
    uint64_t built = 0, claimed = 0, misses = 0, staleDiscarded = 0, failed = 0, blocked = 0;
    uint64_t builtNodes = 0, invalidated = 0, invalidatedNodes = 0;
    uint64_t refillQueued = 0, refillBuilt = 0, refillBlocked = 0, refillCanceled = 0, refillFrames = 0;
    double buildMs = 0, claimMs = 0, refillBuildMs = 0, refillMaxMs = 0;
};

struct CheckoutStats {
    uint64_t prepared = 0, promoted = 0, canceled = 0;
};

struct TextOwnershipStats {
    uint64_t published = 0, preservedRefreshes = 0, nativeRevokes = 0;
};

struct BitmapTextMetrics {
    float textWidth = 0, textHeight = 0, width = 0, height = 0;
    uint32_t glyphCount = 0;
    bool trusted = false, wrote = false;
};

class Runtime {
private:
    enum class ReservationBuildResult { Built, Complete, Blocked, Failed };
    struct ReservationPolicy {
        std::string id, packageName, componentName, blockedOn;
        std::unordered_set<std::string> dependencies;
        size_t target = 0, liveCount = 0, checkoutCount = 0;
        uint64_t nextEligibleFrame = 0, epoch = 1;
        double lastLiveDestroyedMs = 0;
        bool enabled = true, dirty = false, queued = false;
    };
    struct ReservationRefillTicket { std::string policyId; uint64_t epoch = 0; };

public:
    // The host writes the measured surface metrics back into InstanceNode. AuthoredBox preserves
    // the XML box/wrap/alignment used while the detached tree is being built; FacadeNatural uses
    // the same unboxed/top-aligned contract as NativeTextProxy::__nativeTextUpdate.
    using TextPrepare = std::function<bool(int, InstanceNode&, TextPrepareMode)>;
    TextPrepare prepareText;
    std::function<void(int)> releaseNode;
    int solidTextureId = 0;

    // 排队标记落槽:排队时没有 graph,拿不到 slot 代际;落槽前校验节点仍存活且仍受本运行时管理——
    // 失败构建先释放节点、scope 退出后再 flush 的情形不能把版本写回 dead slot(droppedDead 计数)。
    // 同一 scope 内 slot 被释放又复用给新节点的极端情形只会给新节点多一次版本递增(过度信号,安全)。
    void flushPendingMarks(sg::SceneGraph& graph) {
        auto& pending = FacadeSeqTracker::pending;
        if (pending.empty()) return;
        for (const auto& item : pending) {
            const int id = item.first;
            if (!graph.buf || id <= 0 || id >= sg::NODE_CAP || !graph.nodes[id].alive || !instanceForNode(id)) { ++FacadeSeqTracker::droppedDead; continue; }
            FacadeSeqTracker::apply(graph, id, item.second);
        }
        pending.clear();
    }
    int instanceRootForNode(int nativeId) const { Instance* inst = instanceForNode(nativeId); return inst && inst->root > 0 ? inst->root : 0; }
    // 发布提交:把 touched 节点归并到各自实例 root,递增 instanceCommitSeq[root]。只在操作收敛点调用
    // (public 入口结束、帧推进结束),半完成的关系传播永远不会成为合法版本。
    void publishInstanceCommits(sg::SceneGraph& graph) {
        flushPendingMarks(graph);   // 无 graph 写点(touchFacade/note)在收敛点落槽
        auto& touched = FacadeSeqTracker::touched;
        if (touched.empty() || !graph.buf) { touched.clear(); FacadeSeqTracker::touchedBits.clear(); return; }
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        std::vector<int> roots;
        for (int id : touched) {
            Instance* inst = instanceForNode(id); if (!inst || inst->root <= 0) { ++FacadeSeqTracker::orphans; continue; }   // 标记了却映射不到实例 root:提交发不出去
            if (std::find(roots.begin(), roots.end(), inst->root) == roots.end()) roots.push_back(inst->root);
        }
        for (int root : roots) if (root > 0 && root < sg::NODE_CAP) { graph.u32(sg::FGUI_COMMIT_BASE + root) += 1u; ++FacadeSeqTracker::commits; }
        static const bool trace = std::getenv("FGUI_PUBLISH_TRACE") != nullptr;   // 诊断:每次发布打印 touched ids → roots
        if (trace) { std::string ids; for (size_t i = 0; i < touched.size() && i < 24; ++i) { ids += std::to_string(touched[i]); ids += ','; }
            std::string rs; for (int root : roots) { rs += std::to_string(root); rs += ','; }
            std::fprintf(stderr, "[fgui-publish] n=%zu ids=%s roots=%s\n", touched.size(), ids.c_str(), rs.c_str()); }
        touched.clear(); FacadeSeqTracker::touchedBits.clear();
    }
    // public 入口的收敛作用域:嵌套调用只在最外层发布(setController → applyControllerValidated 等)。
    struct FacadeCommitScope {
        Runtime& rt; sg::SceneGraph& graph;
        FacadeCommitScope(Runtime& r, sg::SceneGraph& g) : rt(r), graph(g) { ++FacadeSeqTracker::scopeDepth; }
        ~FacadeCommitScope() { if (--FacadeSeqTracker::scopeDepth == 0) rt.publishInstanceCommits(graph); }
    };
    bool setStringsSource(const std::string& source) {
        XmlParser parser; auto root = parser.parse(source); if (!root) return false;
        translations_.clear(); prototypes_.clear(); ++translationRevision_;
        std::function<void(const XmlNode*)> visit = [&](const XmlNode* node) {
            if (!node) return;
            if (node->name == "string" && !node->attr("name").empty()) {
                std::string value = node->text;
                if (value.empty() && !node->children.empty() && node->children[0]) value = node->children[0]->text;
                translations_[node->attr("name")] = value;
            }
            for (const auto& child : node->children) visit(child.get());
        };
        visit(root.get()); return true;
    }

    // Build is deliberately detached from Runtime state so archive/XML parsing can run on an
    // asset worker. installPackage() is the only operation that publishes into the live maps and
    // therefore remains on the JSC/render thread.
    static std::unique_ptr<Package> buildPackage(const std::string& key, const std::unordered_map<std::string, std::string>& entries) {
        auto pit = entries.find("package.xml");
        if (pit == entries.end()) return nullptr;
        XmlParser parser; auto root = parser.parse(pit->second);
        if (!root || root->name != "packageDescription") return nullptr;
        auto pkg = std::make_unique<Package>(); pkg->key = key; pkg->id = root->attr("id"); pkg->name = root->attr("name"); pkg->descriptors = entries;
        const XmlNode* resources = root->child("resources");
        if (!resources) return nullptr;
        pkg->resourceOrder.reserve(resources->children.size());
        for (const auto& n : resources->children) {
            if (!n) continue; Resource r; r.kind = resourceKind(n->name); r.packageItemType = packageItemType(n->name); r.id = n->attr("id"); r.name = n->attr("name"); r.file = n->attr("file");
            pairValue(n->attr("size"), r.width, r.height);
            const std::string& scale = n->attr("scale");
            if (scale == "tile") r.scaleByTile = true;
            else if (scale == "9grid") { auto g = split(n->attr("scale9grid"), ','); if (g.size() >= 4) { r.hasScale9 = true; r.gridX = number(g[0]); r.gridY = number(g[1]); r.gridW = number(g[2]); r.gridH = number(g[3]); } r.tiledSlices = integer(n->attr("gridTile")); }
            if (!r.name.empty()) pkg->resourceIdByName[r.name] = r.id;
            pkg->resourceOrder.push_back(r.id); pkg->resourcesById[r.id] = r;
        }
        auto sit = entries.find("sprites.bytes");
        if (sit != entries.end()) parseSprites(*pkg, sit->second);
        // MovieClip descriptors are separate <item-id>.xml files.  Parse their timing and trimmed
        // frame rectangles once with the package so component construction never instantiates a
        // Pixi MovieClip merely to discover atlas frames.
        for (auto& item : pkg->resourcesById) if (item.second.kind == ResourceKind::MovieClip) {
            Resource& movie=item.second;auto mit=entries.find(movie.id+".xml");if(mit==entries.end())continue;
            auto xml=parser.parse(mit->second);if(!xml)continue;
            movie.interval=integer(xml->attr("interval"));movie.repeatDelay=integer(xml->attr("repeatDelay"));movie.swing=xml->attr("swing")=="true";
            const XmlNode* frames=xml->child("frames");if(!frames&&!xml->children.empty())frames=xml->children.front().get();if(!frames)continue;
            int index=0;for(const auto& frameNode:frames->children){if(!frameNode){++index;continue;}Resource::MovieFrame frame;auto rect=split(frameNode->attr("rect"),',');if(rect.size()>=4){frame.x=number(rect[0]);frame.y=number(rect[1]);frame.width=number(rect[2]);frame.height=number(rect[3]);}frame.addDelay=integer(frameNode->attr("addDelay"));std::string suffix=frameNode->attr("sprite");if(suffix.empty())suffix=std::to_string(index);frame.spriteId=movie.id+"_"+suffix;movie.movieFrames.push_back(std::move(frame));++index;}
        }
        for (const auto& item : pkg->resourcesById) if (item.second.kind == ResourceKind::Font) {
            auto fit=entries.find(item.second.id+".fnt");if(fit!=entries.end()){
                BitmapFont font=parseBitmapFont(fit->second);
                // The image item is the authoritative texture size for FairyGUI image fonts. Its
                // loader replaces any width/height authored on the char line before resolving the
                // default advance and per-glyph line height.
                for(auto& glyphItem:font.glyphs){
                    BitmapGlyph& glyph=glyphItem.second;
                    // FairyGUI's `ttf` flag names the BMFont atlas form, not a system TTF
                    // rasterisation request. Its glyph rectangle supplies width/height while the
                    // common record supplies the line box.
                    if(font.ttf){glyph.lineHeight=font.commonLineHeight;continue;}
                    auto image=pkg->resourcesById.find(glyph.imageId);
                    if(image==pkg->resourcesById.end())continue;
                    const Resource& resource=image->second;
                    if(glyph.advance==0)glyph.advance=font.commonAdvance!=0?font.commonAdvance:glyph.offsetX+resource.width;
                    glyph.lineHeight=glyph.offsetY<0?resource.height:glyph.offsetY+resource.height;
                    if(font.size>0&&glyph.lineHeight<font.size)glyph.lineHeight=font.size;
                }
                pkg->fonts[item.second.id]=std::move(font);
            }
        }
        for (const auto& sprite : pkg->sprites) if (!sprite.second.atlasId.empty()) pkg->requiredAtlases.insert(sprite.second.atlasId);
        for (const auto& item : pkg->resourcesById) {
            const Resource& r = item.second; if (r.kind != ResourceKind::Component) continue;
            auto eit = entries.find(r.id + ".xml"); if (eit == entries.end()) continue;
            auto xml = parser.parse(eit->second); if (!xml) continue;
            auto c = std::make_unique<Component>(); c->id = r.id; c->name = r.name; c->extension = xml->attr("extention"); c->xml = std::move(xml);
            inspectCompatibility(c->xml.get(), c->hasUnsupportedStaticNode, c->unsupportedReason); pkg->components[c->id] = std::move(c);
        }
        if (pkg->name.empty()) return nullptr;
        return pkg;
    }

    bool installPackage(std::unique_ptr<Package> pkg, sg::SceneGraph* graph = nullptr) {
        if (!pkg || pkg->name.empty()) return false;
        if (graph) invalidateReservationsForPackage(pkg->name, *graph);
        pkg->generation = nextPackageGeneration_++;
        // A top-level template may embed components, images, fonts or loaders from any other
        // package. Invalidate the whole cache on replacement rather than trying to maintain an
        // incomplete dependency graph and accidentally cloning stale authored metadata.
        prototypes_.clear();
        auto old = packagesByName_.find(pkg->name);
        if (old != packagesByName_.end()) {
            Package* stale = old->second.get();
            for (auto it = packagesById_.begin(); it != packagesById_.end();) {
                if (it->second == stale) it = packagesById_.erase(it); else ++it;
            }
            for (auto it = packagesByKey_.begin(); it != packagesByKey_.end();) {
                if (it->second == stale) it = packagesByKey_.erase(it); else ++it;
            }
        }
        // Packages with no atlas dependencies are ready as soon as their descriptors publish.
        // Wake policies blocked by a replacement/missing dependency here; there will be no later
        // preparePackage()/bindAtlas() call to do it for this package.
        pkg->resourcesPrepared = resourcesReady(*pkg);
        Package* raw = pkg.get(); packagesById_[pkg->id] = raw; packagesByKey_[pkg->key] = raw; packagesByName_[pkg->name] = std::move(pkg);
        if (raw->resourcesPrepared) wakeReservationPolicies(*raw);
        return true;
    }

    bool registerPackage(const std::string& key, const std::unordered_map<std::string, std::string>& entries,
                         sg::SceneGraph* graph = nullptr) {
        return installPackage(buildPackage(key, entries), graph);
    }

    void reset(sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        std::fprintf(stderr,
            "[native-fgui-transition] reset live=%zu checkout=%zu active=%zu completed=%zu nextToken=%d\n",
            instances_.size(), checkouts_.size(), activeTransitions_.size(),
            completedTransitions_.size(), nextTransitionToken_);
        for (auto& item : instances_) if (item.second) destroyTree(*item.second, graph);
        for (auto& item : checkouts_) if (item.second) destroyTree(*item.second, graph);
        for (auto& pool : reservations_) for (auto& item : pool.second) if (item) destroyTree(*item, graph);
        instances_.clear(); checkouts_.clear(); reservations_.clear(); reservationPolicies_.clear(); reservationRefillQueue_.clear();
        managedNodeOwners_.clear();
        prototypes_.clear(); packagesById_.clear(); packagesByKey_.clear(); packagesByName_.clear(); translations_.clear();
        activeTransitions_.clear(); completedTransitions_.clear(); transitionSignals_.clear(); transitionVisibilitySignals_.clear(); transitionPropertySignals_.clear(); scheduledTasks_.clear(); completedTasks_.clear();
        templateStats_ = {}; reservationStats_ = {}; checkoutStats_ = {}; textOwnershipStats_ = {}; lastReservationBlock_.clear(); lastCreateFailure_.clear(); lastBuildFailure_.clear(); nextInstance_ = 1;
        nextTransitionToken_ = 1; nextTaskOrder_ = 1; nextPackageGeneration_ = 1; translationRevision_ = 1;
        lastReservationRefillFrame_ = 0; hasReservationRefillFrame_ = false;
    }

    Package* package(const std::string& nameOrId) const {
        auto n = packagesByName_.find(nameOrId); if (n != packagesByName_.end()) return n->second.get();
        auto i = packagesById_.find(nameOrId); if (i != packagesById_.end()) return i->second;
        auto k = packagesByKey_.find(nameOrId); return k == packagesByKey_.end() ? nullptr : k->second;
    }
    // FairyGUI package ids are intended to be global, but the shipped machine archives reuse the
    // same exporter id across mutually-exclusive games. An authored ui:// URL is still
    // unambiguous when it belongs to a node: if its prefix names that node's owning package, the
    // owner must win over the process-wide last-writer id map. Ownerless callers retain FairyGUI's
    // ordinary active-package lookup semantics.
    Package* packageForReference(const std::string& reference,
                                 const std::string& ownerNameOrKey = std::string()) const {
        Package* owner = ownerNameOrKey.empty() ? nullptr : package(ownerNameOrKey);
        if (owner && (reference == owner->id || reference == owner->name || reference == owner->key))
            return owner;
        return package(reference);
    }
    // A cached native blueprint survives UIPackage.removePackage so re-entry does not inflate and
    // compile the archive again. Mirror the subsequent JS UIPackage.addPackage by restoring that
    // cached package as the active target for ownerless id lookups. Contextual lookups above keep
    // already-live packages correct even when another package with the same exporter id activates.
    bool activatePackage(const std::string& nameOrKey) {
        Package* pkg = package(nameOrKey); if (!pkg) return false;
        auto current = packagesById_.find(pkg->id);
        if (current == packagesById_.end() || current->second != pkg) prototypes_.clear();
        packagesById_[pkg->id] = pkg;
        packagesByKey_[pkg->key] = pkg;
        return true;
    }
    const std::string* descriptor(const std::string& packageNameOrKey, const std::string& name) const {
        Package* p = package(packageNameOrKey); if (!p) return nullptr;
        auto it = p->descriptors.find(name); return it == p->descriptors.end() ? nullptr : &it->second;
    }
    const Sprite* sprite(const std::string& packageNameOrKey, const std::string& itemId) const {
        Package* p = package(packageNameOrKey); if (!p) return nullptr;
        auto it = p->sprites.find(itemId); return it == p->sprites.end() ? nullptr : &it->second;
    }
    bool inlineImage(const std::string& url, InlineImageDescriptor& out,
                     const std::string& preferredPackage = std::string()) const {
        out = {}; std::string itemId; Package* pkg = nullptr; Resource* resource = nullptr;
        if (url.compare(0, 5, "ui://") == 0) {
            std::string body = url.substr(5); size_t slash = body.find('/');
            if (slash != std::string::npos) {
                pkg = packageForReference(body.substr(0, slash), preferredPackage);
                if (pkg) {
                    auto named = pkg->resourceIdByName.find(body.substr(slash + 1));
                    if (named != pkg->resourceIdByName.end()) itemId = named->second;
                }
            } else if (body.size() > 8) {
                pkg = packageForReference(body.substr(0, 8), preferredPackage); itemId = body.substr(8);
            }
        } else if (!url.empty() && !preferredPackage.empty()) {
            // Native FGUI deliberately keeps atlas-backed PackageItems out of JavaScript.  In
            // that mode presupposeImg can only leave `imgSrc="itemName"`; resolve it relative to
            // the owning text node, exactly like FairyGUI resolves an unqualified resource.
            pkg = package(preferredPackage);
            if (pkg) {
                auto named = pkg->resourceIdByName.find(url);
                if (named != pkg->resourceIdByName.end()) itemId = named->second;
            }
        } else if (!url.empty()) {
            // Last-resort path for non-managed text. Managed text first resolves the same name
            // in its owning FairyGUI package through inlineImageForNode below. Ambiguous global
            // names deliberately fail rather than sampling a random package atlas.
            for (const auto& entry : packagesByName_) {
                Package* candidate = entry.second.get(); if (!candidate) continue;
                auto named = candidate->resourceIdByName.find(url); if (named == candidate->resourceIdByName.end()) continue;
                if (pkg && pkg != candidate) return false;
                pkg = candidate; itemId = named->second;
            }
        }
        if (!pkg || itemId.empty()) return false;
        auto ri = pkg->resourcesById.find(itemId); if (ri == pkg->resourcesById.end()) return false;
        resource = &ri->second; if (resource->kind != ResourceKind::Image) return false;
        auto si = pkg->sprites.find(resource->id); if (si == pkg->sprites.end()) return false;
        auto ai = pkg->atlases.find(si->second.atlasId);
        if (ai == pkg->atlases.end() || ai->second.textureId <= 0 || ai->second.width <= 0 || ai->second.height <= 0) return false;
        const Sprite& sprite = si->second; const AtlasBinding& atlas = ai->second;
        float x0=sprite.x/atlas.width,y0=sprite.y/atlas.height,x1=(sprite.x+sprite.width)/atlas.width,y1=(sprite.y+sprite.height)/atlas.height;
        if (!sprite.rotated) { out.uv[0]=x0;out.uv[1]=y0;out.uv[2]=x1;out.uv[3]=y0;out.uv[4]=x1;out.uv[5]=y1;out.uv[6]=x0;out.uv[7]=y1; }
        else { out.uv[0]=x0;out.uv[1]=y1;out.uv[2]=x0;out.uv[3]=y0;out.uv[4]=x1;out.uv[5]=y0;out.uv[6]=x1;out.uv[7]=y1; }
        out.textureId=atlas.textureId;out.width=resource->width>0?resource->width:sprite.width;out.height=resource->height>0?resource->height:sprite.height;out.premul=atlas.premul;
        return out.width>0&&out.height>0;
    }
    bool inlineImageForNode(int nativeId,const std::string& url,InlineImageDescriptor& out) const {
        std::string preferred;auto owner=managedNodeOwners_.find(nativeId);
        if(owner!=managedNodeOwners_.end()&&owner->second){
            auto target=owner->second->byNativeId.find(nativeId);
            if(target!=owner->second->byNativeId.end()&&target->second)preferred=target->second->packageName;
        }
        if(!preferred.empty()&&inlineImage(url,out,preferred))return true;
        return inlineImage(url,out);
    }
    void bindAtlas(const std::string& pkgName, const std::string& atlasId, int textureId, float width, float height, float premul,
                   sg::SceneGraph* graph = nullptr) {
        Package* p = package(pkgName); if (!p) return;
        // A prototype can embed an image/font/loader from this package. Rebinding even one atlas
        // makes every cached authored visual suspect; the next reservation must rebuild from XML.
        prototypes_.clear(); p->atlases[atlasId] = {textureId, width, height, premul};
        ++p->resourceRevision; p->resourcesPrepared = resourcesReady(*p);
        // A live native tree stores compositor texture ids rather than Pixi Texture objects.
        // Re-publishing a package therefore has to repaint every dependent visual: unlike a
        // browser Sprite, an already-compiled node cannot observe a changed BaseTexture through
        // object identity.  Without this refresh, scene/resource reloads leave external package
        // images pointing at retired ids while text and layout continue to work.
        if (p->resourcesPrepared && graph) refreshLivePackageVisuals(*p, *graph);
        if (p->resourcesPrepared) wakeReservationPolicies(*p);
    }
    bool preparePackage(const std::string& pkgName, const std::vector<std::pair<std::string, AtlasBinding>>& atlases,
                        sg::SceneGraph* graph = nullptr) {
        Package* p = package(pkgName); if (!p) return false;
        // Publish the immutable binding table atomically. A component can therefore never see a
        // half-prepared package while UIPackage.init is still walking its atlas resources.
        std::unordered_map<std::string, AtlasBinding> next;
        for (const auto& item : atlases) if (!item.first.empty() && item.second.textureId > 0 && item.second.width > 0 && item.second.height > 0) next[item.first] = item.second;
        prototypes_.clear(); p->atlases.swap(next); ++p->resourceRevision; p->resourcesPrepared = resourcesReady(*p);
        if (p->resourcesPrepared && graph) refreshLivePackageVisuals(*p, *graph);
        if (p->resourcesPrepared) wakeReservationPolicies(*p);
        return p->resourcesPrepared;
    }
    int create(const std::string& pkgName, const std::string& componentName, sg::SceneGraph& graph,
               double nowMs = 0, bool* claimedReservation = nullptr,
               uint64_t claimFrame = std::numeric_limits<uint64_t>::max()) {FacadeCommitScope __facadeCommit(*this,graph);
        if (claimedReservation) *claimedReservation = false;
        lastCreateFailure_.clear();
        Package* pkg = package(pkgName);
        if (!pkg) { lastCreateFailure_ = "package-missing:" + pkgName; return 0; }
        if (!pkg->resourcesPrepared && !pkg->requiredAtlases.empty()) {
            lastCreateFailure_ = "resources-unprepared:" + pkg->name; return 0;
        }
        Resource* res = resourceByName(*pkg, componentName);
        if (!res) { lastCreateFailure_ = "component-missing:" + pkg->name + "/" + componentName; return 0; }
        if (res->kind != ResourceKind::Component) {
            lastCreateFailure_ = "component-type:" + pkg->name + "/" + componentName; return 0;
        }
        auto ci = pkg->components.find(res->id);
        if (ci == pkg->components.end() || !ci->second->xml) {
            lastCreateFailure_ = "component-descriptor:" + pkg->name + "/" + componentName; return 0;
        }
        const std::string policyId = reservationPolicyKey(pkg->name, componentName);
        auto policyIt = reservationPolicies_.find(policyId);
        ReservationPolicy* policy = policyIt == reservationPolicies_.end() || !policyIt->second.enabled ? nullptr : &policyIt->second;
        // Reopening before a refill completes cancels only the queued continuation. Already-built,
        // old-enough detached instances remain valid and can still reduce this opening's work.
        if (policy && policy->queued) {
            ++policy->epoch; policy->queued = false; ++reservationStats_.refillCanceled;
        }
        const auto claimStarted = std::chrono::steady_clock::now(); bool hadReservationPool = false, tooYoung = false;
        std::unique_ptr<Instance> inst = takeReservation(*pkg, *res, graph, claimFrame, &hadReservationPool, &tooYoung);
        const bool claimed = static_cast<bool>(inst);
        if (claimed) {
            ++reservationStats_.claimed;
            if (claimedReservation) *claimedReservation = true;
        } else {
            if (hadReservationPool) ++reservationStats_.misses;
            inst = buildInstance(*pkg, *res, *ci->second, componentName, graph);
            if (!inst) {
                lastCreateFailure_ = "build-failed:" + pkg->name + "/" + componentName;
                if (!lastBuildFailure_.empty()) lastCreateFailure_ += ":" + lastBuildFailure_;
                return 0;
            }
            applyAllControllers(*inst, graph);
        }
        // A refill made earlier in this same presentation is intentionally ineligible. Once the
        // synchronous API has had to build a live item, keeping only-young detached duplicates
        // cannot help the remaining same-frame creates and merely doubles scene memory.
        if (!claimed && tooYoung && policy) {
            clearReservationPool(reservationKey(*pkg, *res), graph, false);
            ++reservationStats_.refillCanceled;
        }
        if (policy) inst->reservationPolicyKey = policyId;
        int id = inst->id; instances_[id] = std::move(inst); activateInstance(*instances_[id], nowMs, graph);
        if (policy) {
            ++policy->liveCount; policy->dirty = reservationPoolSize(*pkg, *res) < policy->target;
        }
        if (claimed) reservationStats_.claimMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - claimStarted).count();
        return id;
    }

    // Atomically claim an authored purchase-reward batch without publishing it as live.  JavaScript
    // may populate the exact child handles before promoteBatch(), while coverage, MovieClips and
    // transitions continue to see only instances_.  The pool is inspected in FIFO order and is
    // left untouched when a complete batch is unavailable.


    // Promotion is an all-or-nothing publication boundary. Validation also reserves every native
    // container needed by the no-fail phase, allowing the combined store commit to rasterise text
    // first and then publish structure without another fallible lookup/allocation step.


    // Build a bounded set of detached, inactive components while the package is being prepared.
    // They deliberately stay out of instances_: coverage, MovieClip advancement and transitions
    // therefore cannot observe them until create() claims one.
    size_t reserve(const std::string& pkgName, const std::string& componentName, size_t targetCount,
                   sg::SceneGraph& graph, double preparedAtMs = 0, uint64_t preparedAtFrame = 0) {FacadeCommitScope __facadeCommit(*this,graph);
        lastReservationBlock_.clear();
        targetCount = std::min<size_t>(targetCount, 64);
        Package* pkg = package(pkgName); if (!pkg || targetCount == 0) return 0;
        if (!pkg->resourcesPrepared && !pkg->requiredAtlases.empty()) {
            lastReservationBlock_="resources:"+pkg->name;++reservationStats_.blocked;return 0;
        }
        Resource* res = resourceByName(*pkg, componentName); if (!res || res->kind != ResourceKind::Component) return 0;
        auto ci = pkg->components.find(res->id); if (ci == pkg->components.end() || !ci->second->xml) return 0;
        const std::string policyId = reservationPolicyKey(pkg->name, componentName);
        auto existingPolicy = reservationPolicies_.find(policyId);
        if (existingPolicy != reservationPolicies_.end() && existingPolicy->second.enabled) {
            // Once a successful initial reserve established the policy, later package init/rebind
            // calls must not synchronously recreate all nine trees. Resource publication wakes the
            // policy and the Native frame job repopulates it under the one-build budget.
            existingPolicy->second.target = targetCount;
            existingPolicy->second.dirty = reservationPoolSize(*pkg, *res) < targetCount;
            if (existingPolicy->second.dirty && existingPolicy->second.liveCount == 0 && existingPolicy->second.checkoutCount == 0 &&
                existingPolicy->second.blockedOn.empty()) enqueueReservationPolicy(existingPolicy->second, 0);
            return reservationPoolSize(*pkg, *res);
        }
        const std::string key = reservationKey(*pkg, *res);
        auto& pool = reservations_[key]; discardStaleReservations(pool, *pkg, graph);
        while (pool.size() < targetCount) {
            ReservationBuildResult result = buildReservationOne(*pkg, *res, *ci->second, componentName,
                                                                  targetCount, graph, preparedAtMs,
                                                                  preparedAtFrame, nullptr, false);
            if (result != ReservationBuildResult::Built) break;
        }
        size_t available=pool.size();
        if(available==0)reservations_.erase(key);
        else registerReservationPolicy(*pkg, *res, componentName, targetCount, pool);
        return available;
    }
    void invalidateReservationsForPackage(const std::string& pkgNameOrId, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        Package* pkg = package(pkgNameOrId); const std::string name = pkg ? pkg->name : pkgNameOrId;
        blockReservationPoliciesForPackage(name);
        for (auto item = checkouts_.begin(); item != checkouts_.end();) {
            bool depends = false;
            if (item->second) for (const auto& dependency : item->second->packageDependencies)
                if (dependency.name == name) { depends = true; break; }
            if (!depends) { ++item; continue; }
            if (item->second) { decrementCheckoutPolicy(*item->second); destroyTree(*item->second, graph); }
            item = checkouts_.erase(item); ++checkoutStats_.canceled;
        }
        for (auto pool = reservations_.begin(); pool != reservations_.end();) {
            auto& items = pool->second;
            for (auto item = items.begin(); item != items.end();) {
                bool depends = false;
                if (*item) for (const auto& dependency : (*item)->packageDependencies)
                    if (dependency.name == name) { depends = true; break; }
                if (!depends) { ++item; continue; }
                reservationStats_.invalidatedNodes += (*item)->nodes.size();destroyTree(**item, graph);item = items.erase(item);++reservationStats_.invalidated;
            }
            if (items.empty()) pool = reservations_.erase(pool); else ++pool;
        }
    }
    void invalidateAllReservations(sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        for (auto& item : checkouts_) if (item.second) {
            decrementCheckoutPolicy(*item.second); destroyTree(*item.second, graph); ++checkoutStats_.canceled;
        }
        checkouts_.clear();
        for (auto& pool : reservations_) for (auto& item : pool.second) if (item) {
            reservationStats_.invalidatedNodes += item->nodes.size();destroyTree(*item, graph);++reservationStats_.invalidated;
        }
        reservations_.clear();
        // This path follows a successfully committed strings table on Mac. Policies survive the
        // revision change, but their old detached text must be rebuilt gradually from the new
        // translation revision. A queued epoch makes any pre-change tickets inert.
        for (auto& item : reservationPolicies_) {
            ReservationPolicy& policy = item.second; if (policy.queued) ++reservationStats_.refillCanceled;
            ++policy.epoch; policy.queued = false;
            policy.dirty = true;
            if (policy.enabled && policy.liveCount == 0 && policy.checkoutCount == 0 && policy.blockedOn.empty()) enqueueReservationPolicy(policy, 0);
        }
    }
    Instance* instance(int id) const {
        auto live = instances_.find(id); if (live != instances_.end()) return live->second.get();
        auto checkout = checkouts_.find(id); return checkout == checkouts_.end() ? nullptr : checkout->second.get();
    }
    Instance* liveInstance(int id) const { auto it = instances_.find(id); return it == instances_.end() ? nullptr : it->second.get(); }
    Instance* checkoutInstance(int id) const { auto it = checkouts_.find(id); return it == checkouts_.end() ? nullptr : it->second.get(); }
    bool isManagedNode(int nativeId) const {
        return nativeId > 0 && managedNodeOwners_.find(nativeId) != managedNodeOwners_.end();
    }
    void refreshManagedDisplayVisibility(int nativeId, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        auto owner = managedNodeOwners_.find(nativeId);
        if (owner == managedNodeOwners_.end() || !owner->second) return;
        Instance* inst = owner->second;
        if (instance(inst->id) != inst) return;
        InstanceNode* target = nodeIn(*inst, nativeId);
        if (target) writeNodeVisibility(graph, *target);
    }
    InstanceNode* node(int instanceId, int nativeId) const { Instance* i = instance(instanceId); if (!i) return nullptr; auto it = i->byNativeId.find(nativeId); return it == i->byNativeId.end() ? nullptr : it->second; }
    InstanceNode* child(int instanceId, int parentNativeId, const std::string& nameOrId, int index = -1) const {
        InstanceNode* p = node(instanceId, parentNativeId); if (!p) return nullptr;
        if (index >= 0) return static_cast<size_t>(index) < p->children.size() ? node(instanceId, p->children[index]) : nullptr;
        for (int id : p->children) { InstanceNode* c = node(instanceId, id); if (c && (c->name == nameOrId || c->id == nameOrId)) return c; }
        return nullptr;
    }
    int childCount(int instanceId, int parentNativeId) const {
        InstanceNode* p = node(instanceId, parentNativeId);
        return p ? static_cast<int>(p->children.size()) : 0;
    }

    // Publish CoreText ownership only after every surface in an exact checkout batch rasterised.
    // Validation is two-phase, so a malformed final row cannot leave earlier InstanceNodes marked
    // as owned.  SceneGraph pixels are already present when this no-allocation metadata tail runs.


    // Generic capability gate for deferred FGUI text writes.  The caller supplies only an
    // instance/node handle; Native proves ownership and rejects nodes whose controller/transition
    // semantics could write text between the JavaScript setter and the transaction commit.
    InstanceNode* textMutationTarget(int instanceId, int nativeId, const sg::SceneGraph& graph) {
        Instance* inst = liveInstance(instanceId);
        if (!inst || !inst->compatible) return nullptr;
        InstanceNode* target = nodeIn(*inst, nativeId);
        const bool supportedKind = target &&
            (target->kind == ObjectKind::Text || target->kind == ObjectKind::RichText);
        if (!supportedKind || nativeId <= 0 ||
            nativeId >= sg::NODE_CAP || !graph.nodes[nativeId].alive || target->textGear.present ||
            !(target->mutationCaps & MutationTextFinal))
            return nullptr;
        return target;
    }

    bool writeTextMutationTarget(InstanceNode& node, const std::string& text, int boldMode,
                                 sg::SceneGraph& graph) {
        if (text.size() > 65536 || (boldMode != 0 && boldMode != 1)) return false;
        InstanceNode* target = &node;

        struct Snapshot {
            std::unordered_map<std::string, std::string> attrs;
            float nativeWidth = 0, nativeHeight = 0, nativeLineHeight = 0;
            float bitmapWidth = 0, bitmapHeight = 0;
            uint32_t bitmapGlyphs = 0;
            bool nativeMetrics = false, facadeNatural = false, facadeOwned = false;
            bool bitmapTrusted = false, pristine = false;
        } old;
        old.attrs = target->attrs;
        old.nativeWidth = target->nativeTextWidth;
        old.nativeHeight = target->nativeTextHeight;
        old.nativeLineHeight = target->nativeTextLineHeight;
        old.bitmapWidth = target->bitmapTextWidth;
        old.bitmapHeight = target->bitmapTextHeight;
        old.bitmapGlyphs = target->bitmapGlyphCount;
        old.nativeMetrics = target->nativeTextMetricsValid;
        old.facadeNatural = target->nativeTextFacadeNatural;
        old.facadeOwned = target->nativeTextFacadeOwned;
        old.bitmapTrusted = target->bitmapMetricsTrusted;
        old.pristine = target->facadeSchemaPristine;

        auto restoreMetadata = [&] {
            target->attrs = old.attrs;
            target->nativeTextWidth = old.nativeWidth;
            target->nativeTextHeight = old.nativeHeight;
            target->nativeTextLineHeight = old.nativeLineHeight;
            target->bitmapTextWidth = old.bitmapWidth;
            target->bitmapTextHeight = old.bitmapHeight;
            target->bitmapGlyphCount = old.bitmapGlyphs;
            target->nativeTextMetricsValid = old.nativeMetrics;
            target->nativeTextFacadeNatural = old.facadeNatural;
            target->nativeTextFacadeOwned = old.facadeOwned;
            target->bitmapMetricsTrusted = old.bitmapTrusted;
            target->facadeSchemaPristine = old.pristine; FacadeSeqTracker::note(target->nativeId, FD_ATTRS | FD_TEXT);
        };

        const bool bitmap = isBitmapText(*target);
        target->attrs["text"] = text;
        target->attrs["bold"] = boldMode ? "true" : "false";
        FacadeSeqTracker::mark(graph, target->nativeId, FD_ATTRS | FD_TEXT);
        target->facadeSchemaPristine=false;
        bool ok = false;
        if (bitmap) {
            ok = writeBitmapText(graph, *target);
        } else if (prepareText) {
            // A fixed-box FGUI label must keep its authored width/height after a runtime text
            // write. Rendering every ordinary label at its natural width loses center/right and
            // vertical alignment when no materialised JS facade exists (timers are the common
            // example). Auto-sized fields still use the natural PIXI.Text-compatible surface —
            // unless they carry a non-left/non-top alignment: H5's autoSize+align 的净效果=以
            // authored 盒锚点对齐,FacadeNatural 在 JS 从不 materialise 的字段上会永久丢对齐
            // (热商店划线价贴左即此形态);AuthoredBox 的盒内行对齐与 H5 视觉等效,JS 真接管时
            // claim 认领照常覆盖。
            const bool fixedBox = (target->mutationCaps & MutationTextFixedBox) != 0;
            const TextPrepareMode mode = target->kind == ObjectKind::RichText || fixedBox ||
                textAttrWantsAuthoredBox(*target)
                ? TextPrepareMode::AuthoredBox : TextPrepareMode::FacadeNatural;
            target->nativeTextFacadeNatural = mode == TextPrepareMode::FacadeNatural;
            target->nativeTextFacadeOwned = false;
            ok = prepareTextSurface(*target, mode, graph);
            if (ok) {
                if (!old.facadeOwned) ++textOwnershipStats_.published;
                target->nativeTextFacadeOwned = true;
            }
        }
        if (ok) {
            target->facadeSchemaPristine=false;
            return true;
        }

        // Rendering callbacks are synchronous on the Mac render/JSC thread. Restore the semantic
        // state first, repaint the previous content through the same backend, then restore the
        // exact ownership/metric bits so a failed fast write is observationally atomic.
        restoreMetadata();
        if (bitmap) {
            (void)writeBitmapText(graph, *target);
        } else if (prepareText) {
            (void)prepareTextSurface(*target, old.facadeNatural ? TextPrepareMode::FacadeNatural
                                                               : TextPrepareMode::AuthoredBox,
                                     graph);
        }
        restoreMetadata();
        return false;
    }
    bool setTextMutation(int instanceId, int nativeId, const std::string& text, int boldMode,
                         sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* target = textMutationTarget(instanceId, nativeId, graph);
        return target && writeTextMutationTarget(*target, text, boldMode, graph);
    }
    bool commitTextMutations(const std::vector<TextMutationWrite>& writes, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        if (writes.empty() || writes.size() > 512) return false;
        struct Previous { std::string text; int boldMode = 0; };
        std::vector<InstanceNode*> targets; targets.reserve(writes.size());
        std::vector<Previous> previous; previous.reserve(writes.size());
        std::unordered_set<uint64_t> unique; unique.reserve(writes.size());
        size_t totalBytes = 0;
        for (const TextMutationWrite& write : writes) {
            if (write.instanceId <= 0 || write.nativeId <= 0 ||
                (write.boldMode != 0 && write.boldMode != 1) || write.text.size() > 65536 ||
                totalBytes > 1048576 - write.text.size()) return false;
            totalBytes += write.text.size();
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(write.instanceId)) << 32) |
                                 static_cast<uint32_t>(write.nativeId);
            if (!unique.insert(key).second) return false;
            InstanceNode* target = textMutationTarget(write.instanceId, write.nativeId, graph);
            if (!target) return false;
            auto textIt = target->attrs.find("text"), boldIt = target->attrs.find("bold");
            previous.push_back({textIt == target->attrs.end() ? std::string() : textIt->second,
                                boldIt != target->attrs.end() && boldIt->second == "true" ? 1 : 0});
            targets.push_back(target);
        }

        // All identity/ownership checks are complete before the first raster. Each individual write
        // is already rollback-safe. If a later surface preparation fails, restore every earlier
        // semantic/surface value synchronously; the JS caller then replays the desired rows through
        // the single-write path before the next Native present.
        for (size_t i = 0; i < writes.size(); ++i) if (!writeTextMutationTarget(
                *targets[i], writes[i].text, writes[i].boldMode, graph)) {
            for (size_t rollback = i; rollback-- > 0;)
                (void)writeTextMutationTarget(*targets[rollback], previous[rollback].text,
                                              previous[rollback].boldMode, graph);
            return false;
        }
        return true;
    }
    bool adoptDisplayContainer(int instanceId, int semanticParentId, int displayParentId,
                               sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* parent=node(instanceId,semanticParentId);
        if(!parent||displayParentId<=0)return false;
        // Keep FairyGUI's semantic ownership in InstanceNode, but move the actual native display
        // children below a scroll/list content container in one host call. GList renderOrder is a
        // display-only permutation: getChildAt and layout retain semantic order, while overlapping
        // children are attached in the same ascent/descent/arch order as appendChildrenList().
        std::vector<int> displayOrder=parent->children;
        if(parent->kind==ObjectKind::List){
            const std::string order=parent->attrs.count("renderOrder")?parent->attrs.at("renderOrder"):std::string();
            if(order=="descent")std::reverse(displayOrder.begin(),displayOrder.end());
            else if(order=="arch"){
                const size_t apex=std::min(displayOrder.size(),(size_t)std::max(0,integer(parent->attrs.count("apex")?parent->attrs.at("apex"):std::string())));
                std::vector<int> arch;arch.reserve(displayOrder.size());
                arch.insert(arch.end(),displayOrder.begin(),displayOrder.begin()+apex);
                for(size_t i=displayOrder.size();i>apex;--i)arch.push_back(displayOrder[i-1]);
                displayOrder.swap(arch);
            }
        }
        for(int childId:displayOrder){graph.setParent(childId,displayParentId,-1);InstanceNode* child=node(instanceId,childId);if(child)child->displayParentNativeId=displayParentId;}
        return true;
    }
    bool canReparentExternal(int ownerInstanceId,int childId,int semanticParentId,int displayParentId,
                             int beforeId,const sg::SceneGraph& graph) const {
        Instance* owner=liveInstance(ownerInstanceId);
        if(!owner||childId<=0||childId>=sg::NODE_CAP||semanticParentId<=0||
           displayParentId<=0||displayParentId>=sg::NODE_CAP)return false;
        if(!graph.nodes[childId].alive||!graph.nodes[displayParentId].alive||
           semanticParentId>=sg::NODE_CAP||!graph.nodes[semanticParentId].alive||
           !nodeIn(*owner,semanticParentId))return false;
        if(beforeId < -1 || (beforeId>=0&&(beforeId==0||beforeId>=sg::NODE_CAP||!graph.nodes[beforeId].alive)))return false;
        if(childId==displayParentId||childId==semanticParentId||childId==beforeId)return false;
        // Reject cycles even when a caller supplies a live external display container.
        for(int at=displayParentId,guard=0;at>=0&&at<sg::NODE_CAP&&guard<sg::NODE_CAP;++guard){
            if(at==childId)return false;at=graph.nodes[at].parent;
        }
        return true;
    }
    bool reparent(int instanceId,int childId,int semanticParentId,int displayParentId,
                  int beforeId,sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        Instance* inst=instance(instanceId);if(!inst||childId<=0)return false;
        InstanceNode* child=nodeIn(*inst,childId);
        if(child){
            InstanceNode* old=nodeIn(*inst,child->parentNativeId);
            if(old){old->facadeSchemaPristine=false;auto& list=old->children;list.erase(std::remove(list.begin(),list.end(),childId),list.end());FacadeSeqTracker::markChildren(graph,old->nativeId);}
            InstanceNode* parent=nodeIn(*inst,semanticParentId);
            if(parent){
                parent->facadeSchemaPristine=false;
                auto& list=parent->children;auto at=beforeId>0?std::find(list.begin(),list.end(),beforeId):list.end();
                list.insert(at,childId);child->parentNativeId=semanticParentId;FacadeSeqTracker::markChildren(graph,parent->nativeId);
            }else child->parentNativeId=-1;
            child->displayParentNativeId=displayParentId>0?displayParentId:-1;child->facadeSchemaPristine=false;FacadeSeqTracker::mark(graph,childId,FD_PARENT);
        }
        if(displayParentId>0)graph.setParent(childId,displayParentId,beforeId);else graph.removeFromParent(childId);
        if(child){if(child->kind==ObjectKind::Group)resolveGroupMemberships(*inst);else resolveGroupMembership(*inst,*child);refreshGroupVisibility(*inst,graph);}
        return true;
    }
    bool canController(int instanceId,int componentNativeId,const std::string& name,int selectedIndex,
                       bool checkoutOnly=false) const {
        Instance* inst=checkoutOnly?checkoutInstance(instanceId):instance(instanceId);if(!inst)return false;
        InstanceNode* component=nodeIn(*inst,componentNativeId);if(!component)return false;
        for(const auto& controller:component->controllers)if(controller.name==name)
            return selectedIndex>=-1&&selectedIndex<(int)controller.pageIds.size();
        return false;
    }
    void applyControllerValidated(int instanceId,int componentNativeId,const std::string& name,
                                  int selectedIndex,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        Instance* inst=instance(instanceId);InstanceNode* component=nodeIn(*inst,componentNativeId);
        for(size_t i=0;i<component->controllers.size();++i)if(component->controllers[i].name==name){
            component->facadeSchemaPristine=false;component->controllers[i].selectedIndex=selectedIndex;FacadeSeqTracker::mark(graph,component->nativeId,FD_CONTROLLER);applyController(*inst,*component,(int)i,graph,true);refreshGroupVisibility(*inst,graph);return;
        }
    }
    bool setController(int instanceId, int componentNativeId, const std::string& name, int selectedIndex, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        if(!canController(instanceId,componentNativeId,name,selectedIndex,false))return false;
        applyControllerValidated(instanceId,componentNativeId,name,selectedIndex,graph);return true;
    }
    bool setNodeSize(int instanceId,int nativeId,float width,float height,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);Instance* inst=instance(instanceId);InstanceNode* target=node(instanceId,nativeId);if(!inst||!target)return false;resizeNode(*inst,*target,width,height,graph);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::Size,target->width,target->height);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::XY,target->x,target->y);return true;}
    bool clearNodeRelations(int instanceId, int nativeId, sg::SceneGraph& graph) {
        FacadeCommitScope commit(*this, graph);
        InstanceNode* target = node(instanceId, nativeId);
        if (!target) return false;
        if (!target->relations.empty()) {
            target->relations.clear();
            target->touchFacade(FD_ATTRS);
        }
        return true;
    }
    // 步骤3(B1 兜底):紧凑几何查询——只在「JS 影子判等即将跳写」时调用,回答 C++ semantic
    // authority 的 x/y/width/height/scaleX/scaleY(InstanceNode 语义值,非 SceneGraph 槽——槽含
    // pivot/文本 offset/display override,不能当 GObject 账本用)。
    bool nodeGeometry(int instanceId,int nativeId,float out[6]){
        Instance* inst=instance(instanceId);InstanceNode* n=inst?nodeIn(*inst,nativeId):nullptr;if(!n)return false;
        out[0]=n->x;out[1]=n->y;out[2]=n->width;out[3]=n->height;out[4]=n->scaleX;out[5]=n->scaleY;return true;
    }
    // 写传播计数(fanout):本次写里除目标自身外被标记过的其它节点数——moveNode 的 applyRelations / moveGroupMembers、批提交里的 gear 级联。
    // JS 侧 push 模式据此决定"写后是否立即同步 facade"(FairyGUI setXY/setSize 的传播是同步可观察的),不再靠猜 kind。
    int lastWriteFanout = 0;
    // 传播 = 本次写里收到**几何**标记(FD_GEOMETRY|FD_DERIVED_SIZE)、且自身不是几何写目标的节点数。排除表只放几何写(Position/Size/
    // DisplayPosition/Reparent/Scale/Rotation/Pivot/Display*)的目标——同批对 B 写 alpha 不能把"B 被 A 的 relation 移动"这一传播抵消掉。
    static bool geometryKind(MutationKind k) { return k == MutationKind::Position || k == MutationKind::Size || k == MutationKind::DisplayPosition || k == MutationKind::Reparent || k == MutationKind::Scale || k == MutationKind::Rotation || k == MutationKind::Pivot || k == MutationKind::DisplayScale || k == MutationKind::DisplayRotation; }
    static int fanoutSince(size_t from, const int* geometryTargets, size_t targetCount) {
        const auto& t = FacadeSeqTracker::touched; const auto& b = FacadeSeqTracker::touchedBits; if (t.size() <= from || b.size() != t.size()) return 0;
        std::vector<int> ids; ids.reserve(t.size() - from);
        for (size_t i = from; i < t.size(); ++i) if (b[i] & (FD_GEOMETRY | FD_DERIVED_SIZE)) ids.push_back(t[i]);
        std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        int n = 0; for (int id : ids) { bool isTarget = false; for (size_t i = 0; i < targetCount; ++i) if (geometryTargets[i] == id) { isTarget = true; break; } if (!isTarget) ++n; }
        return n;
    }
    bool setNodePosition(int instanceId,int nativeId,float x,float y,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);lastWriteFanout=0;Instance* inst=instance(instanceId);InstanceNode* target=inst?nodeIn(*inst,nativeId):nullptr;if(!target||!std::isfinite(x)||!std::isfinite(y))return false;const size_t from=FacadeSeqTracker::touched.size();moveNode(*inst,*target,x,y,graph);rememberCurrentXYGearValue(*inst,*target);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::XY,target->x,target->y);lastWriteFanout=fanoutSince(from,&nativeId,1);return true;}
    bool setNodeScale(int instanceId,int nativeId,float scaleX,float scaleY,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);InstanceNode* target=node(instanceId,nativeId);if(!target||!std::isfinite(scaleX)||!std::isfinite(scaleY))return false;traceScaleWrite(*target,"js-bridge",target->scaleX,scaleX);target->directDisplayScaleOverride=false;setSemanticScale(*target,scaleX,scaleY);writeNode(graph,*target);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::Scale,target->scaleX,target->scaleY);return true;}
    bool setNodeRotation(int instanceId,int nativeId,float rotation,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);InstanceNode* target=node(instanceId,nativeId);if(!target||!std::isfinite(rotation))return false;target->directDisplayRotationOverride=false;setSemanticRotation(*target,rotation);writeNode(graph,*target);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::Rotation,target->rotation);return true;}
    bool setNodeAlpha(int instanceId,int nativeId,float alpha,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);InstanceNode* target=node(instanceId,nativeId);if(!target||!std::isfinite(alpha))return false;target->alpha=alpha;writeNode(graph,*target);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::Alpha,target->alpha);return true;}
    bool setNodePivot(int instanceId,int nativeId,float pivotX,float pivotY,bool pivotAsAnchor,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* target=node(instanceId,nativeId);
        if(!target||!std::isfinite(pivotX)||!std::isfinite(pivotY))return false;
        setSemanticPivot(*target,pivotX,pivotY,pivotAsAnchor);
        writeNode(graph,*target);supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::Pivot,target->pivotX,target->pivotY);return true;
    }
    bool setNodeDisplayPosition(int instanceId,int nativeId,float x,float y,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        Instance* inst=instance(instanceId);InstanceNode* target=inst?nodeIn(*inst,nativeId):nullptr;if(!target||!std::isfinite(x)||!std::isfinite(y))return false;
        // This API represents a raw Pixi displayObject.position write, not GObject.setXY. The old
        // bridge converted the requested display coordinate back into semantic x/y and called
        // moveNode(), which also propagated Relations. A temporary Spine-slot move therefore
        // poisoned FairyGUI's authoritative layout and was resurrected when the object was
        // reparented. Store only the delta from the currently published display coordinate.
        const auto current=publishedDisplayPosition(*target);
        target->directDisplayOffsetX += x-current.first;
        target->directDisplayOffsetY += y-current.second;
        writeNode(graph,*target);return true;
    }
    bool setNodeDisplayScale(int instanceId,int nativeId,float scaleX,float scaleY,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* target=node(instanceId,nativeId);
        if(!target||!std::isfinite(scaleX)||!std::isfinite(scaleY))return false;
        target->directDisplayScaleX=scaleX;target->directDisplayScaleY=scaleY;
        target->directDisplayScaleOverride=true;
        writeNode(graph,*target);return true;
    }
    bool setNodeDisplayRotation(int instanceId,int nativeId,float rotation,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* target=node(instanceId,nativeId);
        if(!target||!std::isfinite(rotation))return false;
        target->directDisplayRotation=rotation;target->directDisplayRotationOverride=true;
        writeNode(graph,*target);return true;
    }
    bool setNodeVisible(int instanceId,int nativeId,bool visible,sg::SceneGraph& graph,bool displayOnly=false){FacadeCommitScope __facadeCommit(*this,graph);
        Instance* inst=instance(instanceId);InstanceNode* target=inst?nodeIn(*inst,nativeId):nullptr;if(!target)return false;
        if(displayOnly){
            // `displayObject.visible` is below FairyGUI's semantic GObject visibility.  In the
            // browser a GearDisplay-hidden child is detached from its owning GComponent's display
            // list, so setting its Pixi display object visible cannot expose it there. Native keeps
            // that authored display edge attached and composes the direct flag with gear/group
            // state. A raw displayObject mounted under another container is handled separately by
            // effectiveVisible(), matching GComponent.childStateChanged's parent-checked removal.
            // Keep this layer in the authoritative node model.  Writing only the shared buffer
            // is transient: a later position/size/controller/transition write republishes the
            // node and would otherwise restore the authored visible=true.  Parentless GObjects
            // mounted into a Spine slot rely exclusively on this display layer because they do
            // not have a semantic GComponent parent to call childStateChanged().
            target->displayVisible=visible;target->facadeSchemaPristine=false;
            writeNodeVisibility(graph,*target);return true;
        }
        target->visible=visible;target->facadeSchemaPristine=false;writeNodeVisibility(graph,*target);if(target->kind==ObjectKind::Group)refreshGroupVisibility(*inst,graph);return true;
    }
    // Preserve the exact JavaScript mutation order while paying a single JSC crossing.  Unlike a
    // last-write-wins property bag, this stream deliberately retains A1,B1,A2 because controller
    // gears and relation propagation may observe every intermediate mutation.  The complete
    // stream is validated against a shadow display/semantic tree before the first live write, so
    // an invalid late reparent cannot leave an earlier controller or resize partially published.
    bool commitMutations(const std::vector<MutationWrite>& writes, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        lastWriteFanout=0;
        if (writes.empty() || writes.size() > 2048) return false;
        std::unordered_map<int, int> displayParents;
        std::unordered_map<uint64_t, int> semanticParents;
        auto semanticKey=[](int instanceId,int nodeId)->uint64_t{
            return (static_cast<uint64_t>(static_cast<uint32_t>(instanceId))<<32)|
                   static_cast<uint32_t>(nodeId);
        };
        auto displayParent=[&](int nodeId)->int{
            auto it=displayParents.find(nodeId);
            return it==displayParents.end()?graph.nodes[nodeId].parent:it->second;
        };
        auto semanticParent=[&](int instanceId,InstanceNode* target)->int{
            auto it=semanticParents.find(semanticKey(instanceId,target->nativeId));
            return it==semanticParents.end()?target->parentNativeId:it->second;
        };

        for (const MutationWrite& write : writes) {
            if (write.instanceId <= 0 || write.targetId <= 0) return false;
            Instance* inst=instance(write.instanceId);
            if (!inst) return false;
            switch (write.kind) {
                case MutationKind::Controller:
                    if (write.name.empty() || write.name.size() > 1024 ||
                        !canController(write.instanceId,write.targetId,write.name,write.value0,false))
                        return false;
                    break;
                case MutationKind::Size:
                    if (!nodeIn(*inst,write.targetId) || !std::isfinite(write.number0) ||
                        !std::isfinite(write.number1) || write.number0 < 0 || write.number1 < 0)
                        return false;
                    break;
                case MutationKind::Position:
                case MutationKind::DisplayPosition:
                case MutationKind::Scale:
                case MutationKind::DisplayScale:
                case MutationKind::Pivot:
                    if (!nodeIn(*inst,write.targetId) || !std::isfinite(write.number0) ||
                        !std::isfinite(write.number1) ||
                        ((write.kind==MutationKind::Pivot ||
                          write.kind==MutationKind::DisplayScale) &&
                         (write.value0!=0 && write.value0!=1)) ||
                        (write.kind==MutationKind::DisplayScale &&
                         write.value1!=0 && write.value1!=1))
                        return false;
                    break;
                case MutationKind::Alpha:
                case MutationKind::Rotation:
                case MutationKind::DisplayRotation:
                    if (!nodeIn(*inst,write.targetId) || !std::isfinite(write.number0)) return false;
                    break;
                case MutationKind::Reparent: {
                    const int childId=write.targetId,semanticParentId=write.value0,
                              displayParentId=write.value1,beforeId=write.value2;
                    if (childId >= sg::NODE_CAP || !graph.nodes[childId].alive ||
                        semanticParentId < -1 || displayParentId < -1 || beforeId < -1)
                        return false;
                    InstanceNode* child=nodeIn(*inst,childId);
                    InstanceNode* nextSemantic=semanticParentId>0?nodeIn(*inst,semanticParentId):nullptr;
                    if (semanticParentId>0&&!nextSemantic) return false;
                    if (displayParentId>0&&(displayParentId>=sg::NODE_CAP||
                                           !graph.nodes[displayParentId].alive)) return false;
                    if (beforeId>0&&(beforeId>=sg::NODE_CAP||!graph.nodes[beforeId].alive||
                                     displayParent(beforeId)!=displayParentId)) return false;
                    if (childId==semanticParentId||childId==displayParentId||childId==beforeId)
                        return false;

                    // Validate display cycles against earlier rows in this same transaction.
                    for (int at=displayParentId,guard=0;at>0&&guard<sg::NODE_CAP;++guard) {
                        if (at==childId) return false;
                        at=displayParent(at);
                    }
                    // Native-managed children also maintain FairyGUI's semantic ownership.  Its
                    // cycle check is independent from the display container (lists/scroll panes
                    // intentionally use different semantic and display parents).
                    int semanticGuard=0;
                    if (child) for (InstanceNode* at=nextSemantic;at&&semanticGuard++<sg::NODE_CAP;) {
                        if (at==child) return false;
                        const int parentId=semanticParent(write.instanceId,at);
                        at=parentId>0?nodeIn(*inst,parentId):nullptr;
                    }
                    if(semanticGuard>=sg::NODE_CAP)return false;
                    displayParents[childId]=displayParentId;
                    if (child) semanticParents[semanticKey(write.instanceId,childId)]=semanticParentId;
                    break;
                }
                default:return false;
            }
        }

        // 传播按 mutation 顺序逐行累计(评审 P1:批尾用无序目标集合反推会漏——先写成员 B 的位置、再移动 group 传播改 B,B 是几何目标
        // 被排除 ⇒ fanout=0,JS 保留第一次写入的位置)。每行只排除**本行自己的目标**,几何标记落在其它任何节点都算传播。
        int fanout = 0;
        for (const MutationWrite& write : writes) { const size_t wf = FacadeSeqTracker::touched.size(); switch (write.kind) {
            case MutationKind::Controller:
                applyControllerValidated(write.instanceId,write.targetId,write.name,write.value0,graph);
                break;
            case MutationKind::Size: {
                Instance* inst=instance(write.instanceId);InstanceNode* target=nodeIn(*inst,write.targetId);
                resizeNode(*inst,*target,write.number0,write.number1,graph);
                supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::Size,target->width,target->height);
                supersedeQueuedTransitionProperty(*target,TransitionPropertyKind::XY,target->x,target->y);break;
            }
            case MutationKind::Reparent:
                (void)reparent(write.instanceId,write.targetId,write.value0,write.value1,write.value2,graph);
                break;
            case MutationKind::Position: {
                (void)setNodePosition(write.instanceId,write.targetId,write.number0,write.number1,graph);break;
            }
            case MutationKind::DisplayPosition:
                (void)setNodeDisplayPosition(write.instanceId,write.targetId,write.number0,write.number1,graph);break;
            case MutationKind::Scale:
                (void)setNodeScale(write.instanceId,write.targetId,write.number0,write.number1,graph);break;
            case MutationKind::Rotation:
                (void)setNodeRotation(write.instanceId,write.targetId,write.number0,graph);break;
            case MutationKind::DisplayRotation:
                (void)setNodeDisplayRotation(write.instanceId,write.targetId,write.number0,graph);break;
            case MutationKind::DisplayScale:
                (void)setNodeDisplayScale(write.instanceId,write.targetId,write.number0,write.number1,
                                          graph);break;
            case MutationKind::Alpha:
                (void)setNodeAlpha(write.instanceId,write.targetId,write.number0,graph);break;
            case MutationKind::Pivot:
                (void)setNodePivot(write.instanceId,write.targetId,write.number0,write.number1,write.value0!=0,graph);break;
        } fanout += fanoutSince(wf, &write.targetId, 1); }
        lastWriteFanout = fanout;
        return true;
    }
    bool claimTextFacadeVisual(int nativeId,const std::string& text,float width,float height,float lineHeight,
                               sg::SceneGraph& graph,const std::string& align=std::string(),
                               const std::string& verticalAlign=std::string(),int autoSize=-1){FacadeCommitScope __facadeCommit(*this,graph);
        auto owner=managedNodeOwners_.find(nativeId);if(owner==managedNodeOwners_.end())return false;
        Instance* inst=owner->second;if(!inst||instance(inst->id)!=inst)return false;InstanceNode* target=nodeIn(*inst,nativeId);
        if(!target||(target->kind!=ObjectKind::Text&&target->kind!=ObjectKind::RichText&&target->kind!=ObjectKind::InputText))return false;
        // Rich/input proxies share this host update boundary. They do not use ordinary-text
        // surface ownership, but their live content must still invalidate an exact JS schema.
        target->facadeSchemaPristine=false;target->attrs["text"]=text;FacadeSeqTracker::mark(graph,target->nativeId,FD_ATTRS|FD_TEXT);
        if(!isOrdinaryText(*target))return false;
        target->nativeTextFacadeOwned=true;
        // GTextField.align is mutable at runtime.  The CoreText request already carries the live
        // TextStyle alignment, but the fixed-box offset below used to keep reading only the value
        // parsed from package XML.  A field authored with the default `left` and changed to
        // `center` by business code therefore rasterised the right glyphs while its native node
        // stayed at the left edge of the box.  Make the ownership hand-off include the current
        // layout contract as well as pixels/metrics.  Empty/unknown values preserve the authored
        // metadata so ordinary PIXI.Text nodes cannot mutate an adopted FairyGUI box accidentally.
        if(align=="left"||align=="center"||align=="right"||align=="end")target->attrs["align"]=align;
        if(verticalAlign=="top"||verticalAlign=="middle"||verticalAlign=="bottom")target->attrs["vAlign"]=verticalAlign;
        // GTextField.autoSize is a live layout property, not immutable package metadata.  Shrink
        // keeps the authored box and applies the facade display scale before centre/right and
        // middle/bottom offsets are calculated.  Retaining an authored None value after business
        // code switches to Shrink applies the full unscaled overflow as a negative offset and
        // makes long localised button titles snap back toward the source component's left edge.
        static const char* autoSizeNames[] = {"none", "both", "height", "shrink"};
        if(autoSize>=0&&autoSize<4)target->attrs["autoSize"]=autoSizeNames[autoSize];
        // NativeTextProxy publishes the same natural-size surface used by Pixi.Text. FairyGUI
        // owns the fixed-box center/right/vertical offset around that surface. Remember the mode,
        // not just the ownership bit: a later button controller/hover refresh otherwise repaints
        // the authored fixed box and silently replaces the facade geometry/metrics, which makes
        // titles snap to the left (and then jump again on pointer-over).
        target->nativeTextFacadeNatural=true;FacadeSeqTracker::note(target->nativeId,FD_TEXT);
        ++textOwnershipStats_.published;
        target->nativeTextMetricsValid=std::isfinite(width)&&std::isfinite(height)&&std::isfinite(lineHeight)&&width>=0&&height>=0&&lineHeight>=0;
        target->nativeTextWidth=target->nativeTextMetricsValid?width:0;target->nativeTextHeight=target->nativeTextMetricsValid?height:0;
        target->nativeTextLineHeight=target->nativeTextMetricsValid?lineHeight:0;
        // Taking ownership changes the positioning basis from the authored fixed box to the
        // facade's natural-size texture. Publish that basis change in the same transaction:
        // waiting for a later JS setPosition leaves a materialized fixed-box text at its authored
        // origin whenever FairyGUI legitimately suppresses a same-value layout update.
        writeNode(graph,*target);
        return true;
    }
    bool setBitmapText(int instanceId,int nativeId,const std::string& text,float fontSize,
                       uint32_t color,float letterSpacing,float leading,const std::string& align,
                       const std::string& verticalAlign,bool singleLine,int autoSize,
                       sg::SceneGraph& graph,BitmapTextMetrics* metrics=nullptr,bool forceWrite=false,
                       float localOffsetY=std::numeric_limits<float>::quiet_NaN()){FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* target=node(instanceId,nativeId);if(!target||target->kind!=ObjectKind::Text)return false;
        auto font=target->attrs.find("font");if(font==target->attrs.end()||font->second.compare(0,5,"ui://")!=0)return false;
        Package* pkg=package(target->packageName);if(!pkg)return false;
        auto attrNumber=[&](const char* key,float fallback=0){auto it=target->attrs.find(key);return it==target->attrs.end()?fallback:number(it->second,fallback);};
        auto attrString=[&](const char* key)->std::string{auto it=target->attrs.find(key);return it==target->attrs.end()?std::string():it->second;};
        static const char* autoSizes[]={"none","both","height","shrink"};autoSize=std::max(0,std::min(3,autoSize));
        std::string oldAlign=attrString("align");if(oldAlign.empty())oldAlign="left";std::string oldVertical=attrString("vAlign");if(oldVertical.empty())oldVertical="top";std::string oldAutoSize=attrString("autoSize");if(oldAutoSize.empty())oldAutoSize="both";
        bool hasLocalOffsetY=std::isfinite(localOffsetY);
        bool localOffsetChanged=hasLocalOffsetY&&
            (!std::isfinite(target->bitmapLocalOffsetY)||std::fabs(target->bitmapLocalOffsetY-localOffsetY)>.0001f);
        bool changed=attrString("text")!=text||std::fabs(attrNumber("fontSize",fontSize)-fontSize)>.0001f||
            visualTint(*target)!=(color&0xFFFFFFu)||std::fabs(attrNumber("letterSpacing")-letterSpacing)>.0001f||
            std::fabs(attrNumber("leading",3)-leading)>.0001f||oldAlign!=align||
            oldVertical!=verticalAlign||(attrString("singleLine")=="true")!=singleLine||
            oldAutoSize!=autoSizes[autoSize]||localOffsetChanged;
        target->facadeSchemaPristine=false;target->attrs["text"]=text;FacadeSeqTracker::mark(graph,target->nativeId,FD_ATTRS|FD_TEXT);
        if(fontSize>0)target->attrs["fontSize"]=std::to_string(fontSize);
        char css[16];std::snprintf(css,sizeof(css),"#%06x",color&0xFFFFFFu);target->attrs["color"]=css;
        target->attrs["letterSpacing"]=std::to_string(letterSpacing);target->attrs["leading"]=std::to_string(leading);
        target->attrs["align"]=align;target->attrs["vAlign"]=verticalAlign;target->attrs["singleLine"]=singleLine?"true":"false";target->attrs["autoSize"]=autoSizes[autoSize];
        if(hasLocalOffsetY)target->bitmapLocalOffsetY=localOffsetY;
        // shrink(3) 保持 authored 框(对齐框修复的配套):metrics.width 不得回报内容宽,否则 JS 桥的
        // setSize 会把字段收缩、对齐框随之丢失(仅 both(1) 才把宽交给内容)。
        bool wrote=changed||forceWrite;bool ok=!wrote||writeBitmapText(graph,*target);if(metrics){metrics->textWidth=target->bitmapTextWidth;metrics->textHeight=target->bitmapTextHeight;metrics->width=autoSize==1?target->bitmapTextWidth:target->width;metrics->height=(autoSize==1||autoSize==2)?target->bitmapTextHeight:target->height;metrics->glyphCount=target->bitmapGlyphCount;metrics->trusted=target->bitmapMetricsTrusted;metrics->wrote=wrote;}return ok;
    }
    void destroy(int id, sg::SceneGraph& graph, uint64_t frameSerial = 0, double nowMs = 0) {FacadeCommitScope __facadeCommit(*this,graph);
        auto checkout = checkouts_.find(id);
        if (checkout != checkouts_.end()) {
            const std::string policyId = checkout->second ? checkout->second->reservationPolicyKey : std::string();
            if (checkout->second) { decrementCheckoutPolicy(*checkout->second); destroyTree(*checkout->second, graph); }
            checkouts_.erase(checkout); ++checkoutStats_.canceled;
            auto policyIt = reservationPolicies_.find(policyId);
            if (policyIt != reservationPolicies_.end()) {
                ReservationPolicy& policy = policyIt->second;
                if (policy.enabled && policy.dirty && policy.liveCount == 0 && policy.checkoutCount == 0 &&
                    policy.blockedOn.empty()) {
                    policy.nextEligibleFrame = frameSerial == std::numeric_limits<uint64_t>::max() ? frameSerial : frameSerial + 1;
                    enqueueReservationPolicy(policy, policy.nextEligibleFrame);
                }
            }
            return;
        }
        auto it = instances_.find(id); if (it == instances_.end()) return;
#if PIXINATIVE_DIAG_LOG
        if (it->second && it->second->packageName == "ju_bar_hud") {
            size_t transitionCount = 0;
            for (const auto& active : activeTransitions_) if (active.instanceId == id) ++transitionCount;
            std::fprintf(stderr,
                "[native-fgui-transition] destroy hud inst=%d root=%d component=%s active=%zu now=%.3f frame=%llu\n",
                id, it->second->root, it->second->componentName.c_str(), transitionCount, nowMs,
                static_cast<unsigned long long>(frameSerial));
        }
#endif
        const std::string policyId = it->second->reservationPolicyKey;
        activeTransitions_.erase(std::remove_if(activeTransitions_.begin(),activeTransitions_.end(),[&](const ActiveTransition& a){return a.instanceId==id;}),activeTransitions_.end());
        // Live business state is never recycled. Text, controllers, reparented nodes and external
        // children may all have changed; a later refill always clones the immutable prototype.
        destroyTree(*it->second, graph); instances_.erase(it);
        auto policyIt = reservationPolicies_.find(policyId); if (policyIt == reservationPolicies_.end()) return;
        ReservationPolicy& policy = policyIt->second; const bool hadLive = policy.liveCount > 0;
        if (hadLive) --policy.liveCount;
        if (hadLive && policy.liveCount == 0 && policy.checkoutCount == 0) {
            policy.dirty = true; policy.lastLiveDestroyedMs = nowMs;
            policy.nextEligibleFrame = frameSerial == std::numeric_limits<uint64_t>::max() ? frameSerial : frameSerial + 1;
            if (policy.enabled && policy.blockedOn.empty()) enqueueReservationPolicy(policy, policy.nextEligibleFrame);
        }
    }
    // Refill work is intentionally separate from scheduledTasks_: those tasks publish completion
    // tokens back to JavaScript, whereas reservation maintenance is entirely Native-owned. At
    // most one clean detached instance is constructed per actual presentation serial.
    size_t advanceReservationRefills(double nowMs, uint64_t frameSerial, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        if (hasReservationRefillFrame_ && lastReservationRefillFrame_ == frameSerial) return 0;
        hasReservationRefillFrame_ = true; lastReservationRefillFrame_ = frameSerial;
        size_t candidates = reservationRefillQueue_.size();
        while (candidates-- && !reservationRefillQueue_.empty()) {
            ReservationRefillTicket ticket = std::move(reservationRefillQueue_.front()); reservationRefillQueue_.pop_front();
            auto found = reservationPolicies_.find(ticket.policyId); if (found == reservationPolicies_.end()) continue;
            ReservationPolicy& policy = found->second;
            if (ticket.epoch != policy.epoch || !policy.queued) continue;
            policy.queued = false;
            if (!policy.enabled || !policy.dirty || policy.liveCount > 0 || policy.checkoutCount > 0 || !policy.blockedOn.empty()) continue;
            if (frameSerial < policy.nextEligibleFrame) {
                policy.queued = true; reservationRefillQueue_.push_back({policy.id, policy.epoch}); continue;
            }
            Package* pkg = package(policy.packageName);
            if (!pkg) { blockReservationPolicy(policy, "missing:" + policy.packageName); continue; }
            if (!pkg->resourcesPrepared && !pkg->requiredAtlases.empty()) {
                blockReservationPolicy(policy, "resources:" + pkg->name); continue;
            }
            Resource* resource = resourceByName(*pkg, policy.componentName);
            if (!resource || resource->kind != ResourceKind::Component) {
                policy.enabled = false; policy.dirty = false; ++reservationStats_.failed; continue;
            }
            auto component = pkg->components.find(resource->id);
            if (component == pkg->components.end() || !component->second->xml) {
                policy.enabled = false; policy.dirty = false; ++reservationStats_.failed; continue;
            }
            ReservationBuildResult result = buildReservationOne(*pkg, *resource, *component->second,
                policy.componentName, policy.target, graph, nowMs, frameSerial, &policy, true);
            if (result == ReservationBuildResult::Built) {
                ++reservationStats_.refillFrames;
                policy.dirty = reservationPoolSize(*pkg, *resource) < policy.target;
                const uint64_t nextFrame = frameSerial == std::numeric_limits<uint64_t>::max() ? frameSerial : frameSerial + 1;
                if (policy.dirty && policy.liveCount == 0 && policy.checkoutCount == 0) enqueueReservationPolicy(policy, nextFrame);
                return 1;
            }
            if (result == ReservationBuildResult::Complete) policy.dirty = false;
            else if (result == ReservationBuildResult::Failed) { policy.enabled = false; policy.dirty = false; }
            // A dependency block deliberately leaves no queued ticket. preparePackage/bindAtlas
            // wakes only policies that name the package which actually became ready.
        }
        return 0;
    }
    size_t packageCount() const { return packagesByName_.size(); }
    size_t preparedPackageCount() const { size_t count=0;for(const auto& item:packagesByName_)if(item.second&&item.second->resourcesPrepared)count++;return count; }
    size_t instanceCount() const { return instances_.size(); }
    size_t checkedOutInstanceCount() const { return checkouts_.size(); }
    const TemplateStats& templateStats() const { return templateStats_; }
    const ReservationStats& reservationStats() const { return reservationStats_; }
    const CheckoutStats& checkoutStats() const { return checkoutStats_; }
    const TextOwnershipStats& textOwnershipStats() const { return textOwnershipStats_; }
    const std::string& reservationBlock() const { return lastReservationBlock_; }
    const std::string& createFailure() const { return lastCreateFailure_; }
    std::vector<CompatibilityIssue> compatibilityIssues() const {
        std::vector<CompatibilityIssue> out;
        for (const auto& packageEntry : packagesByName_) {
            const Package* pkg = packageEntry.second.get();
            if (!pkg) continue;
            for (const auto& componentEntry : pkg->components) {
                const Component* component = componentEntry.second.get();
                if (!component || !component->hasUnsupportedStaticNode) continue;
                out.push_back({pkg->name, component->name, component->unsupportedReason});
            }
        }
        std::sort(out.begin(), out.end(), [](const CompatibilityIssue& a, const CompatibilityIssue& b) {
            if (a.packageName != b.packageName) return a.packageName < b.packageName;
            if (a.componentName != b.componentName) return a.componentName < b.componentName;
            return a.reason < b.reason;
        });
        return out;
    }
    size_t reservedInstanceCount() const { size_t count=0;for(const auto& pool:reservations_)count+=pool.second.size();return count; }
    size_t reservationPolicyCount() const { return reservationPolicies_.size(); }
    size_t pendingReservationRefillCount() const { size_t count=0;for(const auto& item:reservationPolicies_)if(item.second.queued)++count;return count; }
    size_t reservationPolicyLiveCount() const { size_t count=0;for(const auto& item:reservationPolicies_)count+=item.second.liveCount;return count; }
    CoverageStats coverage(const sg::SceneGraph& graph) const {
        CoverageStats result;
        if (!graph.buf) return result;
        for (const auto& pair : instances_) {
            if (!pair.second) continue;
            for (const auto& owned : pair.second->nodes) {
                if (!owned) continue;
                ++result.nodes;
                const InstanceNode& n = *owned;
                if (n.kind != ObjectKind::Text) continue;
                auto textIt = n.attrs.find("text");
                std::string text = textIt == n.attrs.end() ? std::string() : richTextPlain(textIt->second);
                bool hasVisibleText = false;
                for (unsigned char c : text) if (!std::isspace(c)) { hasVisibleText = true; break; }
                if (!hasVisibleText) continue;
                ++result.nonEmptyTextNodes;
                if (n.nativeId <= 0 || n.nativeId >= sg::NODE_CAP) {
                    ++result.textFailures;
                    if(result.failureSamples.size()<16)result.failureSamples.push_back(n.packageName+"/"+n.name+" id="+std::to_string(n.nativeId)+" invalid text="+text.substr(0,48));
                    continue;
                }
                const float* slot = graph.buf + static_cast<size_t>(n.nativeId) * sg::STRIDE;
                auto fontIt = n.attrs.find("font");
                bool bitmap = fontIt != n.attrs.end() && fontIt->second.compare(0, 5, "ui://") == 0;
                if (bitmap) {
                    ++result.bitmapTextNodes;
                    if (static_cast<int>(slot[sg::TYPE]) != sg::T_NATIVE_MULTIQUAD || !(slot[sg::QUADCNT] > 0)) {
                        ++result.textFailures;
                        ++result.bitmapTextFailures;
                        if(result.failureSamples.size()<16)result.failureSamples.push_back(n.packageName+"/"+n.name+" id="+std::to_string(n.nativeId)+" bitmap type="+std::to_string((int)slot[sg::TYPE])+" quads="+std::to_string((int)slot[sg::QUADCNT])+" font="+fontIt->second+" text="+text.substr(0,48));
                    }
                } else if (static_cast<int>(slot[sg::TYPE]) != sg::T_SPRITE || !(slot[sg::TEXID] > 0)) {
                    ++result.textFailures;
                    if(result.failureSamples.size()<16)result.failureSamples.push_back(n.packageName+"/"+n.name+" id="+std::to_string(n.nativeId)+" text type="+std::to_string((int)slot[sg::TYPE])+" tex="+std::to_string((int)slot[sg::TEXID])+" text="+text.substr(0,48));
                }
            }
        }
        return result;
    }
    size_t movieClipResourceCount() const { size_t count=0;for(const auto& packageItem:packagesByName_)if(packageItem.second)for(const auto& resource:packageItem.second->resourcesById)if(resource.second.kind==ResourceKind::MovieClip&&!resource.second.movieFrames.empty())count++;return count; }
    size_t movieClipNodeCount() const { size_t count=0;for(const auto& instanceItem:instances_)if(instanceItem.second)for(const auto& owned:instanceItem.second->nodes)if(owned&&owned->kind==ObjectKind::MovieClip)count++;return count; }
    int movieClipState(int instanceId,int nativeId) const { InstanceNode* target=node(instanceId,nativeId);if(!target||target->kind!=ObjectKind::MovieClip)return -1;auto playing=target->attrs.find("playing");bool enabled=playing==target->attrs.end()||(playing->second!="false"&&playing->second!="0");return target->movieFrame*2+(enabled?1:0); }
    bool setMovieClipState(int instanceId,int nativeId,int frame,int playing,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* target=node(instanceId,nativeId);if(!target||target->kind!=ObjectKind::MovieClip)return false;Package* pkg=package(target->packageName);if(!pkg)return false;auto resource=pkg->resourcesById.find(target->sourceId);if(resource==pkg->resourcesById.end()||resource->second.movieFrames.empty())return false;
        target->facadeSchemaPristine=false;
        if(frame>=0){target->movieFrame=std::max(0,std::min((int)resource->second.movieFrames.size()-1,frame));target->movieRepeated=0;target->movieReversed=false;target->movieElapsedMs=0;target->attrs["frame"]=std::to_string(target->movieFrame);}
        if(playing>=0)target->attrs["playing"]=playing?"true":"false";FacadeSeqTracker::mark(graph,target->nativeId,FD_ATTRS);return writeMovieClip(graph,*pkg,resource->second,*target);
    }
    int playTransition(int instanceId, int componentId, const std::string& name, double nowMs, int times, double delaySeconds, bool reversed, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        const bool traceHud = PIXINATIVE_DIAG_LOG && (name == "lobbyIn" || name == "lobbyOut" ||
            name == "machineIn" || name == "machineOut");
        Instance* live = liveInstance(instanceId);
        if (!live) {
            if (traceHud) std::fprintf(stderr,
                "[native-fgui-transition] play FAIL reason=instance-missing inst=%d component=%d name=%s now=%.3f live=%zu checkout=%zu\n",
                instanceId, componentId, name.c_str(), nowMs, instances_.size(), checkouts_.size());
            return 0;
        }
        InstanceNode* component = node(instanceId, componentId);
        if (!component) {
            if (PIXINATIVE_DIAG_LOG && (traceHud || live->packageName == "ju_bar_hud")) std::fprintf(stderr,
                "[native-fgui-transition] play FAIL reason=component-missing inst=%d root=%d pkg=%s object=%s component=%d name=%s now=%.3f nodes=%zu\n",
                instanceId, live->root, live->packageName.c_str(), live->componentName.c_str(),
                componentId, name.c_str(), nowMs, live->byNativeId.size());
            return 0;
        }
        const TransitionDesc* desc = transition(*component, name);
        if (!desc) {
            std::string available;
            for (const auto& candidate : component->transitions) {
                if (!available.empty()) available += ',';
                available += candidate.name;
            }
            if (PIXINATIVE_DIAG_LOG && (traceHud || live->packageName == "ju_bar_hud")) std::fprintf(stderr,
                "[native-fgui-transition] play FAIL reason=transition-missing inst=%d root=%d pkg=%s object=%s component=%d node=%s nodePkg=%s name=%s available=[%s]\n",
                instanceId, live->root, live->packageName.c_str(), live->componentName.c_str(),
                componentId, component->name.c_str(), component->packageName.c_str(), name.c_str(),
                available.c_str());
            return 0;
        }
        for(auto it=activeTransitions_.begin();it!=activeTransitions_.end();)if(it->instanceId==instanceId&&it->componentId==componentId&&it->name==name){releaseTransitionDisplayLocks(*it,graph);it=activeTransitions_.erase(it);}else ++it;
        // FairyGUI applies a transition made entirely of time-zero, non-tween items inside
        // Transition.play() itself. It never enters the playing state. This matters for reset
        // transitions: business code commonly calls reset.play() and immediately writes the
        // final button visibility/touchability. Deferring the reset to the next native frame
        // reverses that ordering and can put a visible LOCKED surface over the enabled PLAY
        // button. Keep the zero-task transition synchronous and do not enqueue it for a second
        // application on the following frame.
        if (delaySeconds <= 0 && transitionRunsSynchronously(*component,*desc,reversed)) {
            applyTransition(*component,*desc,0.0f,reversed,graph,0,component->x,component->y);
            if (traceHud) std::fprintf(stderr,
                "[native-fgui-transition] play immediate inst=%d root=%d pkg=%s object=%s component=%d node=%s name=%s now=%.3f items=%zu reversed=%d\n",
                instanceId, live->root, live->packageName.c_str(), live->componentName.c_str(),
                componentId, component->name.c_str(), name.c_str(), nowMs, desc->items.size(),
                reversed ? 1 : 0);
            return 0;
        }
        ActiveTransition active; active.token = nextTransitionToken_++; if (nextTransitionToken_ <= 0) nextTransitionToken_ = 1; active.instanceId = instanceId; active.componentId = componentId; active.name = name; active.startedMs = nowMs + std::max(0.0, delaySeconds) * 1000.0; active.times = times < 0 ? -1 : std::max(1, times); active.ownerBaseX = component->x; active.ownerBaseY = component->y; active.reversed = reversed;syncTransitionDisplayLocks(active,*component,*desc,-1,graph);activeTransitions_.push_back(std::move(active));
        if (traceHud) {
            const float span = transitionRuntimeSpan(*component, *desc, reversed);
            std::fprintf(stderr,
                "[native-fgui-transition] play OK token=%d inst=%d root=%d pkg=%s object=%s component=%d node=%s name=%s now=%.3f start=%.3f span=%.6f max=%.6f items=%zu times=%d reversed=%d active=%zu\n",
                activeTransitions_.back().token, instanceId, live->root, live->packageName.c_str(),
                live->componentName.c_str(), componentId, component->name.c_str(), name.c_str(), nowMs,
                activeTransitions_.back().startedMs, span, desc->maxTime, desc->items.size(),
                activeTransitions_.back().times, reversed ? 1 : 0, activeTransitions_.size());
        }
        return activeTransitions_.back().token;
    }
    bool stopTransition(int instanceId, int componentId, const std::string& name, bool setToComplete, bool processCallback, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        bool found = false;
        for (auto it = activeTransitions_.begin(); it != activeTransitions_.end();) if (it->instanceId == instanceId && it->componentId == componentId && it->name == name) {
            found = true; if (setToComplete) { InstanceNode* component = node(instanceId, componentId); const TransitionDesc* desc = component ? transition(*component, name) : nullptr; if (component && desc) {float span=transitionRuntimeSpan(*component,*desc,it->reversed);applyTransition(*component, *desc, std::isfinite(span)?span:desc->maxTime, it->reversed, graph, 0, it->ownerBaseX, it->ownerBaseY);} }
            if (processCallback) completedTransitions_.push_back(it->token);releaseTransitionDisplayLocks(*it,graph); it = activeTransitions_.erase(it);
        } else ++it;
        return found;
    }
    bool setTransitionValue(int instanceId, int componentId, const std::string& transitionName, const std::string& label, const std::vector<std::string>& values) {
        InstanceNode* component = node(instanceId, componentId); if (!component || !transition(*component, transitionName)) return false; component->transitionOverrides[transitionName + "\n" + label] = values; return true;
    }
    bool setTransitionTarget(int instanceId, int componentId, const std::string& transitionName, const std::string& label, const std::string& targetId, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        InstanceNode* component=node(instanceId,componentId);if(!component)return false;for(auto& desc:component->transitions)if(desc.name==transitionName){for(auto& item:desc.items)if(item.label==label){item.targetId=targetId;if(item.kind==TransitionKind::Text){InstanceNode* target=transitionTarget(*component,targetId);if(target){if(target->mutationCaps!=MutationNone)FacadeSeqTracker::mark(graph,target->nativeId,FD_ATTRS);target->mutationCaps=MutationNone;}}return true;}return false;}return false;
    }
    bool setTransitionDuration(int instanceId, int componentId, const std::string& transitionName, const std::string& label, float duration) {
        InstanceNode* component=node(instanceId,componentId);if(!component)return false;for(auto& desc:component->transitions)if(desc.name==transitionName){bool changed=false;for(auto& item:desc.items)if(item.tween&&item.label==label){item.duration=std::max(0.0f,duration);changed=true;}if(changed)recomputeTransitionDuration(desc);return changed;}return false;
    }
    bool updateTransitionFromRelations(int instanceId, int componentId, const std::string& transitionName, const std::string& targetId, float dx, float dy) {
        InstanceNode* component=node(instanceId,componentId);if(!component)return false;
        for(auto& desc:component->transitions)if(desc.name==transitionName){
            bool changed=false;std::unordered_set<std::string> overrideLabels;
            for(auto& item:desc.items)if(item.kind==TransitionKind::XY&&item.targetId==targetId){
                if(item.tween){adjustTransitionPair(item.startValue,dx,dy);adjustTransitionPair(item.endValue,dx,dy);if(!item.label.empty())overrideLabels.insert(item.label);if(!item.label2.empty())overrideLabels.insert(item.label2);}
                else{adjustTransitionPair(item.value,dx,dy);if(!item.label.empty())overrideLabels.insert(item.label);}
                changed=true;
            }
            // setValue() is represented by a Native-owned override instead of mutating the XML
            // descriptor. FairyGUI relation updates must move those live values as well.
            for(const std::string& label:overrideLabels){auto found=component->transitionOverrides.find(desc.name+"\n"+label);if(found!=component->transitionOverrides.end())adjustTransitionPair(found->second,dx,dy);}
            return changed;
        }
        return false;
    }
    bool changeTransitionRepeat(int instanceId, int componentId, const std::string& transitionName, int times) {
        bool changed=false;for(auto& active:activeTransitions_)if(active.instanceId==instanceId&&active.componentId==componentId&&active.name==transitionName){active.times=times<0?-1:std::max(1,times);changed=true;}return changed;
    }
    bool isTransitionPlaying(int instanceId,int componentId,const std::string& name) const {
        for(const auto& active:activeTransitions_)if(active.instanceId==instanceId&&active.componentId==componentId&&active.name==name)return true;
        return false;
    }
    const std::vector<int>& advanceTransitions(double nowMs, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        advanceMovieClips(nowMs, graph);
        for (auto it = activeTransitions_.begin(); it != activeTransitions_.end();) {
            InstanceNode* component = node(it->instanceId, it->componentId); const TransitionDesc* desc = component ? transition(*component, it->name) : nullptr;
            const bool traceHud = it->name == "lobbyIn" || it->name == "lobbyOut" ||
                it->name == "machineIn" || it->name == "machineOut";
            if (!component || !desc) {
                if (traceHud) std::fprintf(stderr,
                    "[native-fgui-transition] drop token=%d inst=%d component=%d name=%s reason=%s now=%.3f\n",
                    it->token, it->instanceId, it->componentId, it->name.c_str(),
                    component ? "transition-missing" : "component-missing", nowMs);
                releaseTransitionDisplayLocks(*it,graph);it = activeTransitions_.erase(it); continue;
            }
            if(nowMs+0.001<it->startedMs){++it;continue;}
            double elapsed = std::max(0.0, (nowMs - it->startedMs) / 1000.0); float span = transitionRuntimeSpan(*component,*desc,it->reversed);
            if(!std::isfinite(span)){
                if (traceHud && it->lastElapsed < 0) std::fprintf(stderr,
                    "[native-fgui-transition] infinite token=%d inst=%d component=%d name=%s now=%.3f start=%.3f span=%f\n",
                    it->token, it->instanceId, it->componentId, it->name.c_str(), nowMs, it->startedMs, span);
                emitTransitionSignals(*it,*component,*desc,span,it->lastElapsed,elapsed);it->lastElapsed=elapsed;syncTransitionDisplayLocks(*it,*component,*desc,static_cast<float>(elapsed),graph);applyTransition(*component,*desc,static_cast<float>(elapsed),it->reversed,graph,0,it->ownerBaseX,it->ownerBaseY);++it;continue;}
            int cycle = static_cast<int>(std::floor(elapsed / span));
            double capped=elapsed;if(it->times>=0)capped=std::min(capped,(double)span*it->times);emitTransitionSignals(*it,*component,*desc,span,it->lastElapsed,capped);it->lastElapsed=capped;
            if (it->times >= 0 && cycle >= it->times) {
                if (traceHud) std::fprintf(stderr,
                    "[native-fgui-transition] complete token=%d inst=%d component=%d name=%s now=%.3f start=%.3f elapsed=%.6f span=%.6f cycles=%d\n",
                    it->token, it->instanceId, it->componentId, it->name.c_str(), nowMs,
                    it->startedMs, elapsed, span, cycle);
                applyTransition(*component, *desc, span, it->reversed, graph, 0, it->ownerBaseX, it->ownerBaseY); completedTransitions_.push_back(it->token);releaseTransitionDisplayLocks(*it,graph); it = activeTransitions_.erase(it); continue; }
            float localTimeline=static_cast<float>(elapsed-cycle*span);syncTransitionDisplayLocks(*it,*component,*desc,localTimeline,graph);applyTransition(*component, *desc, localTimeline, it->reversed, graph, 0, it->ownerBaseX, it->ownerBaseY); ++it;
        }
        return completedTransitions_;
    }
    std::vector<int> takeCompletedTransitions() {
        std::vector<int> out;
        out.swap(completedTransitions_);
        return out;
    }
    std::vector<TransitionSignal> takeTransitionSignals(){std::vector<TransitionSignal> out;out.swap(transitionSignals_);return out;}
    std::vector<TransitionVisibilitySignal> takeTransitionVisibilitySignals(){std::vector<TransitionVisibilitySignal> out;out.swap(transitionVisibilitySignals_);return out;}
    std::vector<TransitionPropertySignal> takeTransitionPropertySignals(){std::vector<TransitionPropertySignal> out;out.swap(transitionPropertySignals_);return out;}
    // B1 裁决(journal 版本屏障):任何 native 自治写入 journal 都递增版本。JS 等值 setter
    // 在"即将被影子判等吞掉"时读版本,发现未消费则先 drain 影子再按正常 Router 写入——
    // 取代旧 __b1Reconcile 的每次等值读几何+强写纠偏(双驱动回归,已降级为诊断)。
    uint32_t journalEpoch() const { return journalEpoch_; }
    void scheduleTask(int token, double delayMs, int repeat, double nowMs) {
        if (token <= 0) return; auto found=scheduledTasks_.find(token);uint64_t order=found==scheduledTasks_.end()?nextTaskOrder_++:found->second.order; ScheduledTask task; task.token=token; task.delayMs=std::max(0.0,delayMs); task.repeat=repeat; task.dueMs=nowMs+task.delayMs;task.order=order;scheduledTasks_[token]=task;
    }
    void cancelTask(int token) { scheduledTasks_.erase(token); }
    void advanceTasks(double nowMs) {
        std::vector<std::pair<uint64_t,int>> due;due.reserve(scheduledTasks_.size());for(const auto& item:scheduledTasks_)if(nowMs+0.001>=item.second.dueMs)due.emplace_back(item.second.order,item.first);std::sort(due.begin(),due.end());
        for(const auto& ready:due){auto it=scheduledTasks_.find(ready.second);if(it==scheduledTasks_.end())continue;ScheduledTask& task=it->second;
            completedTasks_.push_back(task.token);
            if(task.repeat>0&&--task.repeat==0){scheduledTasks_.erase(it);continue;}
            // Match GTimer: a delayed loop fires at most once per presentation even after a hitch.
            // Zero/one-millisecond callLater tasks are also deferred to the next native frame.
            task.dueMs=nowMs+std::max(0.001,task.delayMs);
        }
    }
    std::vector<int> takeCompletedTasks(){std::vector<int> out;out.swap(completedTasks_);return out;}
    size_t scheduledTaskCount() const { return scheduledTasks_.size(); }

private:
    struct InstancePrototype {
        int root = 0;
        bool compatible = true;
        std::string packageName, componentName, fallbackReason;
        // These are value copies of the authored state captured before the first live instance
        // is published. Their native ids are template-local reference keys only; they never
        // point at a subsequently mutated/freed SceneGraph node.
        std::vector<InstanceNode> nodes;
    };

    std::unordered_map<std::string, std::unique_ptr<Package>> packagesByName_;
    std::unordered_map<std::string, Package*> packagesById_, packagesByKey_;
    std::unordered_map<int, std::unique_ptr<Instance>> instances_;
    std::unordered_map<int, std::unique_ptr<Instance>> checkouts_;
    std::unordered_map<std::string, std::vector<std::unique_ptr<Instance>>> reservations_;
    // Every fully built FGUI node is indexed before its Instance can enter a reservation pool or
    // become live.  Moving the owning unique_ptr between reservation/checkout/live maps does not
    // change this ownership; destroyTree removes the entry before SceneGraph can recycle the id.
    // unique_ptr moves between reservation, checkout and live maps without moving Instance.
    // Keeping the stable pointee here removes a second map lookup from every Transition item.
    std::unordered_map<int, Instance*> managedNodeOwners_;
    // GGroup.moveChildren is re-entrant for nested groups. While a group is moving its members,
    // FairyGUI suppresses relation callbacks owned by that same group so sibling relations do not
    // apply the delta a second time. Keep the same short-lived semantic guard natively.
    std::unordered_set<int> updatingGroups_;
    std::unordered_map<std::string, ReservationPolicy> reservationPolicies_;
    std::deque<ReservationRefillTicket> reservationRefillQueue_;
    std::unordered_map<std::string, InstancePrototype> prototypes_;
    std::unordered_map<std::string, std::string> translations_;
    TemplateStats templateStats_;
    ReservationStats reservationStats_;
    CheckoutStats checkoutStats_;
    TextOwnershipStats textOwnershipStats_;
    std::string lastReservationBlock_, lastCreateFailure_, lastBuildFailure_;
    uint64_t lastReservationRefillFrame_ = 0;
    bool hasReservationRefillFrame_ = false;
    int nextInstance_ = 1;
    uint64_t nextPackageGeneration_ = 1, translationRevision_ = 1;
    std::vector<ActiveTransition> activeTransitions_;
    std::vector<int> completedTransitions_;
    std::vector<TransitionSignal> transitionSignals_;
    std::vector<TransitionVisibilitySignal> transitionVisibilitySignals_;
    std::vector<TransitionPropertySignal> transitionPropertySignals_;
    int mutationJournalSuppressionDepth_ = 0;
    uint32_t journalEpoch_ = 0;   // B1 版本屏障:自治写入 journal 递增;JS 等值 setter 比对后决定是否先 drain 影子
    int nextTransitionToken_ = 1;
    std::unordered_map<int, ScheduledTask> scheduledTasks_;
    std::vector<int> completedTasks_;
    uint64_t nextTaskOrder_ = 1;

    static std::string reservationKey(const Package& pkg, const Resource& resource) {
        return pkg.name + "\n" + resource.id;
    }

    static std::string reservationPolicyKey(const std::string& packageName, const std::string& componentName) {
        // Package resource ids may change when a package is republished; the business-facing
        // package/component names are the stable identity used to associate later live instances.
        return packageName + "\n" + componentName;
    }

    size_t reservationPoolSize(const Package& pkg, const Resource& resource) const {
        auto found = reservations_.find(reservationKey(pkg, resource));
        return found == reservations_.end() ? 0 : found->second.size();
    }

    void clearReservationPool(const std::string& key, sg::SceneGraph& graph, bool invalidated) {FacadeCommitScope __facadeCommit(*this,graph);
        auto found = reservations_.find(key); if (found == reservations_.end()) return;
        for (auto& item : found->second) if (item) {
            if (invalidated) { ++reservationStats_.invalidated; reservationStats_.invalidatedNodes += item->nodes.size(); }
            destroyTree(*item, graph);
        }
        reservations_.erase(found);
    }

    void enqueueReservationPolicy(ReservationPolicy& policy, uint64_t notBeforeFrame) {
        if (!policy.enabled || policy.queued || policy.liveCount > 0 || policy.checkoutCount > 0 || !policy.blockedOn.empty()) return;
        policy.nextEligibleFrame = std::max(policy.nextEligibleFrame, notBeforeFrame);
        policy.queued = true; reservationRefillQueue_.push_back({policy.id, policy.epoch});
        ++reservationStats_.refillQueued;
    }

    static std::string reservationBlockedKey(const std::string& block) {
        size_t colon = block.find(':'); return colon == std::string::npos ? block : block.substr(colon + 1);
    }

    static bool reservationBlockMatches(const std::string& block, const Package& pkg) {
        const std::string key = reservationBlockedKey(block);
        return key == pkg.name || key == pkg.id || key == pkg.key;
    }

    void blockReservationPolicy(ReservationPolicy& policy, const std::string& block) {
        if (policy.queued) ++reservationStats_.refillCanceled;
        ++policy.epoch; policy.queued = false; policy.dirty = true; policy.blockedOn = block;
        lastReservationBlock_ = block; ++reservationStats_.blocked; ++reservationStats_.refillBlocked;
    }

    void wakeReservationPolicies(const Package& pkg) {
        if (!pkg.resourcesPrepared && !pkg.requiredAtlases.empty()) return;
        for (auto& item : reservationPolicies_) {
            ReservationPolicy& policy = item.second;
            if (!policy.enabled || policy.blockedOn.empty() || !reservationBlockMatches(policy.blockedOn, pkg)) continue;
            ++policy.epoch; policy.queued = false; policy.blockedOn.clear(); policy.dirty = true;
            if (policy.liveCount == 0 && policy.checkoutCount == 0) enqueueReservationPolicy(policy, 0);
        }
    }

    void blockReservationPoliciesForPackage(const std::string& packageName) {
        for (auto& item : reservationPolicies_) {
            ReservationPolicy& policy = item.second;
            if (policy.packageName != packageName && policy.dependencies.find(packageName) == policy.dependencies.end()) continue;
            if (policy.queued) ++reservationStats_.refillCanceled;
            ++policy.epoch; policy.queued = false; policy.dirty = true; policy.blockedOn = "resources:" + packageName;
        }
    }

    void registerReservationPolicy(Package& pkg, Resource& resource, const std::string& componentName,
                                   size_t target, const std::vector<std::unique_ptr<Instance>>& pool) {
        const std::string id = reservationPolicyKey(pkg.name, componentName);
        auto inserted = reservationPolicies_.emplace(id, ReservationPolicy{});
        ReservationPolicy& policy = inserted.first->second;
        if (inserted.second) {
            policy.id = id; policy.packageName = pkg.name; policy.componentName = componentName;
            for (auto& live : instances_) if (live.second && live.second->packageName == pkg.name && live.second->componentName == componentName) {
                live.second->reservationPolicyKey = id; ++policy.liveCount;
            }
            for (auto& checkout : checkouts_) if (checkout.second && checkout.second->packageName == pkg.name && checkout.second->componentName == componentName) {
                checkout.second->reservationPolicyKey = id; ++policy.checkoutCount;
            }
        }
        if (policy.queued) ++reservationStats_.refillCanceled;
        ++policy.epoch; policy.queued = false; policy.enabled = true; policy.target = target;
        policy.blockedOn = lastReservationBlock_; policy.dependencies.clear();
        if (!pool.empty() && pool.back()) for (const auto& dependency : pool.back()->packageDependencies)
            policy.dependencies.insert(dependency.name);
        policy.dirty = pool.size() < target;
        if (policy.dirty && policy.liveCount == 0 && policy.checkoutCount == 0 && policy.blockedOn.empty()) enqueueReservationPolicy(policy, 0);
        (void)resource;
    }

    void decrementCheckoutPolicy(const Instance& inst) {
        if (inst.reservationPolicyKey.empty()) return;
        auto found = reservationPolicies_.find(inst.reservationPolicyKey);
        if (found != reservationPolicies_.end() && found->second.checkoutCount > 0) --found->second.checkoutCount;
    }

    bool reservationCurrent(const Instance& inst, const Package& pkg) const {
        if (!(inst.packageName == pkg.name && inst.packageGeneration == pkg.generation &&
               inst.resourceRevision == pkg.resourceRevision &&
               inst.translationRevision == translationRevision_)) return false;
        for (const auto& dependency : inst.packageDependencies) {
            Package* live = package(dependency.name);
            if (!live || live->generation != dependency.generation ||
                live->resourceRevision != dependency.resourceRevision) return false;
        }
        return true;
    }

    void discardStaleReservations(std::vector<std::unique_ptr<Instance>>& pool,
                                  const Package& pkg, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        for (auto it = pool.begin(); it != pool.end();) {
            if (*it && reservationCurrent(**it, pkg)) { ++it; continue; }
            if (*it) destroyTree(**it, graph);
            it = pool.erase(it); ++reservationStats_.staleDiscarded;
        }
    }

    std::unique_ptr<Instance> takeReservation(const Package& pkg, const Resource& resource,
                                              sg::SceneGraph& graph, uint64_t claimFrame,
                                              bool* hadPool = nullptr, bool* tooYoung = nullptr) {
        if (hadPool) *hadPool = false; if (tooYoung) *tooYoung = false;
        auto found = reservations_.find(reservationKey(pkg, resource));
        if (found == reservations_.end()) return nullptr;
        discardStaleReservations(found->second, pkg, graph);
        if (found->second.empty()) { reservations_.erase(found); return nullptr; }
        if (hadPool) *hadPool = true;
        auto eligible = found->second.end();
        for (auto it = found->second.begin(); it != found->second.end(); ++it) if (*it &&
            (claimFrame == std::numeric_limits<uint64_t>::max() || (*it)->reservedAtFrame < claimFrame)) { eligible = it; break; }
        if (eligible == found->second.end()) { if (tooYoung) *tooYoung = true; return nullptr; }
        std::unique_ptr<Instance> out = std::move(*eligible); found->second.erase(eligible);
        if (found->second.empty()) reservations_.erase(found);
        return out;
    }

    bool stampDependencies(Instance& inst, Package& pkg, std::string* blocked=nullptr) {
        inst.packageDependencies.clear(); std::unordered_set<std::string> dependencyNames;
        bool ready=true;
        auto addPackage = [&](const std::string& key) {
            if(key.empty())return;
            Package* dependency=package(key);
            if(!dependency){ready=false;if(blocked&&blocked->empty())*blocked="missing:"+key;return;}
            dependencyNames.insert(dependency->name);
        };
        addPackage(pkg.name);
        auto addUiDependency = [&](const std::string& value, const std::string& ownerPackage) {
            if (value.compare(0, 5, "ui://") != 0) return;
            std::string body = value.substr(5), packageKey; size_t slash = body.find('/');
            if (slash != std::string::npos) packageKey = body.substr(0, slash);
            else if (body.size() > 8) packageKey = body.substr(0, 8);
            // A no-slash value no longer than eight characters is a package-local short item id.
            // Its owner is already covered by node.packageName; only explicit package keys are
            // required to resolve here.
            if (!packageKey.empty()) {
                Package* dependency = packageForReference(packageKey, ownerPackage);
                if (!dependency) { ready=false;if(blocked&&blocked->empty())*blocked="missing:"+packageKey; }
                else dependencyNames.insert(dependency->name);
            }
        };
        for (const auto& node : inst.nodes) if (node) {
            if (!node->packageName.empty()) addPackage(node->packageName);
            // Loader URLs, bitmap-font URLs and icon/text gear values can reference a package that
            // owns no semantic node in this instance. Include every authored ui:// attribute so
            // republishing that package invalidates the detached tree before it can be claimed.
            for (const auto& attr : node->attrs) addUiDependency(attr.second, node->packageName);
        }
        for (const std::string& name : dependencyNames) if (Package* dependency = package(name)) {
            if (!dependency->resourcesPrepared && !dependency->requiredAtlases.empty()) {
                ready=false;if(blocked&&blocked->empty())*blocked="resources:"+dependency->name;
            }
            inst.packageDependencies.push_back({dependency->name, dependency->generation,
                                                dependency->resourceRevision});
        } else {ready=false;if(blocked&&blocked->empty())*blocked="missing:"+name;}
        return ready;
    }

    ReservationBuildResult buildReservationOne(Package& pkg, Resource& resource, Component& component,
                                                 const std::string& componentName, size_t target,
                                                 sg::SceneGraph& graph, double preparedAtMs,
                                                 uint64_t preparedAtFrame, ReservationPolicy* policy,
                                                 bool refill) {
        const std::string poolKey = reservationKey(pkg, resource);
        auto& pool = reservations_[poolKey]; discardStaleReservations(pool, pkg, graph);
        if (pool.size() >= target) return ReservationBuildResult::Complete;
        const auto started = std::chrono::steady_clock::now();
        std::unique_ptr<Instance> inst = buildInstance(pkg, resource, component, componentName, graph);
        if (!inst) { ++reservationStats_.failed; return ReservationBuildResult::Failed; }
        applyAllControllers(*inst, graph);
        // Controller gears can replace loader/font/icon URLs. Dependency readiness must therefore
        // be checked after the selected authored state has been applied, not from the prototype.
        std::string dependencyBlock;
        if (!inst->compatible) {
            destroyTree(*inst, graph); ++reservationStats_.failed;
            return ReservationBuildResult::Failed;
        }
        if (!stampDependencies(*inst, pkg, &dependencyBlock)) {
            destroyTree(*inst, graph);
            if (policy) blockReservationPolicy(*policy, dependencyBlock);
            else { lastReservationBlock_ = dependencyBlock; ++reservationStats_.blocked; }
            return ReservationBuildResult::Blocked;
        }
        inst->reservedAtMs = preparedAtMs; inst->reservedAtFrame = preparedAtFrame;
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        reservationStats_.buildMs += elapsed; ++reservationStats_.built; reservationStats_.builtNodes += inst->nodes.size();
        if (refill) {
            ++reservationStats_.refillBuilt; reservationStats_.refillBuildMs += elapsed;
            reservationStats_.refillMaxMs = std::max(reservationStats_.refillMaxMs, elapsed);
        }
        if (policy) {
            policy->blockedOn.clear(); policy->dependencies.clear(); lastReservationBlock_.clear();
            for (const auto& dependency : inst->packageDependencies) policy->dependencies.insert(dependency.name);
        }
        pool.push_back(std::move(inst)); return ReservationBuildResult::Built;
    }

    static void assignMutationCapabilities(Instance& inst) {
        std::unordered_set<int> textTimelineTargets;
        for (const auto& owned : inst.nodes) if (owned)
            for (const auto& transition : owned->transitions)
                for (const auto& item : transition.items) if (item.kind == TransitionKind::Text) {
                    InstanceNode* target=nullptr;
                    if (item.targetId.empty()) target=owned.get();
                    else {
                        auto scope=inst.byOwnerAndId.find(owned->nativeId);
                        if(scope!=inst.byOwnerAndId.end()){
                            auto found=scope->second.find(item.targetId);
                            if(found!=scope->second.end())target=found->second;
                        }
                    }
                    if(target)textTimelineTargets.insert(target->nativeId);
                }
        for (const auto& owned : inst.nodes) if (owned) {
            InstanceNode& node = *owned; node.mutationCaps = MutationNone; FacadeSeqTracker::note(node.nativeId, FD_ATTRS);
            if ((node.kind != ObjectKind::Text && node.kind != ObjectKind::RichText) ||
                node.textGear.present ||
                textTimelineTargets.count(node.nativeId)) continue;
            node.mutationCaps |= MutationTextFinal;
            auto autoSize = node.attrs.find("autoSize");
            if (autoSize != node.attrs.end() && autoSize->second == "none")
                node.mutationCaps |= MutationTextFixedBox;
        }
    }

    std::unique_ptr<Instance> buildInstance(Package& pkg, Resource& resource,
                                            Component& component, const std::string& componentName,
                                            sg::SceneGraph& graph) {
        lastBuildFailure_.clear();
        const std::string templateKey = pkg.name + "\n" + resource.id;
        std::unique_ptr<Instance> inst;
        auto prototype = prototypes_.find(templateKey);
        if (prototype != prototypes_.end()) {
            const auto started = std::chrono::steady_clock::now();
            inst = clonePrototype(prototype->second, nextInstance_++, graph);
            if (inst) {
                ++templateStats_.hits; templateStats_.clonedNodes += inst->nodes.size();
                templateStats_.cloneMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            } else {
                // A malformed/stale template must never strand partially allocated SceneGraph
                // nodes. clonePrototype rolls those back; discard it and use the known-correct
                // XML path for this call.
                prototypes_.erase(prototype);
            }
        }
        if (!inst) {
            ++templateStats_.misses;
            inst = std::make_unique<Instance>(); inst->id = nextInstance_++;
            inst->packageName = pkg.name; inst->componentName = componentName;
            if (component.hasUnsupportedStaticNode) reject(*inst, component.name + ":" + component.unsupportedReason);
            int root = buildComponent(*inst, pkg, component, nullptr, 0, 0, graph, 0);
            if (root <= 0) {
                lastBuildFailure_ = "root-allocation:high=" + std::to_string(graph.highWater) +
                    ":free=" + std::to_string(graph.freeList.size());
                destroyTree(*inst, graph); return nullptr;
            }
            inst->root = root;
            // Snapshot before controllers, transitions, lazy handles or business setters can
            // mutate the live instance. No pointer to this value is ever handed to JS.
            prototypes_[templateKey] = capturePrototype(*inst);
        }
        inst->packageName = pkg.name; inst->componentName = componentName;
        inst->packageGeneration = pkg.generation; inst->resourceRevision = pkg.resourceRevision;
        inst->translationRevision = translationRevision_;
        resolveGroupMemberships(*inst);
        // Nested components/loaders can resolve resources from another package.  Stamp every
        // package actually referenced by the compiled nodes so a later atlas republish cannot
        // make a detached reservation display stale textures when it is eventually claimed.
        stampDependencies(*inst, pkg);
        if (!indexManagedNodes(*inst)) { destroyTree(*inst, graph); return nullptr; }
        assignMutationCapabilities(*inst);
        return inst;
    }

    void activateInstance(Instance& inst, double nowMs, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        // Reserved MovieClips have never participated in advanceMovieClips(). Establish their
        // clock at claim time so the first visible frame is authored frame zero, not a catch-up.
        for (const auto& item : inst.nodes) if (item && item->kind == ObjectKind::MovieClip)
            item->movieLastMs = nowMs;
        // Nested components were appended during Native construction and should auto-play now.
        // The instance root is still detached; its original FGUI "added" event starts it later.
        for (const auto& item : inst.nodes) if (item && item->nativeId != inst.root) for (const auto& desc : item->transitions)
            if (desc.autoPlay) playTransition(inst.id, item->nativeId, desc.name, nowMs,
                                              desc.autoPlayRepeat, desc.autoPlayDelay, false, graph);
    }

    static InstancePrototype capturePrototype(const Instance& source) {
        InstancePrototype result; result.root = source.root; result.compatible = source.compatible;
        result.packageName = source.packageName; result.componentName = source.componentName;
        result.fallbackReason = source.fallbackReason; result.nodes.reserve(source.nodes.size());
        for (const auto& node : source.nodes) if (node) result.nodes.push_back(*node);
        return result;
    }

    static bool remapPrototypeId(const std::unordered_map<int, int>& ids, int& id, bool allowZero = true) {
        if (id <= 0) return allowZero;
        auto found = ids.find(id); if (found == ids.end()) return false; id = found->second; return true;
    }

    bool rebuildPrototypeVisual(Instance& inst, InstanceNode& node, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        if (node.kind == ObjectKind::Image) {
            // Direct tag-typed placeholders created for unresolved PackageItems intentionally have
            // no source. Their first XML build is blank, and cloning the cached prototype must
            // preserve that valid blank state instead of turning it into an incompatibility.
            if (node.sourceId.empty()) return true;
            Package* pkg = package(node.packageName); if (!pkg) return false;
            auto resource = pkg->resourcesById.find(node.sourceId);
            return resource != pkg->resourcesById.end() && writeImage(graph, *pkg, resource->second, node);
        }
        if (node.kind == ObjectKind::MovieClip) {
            if (node.sourceId.empty()) return true;
            Package* pkg = package(node.packageName); if (!pkg) return false;
            auto resource = pkg->resourcesById.find(node.sourceId);
            return resource != pkg->resourcesById.end() && writeMovieClip(graph, *pkg, resource->second, node);
        }
        if (node.kind == ObjectKind::Graph) { writeGraph(graph, node); return true; }
        if (node.kind == ObjectKind::Loader) {
            bool resolved = writeLoader(graph, node);
            if (resolved) node.attrs["nativeResolved"] = "true"; else node.attrs.erase("nativeResolved");
            // An empty/unresolved loader is a valid authored container; buildObject only records
            // nativeResolved on success and does not reject the component.
            return true;
        }
        if (isBitmapText(node))
            return writeBitmapText(graph, node);
        if ((node.kind == ObjectKind::Text || node.kind == ObjectKind::RichText || node.kind == ObjectKind::InputText) && prepareText)
            prepareTextSurface(node, TextPrepareMode::AuthoredBox, graph);
        (void)inst;
        return true;
    }

    bool nodeReferencesPackage(const InstanceNode& node, const Package& dependency) const {
        if (node.packageName == dependency.name) return true;
        auto references = [&](const std::string& value) {
            if (value.compare(0, 5, "ui://") != 0) return false;
            const std::string body = value.substr(5); std::string packageKey;
            const size_t slash = body.find('/');
            if (slash != std::string::npos) packageKey = body.substr(0, slash);
            else if (body.size() > 8) packageKey = body.substr(0, 8);
            return !packageKey.empty() && packageForReference(packageKey, node.packageName) == &dependency;
        };
        for (const auto& attr : node.attrs) if (references(attr.second)) return true;
        return false;
    }

    void refreshLivePackageVisuals(Package& dependency, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        for (auto& live : instances_) {
            Instance* inst = live.second.get(); if (!inst) continue;
            for (const auto& owned : inst->nodes) {
                InstanceNode* node = owned.get(); if (!node || !nodeReferencesPackage(*node, dependency)) continue;
                // Geometry/controllers/business text remain live state. Only atlas-backed visual
                // surfaces are republished; repainting ordinary text here would overwrite a
                // facade-owned value with authored XML during an unrelated resource reload.
                if (node->kind == ObjectKind::Image || node->kind == ObjectKind::MovieClip ||
                    node->kind == ObjectKind::Loader || isBitmapText(*node))
                    rebuildPrototypeVisual(*inst, *node, graph);
            }
        }
    }

    static void linkPrototypeMask(sg::SceneGraph& graph, InstanceNode& owner, InstanceNode& mask) {
        // Mirror FairyGUI-cocos2dx: the mask display object owns clipping geometry, and the
        // owner only stores a reference. Axis-aligned rects stay on the cheap scissor path;
        // ellipse / rounded / rotated / multi-quad masks must write stencil geometry.
        graph.buf[(size_t)owner.nativeId * sg::STRIDE + sg::MASKI] = static_cast<float>(mask.nativeId);
        float* slot = graph.buf + (size_t)mask.nativeId * sg::STRIDE;
        int flags = static_cast<int>(slot[sg::HITFLAGS]) | 4;
        const bool hasNativeQuads = mask.nativeId > 0 && mask.nativeId < sg::NODE_CAP &&
            !graph.nodes[mask.nativeId].nativeQuads.empty();
        if (mask.kind == ObjectKind::Image) flags |= 8;
        if (mask.kind == ObjectKind::Graph) {
            auto typeIt = mask.attrs.find("type");
            const std::string type = typeIt == mask.attrs.end() ? std::string() : typeIt->second;
            const bool ellipse = type == "ellipse" || type == "eclipse";
            auto cornerIt = mask.attrs.find("corner");
            const bool rounded = cornerIt != mask.attrs.end() && !cornerIt->second.empty() &&
                number(split(cornerIt->second, ',')[0]) > 0;
            const bool rotated = std::fabs(mask.rotation) > 0.0001f ||
                std::fabs(mask.skewX) > 0.0001f || std::fabs(mask.skewY) > 0.0001f;
            // Prefer exact triangle geometry (flag 64) when writeGraph already emitted quads.
            // Fall back to the shader ellipse disc only when there is no geometry payload.
            if (ellipse && !hasNativeQuads) flags |= 16;
            if (ellipse || rounded || rotated || hasNativeQuads) flags |= 64;
        } else if (hasNativeQuads &&
                   (std::fabs(mask.rotation) > 0.0001f || std::fabs(mask.skewX) > 0.0001f ||
                    std::fabs(mask.skewY) > 0.0001f)) {
            flags |= 64;
        }
        slot[sg::HITFLAGS] = static_cast<float>(flags); slot[sg::RENDERABLE] = 0;
    }

    std::unique_ptr<Instance> clonePrototype(const InstancePrototype& source, int instanceId,
                                             sg::SceneGraph& graph) {
        if (source.nodes.empty() || source.root <= 0) return nullptr;
        std::unordered_map<int, int> ids; ids.reserve(source.nodes.size());
        std::vector<int> allocated; allocated.reserve(source.nodes.size());
        for (const InstanceNode& node : source.nodes) {
            if (node.nativeId <= 0 || ids.count(node.nativeId)) {
                for (auto it = allocated.rbegin(); it != allocated.rend(); ++it) graph.freeNode(*it);
                return nullptr;
            }
            int id = graph.alloc();
            if (id <= 0) {
                for (auto it = allocated.rbegin(); it != allocated.rend(); ++it) graph.freeNode(*it);
                return nullptr;
            }
            ids[node.nativeId] = id; allocated.push_back(id);
        }

        auto inst = std::make_unique<Instance>(); inst->id = instanceId; inst->compatible = source.compatible;
        inst->packageName = source.packageName; inst->componentName = source.componentName;
        inst->fallbackReason = source.fallbackReason; inst->nodes.reserve(source.nodes.size());
        auto root = ids.find(source.root); if (root == ids.end()) {
            for (auto it = allocated.rbegin(); it != allocated.rend(); ++it) graph.freeNode(*it);
            return nullptr;
        }
        inst->root = root->second;

        // Validate the complete reference graph before any allocated id is transferred into the
        // Instance. Error rollback is then a single, non-overlapping free pass.
        for (const InstanceNode& authored : source.nodes) {
            if ((authored.parentNativeId > 0 && !ids.count(authored.parentNativeId)) ||
                (authored.displayParentNativeId > 0 && !ids.count(authored.displayParentNativeId)) ||
                (authored.displayContainerId > 0 && !ids.count(authored.displayContainerId)) ||
                (authored.clipMaskId > 0 && !ids.count(authored.clipMaskId)) ||
                (authored.ownerComponentId > 0 && !ids.count(authored.ownerComponentId))) {
                for (auto it = allocated.rbegin(); it != allocated.rend(); ++it) graph.freeNode(*it);
                return nullptr;
            }
            for (int child : authored.children) if (child <= 0 || !ids.count(child)) {
                for (auto it = allocated.rbegin(); it != allocated.rend(); ++it) graph.freeNode(*it);
                return nullptr;
            }
        }

        for (const InstanceNode& authored : source.nodes) {
            auto node = std::make_unique<InstanceNode>(authored);
            node->nativeId = ids.at(authored.nativeId);
            remapPrototypeId(ids, node->parentNativeId); remapPrototypeId(ids, node->displayParentNativeId);
            remapPrototypeId(ids,node->displayContainerId);remapPrototypeId(ids,node->clipMaskId);
            remapPrototypeId(ids, node->ownerComponentId);
            for (int& child : node->children) remapPrototypeId(ids, child, false);
            InstanceNode* raw = node.get(); inst->byNativeId[raw->nativeId] = raw; inst->nodes.push_back(std::move(node));
        }

        for (const auto& owned : inst->nodes) if (owned) {
            InstanceNode& node = *owned; writeNode(graph, node);
            if (node.grayed) writeNodeGrayed(graph, node);
            int displayParent=node.displayParentNativeId>0?node.displayParentNativeId:node.parentNativeId;
            if (displayParent > 0) graph.setParent(node.nativeId, displayParent, -1);
            if (!rebuildPrototypeVisual(*inst, node, graph)) reject(*inst, "prototype-visual:" + node.packageName + "/" + node.sourceId);
        }
        // MASKI is SceneGraph state rather than InstanceNode metadata. Reconstruct it only after
        // all ids and visuals exist, using the immutable displayMask marker captured on the mask
        // child. This also restores the specialised image/ellipse/rounded-mask hit flags.
        for (const auto& owned : inst->nodes) if (owned&&owned->clipMaskId>0) {
            InstanceNode* mask=nodeIn(*inst,owned->clipMaskId);if(mask)linkPrototypeMask(graph,*owned,*mask);
        }
        return inst;
    }

    // FairyGUI's authored maxTime is only the latest start+one tween duration. Repeats, shakes and
    // nested transitions keep the play callback alive beyond maxTime, but reverse positioning still
    // uses that authored anchor. Keep those two clocks separate; merging them shifts every reversed
    // item and was the source of popup transition size/position drift.
    static void recomputeTransitionDuration(TransitionDesc& desc){desc.maxTime=0;for(const auto& item:desc.items)desc.maxTime=std::max(desc.maxTime,item.time+(item.tween?item.duration:0));}
    static bool authoredDisplayEdgeActive(const sg::SceneGraph& graph,const InstanceNode& node){
        if(node.nativeId<=0||node.nativeId>=sg::NODE_CAP)return true;
        const int expected=node.displayParentNativeId>0?node.displayParentNativeId:node.parentNativeId;
        const int actual=graph.nodes[node.nativeId].parent;
        // A detached node is not paintable and must retain its authored internal state for a later
        // semantic reattach. A positive, different parent means business code moved only the raw
        // Pixi displayObject (Spine slots and guide layers do this) while the GObject still belongs
        // to its original component.
        return expected<=0||actual<=0||actual==expected;
    }
    static bool selfVisible(const InstanceNode& node){return node.visible&&(!node.gearHidden||node.transitionDisplayLocks>0);}
    static bool effectiveVisible(const sg::SceneGraph& graph,const InstanceNode& node){
        bool internalVisible=true;
        if(authoredDisplayEdgeActive(graph,node))
            internalVisible=(!node.gearHidden||node.transitionDisplayLocks>0)&&!node.groupHidden;
        return node.displayVisible&&node.visible&&internalVisible;
    }
    static void writeNodeVisibility(sg::SceneGraph& graph,const InstanceNode& node){if(!graph.buf||node.nativeId<=0||node.nativeId>=sg::NODE_CAP)return;FacadeSeqTracker::mark(graph,node.nativeId,FD_VISIBILITY);graph.buf[(size_t)node.nativeId*sg::STRIDE+sg::VISIBLE]=effectiveVisible(graph,node)?1.0f:0.0f;graph.markLocalInputChanged(node.nativeId);}
    static void resolveGroupMembership(Instance& inst,InstanceNode& node){
        // Keep the old derived bit until refreshGroupVisibility publishes the new result. Clearing
        // it here would make a member reparented out of a hidden group look unchanged to refresh,
        // leaving the old VISIBLE=0 value stranded in the SceneGraph buffer.
        node.groupNativeId=0;
        auto authored=node.attrs.find("group");if(authored==node.attrs.end()||authored->second.empty()||node.parentNativeId<=0)return;
        InstanceNode* parent=nodeIn(inst,node.parentNativeId);if(!parent)return;
        for(int siblingId:parent->children){InstanceNode* sibling=nodeIn(inst,siblingId);if(sibling&&sibling->kind==ObjectKind::Group&&sibling->id==authored->second){node.groupNativeId=sibling->nativeId;return;}}
    }
    static void resolveGroupMemberships(Instance& inst){for(const auto& owned:inst.nodes)if(owned)resolveGroupMembership(inst,*owned);}
    static bool belongsToGroup(const InstanceNode& node,const InstanceNode& group){
        if(group.kind!=ObjectKind::Group||node.nativeId==group.nativeId||
           node.parentNativeId<=0||node.parentNativeId!=group.parentNativeId)return false;
        if(node.groupNativeId>0)return node.groupNativeId==group.nativeId;
        auto authored=node.attrs.find("group");
        return authored!=node.attrs.end()&&!group.id.empty()&&authored->second==group.id;
    }
    bool belongsToUpdatingGroup(Instance& inst,const InstanceNode& node)const{
        for(int groupId:updatingGroups_){
            InstanceNode* group=nodeIn(inst,groupId);
            if(group&&belongsToGroup(node,*group))return true;
        }
        return false;
    }
    static bool groupChainVisible(Instance& inst,const InstanceNode& node,int depth=0){
        if(node.groupNativeId<=0)return true;if(depth>32)return false;
        InstanceNode* group=nodeIn(inst,node.groupNativeId);if(!group||group==&node||group->kind!=ObjectKind::Group)return true;
        return selfVisible(*group)&&groupChainVisible(inst,*group,depth+1);
    }
    static void refreshGroupVisibility(Instance& inst,sg::SceneGraph& graph){
        for(const auto& owned:inst.nodes)if(owned){InstanceNode& node=*owned;bool hidden=node.groupNativeId>0&&!groupChainVisible(inst,node);if(hidden!=node.groupHidden){node.groupHidden=hidden;writeNodeVisibility(graph,node);}}
    }
    // GObject.handleGrayedChanged routes grayed through the object's shared ColorMatrixFilter;
    // natively that is the inheritable per-node color-matrix slot (fd[3]=enable, fd[4..23]=4x5
    // rows). Only that region is touched so filter alpha/blur/post payloads stay intact.
    static void writeNodeGrayed(sg::SceneGraph& graph, const InstanceNode& node) {
        if (!graph.buf || node.nativeId <= 0 || node.nativeId >= sg::NODE_CAP) return;
        float* fd = graph.buf + sg::FILTER_BASE + (size_t)node.nativeId * sg::FILTER_STRIDE_F;
        for (int k = 0; k < 20; k++) fd[4 + k] = 0.0f;
        if (node.grayed) {
            fd[3] = 1.0f;
            fd[4] = fd[9] = fd[14] = 0.299f;    // R contribution per output row
            fd[5] = fd[10] = fd[15] = 0.587f;   // G contribution
            fd[6] = fd[11] = fd[16] = 0.114f;   // B contribution
            fd[22] = 1.0f;                      // alpha row stays identity
        } else {
            fd[3] = 0.0f;
            fd[4] = fd[10] = fd[16] = fd[22] = 1.0f;   // restore the alloc-time identity matrix
        }
        graph.refreshFilterFlag(node.nativeId);   // HAS_FILTER 位:batcher 据此决定是否读 FILTER 区
        graph.markLocalInputChanged(node.nativeId);
    }
    void collectTransitionDisplayLocks(InstanceNode& component,const TransitionDesc& desc,float timeline,bool reversed,std::unordered_set<int>& locks,int depth=0){
        if(depth>12)return;
        if((desc.options&1)!=0)for(const auto& item:desc.items){InstanceNode* target=transitionTarget(component,item.targetId);if(target&&target!=&component)locks.insert(target->nativeId);}
        if(timeline<0)return;
        for(const auto& item:desc.items){
            if(item.kind!=TransitionKind::Nested)continue;InstanceNode* target=transitionTarget(component,item.targetId);if(!target)continue;
            float start=reversed?desc.maxTime-item.time-(item.tween?item.duration:0):item.time,local=timeline-std::max(0.0f,start);if(local<0)continue;
            std::string value=overrideValue(component,desc,item.label,item.value);auto parts=split(value,',');const TransitionDesc* nested=!parts.empty()?transition(*target,parts[0]):nullptr;int times=parts.size()>1?integer(parts[1],1):1;if(!nested||times==0)continue;
            float span=transitionRuntimeSpan(*target,*nested,reversed,depth+1);if(std::isfinite(span)){span=std::max(.0001f,span);if(times>0&&local>=span*times)continue;local=std::fmod(local,span);}collectTransitionDisplayLocks(*target,*nested,local,reversed,locks,depth+1);
        }
    }
    void syncTransitionDisplayLocks(ActiveTransition& active,InstanceNode& component,const TransitionDesc& desc,float timeline,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        std::unordered_set<int> desired;collectTransitionDisplayLocks(component,desc,timeline,active.reversed,desired);std::unordered_set<int> current(active.displayLocks.begin(),active.displayLocks.end());bool groupChanged=false;
        for(int id:active.displayLocks)if(!desired.count(id))if(InstanceNode* target=node(active.instanceId,id)){target->transitionDisplayLocks=std::max(0,target->transitionDisplayLocks-1);writeNodeVisibility(graph,*target);queueTransitionProperty(*target,TransitionPropertyKind::InternalVisible,(!target->gearHidden||target->transitionDisplayLocks>0)?1.0f:0.0f);groupChanged=groupChanged||target->kind==ObjectKind::Group;}
        for(int id:desired)if(!current.count(id))if(InstanceNode* target=node(active.instanceId,id)){++target->transitionDisplayLocks;writeNodeVisibility(graph,*target);queueTransitionProperty(*target,TransitionPropertyKind::InternalVisible,1.0f);groupChanged=groupChanged||target->kind==ObjectKind::Group;}
        active.displayLocks.assign(desired.begin(),desired.end());std::sort(active.displayLocks.begin(),active.displayLocks.end());if(groupChanged)if(Instance* inst=instance(active.instanceId))refreshGroupVisibility(*inst,graph);
    }
    void releaseTransitionDisplayLocks(ActiveTransition& active,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        bool groupChanged=false;for(int id:active.displayLocks)if(InstanceNode* target=node(active.instanceId,id)){target->transitionDisplayLocks=std::max(0,target->transitionDisplayLocks-1);writeNodeVisibility(graph,*target);queueTransitionProperty(*target,TransitionPropertyKind::InternalVisible,(!target->gearHidden||target->transitionDisplayLocks>0)?1.0f:0.0f);groupChanged=groupChanged||target->kind==ObjectKind::Group;}active.displayLocks.clear();if(groupChanged)if(Instance* inst=instance(active.instanceId))refreshGroupVisibility(*inst,graph);
    }
    float transitionRuntimeSpan(InstanceNode& component,const TransitionDesc& desc,bool reversed,int depth=0) const {
        if(depth>12)return std::max(.0001f,desc.maxTime);float span=std::max(0.0f,desc.maxTime);
        for(const auto& item:desc.items){
            float start=reversed?desc.maxTime-item.time-(item.tween?item.duration:0):item.time;
            // A repeat=-1 tween never reaches its completion callback, so $totalTasks never
            // drains and the whole transition keeps playing until stopped. Mirror that with an
            // infinite runtime span (the caller's !isfinite branch already never completes).
            if(item.tween&&item.repeat<0)return std::numeric_limits<float>::infinity();
            start=std::max(0.0f,start);float duration=item.tween?item.duration*std::max(1,item.repeat+1):0;
            if(item.kind==TransitionKind::Shake){auto parts=split(overrideValue(component,desc,item.label,item.value),',');duration=parts.size()>1?std::max(0.0f,number(parts[1])):0;}
            else if(item.kind==TransitionKind::Nested){
                auto parts=split(overrideValue(component,desc,item.label,item.value),',');int times=parts.size()>1?integer(parts[1],1):1;
                InstanceNode* target=transitionTarget(component,item.targetId);const TransitionDesc* nested=target&&!parts.empty()?transition(*target,parts[0]):nullptr;
                if(nested&&times<0)return std::numeric_limits<float>::infinity();
                if(nested&&times>0)duration=transitionRuntimeSpan(*target,*nested,reversed,depth+1)*times;
            }
            span=std::max(span,start+duration);
        }
        return std::max(.0001f,span);
    }
    bool transitionRunsSynchronously(InstanceNode& component,const TransitionDesc& desc,bool reversed,int depth=0) const {
        if(depth>12)return false;
        for(const auto& item:desc.items){
            const float start=reversed?desc.maxTime-item.time-(item.tween?item.duration:0):item.time;
            if(start>0.000001f||item.tween||item.kind==TransitionKind::Shake)return false;
            if(item.kind!=TransitionKind::Nested)continue;
            const std::string value=overrideValue(component,desc,item.label,item.value);
            const auto parts=split(value,',');
            const int times=parts.size()>1?integer(parts[1],1):1;
            if(times==0)continue;
            if(times<0)return false;
            InstanceNode* target=transitionTarget(component,item.targetId);
            const TransitionDesc* nested=target&&!parts.empty()?transition(*target,parts[0]):nullptr;
            // A missing nested transition is a synchronous no-op in FairyGUI. A live nested
            // transition contributes a task only when its own play() remains active.
            if(nested&&!transitionRunsSynchronously(*target,*nested,reversed,depth+1))return false;
        }
        return true;
    }
    void emitTransitionSignalRange(int token,InstanceNode& component,const TransitionDesc& desc,bool reversed,double from,double to,int depth=0){
        if(depth>12||to<from)return;
        for(const auto& item:desc.items){
            double start=reversed?desc.maxTime-item.time-(item.tween?item.duration:0):item.time;start=std::max(0.0,start);
            // hook2/label2 fires from $tweenComplete, which an infinite repeat item never reaches.
            double finish=item.tween&&item.repeat<0?std::numeric_limits<double>::infinity():start+(item.tween?item.duration*std::max(1,item.repeat+1):0);
            if(!item.label.empty()&&from<start&&to+1e-9>=start)transitionSignals_.push_back({token,component.nativeId,desc.name,item.label});
            if(item.tween&&!item.label2.empty()&&from<finish&&to+1e-9>=finish)transitionSignals_.push_back({token,component.nativeId,desc.name,item.label2});
            if(item.kind!=TransitionKind::Nested||to+1e-9<start)continue;
            InstanceNode* target=transitionTarget(component,item.targetId);std::string value=overrideValue(component,desc,item.label,item.value);auto parts=split(value,',');
            const TransitionDesc* nested=target&&!parts.empty()?transition(*target,parts[0]):nullptr;int times=parts.size()>1?integer(parts[1],1):1;if(!target||!nested||times==0)continue;
            double nestedFrom=std::max(-0.000001,from-start),nestedTo=std::max(0.0,to-start);double nestedSpan=transitionRuntimeSpan(*target,*nested,reversed,depth+1);
            if(!std::isfinite(nestedSpan)){emitTransitionSignalRange(token,*target,*nested,reversed,nestedFrom,nestedTo,depth+1);continue;}
            nestedSpan=std::max(0.0001,nestedSpan);double limit=times<0?nestedTo:nestedSpan*std::max(1,times);nestedTo=std::min(nestedTo,limit);if(nestedTo<nestedFrom)continue;
            int first=std::max(0,(int)std::floor(std::max(0.0,nestedFrom)/nestedSpan)),last=std::max(first,(int)std::floor(std::max(0.0,nestedTo)/nestedSpan));if(last-first>128)first=last-128;
            for(int cycle=first;cycle<=last;++cycle){double base=cycle*nestedSpan;if(times>=0&&cycle>=times)break;emitTransitionSignalRange(token,*target,*nested,reversed,nestedFrom-base,nestedTo-base,depth+1);}
        }
    }
    void emitTransitionSignals(const ActiveTransition& active,InstanceNode& component,const TransitionDesc& desc,double runtimeSpan,double from,double to){
        if(to<from)return;if(!std::isfinite(runtimeSpan)){emitTransitionSignalRange(active.token,component,desc,active.reversed,from,to);return;}
        double span=std::max(0.0001,runtimeSpan);int first=std::max(0,(int)std::floor(std::max(0.0,from)/span)),last=std::max(first,(int)std::floor(std::max(0.0,to)/span));if(last-first>128)first=last-128;
        for(int cycle=first;cycle<=last;++cycle){double base=cycle*span;emitTransitionSignalRange(active.token,component,desc,active.reversed,from-base,to-base);}
    }

    static bool resourcesReady(const Package& p) {
        for (const auto& id : p.requiredAtlases) { auto it=p.atlases.find(id); if(it==p.atlases.end()||it->second.textureId<=0||it->second.width<=0||it->second.height<=0)return false; }
        return true;
    }

    static Resource* resourceByName(Package& p, const std::string& name) {
        auto ni = p.resourceIdByName.find(name); if (ni == p.resourceIdByName.end()) return nullptr;
        auto ri = p.resourcesById.find(ni->second); return ri == p.resourcesById.end() ? nullptr : &ri->second;
    }
    static void parseSprites(Package& p, const std::string& source) {
        size_t pos = 0; bool first = true;
        while (pos <= source.size()) {
            size_t end = source.find('\n', pos); if (end == std::string::npos) end = source.size();
            std::string line = source.substr(pos, end - pos); if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!first && !line.empty()) {
                auto a = split(line, ' '); std::vector<std::string> v; for (auto& s : a) if (!s.empty()) v.push_back(s);
                if (v.size() >= 7) { Sprite sp; sp.itemId = v[0]; int bin = integer(v[1], -1); sp.atlasId = bin >= 0 ? "atlas" + std::to_string(bin) : "atlas_" + v[0].substr(0, v[0].find('_')); sp.x = number(v[2]); sp.y = number(v[3]); sp.width = number(v[4]); sp.height = number(v[5]); sp.rotated = v[6] == "1"; p.sprites[sp.itemId] = sp; }
            }
            first = false; if (end == source.size()) break; pos = end + 1;
        }
    }
    static std::unordered_map<std::string,std::string> lineAttributes(const std::string& line){std::unordered_map<std::string,std::string> out;size_t p=line.find(' ');while(p!=std::string::npos&&p<line.size()){while(p<line.size()&&std::isspace((unsigned char)line[p]))p++;size_t eq=line.find('=',p);if(eq==std::string::npos)break;std::string key=line.substr(p,eq-p);size_t end=line.find(' ',eq+1);out[key]=line.substr(eq+1,end==std::string::npos?std::string::npos:end-eq-1);p=end;}return out;}
    static BitmapFont parseBitmapFont(const std::string& source){
        BitmapFont font;float maxCharHeight=0;size_t pos=0;
        while(pos<=source.size()){
            size_t end=source.find('\n',pos);if(end==std::string::npos)end=source.size();
            std::string line=source.substr(pos,end-pos);if(!line.empty()&&line.back()=='\r')line.pop_back();
            auto a=lineAttributes(line);
            if(line.compare(0,4,"info")==0){
                // A face attribute identifies the atlas/TTF flavour. Native image-font quads must
                // never guess at its page/channel semantics.
                font.ttf=a.find("face")!=a.end();
                auto size=a.find("size");if(size!=a.end())font.size=number(size->second);
                auto resize=a.find("resizable");font.resizable=resize!=a.end()&&resize->second=="true";
                auto color=a.find("colored");font.colorable=color!=a.end()&&color->second=="true";
            }else if(line.compare(0,6,"common")==0){
                auto height=a.find("lineHeight");if(height!=a.end())font.commonLineHeight=number(height->second);
                auto advance=a.find("xadvance");if(advance!=a.end())font.commonAdvance=number(advance->second);
            }else if(line.compare(0,4,"char")==0){
                auto id=a.find("id");int cp=id==a.end()?-1:integer(id->second,-1);
                if(cp>=0){
                    BitmapGlyph glyph;auto image=a.find("img");if(image!=a.end())glyph.imageId=image->second;
                    auto x=a.find("x");if(x!=a.end())glyph.x=number(x->second);
                    auto y=a.find("y");if(y!=a.end())glyph.y=number(y->second);
                    auto width=a.find("width");if(width!=a.end())glyph.width=number(width->second);
                    auto height=a.find("height");if(height!=a.end())glyph.height=number(height->second);
                    auto ox=a.find("xoffset");if(ox!=a.end())glyph.offsetX=number(ox->second);
                    auto oy=a.find("yoffset");if(oy!=a.end())glyph.offsetY=number(oy->second);
                    auto advance=a.find("xadvance");if(advance!=a.end())glyph.advance=number(advance->second);
                    auto channel=a.find("chnl");if(channel!=a.end())glyph.channel=integer(channel->second);
                    maxCharHeight=std::max(maxCharHeight,glyph.height);
                    font.glyphs[(uint32_t)cp]=std::move(glyph);
                }
            }
            if(end==source.size())break;pos=end+1;
        }
        // This mirrors UIPackage.loadFont: common lineHeight supplies a missing info size, and a
        // missing common lineHeight in turn inherits info size. Legacy descriptors may have
        // neither, in which case FairyGUI uses the tallest authored char record.
        if(font.size==0&&font.commonLineHeight!=0)font.size=font.commonLineHeight;
        if(font.commonLineHeight==0&&font.size!=0)font.commonLineHeight=font.size;
        if(font.size==0&&maxCharHeight>0)font.size=maxCharHeight;
        return font;
    }
    static void inspectCompatibility(const XmlNode* n, bool& unsupported, std::string& reason) {
        if (!n || unsupported) return;
        // Text input/rich text/movie clips have native semantic handles.  Keeping them in the
        // blueprint is essential: rejecting the containing component falls back the entire popup
        // to a Pixi tree and a later resource churn can make unrelated labels/buttons disappear.
        if (n->name == "transition") for (const auto& item : n->children) if (item && item->name == "item") {
            const std::string& type = item->attr("type");
            if (transitionKind(type) == TransitionKind::Unknown) { unsupported = true; reason = "transition:" + type; break; }
        }
        if (n->name == "graph" && !n->attr("type").empty()) {
            const std::string& type = n->attr("type");
            // FairyGUI's package XML historically spells the ellipse graph type "eclipse".
            // Its JS runtime treats every non-rect graph as an ellipse, so accepting only the
            // geometrically-correct spelling made otherwise native-compatible HUDs fall back.
            if (type != "rect" && type != "ellipse" && type != "eclipse" && type != "empty") { unsupported = true; reason = "graph:" + type; }
        }
        // Accept the full gear set used by this project's asset census (and the stock
        // FairyGUI JS GearXMLNodeNameMap). Unknown exporter-only gears are ignored instead of
        // rejecting the entire component — matching the browser path for gearDisplay2.
        // Census across assets/: gearDisplay/XY/Size/Look/Icon/Color/Text/FontSize/Ani (+Display2).
        if (n->name.size() > 4 && n->name.compare(0, 4, "gear") == 0 &&
            n->name != "gearDisplay" && n->name != "gearDisplay2" && n->name != "gearXY" && n->name != "gearSize" && n->name != "gearLook" &&
            n->name != "gearIcon" && n->name != "gearText" && n->name != "gearColor" &&
            n->name != "gearAni" && n->name != "gearAnimation" && n->name != "gearFontSize" &&
            n->name != "gearFont" && n->name != "gearIconColor") {
            // Still unknown after the project census: log once via reason but do not reject —
            // a single rare gear must not force the whole popup onto the JS FGUI tree.
            // (Previously: unsupported=true. That made one exotic gear destroy native coverage.)
            if (reason.empty()) reason = "gear-ignored:" + n->name;
        }
        for (const auto& c : n->children) inspectCompatibility(c.get(), unsupported, reason);
    }
    static void readTransform(const XmlNode* desc, const Resource* res, const XmlNode* componentXml, InstanceNode& out) {
        if (res) { out.width = res->width; out.height = res->height; }
        if (componentXml) {
            // Component-root attributes (overflow, margin, scroll configuration, opaque, ...)
            // shape the Native display hierarchy even when no GComponent facade is ever read.
            out.attrs = componentXml->attrs;
            pairValue(componentXml->attr("size"), out.width, out.height, out.width, out.height);
            if (!componentXml->attr("pivot").empty()) pairValue(componentXml->attr("pivot"), out.pivotX, out.pivotY, out.pivotX, out.pivotY);
            if (!componentXml->attr("anchor").empty()) out.pivotAsAnchor = componentXml->attr("anchor") == "true";
        }
        if (!desc) return;
        for (const auto& kv : desc->attrs) out.attrs[kv.first] = kv.second;
        pairValue(desc->attr("xy"), out.x, out.y); pairValue(desc->attr("size"), out.width, out.height, out.width, out.height);
        if (!desc->attr("scale").empty()) pairValue(desc->attr("scale"), out.scaleX, out.scaleY, out.scaleX, out.scaleY);
        if (!desc->attr("pivot").empty()) {
            pairValue(desc->attr("pivot"), out.pivotX, out.pivotY, out.pivotX, out.pivotY);
            // Match GObject.setupBeforeAdd(): `anchor` is read only as part of an authored
            // instance pivot, and an omitted anchor means false.  Keeping the component root's
            // anchor here changes the meaning of the child instance's x/y after scaling (buttons
            // then jump left/up by pivot * size).  Resource defaults are valid until the instance
            // supplies its own pivot; from that point the instance pivot+anchor pair is atomic.
            out.pivotAsAnchor = desc->attr("anchor") == "true";
        }
        if (!desc->attr("skew").empty()) pairValue(desc->attr("skew"), out.skewX, out.skewY, out.skewX, out.skewY);
        if (!desc->attr("rotation").empty()) out.rotation = number(desc->attr("rotation"), out.rotation);
        if (!desc->attr("alpha").empty()) out.alpha = number(desc->attr("alpha"), out.alpha);
        out.visible = desc->attr("visible") != "false"; out.touchable = desc->attr("touchable") != "false";
        if (desc->attr("grayed") == "true") out.grayed = true;
    }
    static void relationsFrom(const XmlNode* desc, InstanceNode& node,
                              bool targetInOwner = false) {
        if (!desc) return;
        for (const auto& child : desc->children) if (child && child->name == "relation") {
            const std::string& target = child->attr("target");
            const auto sides = split(child->attr("sidePair"), ',');
            // Match this project's shipped Relations.addItems implementation exactly: legacy
            // FairyGUI reads at most the first horizontal and vertical entries (`i < 2`). Some
            // packages still contain newer-editor trailing entries such as center-center and
            // middle-middle. Treating those as live relations makes a GearSize resize compensate
            // its pivot and then recenter itself a second time, so button text jumps on OVER/DOWN.
            const size_t relationCount = std::min<size_t>(2, sides.size());
            for (size_t i = 0; i < relationCount; ++i) {
                std::string side = sides[i];
                if (side.empty()) continue;
                bool percent = side.back() == '%';
                if (percent) side.pop_back();
                if (side.find('-') == std::string::npos) side += "-" + side;
                node.relations.push_back({target, side, percent, targetInOwner});
                // RelationItem.internalAdd enables pixel snapping for percentage and centre/middle
                // relations. The setter immediately calls handleXYChanged(), which is observable
                // for an already-negatively-scaled GImage during construction.
                if (percent || side == "left-center" || side == "center-center" ||
                    side == "right-center" || side == "top-middle" ||
                    side == "middle-middle" || side == "bottom-middle")
                    node.pixelSnapping = true;
            }
        }
    }
    static std::vector<std::string> pageList(const std::string& s) { return s.empty() ? std::vector<std::string>{} : split(s, ','); }
    void controllersFrom(const XmlNode& componentXml, InstanceNode& node) {
        for (const auto& c : componentXml.children) if (c && c->name == "controller") {
            ControllerState st; st.name = c->attr("name"); st.autoRadioGroupDepth = c->attr("autoRadioGroupDepth") == "true"; auto pages = split(c->attr("pages"), ',');
            for (size_t i = 0; i < pages.size(); i += 2) { st.pageIds.push_back(pages[i]); st.pageNames.push_back(i + 1 < pages.size() ? pages[i + 1] : std::string()); }
            for (const auto& action : c->children) if (action) {
                const std::string& type = action->attr("type"); if (type != "play_transition" && type != "change_page") continue;
                ControllerActionDesc out; out.type = type; out.fromPage = action->attr("fromPage"); out.toPage = action->attr("toPage");
                out.objectId = action->attr("objectId"); out.controller = action->attr("controller"); out.targetPage = action->attr("targetPage");
                out.transition = action->attr("transition"); out.repeat = integer(action->attr("repeat"), 1); out.delay = number(action->attr("delay")); out.stopOnExit = action->attr("stopOnExit") == "true"; st.actions.push_back(std::move(out));
            }
            // Legacy FairyGUI exports encode transition actions as from-to=name triples on the
            // controller itself. Preserve the stock runtime's exact page-id conversion.
            for (std::string legacy : split(c->attr("transitions"), ',')) if (!legacy.empty()) {
                const size_t eq = legacy.find('='); if (eq == std::string::npos) continue; ControllerActionDesc out; out.type = "play_transition"; out.transition = legacy.substr(eq + 1); out.stopOnExit = true;
                std::string range = legacy.substr(0, eq); const size_t dash = range.find('-'); if (dash == std::string::npos) continue; std::string from = range.substr(0, dash), to = range.substr(dash + 1);
                int toIndex = integer(to, -1); if (toIndex >= 0 && toIndex < (int)st.pageIds.size()) out.toPage = st.pageIds[(size_t)toIndex];
                if (from != "*") { int fromIndex = integer(from, -1); if (fromIndex >= 0 && fromIndex < (int)st.pageIds.size()) out.fromPage = st.pageIds[(size_t)fromIndex]; }
                st.actions.push_back(std::move(out));
            }
            // This project ships the legacy FairyGUI Controller.setup implementation, which does
            // not read the XML `selected` attribute: attached controllers start on page 0 and
            // empty controllers start at -1. Matching that contract is essential because display
            // gears apply immediately during construction (for example coupon hover tips).
            st.selectedIndex = st.pageIds.empty() ? -1 : 0; node.controllers.push_back(std::move(st));
        }
    }
    static GearSizeValue sizeGearValue(const std::string& source, const GearSizeValue& fallback) {
        auto p = split(source, ','); GearSizeValue out = fallback;
        if (!p.empty()) out.width = number(p[0], fallback.width);
        if (p.size() > 1) out.height = number(p[1], fallback.height);
        if (p.size() > 2) out.scaleX = number(p[2], fallback.scaleX);
        if (p.size() > 3) out.scaleY = number(p[3], fallback.scaleY);
        return out;
    }
    static GearLookValue lookGearValue(const std::string& source, const GearLookValue& fallback) {
        auto p = split(source, ','); GearLookValue out = fallback;
        if (!p.empty()) out.alpha = number(p[0], fallback.alpha);
        if (p.size() > 1) out.rotation = number(p[1], fallback.rotation);
        if (p.size() > 2) out.grayed = p[2] == "1";   // GearLook.addStatus: arr[2] == "1"
        if (p.size() > 3) out.touchable = p[3] != "0" && p[3] != "false";
        return out;
    }
    void gearsFrom(const XmlNode* desc, InstanceNode& node) {
        if (!desc) return;
        for (const auto& c : desc->children) if (c) {
            if (c->name == "gearDisplay") {
                node.displayGear.controller = c->attr("controller"); node.displayGear.pages = pageList(c->attr("pages"));
            } else if (c->name == "gearXY") {
                node.xyGear.present = true; node.xyGear.controller = c->attr("controller"); node.xyGear.pages = pageList(c->attr("pages")); node.xyGear.defaultX = node.x; node.xyGear.defaultY = node.y;
                if (!c->attr("default").empty()) pairValue(c->attr("default"), node.xyGear.defaultX, node.xyGear.defaultY, node.x, node.y);
                for (const auto& value : split(c->attr("values"), '|')) { float x = node.xyGear.defaultX, y = node.xyGear.defaultY; pairValue(value, x, y, x, y); node.xyGear.values.emplace_back(x, y); }
            } else if (c->name == "gearSize") {
                node.sizeGear.present = true; node.sizeGear.controller = c->attr("controller"); node.sizeGear.pages = pageList(c->attr("pages")); node.sizeGear.defaultValue = {node.width, node.height, node.scaleX, node.scaleY};
                if (!c->attr("default").empty()) node.sizeGear.defaultValue = sizeGearValue(c->attr("default"), node.sizeGear.defaultValue);
                for (const auto& value : split(c->attr("values"), '|')) node.sizeGear.values.push_back(sizeGearValue(value, node.sizeGear.defaultValue));
            } else if (c->name == "gearLook") {
                node.lookGear.present = true; node.lookGear.controller = c->attr("controller"); node.lookGear.pages = pageList(c->attr("pages")); node.lookGear.defaultValue = {node.alpha, node.rotation, node.touchable, node.grayed};
                if (!c->attr("default").empty()) node.lookGear.defaultValue = lookGearValue(c->attr("default"), node.lookGear.defaultValue);
                for (const auto& value : split(c->attr("values"), '|')) node.lookGear.values.push_back(lookGearValue(value, node.lookGear.defaultValue));
            } else if (c->name == "gearIcon" || c->name == "gearText" || c->name == "gearColor" ||
                       c->name == "gearAni" || c->name == "gearAnimation" || c->name == "gearFontSize") {
                const bool animationGear = c->name == "gearAni" || c->name == "gearAnimation";
                GearStringState& gear = c->name == "gearIcon" ? node.iconGear :
                    (c->name == "gearText" ? node.textGear : (c->name == "gearColor" ? node.colorGear :
                    (animationGear ? node.animationGear : node.fontSizeGear)));
                gear.present = true; gear.controller = c->attr("controller"); gear.pages = pageList(c->attr("pages")); gear.values = split(c->attr("values"), '|'); gear.defaultValue = c->attr("default");
                if (gear.defaultValue.empty()) {
                    if (c->name == "gearIcon") gear.defaultValue = desc->attr("url");
                    else if (c->name == "gearText") gear.defaultValue = desc->attr("text");
                    else if (c->name == "gearColor") gear.defaultValue = desc->attr("color") + "," + desc->attr("strokeColor");
                    else if (animationGear) gear.defaultValue = desc->attr("frame") + "," + desc->attr("playing");
                    else gear.defaultValue = desc->attr("fontSize");
                }
            }
        }
    }
    static TransitionKind transitionKind(const std::string& type) {
        if (type == "XY") return TransitionKind::XY; if (type == "Size") return TransitionKind::Size; if (type == "Scale") return TransitionKind::Scale; if (type == "Pivot") return TransitionKind::Pivot; if (type == "Alpha") return TransitionKind::Alpha; if (type == "Rotation") return TransitionKind::Rotation; if (type == "Color") return TransitionKind::Color; if (type == "Animation") return TransitionKind::Animation; if (type == "Visible") return TransitionKind::Visible; if (type == "Sound") return TransitionKind::Sound; if (type == "Transition") return TransitionKind::Nested; if (type == "Shake") return TransitionKind::Shake; if (type == "ColorFilter") return TransitionKind::ColorFilter; if (type == "Skew") return TransitionKind::Skew; if (type == "Text") return TransitionKind::Text; if (type == "Icon") return TransitionKind::Icon; return TransitionKind::Unknown;
    }
    static void transitionsFrom(const XmlNode& xml, InstanceNode& node) {
        for (const auto& child : xml.children) if (child && child->name == "transition") {
            TransitionDesc desc; desc.name = child->attr("name"); desc.options = integer(child->attr("options")); desc.autoPlay = child->attr("autoPlay") == "true"; desc.autoPlayRepeat = integer(child->attr("autoPlayRepeat"), 1); desc.autoPlayDelay = number(child->attr("autoPlayDelay"));
            for (const auto& source : child->children) if (source && source->name == "item") {
                TransitionItemDesc item; item.kind = transitionKind(source->attr("type")); item.targetId = source->attr("target"); item.time = number(source->attr("time")) / 24.0f; item.tween = source->attr("tween") == "true"; item.duration = number(source->attr("duration")) / 24.0f; item.repeat = integer(source->attr("repeat")); item.yoyo = source->attr("yoyo") == "true"; item.value = source->attr("value"); item.startValue = source->attr("startValue"); item.endValue = source->attr("endValue"); item.label = source->attr("label"); item.label2 = source->attr("label2"); item.ease = source->attr("ease");
                // TransitionItem constructs with easeType=ParseEaseType("Quad.Out") and only an
                // authored ease attribute overrides it. An explicit unknown name (e.g. "Custom")
                // must keep ParseEaseType's Linear fallback, so default only the absent case.
                if (item.ease.empty()) item.ease = "Quad.Out";
                if (item.tween && item.endValue.empty()) { item.tween = false; item.value = item.startValue; }
                desc.maxTime = std::max(desc.maxTime, item.time + (item.tween ? item.duration : 0)); desc.items.push_back(std::move(item));
            }
            node.transitions.push_back(std::move(desc));
        }
    }
    static void reject(Instance& inst, const std::string& reason) { inst.compatible = false; if (inst.fallbackReason.empty()) inst.fallbackReason = reason; }
    static std::string richTextPlain(const std::string& source) {
        std::string out; out.reserve(source.size());
        for (size_t i=0;i<source.size();++i) {
            char c=source[i];
            if(c=='<'){
                size_t end=source.find('>',i+1);if(end==std::string::npos)break;
                std::string name=source.substr(i+1,end-i-1);for(char& q:name)q=static_cast<char>(std::tolower(static_cast<unsigned char>(q)));
                if(name=="br"||name=="br/"||name=="p"||name=="/p")out.push_back('\n');i=end;continue;
            }
            if(c=='&'){
                size_t end=source.find(';',i+1);if(end!=std::string::npos&&end-i<12){std::string e=source.substr(i,end-i+1);if(e=="&lt;")out.push_back('<');else if(e=="&gt;")out.push_back('>');else if(e=="&amp;")out.push_back('&');else if(e=="&quot;")out.push_back('"');else if(e=="&#39;")out.push_back('\'');else out+=e;i=end;continue;}
            }
            out.push_back(c);
        }
        return out;
    }
    bool translationFor(Instance& inst, int ownerComponent, const std::string& elementId, const std::string& suffix, std::string& out) const {
        InstanceNode* owner = nodeIn(inst, ownerComponent); if (!owner || elementId.empty()) return false;
        Package* pkg = package(owner->packageName); if (!pkg) return false;
        auto it = translations_.find(pkg->id + owner->sourceId + "-" + elementId + suffix);
        if (it == translations_.end()) return false; out = it->second; return true;
    }
    static std::string extensionOf(Package& pkg, const Resource* res) {
        if (!res || res->kind != ResourceKind::Component) return {};
        auto it = pkg.components.find(res->id); return it == pkg.components.end() ? std::string() : it->second->extension;
    }
    bool resolveComponentUrl(const std::string& url, const std::string& ownerPackage,
                             Package*& pkg, Resource*& res, Component*& component) const {
        pkg = nullptr; res = nullptr; component = nullptr; if (url.compare(0, 5, "ui://") != 0) return false;
        std::string body = url.substr(5), item; size_t slash = body.find('/');
        if (slash != std::string::npos) { pkg = packageForReference(body.substr(0, slash), ownerPackage); if (pkg) res = resourceByName(*pkg, body.substr(slash + 1)); }
        else if (body.size() > 8) { pkg = packageForReference(body.substr(0, 8), ownerPackage); item = body.substr(8); if (pkg) { auto it = pkg->resourcesById.find(item); if (it != pkg->resourcesById.end()) res = &it->second; } }
        if (!pkg || !res || res->kind != ResourceKind::Component) return false; auto ci = pkg->components.find(res->id); if (ci == pkg->components.end()) return false; component = ci->second.get(); return component != nullptr;
    }
    int buildComponent(Instance& inst, Package& pkg, Component& component, const XmlNode* desc, int parent, int ownerComponent, sg::SceneGraph& graph, int depth) {
        if (depth > 96) { reject(inst, "component-depth"); return 0; }
        if (component.hasUnsupportedStaticNode) reject(inst, component.name + ":" + component.unsupportedReason);
        if (!component.extension.empty() && component.extension != "Button" && component.extension != "Label" &&
            component.extension != "ProgressBar" && component.extension != "ScrollBar" && component.extension != "Slider" &&
            component.extension != "ComboBox")
            reject(inst, "extension:" + component.extension);
        int id = graph.alloc(); if (id <= 0) return 0;
        auto n = std::make_unique<InstanceNode>(); n->nativeId = id; n->parentNativeId = parent; n->ownerComponentId = ownerComponent ? ownerComponent : id; n->kind = ObjectKind::Component; n->packageName = pkg.name; n->sourceId = component.id; n->extension = component.extension;
        n->id = desc ? desc->attr("id") : component.id; n->name = desc ? desc->attr("name") : component.name; readTransform(desc, nullptr, component.xml.get(), *n);
        // constructInternal first installs the component resource size, then setupBeforeAdd may
        // replace initWidth/initHeight with the containing descriptor size. readTransform already
        // produced the latter as live geometry; recover the former directly from the component.
        n->sourceWidth = n->width; n->sourceHeight = n->height;
        pairValue(component.xml->attr("size"), n->sourceWidth, n->sourceHeight, n->width, n->height);
        n->initWidth = n->width; n->initHeight = n->height;
        relationsFrom(component.xml.get(), *n, /*targetInOwner=*/true);
        relationsFrom(desc, *n); controllersFrom(*component.xml, *n); gearsFrom(desc, *n); transitionsFrom(*component.xml, *n);
        // Preserve the small amount of extension/setup metadata required by lazy semantic
        // handles (Button/Label/ProgressBar) without sending the whole XML tree back to JS.
        for (const auto& c : component.xml->children) if (c && c->name == component.extension)
            for (const auto& kv : c->attrs) n->attrs["component." + kv.first] = kv.second;
        // setupAfterAdd receives the containing display-list descriptor only for a placed child.
        // Keep that construction boundary explicit: a nested ProgressBar without a setup node
        // retains its constructor value (50/100), while an authored but empty <ProgressBar/>
        // resets both fields to zero. Attribute presence alone cannot distinguish those cases.
        if (desc && component.extension == "ProgressBar") n->attrs["setup.placed"] = "true";
        // GButton.constructFromXML gives every scale-down button a centred, non-anchor pivot
        // before the containing component applies this instance descriptor.  The distinction is
        // observable even at scale=1: an authored component-root anchor describes x/y as its
        // centre, while the final button pivot keeps x/y at the visual top-left.  A lazy button
        // facade used to perform this conversion only on its first pointer event, making the
        // button jump and exposing any correctly-centred progress/mask artwork underneath it.
        // Compile the stock extension rule into the native blueprint.  An explicit instance
        // pivot still wins, matching setupBeforeAdd() which runs after constructFromXML().
        if (component.extension == "Button" &&
            n->attrs["component.downEffect"] == "scale" &&
            (!desc || desc->attr("pivot").empty())) {
            n->pivotX = 0.5f;
            n->pivotY = 0.5f;
            n->pivotAsAnchor = false;
        }
        if (desc) for (const auto& c : desc->children) if (c && c->name == component.extension) {
            if (component.extension == "ProgressBar") n->attrs["setup.extensionPresent"] = "true";
            for (const auto& kv : c->attrs) n->attrs["setup." + kv.first] = kv.second;
            // ComboBox setup contains a variable number of authored item children.  Keep them in
            // compact indexed metadata so its semantic facade can populate the native-backed
            // dropdown lazily without retaining or reparsing the parent component XML in JS.
            if (component.extension == "ComboBox") {
                int itemIndex = 0;
                for (const auto& item : c->children) if (item && item->name == "item") {
                    const std::string prefix = "setup.item." + std::to_string(itemIndex++) + ".";
                    for (const auto& kv : item->attrs) n->attrs[prefix + kv.first] = kv.second;
                }
                n->attrs["setup.itemCount"] = std::to_string(itemIndex);
            }
        }
        if (desc && ownerComponent > 0) {
            std::string value;
            if (translationFor(inst, ownerComponent, n->id, "", value)) n->attrs["setup.title"] = value;
            if (translationFor(inst, ownerComponent, n->id, "-0", value)) n->attrs["setup.selectedTitle"] = value;
            if (translationFor(inst, ownerComponent, n->id, "-tips", value)) n->attrs["tooltips"] = value;
        }
        // GButton.setupAfterAdd applies an authored checked="true" before the first frame. Lazy
        // Native buttons do not run that JS method until first use, so seed the same two pieces of
        // controller state while the blueprint is built: the button's visual state becomes down
        // (selectedDisabled when authored grayed), and its related parent controller selects the
        // button's page. Without this, a default Radio tab has correct data/content but paints as
        // an unselected `up` tab until it is clicked once.
        if (component.extension == "Button" && n->attrs["setup.checked"] == "true" &&
            n->attrs["component.mode"] != "Common" && !n->attrs["component.mode"].empty()) {
            const std::string visualPage = n->grayed ? "selectedDisabled" : "down";
            for (ControllerState& controller : n->controllers) if (controller.name == "button") {
                auto page = std::find(controller.pageNames.begin(), controller.pageNames.end(), visualPage);
                if (page != controller.pageNames.end()) controller.selectedIndex = static_cast<int>(page - controller.pageNames.begin());
                break;
            }
            const std::string relatedName = n->attrs["setup.controller"], relatedPage = n->attrs["setup.page"];
            InstanceNode* relatedOwner = parent > 0 ? nodeIn(inst, parent) : nullptr;
            if (relatedOwner && !relatedName.empty() && !relatedPage.empty()) {
                for (ControllerState& controller : relatedOwner->controllers) if (controller.name == relatedName) {
                    auto page = std::find(controller.pageIds.begin(), controller.pageIds.end(), relatedPage);
                    if (page != controller.pageIds.end()) controller.selectedIndex = static_cast<int>(page - controller.pageIds.begin());
                    break;
                }
            }
        }
        n->displayParentNativeId = parent;
        writeNode(graph, *n); if (parent > 0) graph.setParent(id, parent, -1); InstanceNode* raw = n.get(); inst.byNativeId[id] = raw; inst.nodes.push_back(std::move(n));
        if (raw->grayed) writeNodeGrayed(graph, *raw);

        float marginTop=0,marginBottom=0,marginLeft=0,marginRight=0;
        auto marginValues=split(raw->attrs["margin"],',');
        if(marginValues.size()==1&&!marginValues[0].empty())marginTop=marginBottom=marginLeft=marginRight=number(marginValues[0]);
        else if(marginValues.size()>=4){marginTop=number(marginValues[0]);marginBottom=number(marginValues[1]);marginLeft=number(marginValues[2]);marginRight=number(marginValues[3]);}
        const std::string overflow=raw->attrs.count("overflow")?raw->attrs["overflow"]:std::string();
        const bool scrolling=overflow=="scroll",hidden=overflow=="hidden";
        const bool needsContent=scrolling||hidden||std::fabs(marginLeft)>0.0001f||std::fabs(marginTop)>0.0001f;
        auto internalNode=[&](ObjectKind kind,const char* name)->InstanceNode*{
            int internalId=graph.alloc();if(internalId<=0){reject(inst,"internal-display-node");return nullptr;}
            auto internal=std::make_unique<InstanceNode>();internal->nativeId=internalId;internal->parentNativeId=id;internal->displayParentNativeId=id;internal->ownerComponentId=id;internal->kind=kind;internal->packageName=pkg.name;internal->sourceId=component.id;internal->name=name;internal->touchable=false;internal->internalDisplayNode=true;
            InstanceNode* result=internal.get();writeNode(graph,*result);graph.setParent(internalId,id,-1);inst.byNativeId[internalId]=result;inst.nodes.push_back(std::move(internal));return result;
        };
        if(needsContent){
            InstanceNode* content=internalNode(ObjectKind::Component,"$nativeContent");
            if(content){content->x=scrolling?0:marginLeft;content->y=scrolling?0:marginTop;writeNode(graph,*content);raw->displayContainerId=content->nativeId;}
        }
        const int scrollFlags=integer(raw->attrs["scrollBarFlags"]);
        if((hidden||scrolling)&&!(scrolling&&(scrollFlags&512))){
            InstanceNode* mask=internalNode(ObjectKind::Graph,"$nativeClip");
            if(mask){mask->displayMask=true;mask->attrs["type"]="rect";mask->x=scrolling?marginLeft:0;mask->y=scrolling?marginTop:0;mask->width=std::max(0.0f,raw->width-(scrolling?marginLeft:0)-marginRight);mask->height=std::max(0.0f,raw->height-(scrolling?marginTop:0)-marginBottom);writeNode(graph,*mask);writeGraph(graph,*mask);raw->clipMaskId=mask->nativeId;linkPrototypeMask(graph,*raw,*mask);}
        }

        const XmlNode* list = component.xml->child("displayList"); if (list) for (const auto& childDesc : list->children) if (childDesc) {
            int childId=buildObject(inst,pkg,*childDesc,id,id,graph,depth+1);
            if(childId>0&&raw->displayContainerId>0){InstanceNode* child=nodeIn(inst,childId);if(child){child->displayParentNativeId=raw->displayContainerId;graph.setParent(childId,raw->displayContainerId,-1);}}
        }
        const std::string& maskId = component.xml->attr("mask");
        if (std::getenv("NATIVE_FGUI_TRACE_MASK")) {
            std::fprintf(stderr, "[native-fgui-mask] component=%s/%s node=%s mask=%s children=%zu\n",
                pkg.name.c_str(), component.name.c_str(), raw->name.c_str(), maskId.c_str(), raw->children.size());
            for (int childId : raw->children) {
                InstanceNode* child = nodeIn(inst, childId);
                if (child && (child->kind == ObjectKind::Graph || child->name == "mask"))
                    std::fprintf(stderr, "[native-fgui-mask]   graph native=%d id=%s name=%s type=%s size=%.1fx%.1f\n",
                        child->nativeId, child->id.c_str(), child->name.c_str(), child->attrs["type"].c_str(), child->width, child->height);
            }
        }
        if (!maskId.empty()) {
            // The instance is published only after construction, so resolve masks in the
            // local node table instead of calling Runtime::child(instanceId,...).
            for (int childId : raw->children) {
                InstanceNode* m = nodeIn(inst, childId);
                if (m && (m->id == maskId || m->name == maskId)) {
                    m->displayMask = true;
                    raw->clipMaskId = m->nativeId;
                    graph.buf[(size_t)id * sg::STRIDE + sg::MASKI] = static_cast<float>(m->nativeId);
                    // A Pixi mask participates in clipping but is not painted as an ordinary
                    // display-list child. Without this, large white rectangle masks become
                    // visible when an entire component is constructed directly in C++.
                    float* mask = graph.buf + (size_t)m->nativeId * sg::STRIDE;
                    int maskFlags = static_cast<int>(mask[sg::HITFLAGS]) | 4;
                    if (m->kind == ObjectKind::Image) maskFlags |= 8;
                    if (m->kind == ObjectKind::Graph) {
                        const std::string type = m->attrs.count("type") ? m->attrs.at("type") : std::string();
                        const bool ellipse = type == "ellipse" || type == "eclipse";
                        const bool rounded = m->attrs.count("corner") && !m->attrs.at("corner").empty() && number(split(m->attrs.at("corner"), ',')[0]) > 0;
                        if (ellipse) maskFlags |= 16;
                        if (ellipse || rounded || std::fabs(m->rotation) > 0.0001f || std::fabs(m->skewX) > 0.0001f || std::fabs(m->skewY) > 0.0001f) maskFlags |= 64;
                    }
                    mask[sg::HITFLAGS] = static_cast<float>(maskFlags);
                    mask[sg::RENDERABLE] = 0;
                    break;
                }
            }
        }
        // FairyGUI constructs a nested instance at its authored source size and only afterwards
        // applies the editor placement size: constructInternal builds at sourceWidth/Height,
        // then setupBeforeAdd's setSize fires SIZE_CHANGED into the child's already-live
        // relations, letting each direct child absorb the delta. The native builder reads the
        // placed size before children exist, so replay that source->placed resize here.
        // Direct children only: sibling relations targeting this instance are wired after all
        // placed sizes in the stock runtime, so they must not react (and this instance is not
        // yet in its parent's child list, keeping applyRelations' recursion inside the subtree).
        if (desc && !desc->attr("size").empty()) {
            float sourceWidth = raw->width, sourceHeight = raw->height;
            pairValue(component.xml->attr("size"), sourceWidth, sourceHeight, raw->width, raw->height);
            if (std::fabs(raw->width - sourceWidth) > 0.0001f || std::fabs(raw->height - sourceHeight) > 0.0001f) {
                for (int childId : raw->children) {
                    InstanceNode* child = nodeIn(inst, childId); if (!child) continue;
                    InstanceNode& owner = *child;
                    float oldX = owner.x, oldY = owner.y, oldW = owner.width, oldH = owner.height;
                    bool matched = false;
                    for (const RelationDesc& relation : owner.relations) if (relationTarget(inst, owner, relation) == raw) {
                        const float relationOldW = owner.width, relationOldH = owner.height;
                        applyTargetSizeRelation(owner, *raw, relation, sourceWidth, sourceHeight); matched = true;
                        applyRelationResizeSemantics(owner, *raw, relation, relationOldW, relationOldH);
                    }
                    if (!matched) continue;
                    adjustAuthoredRelationDeltas(*raw, owner, owner.x - oldX, owner.y - oldY, owner.width - oldW, owner.height - oldH);
                    if (owner.x != oldX || owner.y != oldY || owner.width != oldW || owner.height != oldH) {
                        const float ownerDx=owner.x-oldX,ownerDy=owner.y-oldY;
                        if (ownerDx != 0.0f || ownerDy != 0.0f) queueTransitionProperty(owner, TransitionPropertyKind::XY, owner.x, owner.y);
                        if (owner.width != oldW || owner.height != oldH) queueTransitionProperty(owner, TransitionPropertyKind::Size, owner.width, owner.height);
                        writeNode(graph, owner); refreshVisual(owner, graph);
                        applyRelations(inst, owner, ownerDx, ownerDy,
                                       owner.width - oldW, owner.height - oldH, graph, 1);
                        moveGroupMembers(inst, owner, ownerDx, ownerDy, graph);
                    }
                }
            }
        }
        return id;
    }
    int buildObject(Instance& inst, Package& ownerPkg, const XmlNode& desc, int parent, int ownerComponent, sg::SceneGraph& graph, int depth) {
        Package* srcPkg = &ownerPkg; const std::string& pkgId = desc.attr("pkg");
        if (!pkgId.empty()) srcPkg = packageForReference(pkgId, ownerPkg.name);
        Resource* res = nullptr; const std::string& src = desc.attr("src");
        if (srcPkg && !src.empty()) { auto it = srcPkg->resourcesById.find(src); if (it != srcPkg->resourcesById.end()) res = &it->second; }
        // Match UIPackage.loadComponentChildren: an unresolved package/item is not a malformed
        // component. FairyGUI records DisplayListItem(null, tagName), then constructs a direct
        // GImage/GComponent/etc. placeholder from the XML tag. This lets stale decorative assets
        // remain blank without aborting their owning popup (for example Inbox HeadIcon's removed
        // dac615xb/ju391t frame). Keep the owner package only as semantic context; a direct object
        // has no PackageItem/source id and must not acquire the similarly named owner resource.
        // This FairyGUI/Pixi runtime stores gridTile but UIImage.tiledSlices deliberately ignores
        // it.  Native NineSlice has the same stretched-centre semantics, so the flag is compatible
        // and must not force the containing component back to a full JavaScript display tree.
        if (res && res->kind == ResourceKind::Component) {
            auto ci = srcPkg->components.find(res->id); if (ci != srcPkg->components.end()) {
                int id = buildComponent(inst, *srcPkg, *ci->second, &desc, parent, ownerComponent, graph, depth);
                if (id > 0) { InstanceNode* p = nodeIn(inst, parent); if (p) p->children.push_back(id); }
                return id;
            }
        }
        int id = graph.alloc(); if (id <= 0) return 0;
        auto n = std::make_unique<InstanceNode>(); n->nativeId = id; n->parentNativeId = parent; n->ownerComponentId = ownerComponent; n->kind = res && res->kind == ResourceKind::Image ? ObjectKind::Image : objectKind(desc.name); n->packageName = res ? srcPkg->name : ownerPkg.name; n->sourceId = res ? res->id : std::string(); n->extension = res ? extensionOf(*srcPkg, res) : std::string(); n->id = desc.attr("id"); n->name = desc.attr("name"); readTransform(&desc, res, nullptr, *n); initializeImageDisplayPosition(*n, desc);
        // Resource-backed objects are constructed at PackageItem size before their descriptor is
        // applied. Non-resource objects retain the prior live-size fallback while still exposing
        // a stable init size to late semantic facades.
        n->sourceWidth = res ? res->width : n->width; n->sourceHeight = res ? res->height : n->height;
        n->initWidth = n->width; n->initHeight = n->height;
        relationsFrom(&desc, *n);
        // Relations.setup runs after GImage.setupBeforeAdd. Enabling pixelSnapping is itself a
        // handleXYChanged call, so it sees the final authored scale and explicit flip state.
        if (n->kind == ObjectKind::Image && n->pixelSnapping) handleXYChanged(*n);
        gearsFrom(&desc, *n);
        // GLoader.setupBeforeAdd resolves a package URL after the authored placeholder size and
        // pivot have been installed. updateLayout then replaces an auto-size Loader's live box
        // with the image dimensions. Native paints the resolved image directly and has no
        // duplicate JS $content, so compile that semantic resize here as part of the blueprint.
        // Keep initWidth/initHeight above as the authored setup size, matching GObject.
        if (n->kind == ObjectKind::Loader) {
            float contentWidth = 0, contentHeight = 0;
            if (resolvedLoaderAutoSize(*n, contentWidth, contentHeight)) {
                const float dw = contentWidth - n->width, dh = contentHeight - n->height;
                if (!n->pivotAsAnchor) {
                    n->x -= n->pivotX * dw;
                    n->y -= n->pivotY * dh;
                }
                n->width = contentWidth;
                n->height = contentHeight;
            }
        }
        if (ownerComponent > 0) {
            std::string value;
            if ((n->kind == ObjectKind::Text || n->kind == ObjectKind::RichText) && translationFor(inst, ownerComponent, n->id, "", value)) n->attrs["text"] = value;
            if (translationFor(inst, ownerComponent, n->id, "-tips", value)) n->attrs["tooltips"] = value;
        }
        // RichText markup is intentionally preserved. The host converts it to a native
        // attributed string; stripping tags here used to make every styled run disappear.
        if (n->kind == ObjectKind::Unknown) reject(inst, "object:" + desc.name);
        if (n->kind == ObjectKind::Text && std::getenv("NATIVE_FGUI_TRACE")) {
            const auto value = [&](const char* key) -> const char* { auto it = n->attrs.find(key); return it == n->attrs.end() ? "" : it->second.c_str(); };
            std::fprintf(stderr, "[native-fgui-text] %s/%s native=%d parent=%d owner=%d id=%s name=%s xy=%.1f,%.1f size=%.1fx%.1f font=%s fontSize=%s autoSize=%s align=%s text=%s\n",
                inst.packageName.c_str(), inst.componentName.c_str(), n->nativeId, n->parentNativeId, n->ownerComponentId, n->id.c_str(), n->name.c_str(), n->x, n->y, n->width, n->height,
                value("font"), value("fontSize"), value("autoSize"), value("align"), value("text"));
        }
        if (n->kind == ObjectKind::Image || n->kind == ObjectKind::Text || n->kind == ObjectKind::RichText || n->kind == ObjectKind::MovieClip || n->kind == ObjectKind::Group) n->touchable = false;
        writeNode(graph, *n); if (parent > 0) graph.setParent(id, parent, -1); if (n->kind == ObjectKind::Image && res) { if (!writeImage(graph, *srcPkg, *res, *n)) reject(inst, "atlas:" + srcPkg->name + "/"+res->id); } else if (n->kind == ObjectKind::MovieClip && res) { if(!writeMovieClip(graph,*srcPkg,*res,*n))reject(inst,"movieclip:"+srcPkg->name+"/"+res->id); } else if (n->kind == ObjectKind::Graph) writeGraph(graph, *n); else if (n->kind == ObjectKind::Loader) { if (writeLoader(graph, *n)) n->attrs["nativeResolved"] = "true"; } else if (isBitmapText(*n)) { if(!writeBitmapText(graph,*n))reject(inst,"bitmap-font:"+n->attrs.find("font")->second); } else if ((n->kind == ObjectKind::Text || n->kind == ObjectKind::RichText || n->kind == ObjectKind::InputText) && prepareText) prepareTextSurface(*n, TextPrepareMode::AuthoredBox, graph);
        InstanceNode* raw = n.get(); inst.byNativeId[id] = raw; inst.nodes.push_back(std::move(n)); InstanceNode* p = nodeIn(inst, parent); if (p) p->children.push_back(id);
        if (raw->grayed) writeNodeGrayed(graph, *raw);
        if (raw->kind == ObjectKind::List) {
            // Authored item placement mirrors GList.updateBounds (the non-autoResizeItem
            // branches) plus handleAlign. Coordinates are content-space: the runtime parks items
            // in an inner container at margin+alignOffset, and native bakes that offset into
            // each child because authored lists have no separate content node.
            const std::string layout = raw->attrs["layout"]; float gapX = number(raw->attrs["colGap"]), gapY = number(raw->attrs["lineGap"]);
            float marginTop = 0, marginBottom = 0, marginLeft = 0, marginRight = 0;
            { auto margin = split(raw->attrs["margin"], ',');
              if (margin.size() == 1 && !margin[0].empty()) marginTop = marginBottom = marginLeft = marginRight = number(margin[0]);
              else if (margin.size() >= 4) { marginTop = number(margin[0]); marginBottom = number(margin[1]); marginLeft = number(margin[2]); marginRight = number(margin[3]); } }
            const float viewWidth = raw->width - marginLeft - marginRight, viewHeight = raw->height - marginTop - marginBottom;
            // lineItemCount forces the wrap count (flow_hz/pagination columns, flow_vt lines);
            // lineItemCount2 is the pagination line count (GList.setupBeforeAdd).
            int columnCount = 0, lineCount = 0;
            { const int forced = std::max(0, integer(raw->attrs["lineItemCount"]));
              if (layout == "flow_hz" || layout == "pagination") columnCount = forced;
              else if (layout == "flow_vt") lineCount = forced;
              auto second = raw->attrs.find("lineItemCount2");
              if (second != raw->attrs.end() && !second->second.empty()) lineCount = std::max(0, integer(second->second)); }
            float curX = 0, curY = 0, maxWidth = 0, maxHeight = 0;
            int lineItems = 0, pageLines = 0, page = 0;
            std::vector<InstanceNode*> laidOut;
            for (const auto& item : desc.children) if (item && item->name == "item") {
                std::string url = item->attr("url"); if (url.empty()) url = raw->attrs["defaultItem"]; Package* itemPkg = nullptr; Resource* itemRes = nullptr; Component* itemComponent = nullptr;
                // Some exported lists omit defaultItem and put the resource URL only on their
                // first authored item. The browser GList learns the template while constructing
                // that item; preserve the same value for the lightweight business handle.
                if (raw->attrs["defaultItem"].empty() && !url.empty()) raw->attrs["defaultItem"] = url;
                if (!resolveComponentUrl(url, raw->packageName, itemPkg, itemRes, itemComponent)) continue;
                int childId = buildComponent(inst, *itemPkg, *itemComponent, nullptr, id, 0, graph, depth + 1); InstanceNode* child = nodeIn(inst, childId); if (!child) continue;
                raw->children.push_back(childId); child->authoredListItem = true; if (!item->attr("name").empty()) child->name = item->attr("name"); if (!item->attr("title").empty()) child->attrs["setup.title"] = item->attr("title"); if (!item->attr("icon").empty()) child->attrs["setup.icon"] = item->attr("icon");
                const float w = std::ceil(child->width), h = std::ceil(child->height);
                if (layout == "row") {
                    if (curX != 0) curX += gapX;
                    child->x = curX; child->y = 0; curX += w;
                    maxWidth = std::max(maxWidth, curX); maxHeight = std::max(maxHeight, h);
                } else if (layout == "flow_hz") {
                    if (curX != 0) curX += gapX;
                    if ((columnCount != 0 && lineItems >= columnCount) ||
                        (columnCount == 0 && curX + w > viewWidth && maxHeight != 0)) {
                        curX = 0; curY += std::ceil(maxHeight) + gapY; maxHeight = 0; lineItems = 0;
                    }
                    child->x = curX; child->y = curY; curX += w;
                    maxWidth = std::max(maxWidth, curX); maxHeight = std::max(maxHeight, h); ++lineItems;
                } else if (layout == "flow_vt") {
                    if (curY != 0) curY += gapY;
                    if ((lineCount != 0 && lineItems >= lineCount) ||
                        (lineCount == 0 && curY + h > viewHeight && maxWidth != 0)) {
                        curY = 0; curX += std::ceil(maxWidth) + gapX; maxWidth = 0; lineItems = 0;
                    }
                    child->x = curX; child->y = curY; curY += h;
                    maxHeight = std::max(maxHeight, curY); maxWidth = std::max(maxWidth, w); ++lineItems;
                } else if (layout == "pagination") {
                    if (curX != 0) curX += gapX;
                    if ((columnCount != 0 && lineItems >= columnCount) ||
                        (columnCount == 0 && curX + w > viewWidth && maxHeight != 0)) {
                        curX = 0; curY += std::ceil(maxHeight) + gapY; maxHeight = 0; lineItems = 0; ++pageLines;
                        if ((lineCount != 0 && pageLines >= lineCount) ||
                            (lineCount == 0 && curY + h > viewHeight && maxWidth != 0)) { ++page; curY = 0; pageLines = 0; }
                    }
                    child->x = page * viewWidth + curX; child->y = curY; curX += w;
                    maxWidth = std::max(maxWidth, curX); maxHeight = std::max(maxHeight, h); ++lineItems;
                } else {   // column (SingleColumn default)
                    if (curY != 0) curY += gapY;
                    child->x = 0; child->y = curY; curY += h;
                    maxWidth = std::max(maxWidth, w); maxHeight = std::max(maxHeight, curY);
                }
                laidOut.push_back(child);
            }
            if (!laidOut.empty()) {
                float contentWidth, contentHeight;
                if (layout == "row") { contentWidth = curX; contentHeight = std::ceil(maxHeight); }
                else if (layout == "flow_hz") { contentWidth = std::ceil(maxWidth); contentHeight = curY + std::ceil(maxHeight); }
                else if (layout == "flow_vt") { contentWidth = curX + std::ceil(maxWidth); contentHeight = std::ceil(maxHeight); }
                else if (layout == "pagination") { contentHeight = page > 0 ? viewHeight : curY + std::ceil(maxHeight); contentWidth = (page + 1) * viewWidth; }
                else { contentWidth = std::ceil(maxWidth); contentHeight = maxHeight; }
                const std::string align = raw->attrs["align"], vAlign = raw->attrs["vAlign"];
                float offsetX = 0, offsetY = 0;
                if (contentHeight < viewHeight) {
                    if (vAlign == "middle") offsetY = std::floor((viewHeight - contentHeight) / 2);
                    else if (vAlign == "bottom") offsetY = viewHeight - contentHeight;
                }
                if (contentWidth < viewWidth) {
                    if (align == "center") offsetX = std::floor((viewWidth - contentWidth) / 2);
                    else if (align == "right") offsetX = viewWidth - contentWidth;
                }
                for (InstanceNode* child : laidOut) {
                    child->x += marginLeft + offsetX; child->y += marginTop + offsetY;
                    writeNode(graph, *child);
                }
            }
        }
        return id;
    }
    static InstanceNode* nodeIn(Instance& inst, int id) { auto it = inst.byNativeId.find(id); return it == inst.byNativeId.end() ? nullptr : it->second; }
    // 诊断过滤器一次性缓存:writeNode 是三平台每帧数百次的热路径,逐次 getenv 扫 environ 白付
    // 最多 ~0.5ms/帧(compute 的 envChecked 同一先例;环境变量进程内不变)。
    static const char* traceRelationsFilter() {
        static const char* f = std::getenv("NATIVE_FGUI_TRACE_RELATIONS");
        return f;
    }
    // 临时诊断(PiggyBank scale 撕裂):scaleX 写点溯源——NATIVE_FGUI_TRACE_RELATIONS 过滤器命中的
    // 节点,每次 scaleX 变化打印写者标签+新旧值。定位后移除。
    static void traceScaleWrite(const InstanceNode& n, const char* who, float oldSx, float newSx) {
        if (oldSx == newSx) return;
        const char* f = traceRelationsFilter();
        if (!f || (std::strcmp(f, "all") != 0 && n.name.find(f) == std::string::npos)) return;
        std::fprintf(stderr, "[fgui-scale] %s %s %.4f -> %.4f (w=%.1f)\n", n.name.c_str(), who, oldSx, newSx, n.width);
    }
    static int parseImageFlipState(const std::string& value) {
        if (value == "hz") return 1;
        if (value == "vt") return 2;
        if (value == "both") return 3;
        return 0;
    }
    static std::pair<float,float> gObjectDisplayPosition(const InstanceNode& n) {
        // FairyGUI does not assign Pixi's transform.pivot directly.  GObject::updatePivotOffset
        // transforms the authored pivot by the complete local linear matrix and then folds the
        // compensation into displayObject.position.  Merely subtracting pivot*size for anchors
        // makes every rotated child sharing one authored anchor collapse onto the same point until
        // a later JS scale/rotation write happens to call applyPivot().  Runtime-added Text, Spine
        // and particles then visibly detach from otherwise-correct native FGUI artwork.
        constexpr float kDegToRad = 0.01745329251994329577f;
        const float rotation = n.rotation * kDegToRad;
        // GObject::setSkew maps FairyGUI skewX to Pixi skew.x with the opposite sign.
        const float skewX = -n.skewX * kDegToRad, skewY = n.skewY * kDegToRad;
        const float la = std::cos(rotation + skewY) * n.scaleX;
        const float lb = std::sin(rotation + skewY) * n.scaleX;
        const float lc = -std::sin(rotation - skewX) * n.scaleY;
        const float ld = std::cos(rotation - skewX) * n.scaleY;
        const float pivotX = n.pivotX * n.width, pivotY = n.pivotY * n.height;
        const float transformedPivotX = pivotX * la + pivotY * lc;
        const float transformedPivotY = pivotX * lb + pivotY * ld;
        float baseX = n.x - (n.pivotAsAnchor ? pivotX : 0.0f);
        float baseY = n.y - (n.pivotAsAnchor ? pivotY : 0.0f);
        if (n.pixelSnapping) { baseX = std::round(baseX); baseY = std::round(baseY); }
        const float pivotOffsetX = pivotX - transformedPivotX;
        const float pivotOffsetY = pivotY - transformedPivotY;
        return {baseX + pivotOffsetX, baseY + pivotOffsetY};
    }
    static void handleXYChanged(InstanceNode& n) {
        auto position = gObjectDisplayPosition(n);
        if (n.kind == ObjectKind::Image && n.imageFlipState != 0) {
            // This is deliberately the exact equality used by the shipped GImage implementation,
            // not a generic "negative scale" rule. scale=-2 remains a normal transform.
            if (n.scaleX == -1.0f) position.first += n.width;
            if (n.scaleY == -1.0f) position.second += n.height;
        }
        if (n.kind == ObjectKind::Image) {
            n.imageDisplayX = position.first;
            n.imageDisplayY = position.second;
            n.imageDisplayPositionValid = true;
        }
        // Browser handleXYChanged writes displayObject.position.set(...), replacing a prior raw
        // display-object move even when semantic x/y itself did not change (pivot/pixel snapping).
        n.directDisplayOffsetX = 0;
        n.directDisplayOffsetY = 0;
    }
    static void setSemanticPosition(InstanceNode& n, float x, float y) {
        if (n.x == x && n.y == y) return;
        n.x = x;
        n.y = y;
        handleXYChanged(n);
    }
    static void setSemanticScale(InstanceNode& n, float scaleX, float scaleY) {
        if (n.scaleX == scaleX && n.scaleY == scaleY) return;
        n.scaleX = scaleX;
        n.scaleY = scaleY;
        // GObject.setScale calls applyPivot(); applyPivot is a no-op at a zero pivot.
        if (n.pivotX != 0.0f || n.pivotY != 0.0f) handleXYChanged(n);
    }
    static void setSemanticPivot(InstanceNode& n, float pivotX, float pivotY, bool pivotAsAnchor) {
        if (n.pivotX == pivotX && n.pivotY == pivotY && n.pivotAsAnchor == pivotAsAnchor) return;
        n.pivotX = pivotX;
        n.pivotY = pivotY;
        n.pivotAsAnchor = pivotAsAnchor;
        // Unlike applyPivot(), setPivot always calls handleXYChanged, including a transition back
        // to a zero pivot.
        handleXYChanged(n);
    }
    static void setSemanticRotation(InstanceNode& n, float rotation) {
        if (n.rotation == rotation) return;
        n.rotation = rotation;
        if (n.pivotX != 0.0f || n.pivotY != 0.0f) handleXYChanged(n);
    }
    static void setSemanticSkew(InstanceNode& n, float skewX, float skewY) {
        if (n.skewX == skewX && n.skewY == skewY) return;
        n.skewX = skewX;
        n.skewY = skewY;
        if (n.pivotX != 0.0f || n.pivotY != 0.0f) handleXYChanged(n);
    }
    static void initializeImageDisplayPosition(InstanceNode& n, const XmlNode& desc) {
        if (n.kind != ObjectKind::Image) return;
        // GImage.constructFromResource starts at (0,0). setupBeforeAdd then calls setXY before it
        // applies size/scale/rotation/skew/pivot. With no later handleXYChanged the display object
        // therefore retains the authored xy even though the semantic scale is already negative.
        n.imageDisplayX = n.x;
        n.imageDisplayY = n.y;
        n.imageDisplayPositionValid = true;
        n.imageFlipState = -1; // GImage.$flip is still undefined while super.setupBeforeAdd runs.
        if (!desc.attr("pivot").empty() &&
            (n.pivotX != 0.0f || n.pivotY != 0.0f || n.pivotAsAnchor))
            handleXYChanged(n); // setPivot is after setScale in GObject.setupBeforeAdd.
        const std::string& flip = desc.attr("flip");
        if (!flip.empty()) n.imageFlipState = parseImageFlipState(flip); // GImage.setupBeforeAdd.
    }
    static std::pair<float,float> nodeDisplayPosition(const InstanceNode& n) {
        // GTextField deliberately overrides GObject.handleXYChanged. The base method first writes
        // the semantic pivot compensation, then GTextField.updatePosition immediately replaces
        // displayObject.position with x + its text-layout offset. GTextInput inherits that
        // override, but the shipped `richtext` registry maps to GTaggedText, which derives directly
        // from GObject and must retain the normal pivot compensation. Treating every text-like
        // object as a GTextField moves anchored TaggedText by pivot * size (half a 480px mission
        // label in the common 0.5-anchor case). Keep the distinction at the class-semantics layer;
        // publishedDisplayPosition adds the natural GTextField layout offset below when needed.
        const bool textField = n.kind == ObjectKind::Text || n.kind == ObjectKind::InputText;
        std::pair<float,float> position = textField
            ? std::make_pair(n.x, n.y)
            : (n.kind == ObjectKind::Image && n.imageDisplayPositionValid
                ? std::make_pair(n.imageDisplayX, n.imageDisplayY)
                : gObjectDisplayPosition(n));
        position.first += n.shakeX;
        position.second += n.shakeY;
        return position;
    }
    // FacadeNatural 文本的对齐偏移：表面是自然宽高（PIXI.Text 语义），FairyGUI 的固定盒
    // center/right/middle/bottom 对齐体现为"表面相对语义盒的显示偏移"。此前该偏移只在 JS
    // GTextField 首次排版时写入 slot，任何后续语义 writeNode（controller gear/transition/
    // resize/relations/visibility 组合发布）都会把它吹掉——按钮 hover 换页改盒宽后标题贴左
    // 就是这一族。offset 的全部输入都在节点上（盒宽高/表面 metrics/authored 对齐），所以由
    // writeNode 每次现算，几何来源与对齐职责收进同一层；JS 首摆写入的是同一公式的同一值，
    // 双方幂等。metrics 无效（宿主未回填）时偏移为 0，行为与旧版一致。
    // FairyGUI GTextField adds a two-pixel gutter on every side to the measured Pixi/CoreText
    // glyph bounds before it computes align/vAlign.  Keep that public layout contract here too;
    // otherwise a later native publication differs from GTextField.updatePosition by 2px and a
    // no-op display write is misclassified as a business move.
    static float naturalTextLayoutExtent(float measured) {
        return measured > 0.0f ? measured + 4.0f : 0.0f;
    }
    static float naturalTextLayoutScale(const InstanceNode& n, bool horizontal) {
        auto autoSize = n.attrs.find("autoSize");
        if (autoSize == n.attrs.end() || autoSize->second != "shrink") return 1.0f;
        // GTextField.layoutAlign multiplies its measured width/height by the live Pixi text
        // display scale only for AutoSizeType.Shrink.  On a native-managed facade that scale is
        // a raw display-layer override: the semantic GObject scale remains independent.  Using
        // the unscaled natural metrics here makes every later controller/visibility publication
        // restore a negative centre offset after JS has correctly fitted a translated title.
        const float scale = n.directDisplayScaleOverride
            ? (horizontal ? n.directDisplayScaleX : n.directDisplayScaleY)
            : (horizontal ? n.scaleX : n.scaleY);
        return std::isfinite(scale) ? scale : 1.0f;
    }
    static float naturalTextOffsetX(const InstanceNode& n) {
        auto it = n.attrs.find("align");
        if (it == n.attrs.end() || it->second == "left") return 0.0f;
        const float f = it->second == "center" ? 0.5f :
            ((it->second == "right" || it->second == "end") ? 1.0f : 0.0f);
        const float textWidth = naturalTextLayoutExtent(n.nativeTextWidth) *
                                naturalTextLayoutScale(n, true);
        return (n.width - textWidth) * f;
    }
    static float naturalTextOffsetY(const InstanceNode& n) {
        auto it = n.attrs.find("vAlign");
        // GTextField.layoutAlign keeps the top gutter even for an empty string.
        if (it == n.attrs.end() || it->second == "top") return 2.0f;
        const float f = it->second == "middle" ? 0.5f : (it->second == "bottom" ? 1.0f : 0.0f);
        const float textHeight = naturalTextLayoutExtent(n.nativeTextHeight) *
                                 naturalTextLayoutScale(n, false);
        const auto autoSize = n.attrs.find("autoSize");
        const bool shrink = autoSize != n.attrs.end() && autoSize->second == "shrink";
        const float gap = n.height - textHeight;
        return (shrink ? gap : std::max(0.0f, gap)) * f;
    }
    static std::pair<float, float> publishedDisplayPosition(const InstanceNode& n) {
        auto position = nodeDisplayPosition(n);
        if (n.nativeTextFacadeOwned && n.nativeTextFacadeNatural && n.nativeTextMetricsValid) {
            position.first += naturalTextOffsetX(n);
            position.second += naturalTextOffsetY(n);
        }
        position.first += n.directDisplayOffsetX;
        position.second += n.directDisplayOffsetY;
        return position;
    }
    static void writeNode(sg::SceneGraph& graph, InstanceNode& n) {
        FacadeSeqTracker::mark(graph, n.nativeId, FD_GEOMETRY | FD_LOOK);   // 几何/外观投影必经点(过度信号安全,S2 可加值比较)
        // 临时诊断(PiggyBank):writeNode=一切几何变更的必经投影点。filter 命中节点的
        // x/y/w/h/sx/sy 任一变化打一行流水,配 JS 事件时序推调用者。定位后移除。
        {
            const char* f = traceRelationsFilter();
            if (f && (std::strcmp(f, "all") == 0 || n.name.find(f) != std::string::npos)) {
                static std::unordered_map<int, std::array<float,6>> traceGeo;
                std::array<float,6> cur{n.x, n.y, n.width, n.height, n.scaleX, n.scaleY};
                auto it = traceGeo.find(n.nativeId);
                if (it == traceGeo.end() || it->second != cur) {
                    std::fprintf(stderr, "[fgui-geo] %s id=%d x=%.1f y=%.1f w=%.1f h=%.1f sx=%.3f sy=%.3f anchor=%d\n",
                                 n.name.c_str(), n.nativeId, n.x, n.y, n.width, n.height, n.scaleX, n.scaleY,
                                 n.pivotAsAnchor ? 1 : 0);
                    traceGeo[n.nativeId] = cur;
                }
            }
        }
        n.facadeSchemaPristine=false;
        float* s = graph.buf + (size_t)n.nativeId * sg::STRIDE;
        const auto displayPosition=publishedDisplayPosition(n);
        constexpr float kDegToRad = 0.01745329251994329577f;
        // A Pixi displayObject rotation is an independent display-layer transform, expressed in
        // radians.  Keep it out of gObjectDisplayPosition(): that helper reproduces FairyGUI's
        // semantic pivot compensation, which has already established the raw x/y.  Applying the
        // display override there as well would rotate the semantic pivot twice.
        const float rotation = n.directDisplayRotationOverride
            ? n.directDisplayRotation : n.rotation * kDegToRad;
        const float skewX = -n.skewX * kDegToRad, skewY = n.skewY * kDegToRad;
        const bool lazyListHit = n.authoredListItem && (n.kind == ObjectKind::Component || n.kind == ObjectKind::List);
        // A package-authored Button can stay facade-free until its first pointer event just like
        // a list item. Publish its component root as the static rectangular target so JavaScript
        // can materialise the semantic GButton at the input boundary. Transparent layout-only
        // components remain passive and cannot steal clicks from their descendants or siblings.
        const bool lazyButtonHit = n.kind == ObjectKind::Component && n.extension == "Button";
        const bool lazyFacadeHit = lazyListHit || lazyButtonHit;
        s[sg::X] = displayPosition.first; s[sg::Y] = displayPosition.second;
        s[sg::SX] = n.directDisplayScaleOverride ? n.directDisplayScaleX : n.scaleX;
        s[sg::SY] = n.directDisplayScaleOverride ? n.directDisplayScaleY : n.scaleY;
        s[sg::ROT] = rotation; s[sg::SKX] = skewX; s[sg::SKY] = skewY; s[sg::ALPHA] = n.alpha; s[sg::VISIBLE] = effectiveVisible(graph,n) ? 1 : 0; s[sg::RENDERABLE] = n.displayMask ? 0 : 1;
        // SceneGraph event modes are 1=none, 3=static. A native-only authored list item has no
        // facade to call R.writeHitMeta yet, so its component root must remain a valid candidate;
        // JS then materialises only the item actually hit. Non-container leaves retain the old
        // auto/none-compatible value and continue to act as geometric witnesses.
        // After publication the shared slot is the source of truth for Pixi's live eventMode.
        // Rewriting geometry is intentionally orthogonal to interactivity. Masks are the only
        // unconditional exception because they can never participate in event dispatch.
        if (n.displayMask || !n.runtimeEventModeOwned)
            s[sg::EVENTMODE] = n.displayMask ? 0 : (lazyFacadeHit && n.touchable ? 3 : (n.touchable ? 1 : 0));
        if (lazyFacadeHit) {
            int hitFlags = static_cast<int>(s[sg::HITFLAGS]) & ~1;
            if (n.touchable && n.width > 0 && n.height > 0) {
                hitFlags |= 1; float* hit = graph.buf + sg::BOUNDS_BASE + (size_t)n.nativeId * sg::BOUNDS_STRIDE_F + 16;
                hit[0] = 0; hit[1] = 0; hit[2] = n.width; hit[3] = n.height;
            }
            s[sg::HITFLAGS] = static_cast<float>(hitFlags);
        }
        if (n.displayMask) s[sg::HITFLAGS] = static_cast<float>(static_cast<int>(s[sg::HITFLAGS]) | 4);
        graph.markLocalInputChanged(n.nativeId);
    }
    static void writeRuntimeTouchable(sg::SceneGraph& graph, const InstanceNode& n) {
        if (!graph.buf || n.nativeId <= 0 || n.nativeId >= sg::NODE_CAP) return;
        // Pixi's legacy interactive=true/false maps to static/auto. Runtime GearLook changes
        // bypass the JS setter, so publish that exact mapping here instead of the construction
        // sentinel (`none`) used before a semantic facade exists.
        graph.buf[(size_t)n.nativeId * sg::STRIDE + sg::EVENTMODE] =
            n.displayMask ? 0.0f : (n.touchable ? 3.0f : 0.0f);
        graph.markLocalInputChanged(n.nativeId);
    }
    static InstanceNode* relationTarget(Instance& inst, const InstanceNode& owner, const RelationDesc& relation) {
        if (relation.targetInOwner) {
            if (relation.target.empty()) return nullptr;
            for (int childId : owner.children) {
                InstanceNode* child = nodeIn(inst, childId);
                if (child && (child->id == relation.target || child->name == relation.target)) {
                    // GComponent.constructInternal installs component-root relations before it
                    // runs each child's setupAfterAdd. A bitmap GTextField still owns its initial
                    // UITextField display object at that point, so RelationItem subscribes to
                    // SIZE_CHANGED on that object. The first renderWithBitmapFont then calls
                    // switchDisplayObject($btContainer); GObject.switchDisplayObject copies
                    // transforms/state but not event listeners. Every later autoSize event is
                    // emitted by the new bitmap container and the root relation is therefore
                    // observably detached in the shipped H5 runtime.
                    //
                    // Native relations normally (and correctly) follow semantic nodes rather
                    // than renderer objects. Preserve this one H5 lifecycle boundary explicitly:
                    // by the time a native bitmap node can publish a runtime size mutation, its
                    // initial bitmap render has already performed the display-object switch.
                    // Ordinary text/component targets retain the live component-root relation.
                    if (isBitmapText(*child)) return nullptr;
                    return child;
                }
            }
            return nullptr;
        }
        if (relation.target.empty()) return nodeIn(inst, owner.parentNativeId);
        InstanceNode* scope = owner.parentNativeId > 0 ? nodeIn(inst, owner.parentNativeId) : const_cast<InstanceNode*>(&owner);
        if (scope) for (int childId : scope->children) {
            InstanceNode* child = nodeIn(inst, childId);
            if (child && (child->id == relation.target || child->name == relation.target)) return child;
        }
        // Cross-package component descriptors occasionally keep the editor id of a sibling in
        // the owning component rather than the immediate native parent. Resolve that without
        // exposing a JavaScript child tree.
        for (const auto& candidate : inst.nodes) if (candidate &&
            (candidate->id == relation.target || candidate->name == relation.target) &&
            candidate->ownerComponentId == owner.ownerComponentId) return candidate.get();
        return nullptr;
    }
    static float relationOffset(float value, float oldExtent, float newExtent, bool percent) {
        return percent && std::fabs(oldExtent) > 0.0001f ? value / oldExtent * newExtent : value;
    }
    static float horizontalFactor(const std::string& side) {
        if (side == "center" || side == "middle") return 0.5f;
        if (side == "right") return 1.0f;
        return 0.0f;
    }
    static float verticalFactor(const std::string& side) {
        if (side == "middle" || side == "center") return 0.5f;
        if (side == "bottom") return 1.0f;
        return 0.0f;
    }
    static void applyOwnerSelfRelations(InstanceNode& owner, float dw, float dh) {
        if (dw == 0 && dh == 0) return;
        for (const RelationDesc& relation : owner.relations) {
            const std::string& side = relation.sidePair;
            if (side == "center-center" || side == "right-center")
                setSemanticPosition(owner, owner.x - dw * 0.5f, owner.y);
            else if (side == "right-left" || side == "right-right")
                setSemanticPosition(owner, owner.x - dw, owner.y);
            else if (side == "middle-middle" || side == "bottom-middle")
                setSemanticPosition(owner, owner.x, owner.y - dh * 0.5f);
            else if (side == "bottom-top" || side == "bottom-bottom")
                setSemanticPosition(owner, owner.x, owner.y - dh);
        }
    }
    static void applyRelationResizeSemantics(InstanceNode& owner, const InstanceNode& target,
                                             const RelationDesc& relation,
                                             float oldWidth, float oldHeight) {
        const float dw = owner.width - oldWidth, dh = owner.height - oldHeight;
        if (std::fabs(dw) < 0.0001f && std::fabs(dh) < 0.0001f) return;

        // RelationItem does not have one uniform resize path in the stock runtime. A width/height
        // relation to the owner parent calls setSize(..., ignorePivot=true), because the logical
        // box is following its container. Every sibling width/height relation and every Ext
        // relation writes owner.width/height instead; those property setters use setSize's normal
        // pivot compensation. Native used to treat all relation-driven resizes like the parent
        // branch, so a centred image widened by a sibling text field kept its left x and grew only
        // to the right. Preserve the distinction here rather than special-casing images or views.
        const bool parentSizeRelation = target.nativeId == owner.parentNativeId &&
            (relation.sidePair == "width-width" || relation.sidePair == "height-height");
        if (!parentSizeRelation && !owner.pivotAsAnchor)
            setSemanticPosition(owner, owner.x - owner.pivotX * dw,
                                owner.y - owner.pivotY * dh);
        else if (owner.pivotAsAnchor && (owner.pivotX != 0.0f || owner.pivotY != 0.0f))
            handleXYChanged(owner);

        // GObject.setSize invokes Relations.onOwnerSizeChanged after the pivot adjustment. Do
        // this for each individual relation definition: later definitions observe the position
        // produced by earlier width/height setters, exactly as RelationItem.$defs.forEach does.
        applyOwnerSelfRelations(owner, dw, dh);
    }
    // When construction-time relations move or resize a child, the authored controller/timeline
    // state must follow ($targetSizeChanged -> updateGearFromRelations for gearXY/gearSize and
    // Transition.updateFromRelations for the owning component's XY keyframes). Otherwise the
    // first controller flip or timeline replay snaps the child back to its unshifted value.
    static void adjustAuthoredRelationDeltas(InstanceNode& component, InstanceNode& owner,
                                             float dx, float dy, float dw, float dh) {
        if (std::fabs(dx) > 0.0001f || std::fabs(dy) > 0.0001f) {
            if (owner.xyGear.present) {
                owner.xyGear.defaultX += dx; owner.xyGear.defaultY += dy;
                for (auto& value : owner.xyGear.values) { value.first += dx; value.second += dy; }
            }
            if (!owner.id.empty()) for (auto& transitionDesc : component.transitions)
                for (auto& item : transitionDesc.items) if (item.kind == TransitionKind::XY && item.targetId == owner.id) {
                    if (item.tween) { adjustTransitionPair(item.startValue, dx, dy); adjustTransitionPair(item.endValue, dx, dy); }
                    else adjustTransitionPair(item.value, dx, dy);
                }
        }
        if ((std::fabs(dw) > 0.0001f || std::fabs(dh) > 0.0001f) && owner.sizeGear.present) {
            owner.sizeGear.defaultValue.width += dw; owner.sizeGear.defaultValue.height += dh;
            for (auto& value : owner.sizeGear.values) { value.width += dw; value.height += dh; }
        }
    }
    static void applyTargetSizeRelation(InstanceNode& owner, const InstanceNode& target,
                                        const RelationDesc& relation, float oldTargetWidth,
                                        float oldTargetHeight, float targetDx = 0,
                                        float targetDy = 0) {
        const std::string& pair = relation.sidePair;
        size_t dash = pair.find('-'); if (dash == std::string::npos) return;
        std::string from = pair.substr(0, dash), to = pair.substr(dash + 1);
        float targetX = target.nativeId == owner.parentNativeId ? 0.0f : target.x;
        float targetY = target.nativeId == owner.parentNativeId ? 0.0f : target.y;
        float oldTargetX = target.nativeId == owner.parentNativeId ? 0.0f : target.x - targetDx;
        float oldTargetY = target.nativeId == owner.parentNativeId ? 0.0f : target.y - targetDy;
        float newTargetWidth = target.width, newTargetHeight = target.height;
        auto followHorizontal = [&](float ownerFactor, float targetFactor) {
            float oldAnchor = oldTargetX + targetFactor * oldTargetWidth;
            float offset = owner.x + ownerFactor * owner.width - oldAnchor;
            offset = relationOffset(offset, oldTargetWidth, newTargetWidth, relation.usePercent);
            setSemanticPosition(owner,
                targetX + targetFactor * newTargetWidth + offset - ownerFactor * owner.width,
                owner.y);
        };
        auto followVertical = [&](float ownerFactor, float targetFactor) {
            float oldAnchor = oldTargetY + targetFactor * oldTargetHeight;
            float offset = owner.y + ownerFactor * owner.height - oldAnchor;
            offset = relationOffset(offset, oldTargetHeight, newTargetHeight, relation.usePercent);
            setSemanticPosition(owner, owner.x,
                targetY + targetFactor * newTargetHeight + offset - ownerFactor * owner.height);
        };
        if (from == "left" || from == "center" || from == "right") {
            followHorizontal(horizontalFactor(from), horizontalFactor(to)); return;
        }
        if (from == "top" || from == "middle" || from == "bottom") {
            followVertical(verticalFactor(from), verticalFactor(to)); return;
        }
        if (from == "width" && to == "width") {
            float offset = relationOffset(owner.width - oldTargetWidth, oldTargetWidth, newTargetWidth, relation.usePercent);
            owner.width = std::max(0.0f, newTargetWidth + offset); return;
        }
        if (from == "height" && to == "height") {
            float offset = relationOffset(owner.height - oldTargetHeight, oldTargetHeight, newTargetHeight, relation.usePercent);
            owner.height = std::max(0.0f, newTargetHeight + offset); return;
        }
        if (from == "leftext") {
            float oldLeft = owner.x, right = owner.x + owner.width;
            followHorizontal(0, horizontalFactor(to)); owner.width = std::max(0.0f, right - owner.x);
            (void)oldLeft; return;
        }
        if (from == "rightext") {
            float oldRight = owner.x + owner.width;
            float oldAnchor = oldTargetX + horizontalFactor(to) * oldTargetWidth;
            float offset = relationOffset(oldRight - oldAnchor, oldTargetWidth, newTargetWidth, relation.usePercent);
            owner.width = std::max(0.0f, targetX + horizontalFactor(to) * newTargetWidth + offset - owner.x); return;
        }
        if (from == "topext") {
            float bottom = owner.y + owner.height;
            followVertical(0, verticalFactor(to)); owner.height = std::max(0.0f, bottom - owner.y); return;
        }
        if (from == "bottomext") {
            float oldBottom = owner.y + owner.height;
            float oldAnchor = oldTargetY + verticalFactor(to) * oldTargetHeight;
            float offset = relationOffset(oldBottom - oldAnchor, oldTargetHeight, newTargetHeight, relation.usePercent);
            owner.height = std::max(0.0f, targetY + verticalFactor(to) * newTargetHeight + offset - owner.y);
        }
    }
    void syncInternalClipMask(Instance& inst, InstanceNode& target, sg::SceneGraph& graph) {
        InstanceNode* viewport = nodeIn(inst, target.clipMaskId);
        if (!viewport || !viewport->internalDisplayNode) return;
        float top = 0, bottom = 0, left = 0, right = 0;
        auto margin = split(target.attrs["margin"], ',');
        if (margin.size() == 1 && !margin[0].empty()) top = bottom = left = right = number(margin[0]);
        else if (margin.size() >= 4) {
            top = number(margin[0]); bottom = number(margin[1]);
            left = number(margin[2]); right = number(margin[3]);
        }
        const bool scrolling = target.attrs["overflow"] == "scroll";
        viewport->x = scrolling ? left : 0; viewport->y = scrolling ? top : 0;
        viewport->width = std::max(0.0f, target.width - (scrolling ? left : 0) - right);
        viewport->height = std::max(0.0f, target.height - (scrolling ? top : 0) - bottom);
        writeNode(graph, *viewport); writeGraph(graph, *viewport);
        linkPrototypeMask(graph, target, *viewport);
    }
    void applyRelations(Instance& inst, InstanceNode& target, float dx, float dy,
                        float dw, float dh,
                        sg::SceneGraph& graph, int depth = 0) {
        if ((std::fabs(dx) < 0.0001f && std::fabs(dy) < 0.0001f &&
             std::fabs(dw) < 0.0001f && std::fabs(dh) < 0.0001f) || depth > 32) return;
        float oldTargetWidth = target.width - dw, oldTargetHeight = target.height - dh;
        for (const auto& owned : inst.nodes) {
            if (!owned || owned.get() == &target) continue;
            InstanceNode& owner = *owned; float oldX = owner.x, oldY = owner.y, oldW = owner.width, oldH = owner.height;
            // GGroup.moveChildren uses child.setXY while the group is marked updating. Browser
            // RelationItem ignores those XY notifications for owners in that group; each member
            // receives the group delta exactly once from moveChildren below.
            if ((dx != 0.0f || dy != 0.0f) && dw == 0.0f && dh == 0.0f &&
                belongsToUpdatingGroup(inst, owner)) continue;
            bool matched = false;
            for (const RelationDesc& relation : owner.relations) if (relationTarget(inst, owner, relation) == &target) {
                const float relationOldW = owner.width, relationOldH = owner.height;
                applyTargetSizeRelation(owner, target, relation, oldTargetWidth, oldTargetHeight,
                                        dx, dy); matched = true;
                applyRelationResizeSemantics(owner, target, relation, relationOldW, relationOldH);
            }
            if (!matched) continue;
            float ownerDw = owner.width - oldW, ownerDh = owner.height - oldH;
            if (owner.x != oldX || owner.y != oldY || owner.width != oldW || owner.height != oldH) {
                const float ownerDx = owner.x - oldX, ownerDy = owner.y - oldY;
                // Width-only bitmap auto-size does not run GTextField.layoutAlign in the
                // browser ($updatingSize suppresses handleSizeChanged). Only a changed height
                // creates a new vertical-alignment lifecycle.
                if (ownerDh != 0.0f && isBitmapText(owner))
                    owner.bitmapVerticalAlignActivated = true;
                // Browser Relations writes through GObject.setXY, whose handleXYChanged replaces
                // any earlier direct displayObject.position override.
                if (ownerDx != 0.0f || ownerDy != 0.0f) {
                    owner.directDisplayOffsetX = 0; owner.directDisplayOffsetY = 0;
                }
                if (ownerDx != 0.0f || ownerDy != 0.0f) queueTransitionProperty(owner, TransitionPropertyKind::XY, owner.x, owner.y);
                if (ownerDw != 0.0f || ownerDh != 0.0f) queueTransitionProperty(owner, TransitionPropertyKind::Size, owner.width, owner.height);
                InstanceNode* component = nodeIn(inst, owner.ownerComponentId);
                if (component) adjustAuthoredRelationDeltas(*component, owner, ownerDx, ownerDy,
                                                            ownerDw, ownerDh);
                writeNode(graph, owner); refreshVisual(owner, graph);
                // A relation can resize a component without going through its public setSize path.
                // Keep the generated overflow:hidden/scroll viewport tied to the new semantic box;
                // otherwise descendants render through the authored-size clip until another direct
                // resize happens (for example XP stars leaking past the current progress fill).
                syncInternalClipMask(inst, owner, graph);
                applyRelations(inst, owner, ownerDx, ownerDy, ownerDw, ownerDh, graph, depth + 1);
                moveGroupMembers(inst, owner, ownerDx, ownerDy, graph);
            }
        }
    }
    // compensatePivot mirrors browser GObject.setSize's default (!ignorePivot) branch: a resize
    // keeps the authored pivot point stationary by shifting x/y with -pivot*Δsize. Callers map
    // 1:1 onto the browser call sites — the JS facade bridge passes false because the stock
    // GObject.setSize already performed that shift in JS and delivered it through the position
    // bridge; RelationItem resizes browser-side use ignorePivot=true so applyRelations stays
    // uncompensated; Transition size items and GearSize (when no same-controller GearXY drives
    // the position) use the compensating default in the browser and therefore pass true here.
    void resizeNode(Instance& inst, InstanceNode& target, float width, float height, sg::SceneGraph& graph, bool compensatePivot = false) {
        float oldW = target.width, oldH = target.height;
        target.width = std::max(0.0f, width); target.height = std::max(0.0f, height);
        if (target.width != oldW || target.height != oldH) FacadeSeqTracker::mark(graph, target.nativeId, FD_GEOMETRY | FD_DERIVED_SIZE);
        float dw = target.width - oldW, dh = target.height - oldH;
        // Horizontal natural-size publication must not activate vAlign. In FairyGUI bitmap
        // renderNow it happens under $updatingSize and therefore skips layoutAlign entirely.
        if (dh != 0.0f && isBitmapText(target))
            target.bitmapVerticalAlignActivated = true;
        float compDx = 0, compDy = 0;
        if (compensatePivot && !target.pivotAsAnchor && (target.pivotX != 0.0f || target.pivotY != 0.0f)) {
            compDx = -target.pivotX * dw; compDy = -target.pivotY * dh;
            setSemanticPosition(target, target.x + compDx, target.y + compDy);
            queueTransitionProperty(target, TransitionPropertyKind::XY, target.x, target.y);
        } else if (target.pivotAsAnchor && (target.pivotX != 0.0f || target.pivotY != 0.0f)) {
            // GObject.setSize calls applyPivot for an anchor pivot even when ignorePivot=true.
            // That is a real handleXYChanged publication and may move a mirrored GImage surface.
            handleXYChanged(target);
        }
        const char* traceFilter = std::getenv("NATIVE_FGUI_TRACE_RELATIONS");
        const bool traceRelations = traceFilter &&
            (std::strcmp(traceFilter, "all") == 0 || target.name.find(traceFilter) != std::string::npos ||
             (std::strcmp(traceFilter, "bars") == 0 &&
              (target.name == "TopBarView" || target.name == "BottomMachineView")));
        if (traceRelations) {
            std::fprintf(stderr, "[native-fgui-relation] resize %s %.1fx%.1f -> %.1fx%.1f nodes=%zu\n",
                         target.name.c_str(), oldW, oldH, target.width, target.height, inst.nodes.size());
            for (const auto& owned : inst.nodes) if (owned && owned->parentNativeId == target.nativeId) {
                std::fprintf(stderr, "[native-fgui-relation]   before %s id=%d x=%.1f w=%.1f relations=%zu\n",
                             owned->name.c_str(), owned->nativeId, owned->x, owned->width,
                             owned->relations.size());
                for (const RelationDesc& relation : owned->relations)
                    std::fprintf(stderr, "[native-fgui-relation]     %s -> %s percent=%d\n",
                                 relation.sidePair.c_str(), relation.target.c_str(),
                                 relation.usePercent ? 1 : 0);
            }
        }
        // Browser Relations::onOwnerSizeChanged runs for every successful GObject.setSize, not
        // only when a relation target resized the owner.  Its self-resize pass preserves the
        // anchored edge/centre (`center-center`, `right-*`, `middle-middle`, `bottom-*`) while the
        // owner's box changes. Native facades deliberately do not install a second JS relation
        // graph, so this public/native setSize path must perform that pass here. Omitting it makes
        // an auto-sized centred text field grow only to the right: the production Free Spin field
        // grows from 189 to 239 and therefore lands exactly 25 design pixels right of centre.
        //
        // Relation-driven resizes do not enter resizeNode: applyRelations mutates the dependent
        // owner directly and already calls applyOwnerSelfRelations once. Keeping the pass here
        // therefore mirrors the two browser call sites without double-applying either one.
        const float relationX = target.x, relationY = target.y;
        applyOwnerSelfRelations(target, dw, dh);
        compDx += target.x - relationX; compDy += target.y - relationY;
        // self-relation 在 pivot 补偿信号入队之后仍会改 x/y——不重新入队,journal 里留的是
        // 中间坐标(或 anchor 分支根本没入队),影子按错值追平。coalesce 按 node+kind 覆盖,
        // 这里入队的就是最终坐标。
        if (target.x != relationX || target.y != relationY)
            queueTransitionProperty(target, TransitionPropertyKind::XY, target.x, target.y);
        //
        // 固定盒（AuthoredBox）文本把对齐烘焙进表面，盒尺寸一变旧表面的烘焙就失效——浏览器
        // GTextField.handleSizeChanged 会无条件按新盒重新排版。attrs["text"] 始终持有业务
        // 最新文本（mutation/claim 两条通道都回存），重画不会回退内容。自然宽表面与盒无关，
        // 其对齐由 writeNode 的 naturalTextOffsetX/Y 现算，无需重画。
        if ((dw != 0.0f || dh != 0.0f) && target.nativeTextFacadeOwned && !target.nativeTextFacadeNatural &&
            (target.kind == ObjectKind::Text || target.kind == ObjectKind::RichText || target.kind == ObjectKind::InputText))
            prepareTextSurface(target, TextPrepareMode::AuthoredBox, graph);
        writeNode(graph, target); refreshVisual(target, graph);
        syncInternalClipMask(inst, target, graph);
        if (traceRelations) {
            InstanceNode* viewport = nodeIn(inst, target.clipMaskId);
            if (viewport)
                std::fprintf(stderr, "[native-fgui-relation]   clip %s id=%d size=%.1fx%.1f\n",
                             viewport->name.c_str(), viewport->nativeId,
                             viewport->width, viewport->height);
            else
                std::fprintf(stderr, "[native-fgui-relation]   clip none id=%d\n", target.clipMaskId);
        }
        applyRelations(inst, target, compDx, compDy, dw, dh, graph);
        moveGroupMembers(inst, target, compDx, compDy, graph);
        if (traceRelations) for (const auto& owned : inst.nodes)
            if (owned && owned->parentNativeId == target.nativeId)
                std::fprintf(stderr, "[native-fgui-relation]   after  %s id=%d x=%.1f w=%.1f\n",
                             owned->name.c_str(), owned->nativeId, owned->x, owned->width);
    }
    void moveGroupMembers(Instance& inst,InstanceNode& group,float dx,float dy,sg::SceneGraph& graph){
        if(group.kind!=ObjectKind::Group||(dx==0.0f&&dy==0.0f)||
           !updatingGroups_.insert(group.nativeId).second)return;
        std::vector<InstanceNode*> members;
        members.reserve(group.parentNativeId>0?8:0);
        for(const auto& owned:inst.nodes)
            if(owned&&belongsToGroup(*owned,group))members.push_back(owned.get());
        for(InstanceNode* member:members){
            moveNode(inst,*member,member->x+dx,member->y+dy,graph);
            // GGroup 移动是 native 自治写:成员 x/y 变了而 JS 影子无从得知——必须入 journal,
            // 否则后续同值 setXY 被影子判等吞掉(与 relations 撕裂同族)。moveNode 有意不入队
            // (语义移动的调用方决定),这里是调用方。
            queueTransitionProperty(*member, TransitionPropertyKind::XY, member->x, member->y);
            // GearBase.updateState is suppressed while an object is being constructed. An
            // indexed node is already out of that phase, so member setXY must update GearXY just
            // like the browser GGroup implementation does.
            if(managedNodeOwners_.find(member->nativeId)!=managedNodeOwners_.end())
                rememberCurrentXYGearValue(inst,*member);
        }
        updatingGroups_.erase(group.nativeId);
    }
    void moveNode(Instance& inst, InstanceNode& target, float x, float y, sg::SceneGraph& graph) {
        const float oldX = target.x, oldY = target.y;
        // GObject.setXY is edge-triggered: assigning the current value does not call
        // handleXYChanged and therefore must not disturb a retained GImage display position.
        setSemanticPosition(target, x, y);
        writeNode(graph, target);
        const float dx=target.x-oldX,dy=target.y-oldY;
        applyRelations(inst, target, dx, dy, 0, 0, graph);
        moveGroupMembers(inst, target, dx, dy, graph);
    }
    static void quadUv(sg::NativeQuad& q, const Sprite& sp, const AtlasBinding& atlas) {
        float x0 = sp.x / std::max(1.0f, atlas.width), y0 = sp.y / std::max(1.0f, atlas.height), x1 = (sp.x + sp.width) / std::max(1.0f, atlas.width), y1 = (sp.y + sp.height) / std::max(1.0f, atlas.height);
        if (!sp.rotated) { q[9]=x0;q[10]=y0;q[11]=x1;q[12]=y0;q[13]=x1;q[14]=y1;q[15]=x0;q[16]=y1; }
        // FairyGUI's editor rotates packed sprites by -90 degrees and its Pixi
        // runtime compensates with GroupD8.N (rotate = 6), not the usual
        // TexturePacker clockwise flag (rotate = 2).  Keep these corners in
        // exactly the same order as PIXI.TextureUvs#set(..., 6).
        else { q[9]=x0;q[10]=y1;q[11]=x0;q[12]=y0;q[13]=x1;q[14]=y0;q[15]=x1;q[16]=y1; }
    }
    static uint32_t visualTint(const InstanceNode& n) {
        auto it = n.attrs.find("color"); if (it == n.attrs.end() || it->second.empty()) return 0xFFFFFF;
        std::string value = it->second; if (!value.empty() && value[0] == '#') value.erase(value.begin());
        char* end = nullptr; unsigned long parsed = std::strtoul(value.c_str(), &end, 16);
        return end && *end == 0 ? static_cast<uint32_t>(parsed & 0xFFFFFF) : 0xFFFFFF;
    }
    static std::vector<uint32_t> utf8Codepoints(const std::string& text){std::vector<uint32_t> out;for(size_t i=0;i<text.size();){uint8_t c=(uint8_t)text[i++];uint32_t cp=c;if((c&0xE0)==0xC0&&i<text.size())cp=((c&31)<<6)|((uint8_t)text[i++]&63);else if((c&0xF0)==0xE0&&i+1<text.size())cp=((c&15)<<12)|(((uint8_t)text[i]&63)<<6)|((uint8_t)text[i+1]&63),i+=2;else if((c&0xF8)==0xF0&&i+2<text.size())cp=((c&7)<<18)|(((uint8_t)text[i]&63)<<12)|(((uint8_t)text[i+1]&63)<<6)|((uint8_t)text[i+2]&63),i+=3;out.push_back(cp);}return out;}
    // GTextField walks JavaScript strings with charAt/charCodeAt, i.e. UTF-16 code units.  The
    // exact native path is deliberately BMP-only: supplementary characters and lone surrogates
    // stay on FairyGUI's stock renderer instead of being silently combined into a different key.
    static bool decodeBmpUtf16(const std::string& text,std::vector<uint16_t>& out){
        out.clear();out.reserve(text.size());
        for(size_t i=0;i<text.size();){
            uint8_t c=(uint8_t)text[i++];uint32_t cp=0;
            if(c<0x80)cp=c;
            else if(c>=0xC2&&c<=0xDF){
                if(i>=text.size())return false;uint8_t b=(uint8_t)text[i++];if((b&0xC0)!=0x80)return false;
                cp=((uint32_t)(c&0x1F)<<6)|(b&0x3F);
            }else if(c>=0xE0&&c<=0xEF){
                if(i+1>=text.size())return false;uint8_t b1=(uint8_t)text[i++],b2=(uint8_t)text[i++];
                if((b1&0xC0)!=0x80||(b2&0xC0)!=0x80||(c==0xE0&&b1<0xA0)||(c==0xED&&b1>=0xA0))return false;
                cp=((uint32_t)(c&0x0F)<<12)|((uint32_t)(b1&0x3F)<<6)|(b2&0x3F);
            }else{
                // Valid four-byte UTF-8 is necessarily a surrogate pair in JavaScript, and any
                // other leading byte is malformed. Both cases require the stock path.
                return false;
            }
            if(cp==0||cp>0xFFFF||(cp>=0xD800&&cp<=0xDFFF))return false;
            out.push_back((uint16_t)cp);
        }
        return true;
    }
    bool writeBitmapText(sg::SceneGraph& graph,InstanceNode& n){
        n.bitmapTextWidth=0;n.bitmapTextHeight=0;n.bitmapGlyphCount=0;n.bitmapMetricsTrusted=false;FacadeSeqTracker::note(n.nativeId,FD_TEXT);
        auto fi=n.attrs.find("font");if(fi==n.attrs.end()||fi->second.compare(0,5,"ui://")!=0)return false;std::string body=fi->second.substr(5);Package* pkg=nullptr;std::string fontId;if(body.size()>8){pkg=packageForReference(body.substr(0,8),n.packageName);fontId=body.substr(8);}if(!pkg){pkg=package(n.packageName);fontId=body;}if(!pkg)return false;auto fontIt=pkg->fonts.find(fontId);if(fontIt==pkg->fonts.end())return false;const BitmapFont& font=fontIt->second;
        std::string text=n.attrs.count("text")?n.attrs.at("text"):std::string();
        float requested=number(n.attrs.count("fontSize")?n.attrs.at("fontSize"):std::string(),font.size);
        float scale=font.resizable&&font.size>0?requested/font.size:1.0f;
        // FairyGUI scales authored glyph metrics only. letterSpacing and leading are UI-space
        // values and must not be multiplied by the font scale.
        float spacing=number(n.attrs.count("letterSpacing")?n.attrs.at("letterSpacing"):std::string());
        float leading=number(n.attrs.count("leading")?n.attrs.at("leading"):std::string(),0);
        std::string autoSize=n.attrs.count("autoSize")?n.attrs.at("autoSize"):std::string();if(autoSize.empty())autoSize="both";
        // shrink ≠ widthAuto:fgui 的 shrink 语义是"保持 authored 框、文本超框时缩放适配"
        // (fgui.umd TTF updateSize :12645 的 shrink 分支不改 w + :12670 尾部 layoutAlign→框内
        // 居中)。fgui.umd 的 BMFont 分支缺这条特判(把 shrink 并进 widthAuto 收缩了对齐框),
        // 此处曾复刻了那个错的分支——coinBooster reward_font(360 框 center+shrink)因此贴左。
        // 以 shrink 设计本意+TTF 分支语义+实测观察为准:对齐框=authored 宽。
        bool shrinkMode=autoSize=="shrink";
        bool widthAuto=autoSize=="both";
        bool heightAuto=autoSize=="both"||autoSize=="height";
        bool singleLine=n.attrs.count("singleLine")&&n.attrs.at("singleLine")=="true";
        std::vector<uint16_t> units;
        // FairyGUI image fonts are allowed to contain only `char ... img=...` records. In that
        // legacy form there is no info.size/common.lineHeight, so font.size legitimately remains
        // zero while every glyph still has authoritative dimensions from its image resource.
        // Atlas-backed (`face=...`) BMFonts instead use x/y/width/height within the font item's
        // sprite. H5 lays non-resizable fonts out at scale 1; only a resizable font needs a
        // non-zero source size for requested/font.size scaling.
        bool nativeLayout=(!font.resizable||font.size>0)&&
            std::isfinite(scale)&&scale>0&&decodeBmpUtf16(text,units);
        // Supplementary Unicode still uses FairyGUI's semantic glyph renderer. Clear the authored
        // native quads first: keeping conservative paint here made both trees visible at once and
        // produced duplicated glyphs. BMP image-font and atlas-font text below is fully native.
        if(!nativeLayout){
            graph.clearNativeQuads(n.nativeId);graph.buf[(size_t)n.nativeId*sg::STRIDE+sg::TYPE]=sg::T_CONTAINER;
            return true;
        }

        struct ResolvedGlyph{
            const BitmapGlyph* glyph=nullptr;
            Sprite sprite;
            const AtlasBinding* atlas=nullptr;
            float width=0,height=0;
        };
        auto resolve=[&](uint16_t code,ResolvedGlyph& out)->bool{
            auto glyphIt=font.glyphs.find(code);if(glyphIt==font.glyphs.end())return true;
            const BitmapGlyph& glyph=glyphIt->second;
            const Sprite* sprite=nullptr;
            if(font.ttf){
                auto fontSprite=pkg->sprites.find(fontId);if(fontSprite==pkg->sprites.end())return false;
                // UIPackage.loadFont slices a plain rectangle from the packed font sprite. The
                // exporter therefore keeps font sprites unrotated; reject a malformed rotated
                // record instead of sampling a silently transposed glyph grid.
                if(fontSprite->second.rotated)return false;
                if(!std::isfinite(glyph.x)||!std::isfinite(glyph.y)||
                   !std::isfinite(glyph.width)||!std::isfinite(glyph.height)||
                   glyph.x<0||glyph.y<0||glyph.width<=0||glyph.height<=0)return false;
                out.sprite=fontSprite->second;
                out.sprite.x+=glyph.x;out.sprite.y+=glyph.y;
                out.sprite.width=glyph.width;out.sprite.height=glyph.height;
                out.sprite.rotated=false;out.width=glyph.width;out.height=glyph.height;
                sprite=&out.sprite;
            }else{
                auto imageIt=pkg->resourcesById.find(glyph.imageId);if(imageIt==pkg->resourcesById.end())return false;
                auto spriteIt=pkg->sprites.find(imageIt->second.id);if(spriteIt==pkg->sprites.end())return false;
                out.sprite=spriteIt->second;out.width=imageIt->second.width;out.height=imageIt->second.height;
                sprite=&out.sprite;
            }
            auto atlasIt=pkg->atlases.find(sprite->atlasId);
            if(atlasIt==pkg->atlases.end()||atlasIt->second.textureId<=0||atlasIt->second.width<=0||atlasIt->second.height<=0)return false;
            // Match H5's baseTexture slice: a few shipped font PackageItems round their logical
            // size down by one pixel even though the final glyph rectangle is valid in the atlas.
            // Validate against the physical atlas, not the font sprite's advisory width/height.
            if(font.ttf&&(sprite->x<0||sprite->y<0||
               sprite->x+sprite->width>atlasIt->second.width+.01f||
               sprite->y+sprite->height>atlasIt->second.height+.01f))return false;
            if(!std::isfinite(glyph.advance)||!std::isfinite(glyph.lineHeight)||glyph.lineHeight<=0||
               !std::isfinite(out.width)||!std::isfinite(out.height)||out.width<=0||out.height<=0)return false;
            out.glyph=&glyph;out.atlas=&atlasIt->second;return true;
        };
        // Validate every drawable glyph up front. A missing character is valid FairyGUI input: it
        // advances by letterSpacing only. A broken packaged glyph is not, and stays on the stock
        // path after removing the old native quad list.
        for(uint16_t code:units){ResolvedGlyph glyph;if(code!='\n'&&!resolve(code,glyph))nativeLayout=false;}
        if(!nativeLayout){
            graph.clearNativeQuads(n.nativeId);graph.buf[(size_t)n.nativeId*sg::STRIDE+sg::TYPE]=sg::T_CONTAINER;
            return true;
        }

        struct BitmapLine{float width=0,height=0,textHeight=0,y=0;std::vector<uint16_t> text;};
        std::vector<BitmapLine> lines;std::vector<uint16_t> lineBuffer;
        float lineSpacing=leading-1.0f,rectWidth=n.width-4.0f,lineWidth=0,lineHeight=0,lineTextHeight=0;
        float wordStart=0,wordEnd=0,lastLineHeight=0,lineY=2.0f;
        int wordChars=0;bool wordWrap=!widthAuto&&!singleLine&&!shrinkMode;   // shrink 不断行(缩放适配)
        auto trimRight=[](std::vector<uint16_t> value){while(!value.empty()&&value.back()<=32)value.pop_back();return value;};
        auto pushLine=[&](BitmapLine&& line){line.y=lineY;lineY+=line.height+lineSpacing;n.bitmapTextWidth=std::max(n.bitmapTextWidth,line.width);lines.push_back(std::move(line));};
        for(uint16_t code:units){
            if(code=='\n'){
                lineBuffer.push_back(code);BitmapLine line;line.width=lineWidth;
                if(lineTextHeight==0){if(lastLineHeight==0)lastLineHeight=std::ceil(requested*scale);if(lineHeight==0)lineHeight=lastLineHeight;lineTextHeight=lineHeight;}
                line.height=lineHeight;lastLineHeight=lineHeight;line.textHeight=lineTextHeight;line.text=std::move(lineBuffer);pushLine(std::move(line));
                lineBuffer.clear();lineWidth=lineHeight=lineTextHeight=0;wordChars=0;wordStart=wordEnd=0;continue;
            }
            if(code>256||code<=32){if(wordChars>0)wordEnd=lineWidth;wordChars=0;}
            else{if(wordChars==0)wordStart=lineWidth;++wordChars;}
            ResolvedGlyph resolved;resolve(code,resolved);float glyphWidth=0,glyphHeight=0;
            if(code==' '){glyphWidth=std::ceil(requested*.5f);glyphHeight=std::ceil(requested);}
            else if(resolved.glyph){glyphWidth=std::ceil(resolved.glyph->advance*scale);glyphHeight=std::ceil(resolved.glyph->lineHeight*scale);}
            lineTextHeight=std::max(lineTextHeight,glyphHeight);lineHeight=std::max(lineHeight,glyphHeight);
            if(lineWidth!=0)lineWidth+=spacing;lineWidth+=glyphWidth;
            if(!wordWrap||lineWidth<=rectWidth)lineBuffer.push_back(code);
            else{
                BitmapLine line;line.height=lineHeight;line.textHeight=lineTextHeight;
                if(lineBuffer.empty())line.text.push_back(code);
                else if(wordChars>0&&wordEnd>0){
                    lineBuffer.push_back(code);size_t len=lineBuffer.size()-(size_t)wordChars;
                    line.text=trimRight(std::vector<uint16_t>(lineBuffer.begin(),lineBuffer.begin()+len));line.width=wordEnd;
                    lineBuffer=std::vector<uint16_t>(lineBuffer.begin()+std::min(lineBuffer.size(),len+1),lineBuffer.end());lineWidth-=wordStart;
                }else{
                    line.text=lineBuffer;line.width=lineWidth-(glyphWidth+spacing);lineBuffer.assign(1,code);
                    lineWidth=glyphWidth;lineHeight=glyphHeight;lineTextHeight=glyphHeight;
                }
                pushLine(std::move(line));wordChars=0;wordStart=wordEnd=0;
            }
        }
        if(!lineBuffer.empty()||(!lines.empty()&&!lines.back().text.empty()&&lines.back().text.back()=='\n')){
            BitmapLine line;line.width=lineWidth;if(lineHeight==0)lineHeight=lastLineHeight;if(lineTextHeight==0)lineTextHeight=lineHeight;
            line.height=lineHeight;line.textHeight=lineTextHeight;line.text=std::move(lineBuffer);line.y=lineY;
            n.bitmapTextWidth=std::max(n.bitmapTextWidth,line.width);lines.push_back(std::move(line));
        }
        if(n.bitmapTextWidth>0)n.bitmapTextWidth+=4.0f;
        n.bitmapTextHeight=lines.empty()?0.0f:lines.back().y+lines.back().height+2.0f;
        float layoutWidth=widthAuto?n.bitmapTextWidth:n.width,layoutHeight=heightAuto?n.bitmapTextHeight:n.height;
        // Bitmap shrink uses the same natural-surface contract as ordinary text: measure first,
        // preserve the authored box, then uniformly scale the complete surface (gutters,
        // advances, offsets and glyph images). Scaling only the image rectangles leaves their
        // advances unscaled and is the generic source of jackpot values bleeding into the next
        // field. Keep the published text metrics natural; callers use them just like Pixi's
        // GTextField.textWidth while width/height remain the authored shrink box.
        const float fitScale=shrinkMode&&n.bitmapTextWidth>n.width&&n.bitmapTextWidth>0
            ?std::max(0.0f,n.width/n.bitmapTextWidth):1.0f;
        std::string align=n.attrs.count("align")?n.attrs.at("align"):std::string();
        // Match the shipped GTextField lifecycle for ordinary bitmap labels. Its initial
        // renderWithBitmapFont call does not run layoutAlign, so those glyphs start at line.y; a
        // later real resize activates vAlign. Shrink is different: native completes the missing
        // natural-surface fit itself, so it must also complete that fit's layoutAlign immediately.
        // Otherwise every centred jackpot amount is scaled correctly but remains pinned to the
        // top gutter. Keeping the exception shrink-only preserves Win Rush's non-shrink baseline.
        std::string verticalAlign=n.attrs.count("vAlign")?n.attrs.at("vAlign"):std::string();
        const float renderedTextHeight=n.bitmapTextHeight*fitScale;
        const float verticalGap=std::max(0.0f,layoutHeight-renderedTextHeight);
        const float alignedVerticalOffset=shrinkMode?
            (verticalAlign=="middle"?verticalGap*.5f:(verticalAlign=="bottom"?verticalGap:0.0f)):
            (n.bitmapVerticalAlignActivated?
                (verticalAlign=="middle"?verticalGap*.5f:(verticalAlign=="bottom"?verticalGap:2.0f)):
                0.0f);
        // Shrink is a native completion of a missing bitmap-font fit and owns its complete visual
        // alignment. Ordinary bitmap fields instead use the facade's actual display-only offset;
        // applying it to the node transform would corrupt relations/controllers and applying both
        // offsets would double middle/bottom alignment after a real resize.
        // Once a real height/controller/relation resize has occurred, C++ owns the current box
        // and its fresh aligned offset. A facade value captured before that resize is stale and
        // must not pull later text mutations back to the construction-time baseline. Before any
        // such resize, preserve the display-local offset captured from the shipped JS lifecycle.
        const float verticalOffset=(shrinkMode||n.bitmapVerticalAlignActivated)?alignedVerticalOffset:
            (std::isfinite(n.bitmapLocalOffsetY)?n.bitmapLocalOffsetY:2.0f);
        uint32_t tint=font.colorable?visualTint(n):0xFFFFFFu;
        std::vector<sg::NativeQuad> quads;if(layoutWidth>0&&layoutHeight>0){
            const float naturalLayoutWidth=shrinkMode?n.bitmapTextWidth:layoutWidth;
            const float innerWidth=naturalLayoutWidth-4.0f;
            const float renderedTextWidth=n.bitmapTextWidth*fitScale;
            const float horizontalGap=std::max(0.0f,layoutWidth-renderedTextWidth);
            const float horizontalOffset=shrinkMode?
                (align=="center"?horizontalGap*.5f:(align=="right"?horizontalGap:0.0f)):
                0.0f;
            for(const BitmapLine& line:lines){
                float charX=2.0f,lineIndent=align=="center"?(innerWidth-line.width)*.5f:(align=="right"?innerWidth-line.width:0.0f);
                for(uint16_t code:line.text){
                    ResolvedGlyph resolved;resolve(code,resolved);
                    if(resolved.glyph){
                        float charIndent=(line.height+line.textHeight)*.5f-std::ceil(resolved.glyph->lineHeight*scale);
                        float x0=horizontalOffset+(charX+lineIndent+std::ceil(resolved.glyph->offsetX*scale))*fitScale;
                        float y0=verticalOffset+(line.y+charIndent+std::ceil(resolved.glyph->offsetY*scale))*fitScale;
                        float x1=x0+resolved.width*scale*fitScale,y1=y0+resolved.height*scale*fitScale;sg::NativeQuad q{};
                        q[0]=resolved.atlas->textureId;q[1]=x0;q[2]=y0;q[3]=x1;q[4]=y0;q[5]=x1;q[6]=y1;q[7]=x0;q[8]=y1;
                        quadUv(q,resolved.sprite,*resolved.atlas);q[17]=(float)tint;q[18]=1;q[19]=-1;q[20]=-1;q[22]=resolved.atlas->premul;quads.push_back(q);
                        charX+=spacing+std::ceil(resolved.glyph->advance*scale);
                    }else if(code==' ')charX+=spacing+std::ceil(font.size*scale*.5f);
                    else charX+=spacing;
                }
            }
        }
        n.bitmapGlyphCount=(uint32_t)quads.size();n.bitmapMetricsTrusted=true;

        graph.setNativeQuads(n.nativeId,std::move(quads));
        return true;
    }
    bool writeMovieClip(sg::SceneGraph& graph,Package& pkg,const Resource& movie,InstanceNode& n){
        if(movie.movieFrames.empty())return false;
        int requested=integer(n.attrs.count("frame")?n.attrs.at("frame"):std::string(),n.movieFrame);
        requested=std::max(0,std::min((int)movie.movieFrames.size()-1,requested));
        if(requested!=n.movieFrame){n.movieFrame=requested;n.movieElapsedMs=0;}
        const Resource::MovieFrame& frame=movie.movieFrames[(size_t)n.movieFrame];
        float* s=graph.buf+(size_t)n.nativeId*sg::STRIDE;
        if(frame.width<=0||frame.height<=0){graph.clearNativeQuads(n.nativeId);s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;s[sg::RW]=s[sg::RH]=0;graph.markBoundsDirty(n.nativeId);return true;}
        auto si=pkg.sprites.find(frame.spriteId);if(si==pkg.sprites.end())return false;
        auto ai=pkg.atlases.find(si->second.atlasId);if(ai==pkg.atlases.end()||ai->second.textureId<=0)return false;
        const AtlasBinding& atlas=ai->second;sg::NativeQuad q{};quadUv(q,si->second,atlas);
        graph.clearNativeQuads(n.nativeId);s[sg::TYPE]=sg::T_SPRITE;s[sg::TEXID]=atlas.textureId;s[sg::RX]=frame.x;s[sg::RY]=frame.y;s[sg::RW]=frame.width;s[sg::RH]=frame.height;
        for(int k=0;k<8;k++)s[sg::U0+k]=q[9+k];s[sg::TINT]=(float)visualTint(n);s[sg::PREMUL]=atlas.premul;graph.markBoundsDirty(n.nativeId);return true;
    }
    void advanceMovieClips(double nowMs,sg::SceneGraph& graph){FacadeCommitScope __facadeCommit(*this,graph);
        for(auto& instanceItem:instances_){
            Instance* inst=instanceItem.second.get();if(!inst)continue;
            for(auto& owned:inst->nodes){
                if(!owned||owned->kind!=ObjectKind::MovieClip)continue;InstanceNode& n=*owned;
                Package* pkg=package(n.packageName);if(!pkg)continue;auto resource=pkg->resourcesById.find(n.sourceId);if(resource==pkg->resourcesById.end())continue;
                Resource& movie=resource->second;if(movie.movieFrames.empty())continue;
                if(n.movieLastMs<=0){n.movieLastMs=nowMs;continue;}
                double elapsed=std::max(0.0,std::min(250.0,nowMs-n.movieLastMs));n.movieLastMs=nowMs;
                auto playing=n.attrs.find("playing");if(playing!=n.attrs.end()&&(playing->second=="false"||playing->second=="0"))continue;
                n.movieFrame=std::max(0,std::min((int)movie.movieFrames.size()-1,n.movieFrame));
                n.movieElapsedMs+=elapsed;const Resource::MovieFrame& frame=movie.movieFrames[(size_t)n.movieFrame];
                double interval=std::max(1,movie.interval+frame.addDelay+((n.movieFrame==0&&n.movieRepeated>0)?movie.repeatDelay:0));
                if(n.movieElapsedMs<interval)continue;n.movieElapsedMs-=interval;if(n.movieElapsedMs>movie.interval)n.movieElapsedMs=movie.interval;
                if(movie.swing){
                    if(n.movieReversed){if(--n.movieFrame<0){n.movieFrame=std::min(1,(int)movie.movieFrames.size()-1);n.movieRepeated++;n.movieReversed=false;}}
                    else if(++n.movieFrame>(int)movie.movieFrames.size()-1){n.movieFrame=std::max(0,(int)movie.movieFrames.size()-2);n.movieRepeated++;n.movieReversed=true;}
                }else if(++n.movieFrame>(int)movie.movieFrames.size()-1){n.movieFrame=0;n.movieRepeated++;}
                n.touchFacade(FD_ATTRS);n.attrs["frame"]=std::to_string(n.movieFrame);writeMovieClip(graph,*pkg,movie,n);
            }
        }
    }
    // ox/oy:内容在节点局部空间的偏移(Loader 对齐用);sprite 路径写 RX/RY,multi-quad 路径直接平移顶点。
    static bool writeImage(sg::SceneGraph& graph, Package& pkg, const Resource& res, const InstanceNode& n, float ox = 0, float oy = 0) {
        auto si = pkg.sprites.find(res.id); if (si == pkg.sprites.end()) return false; auto ai = pkg.atlases.find(si->second.atlasId); if (ai == pkg.atlases.end() || ai->second.textureId <= 0) return false;
        const Sprite& sp = si->second; const AtlasBinding& atlas = ai->second;
        // readTransform() already seeds an authored image from its resource dimensions.  A later
        // zero width/height therefore means GImage.setSize(0, ...) intentionally collapsed the
        // surface (ProgressBar uses exactly this for 0%).  Falling back to the resource size here
        // turned an empty progress bar into a full one until its first non-zero value arrived.
        // Match Pixi/FGUI zero-size semantics for ordinary, tiled and nine-sliced images alike.
        if (n.width <= 0 || n.height <= 0) {
            graph.clearNativeQuads(n.nativeId);
            float* s = graph.buf + (size_t)n.nativeId * sg::STRIDE;
            s[sg::TYPE] = sg::T_CONTAINER; s[sg::TEXID] = 0; s[sg::QUADCNT] = 0;
            s[sg::RW] = s[sg::RH] = 0; graph.markBoundsDirty(n.nativeId); return true;
        }
        float w = n.width, h = n.height;
        // FairyGUI applies GImage.flip in the image's logical (unpacked) coordinate space.  It
        // cannot be represented by changing the atlas rectangle directly because rotated atlas
        // sprites exchange the physical U/V axes.  Ignoring the authored `flip` happened to look
        // harmless for opaque art, but reverses alpha ramps such as tournament's full-height edge
        // shadows and leaves a hard seam at the inner edge.  Interpolate from quadUv() first, then
        // mirror logical coordinates so ordinary, rotated and nine-sliced sprites share the same
        // browser semantics.
        std::string flip;
        auto flipIt = n.attrs.find("flip"); if (flipIt != n.attrs.end()) flip = flipIt->second;
        const bool flipH = flip == "hz" || flip == "horizontal" || flip == "both" || flip == "1" || flip == "3";
        const bool flipV = flip == "vt" || flip == "vertical" || flip == "both" || flip == "2" || flip == "3";
        sg::NativeQuad base{}; quadUv(base,sp,atlas);
        auto sourceUvAt=[&](float x,float y,float& u,float& v){float ax=1-x,ay=1-y;u=base[9]*ax*ay+base[11]*x*ay+base[13]*x*y+base[15]*ax*y;v=base[10]*ax*ay+base[12]*x*ay+base[14]*x*y+base[16]*ax*y;};
        auto uvAt=[&](float x,float y,float& u,float& v){sourceUvAt(flipH?1-x:x,flipV?1-y:y,u,v);};
        if (res.scaleByTile) {
            // GL_CLAMP_TO_EDGE applies to the entire atlas, not this sprite's frame. Match
            // Pixi TilingSprite's uClampFrame in the fragment shader: linear sampling at a tile
            // boundary must not mix transparent gutters/adjacent sprites into the floor.
            // Keep geometry and UV interpolation unchanged (including partial tiles and flips).
            const float aw=std::max(1.0f,atlas.width),ah=std::max(1.0f,atlas.height);
            const float minU=(sp.x+0.5f)/aw,minV=(sp.y+0.5f)/ah;
            const float maxU=(sp.x+std::max(1.0f,sp.width)-0.5f)/aw;
            const float maxV=(sp.y+std::max(1.0f,sp.height)-0.5f)/ah;
            // 上限从 4096 提到 16384(约 2MB/节点),超限打日志截断——静默丢块曾把大面积平铺背景裁缺角。
            const size_t kTileCap = 16384;
            float tw=std::max(1.0f,res.width),th=std::max(1.0f,res.height);std::vector<sg::NativeQuad> qs;int nx=std::max(1,(int)std::ceil(w/tw)),ny=std::max(1,(int)std::ceil(h/th));qs.reserve(std::min(kTileCap,(size_t)nx*(size_t)ny));
            uint32_t tint=visualTint(n);for(int ry=0;ry<ny&&qs.size()<kTileCap;ry++)for(int cx=0;cx<nx&&qs.size()<kTileCap;cx++){float x0=ox+cx*tw,y0=oy+ry*th,x1=ox+std::min(w,cx*tw+tw),y1=oy+std::min(h,ry*th+th),ux=(x1-x0)/tw,vy=(y1-y0)/th;sg::NativeQuad q{};q[0]=atlas.textureId;q[1]=x0;q[2]=y0;q[3]=x1;q[4]=y0;q[5]=x1;q[6]=y1;q[7]=x0;q[8]=y1;uvAt(0,0,q[9],q[10]);uvAt(ux,0,q[11],q[12]);uvAt(ux,vy,q[13],q[14]);uvAt(0,vy,q[15],q[16]);q[17]=(float)tint;q[18]=1;q[19]=-1;q[20]=-1;q[22]=atlas.premul;qs.push_back(q);}
            if((size_t)nx*(size_t)ny>kTileCap){static int tileCapLogN=0;if(tileCapLogN<8){++tileCapLogN;std::fprintf(stderr,"[native-fgui] tile 截断 %s/%s node=%d %dx%d 块(上限 %zu),内容 %.0fx%.0f tile %.0fx%.0f\n",pkg.name.c_str(),res.id.c_str(),n.nativeId,nx,ny,kTileCap,w,h,tw,th);}}
            for(auto& q:qs){q[23]=sg::Q_SHADER_ATLAS_CLAMP;q[24]=minU;q[25]=minV;q[26]=maxU;q[27]=maxV;}
            graph.setNativeQuads(n.nativeId,std::move(qs));return true;
        }
        if (!res.hasScale9) { graph.clearNativeQuads(n.nativeId);   // iconGear 从 9grid/tile 切回普通图时清掉残留 quads
            float* s = graph.buf + (size_t)n.nativeId * sg::STRIDE; s[sg::TYPE]=sg::T_SPRITE;s[sg::TEXID]=atlas.textureId;s[sg::RX]=ox;s[sg::RY]=oy;s[sg::RW]=w;s[sg::RH]=h;sg::NativeQuad q{};uvAt(0,0,q[9],q[10]);uvAt(1,0,q[11],q[12]);uvAt(1,1,q[13],q[14]);uvAt(0,1,q[15],q[16]);for(int k=0;k<8;k++)s[sg::U0+k]=q[9+k];s[sg::TINT]=(float)visualTint(n);s[sg::PREMUL]=atlas.premul;graph.markBoundsDirty(n.nativeId);return true; }
        // 目标尺寸小于边框之和时按比例收缩边框(同 cbNativeNineSlice 的 msc);原先只 clamp,
        // 左边框不缩、右边框压扁,w<gridX 时甚至产出负宽度翻转 quad(ProgressBar 低进度即触发)。
        const float sourceL=res.gridX,sourceR=res.width-res.gridX-res.gridW,sourceT=res.gridY,sourceB=res.height-res.gridY-res.gridH;
        float lw=flipH?sourceR:sourceL, rw=flipH?sourceL:sourceR, th9=flipV?sourceB:sourceT, bh9=flipV?sourceT:sourceB;
        float sw=lw+rw, sh=th9+bh9;
        float msc=std::min((w>sw||sw<=0)?1.0f:w/sw,(h>sh||sh<=0)?1.0f:h/sh);
        float xs[4]={0,lw*msc,w-rw*msc,w}, ys[4]={0,th9*msc,h-bh9*msc,h}; if(xs[2]<xs[1])xs[2]=xs[1];if(ys[2]<ys[1])ys[2]=ys[1];
        float us[4]={0,lw/std::max(1.0f,res.width),(res.width-rw)/std::max(1.0f,res.width),1}, vs[4]={0,th9/std::max(1.0f,res.height),(res.height-bh9)/std::max(1.0f,res.height),1};
        std::vector<sg::NativeQuad> qs; qs.reserve(9);
        uint32_t tint=visualTint(n);for(int r=0;r<3;r++)for(int c=0;c<3;c++){sg::NativeQuad q{};q[0]=atlas.textureId;q[1]=ox+xs[c];q[2]=oy+ys[r];q[3]=ox+xs[c+1];q[4]=oy+ys[r];q[5]=ox+xs[c+1];q[6]=oy+ys[r+1];q[7]=ox+xs[c];q[8]=oy+ys[r+1];uvAt(us[c],vs[r],q[9],q[10]);uvAt(us[c+1],vs[r],q[11],q[12]);uvAt(us[c+1],vs[r+1],q[13],q[14]);uvAt(us[c],vs[r+1],q[15],q[16]);q[17]=(float)tint;q[18]=1;q[19]=-1;q[20]=-1;q[22]=atlas.premul;qs.push_back(q);}graph.setNativeQuads(n.nativeId,std::move(qs));return true;
    }
    bool resolveImageUrl(const std::string& url, const std::string& ownerPackage,
                         Package*& pkg, Resource*& res) const {
        pkg = nullptr; res = nullptr; if (url.compare(0, 5, "ui://") != 0) return false;
        std::string body = url.substr(5), item;
        size_t slash = body.find('/');
        if (slash != std::string::npos) { pkg = packageForReference(body.substr(0, slash), ownerPackage); if (pkg) res = resourceByName(*pkg, body.substr(slash + 1)); }
        else if (body.size() > 8) { pkg = packageForReference(body.substr(0, 8), ownerPackage); item = body.substr(8); if (pkg) { auto it = pkg->resourcesById.find(item); if (it != pkg->resourcesById.end()) res = &it->second; } }
        return pkg && res && res->kind == ResourceKind::Image;
    }
    bool resolvedLoaderAutoSize(const InstanceNode& loader, float& width, float& height) const {
        auto autoSize = loader.attrs.find("autoSize");
        auto url = loader.attrs.find("url");
        if (loader.kind != ObjectKind::Loader || autoSize == loader.attrs.end() ||
            autoSize->second != "true" || url == loader.attrs.end() || url->second.empty())
            return false;
        Package* pkg = nullptr;
        Resource* resource = nullptr;
        if (!resolveImageUrl(url->second, loader.packageName, pkg, resource)) return false;
        width = resource->width;
        height = resource->height;
        return true;
    }
    bool writeLoader(sg::SceneGraph& graph, const InstanceNode& loader) {
        // 空 URL / 解析失败必须清视觉再返回:FGUI 里"该页不显示图标"就是把 loader url 置空,
        // 只 return false 会让旧图标残留(refreshVisual 对 false 不做任何清理)。
        auto clearVisual = [&]() {
            if (loader.nativeId <= 0) return;
            graph.clearNativeQuads(loader.nativeId);
            float* s = graph.buf + (size_t)loader.nativeId * sg::STRIDE;
            s[sg::TYPE] = sg::T_CONTAINER; s[sg::TEXID] = 0; s[sg::QUADCNT] = 0;
            graph.markBoundsDirty(loader.nativeId);
        };
        auto ui = loader.attrs.find("url"); if (ui == loader.attrs.end() || ui->second.empty()) { clearVisual(); return false; }
        // 9grid/tile 资源不再拒绝:writeImage 原生支持两者,偏移经 ox/oy 传入。
        // 原先返回 false 后调用方不逃逸,iconGear 切到 9grid URL 时静默停在旧图。
        Package* pkg = nullptr; Resource* res = nullptr; if (!resolveImageUrl(ui->second, loader.packageName, pkg, res)) { clearVisual(); return false; }
        InstanceNode draw = loader; float cw = res->width, ch = res->height, sx = 1, sy = 1;
        const std::string fill = loader.attrs.count("fill") ? loader.attrs.at("fill") : std::string();
        if (!fill.empty() && fill != "none" && cw > 0 && ch > 0) {
            sx = loader.width / cw; sy = loader.height / ch;
            if (fill == "scale") sx = sy = std::min(sx, sy);
            else if (fill == "scaleMatchHeight") sx = sy;
            else if (fill == "scaleMatchWidth") sy = sx;
            else if (fill == "scaleNoBorder") sx = sy = std::max(sx, sy);
        }
        draw.width = cw * sx; draw.height = ch * sy; if (loader.attrs.count("autoSize") && loader.attrs.at("autoSize") == "true") { draw.width = cw; draw.height = ch; }
        float ox = 0, oy = 0; const std::string align = loader.attrs.count("align") ? loader.attrs.at("align") : std::string(); const std::string valign = loader.attrs.count("vAlign") ? loader.attrs.at("vAlign") : std::string();
        if (align == "center") ox = std::floor((loader.width - draw.width) * 0.5f); else if (align == "right") ox = loader.width - draw.width;
        if (valign == "middle") oy = std::floor((loader.height - draw.height) * 0.5f); else if (valign == "bottom") oy = loader.height - draw.height;
        return writeImage(graph, *pkg, *res, draw, ox, oy);   // 偏移已随 ox/oy 写入(sprite=RX/RY,multi-quad=顶点平移)
    }
    static void graphColor(const std::string& source, uint32_t defaultRgb, float defaultAlpha, uint32_t& rgb, float& alpha) {
        std::string value = source; if (!value.empty() && value[0] == '#') value.erase(value.begin());
        if (value.empty()) { rgb = defaultRgb; alpha = defaultAlpha; return; }
        char* end = nullptr; unsigned long parsed = std::strtoul(value.c_str(), &end, 16);
        if (!end || *end != 0) { rgb = defaultRgb; alpha = defaultAlpha; return; }
        if (value.size() == 8) { alpha = ((parsed >> 24) & 255) / 255.0f; rgb = static_cast<uint32_t>(parsed & 0xFFFFFF); }
        else { alpha = 1; rgb = static_cast<uint32_t>(parsed & 0xFFFFFF); }
    }
    static sg::NativeQuad solidQuad(int texture, float x0, float y0, float x1, float y1, uint32_t tint, float alpha) {
        sg::NativeQuad q{}; q[0] = static_cast<float>(texture); q[1]=x0;q[2]=y0;q[3]=x1;q[4]=y0;q[5]=x1;q[6]=y1;q[7]=x0;q[8]=y1;
        q[9]=0;q[10]=0;q[11]=1;q[12]=0;q[13]=1;q[14]=1;q[15]=0;q[16]=1;q[17]=static_cast<float>(tint);q[18]=alpha;q[19]=-1;q[20]=-1;q[22]=0; return q;
    }
    static sg::NativeQuad solidTriangle(int texture, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t tint, float alpha) {
        // solidQuad derives its second corner as (x1,y0).  A triangle/four-point polygon supplies
        // every vertex explicitly, so retaining that derived Y bends every fan edge back toward
        // the first vertex.  Rounded-rect and ellipse masks then become large centre triangles
        // instead of their authored outline.  Publish the actual second vertex before collapsing
        // the last corner onto the third one.
        sg::NativeQuad q = solidQuad(texture,x0,y0,x1,y1,tint,alpha);q[3]=x1;q[4]=y1;q[5]=x2;q[6]=y2;q[7]=x2;q[8]=y2;return q;
    }
    static sg::NativeQuad solidFour(int texture, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t tint, float alpha) {
        sg::NativeQuad q = solidQuad(texture,x0,y0,x1,y1,tint,alpha);q[3]=x1;q[4]=y1;q[5]=x2;q[6]=y2;q[7]=x3;q[8]=y3;return q;
    }
    void writeGraph(sg::SceneGraph& graph, const InstanceNode& n) {
        // GGraph is mutable geometry.  A progress bar, controller or tween can first draw a
        // non-empty shape and then collapse it to zero (ChallengeTaskItem initializes its bar
        // as 100%, then immediately applies the real 0% value).  Returning early for the zero
        // size without clearing the previous quads leaves that old full-size shape on screen.
        // Rebuild from an empty visual every time, matching Graphics.clear()+redraw semantics
        // for zero sizes, empty fills and runtime shape changes.
        graph.clearNativeQuads(n.nativeId);
        float* s = graph.buf + (size_t)n.nativeId * sg::STRIDE; s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;s[sg::QUADCNT]=0;s[sg::RX]=0;s[sg::RY]=0;s[sg::RW]=n.width;s[sg::RH]=n.height;
        if (n.touchable && n.width > 0 && n.height > 0) {
            s[sg::HITFLAGS] = static_cast<float>(static_cast<int>(s[sg::HITFLAGS]) | 1);
            float* hit = graph.buf + sg::BOUNDS_BASE + (size_t)n.nativeId * sg::BOUNDS_STRIDE_F + 16; hit[0]=0;hit[1]=0;hit[2]=n.width;hit[3]=n.height;
        }
        if (solidTextureId <= 0 || n.width <= 0 || n.height <= 0) return;
        uint32_t fillRgb=0xFFFFFF,lineRgb=0;float fillAlpha=1,lineAlpha=1;graphColor(n.attrs.count("fillColor")?n.attrs.at("fillColor"):std::string(),0xFFFFFF,1,fillRgb,fillAlpha);graphColor(n.attrs.count("lineColor")?n.attrs.at("lineColor"):std::string(),0,1,lineRgb,lineAlpha);
        float line = std::max(0.0f, number(n.attrs.count("lineSize")?n.attrs.at("lineSize"):std::string(),1)); std::vector<sg::NativeQuad> quads;
        const std::string type=n.attrs.count("type")?n.attrs.at("type"):std::string();
        if (type.empty() || type == "empty") return;
        if (type == "ellipse" || type == "eclipse") {
            const int segments=32;float cx=n.width*0.5f,cy=n.height*0.5f,rx=std::max(0.0f,cx-line),ry=std::max(0.0f,cy-line);
            if(fillAlpha>0)for(int i=0;i<segments;i++){float a0=6.28318530718f*i/segments,a1=6.28318530718f*(i+1)/segments;quads.push_back(solidTriangle(solidTextureId,cx,cy,cx+cosf(a0)*rx,cy+sinf(a0)*ry,cx+cosf(a1)*rx,cy+sinf(a1)*ry,fillRgb,fillAlpha));}
            if(line>0&&lineAlpha>0)for(int i=0;i<segments;i++){float a0=6.28318530718f*i/segments,a1=6.28318530718f*(i+1)/segments;float ox0=cx+cosf(a0)*cx,oy0=cy+sinf(a0)*cy,ox1=cx+cosf(a1)*cx,oy1=cy+sinf(a1)*cy,ix0=cx+cosf(a0)*rx,iy0=cy+sinf(a0)*ry,ix1=cx+cosf(a1)*rx,iy1=cy+sinf(a1)*ry;sg::NativeQuad q{};q[0]=solidTextureId;q[1]=ox0;q[2]=oy0;q[3]=ox1;q[4]=oy1;q[5]=ix1;q[6]=iy1;q[7]=ix0;q[8]=iy0;q[9]=0;q[10]=0;q[11]=1;q[12]=0;q[13]=1;q[14]=1;q[15]=0;q[16]=1;q[17]=lineRgb;q[18]=lineAlpha;q[19]=-1;q[20]=-1;quads.push_back(q);}
        } else {
            float radius=0;auto ci=n.attrs.find("corner");if(ci!=n.attrs.end()&&!ci->second.empty()){auto values=split(ci->second,',');if(!values.empty())radius=std::max(0.0f,number(values[0]));}
            radius=std::min(radius,std::min(n.width,n.height)*0.5f);
            if(radius>0.0f){
                const int arcSegments=8;
                auto rounded=[&](float inset,float r){std::vector<std::pair<float,float>> points;points.reserve(4*(arcSegments+1));float left=inset,top=inset,right=std::max(left,n.width-inset),bottom=std::max(top,n.height-inset),rr=std::min(r,std::min(right-left,bottom-top)*0.5f);float cx[4]={right-rr,right-rr,left+rr,left+rr},cy[4]={top+rr,bottom-rr,bottom-rr,top+rr},start[4]={-1.57079632679f,0,1.57079632679f,3.14159265359f};for(int c=0;c<4;c++)for(int i=0;i<=arcSegments;i++){float a=start[c]+1.57079632679f*i/arcSegments;points.emplace_back(cx[c]+std::cos(a)*rr,cy[c]+std::sin(a)*rr);}return points;};
                auto outer=rounded(0,radius),inner=rounded(line,std::max(0.0f,radius-line));
                const auto& fillPoly=line>0?inner:outer;if(fillAlpha>0&&!fillPoly.empty()){float cx=n.width*.5f,cy=n.height*.5f;for(size_t i=0;i<fillPoly.size();i++){const auto& a=fillPoly[i];const auto& b=fillPoly[(i+1)%fillPoly.size()];quads.push_back(solidTriangle(solidTextureId,cx,cy,a.first,a.second,b.first,b.second,fillRgb,fillAlpha));}}
                if(line>0&&lineAlpha>0&&outer.size()==inner.size())for(size_t i=0;i<outer.size();i++){const auto& a=outer[i];const auto& b=outer[(i+1)%outer.size()];const auto& c=inner[(i+1)%inner.size()];const auto& d=inner[i];quads.push_back(solidFour(solidTextureId,a.first,a.second,b.first,b.second,c.first,c.second,d.first,d.second,lineRgb,lineAlpha));}
            }else{
                if(fillAlpha>0)quads.push_back(solidQuad(solidTextureId,line,line,std::max(line,n.width-line),std::max(line,n.height-line),fillRgb,fillAlpha));
                if(line>0&&lineAlpha>0){quads.push_back(solidQuad(solidTextureId,0,0,n.width,line,lineRgb,lineAlpha));quads.push_back(solidQuad(solidTextureId,0,n.height-line,n.width,n.height,lineRgb,lineAlpha));quads.push_back(solidQuad(solidTextureId,0,line,line,n.height-line,lineRgb,lineAlpha));quads.push_back(solidQuad(solidTextureId,n.width-line,line,n.width,n.height-line,lineRgb,lineAlpha));}
            }
        }
        if(!quads.empty())graph.setNativeQuads(n.nativeId,std::move(quads));
    }
    static const TransitionDesc* transition(const InstanceNode& component, const std::string& name) {
        for (const auto& desc : component.transitions) if (desc.name == name) return &desc; return nullptr;
    }
    Instance* instanceForNode(int nativeId) const {
        auto owner=managedNodeOwners_.find(nativeId);
        return owner==managedNodeOwners_.end()?nullptr:owner->second;
    }
    static float transitionEase(const std::string& name, float t) {
        t = std::max(0.0f, std::min(1.0f, t)); std::string s; s.reserve(name.size()); for (char c : name) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        bool inOut=s.find("inout")!=std::string::npos, inOnly=!inOut&&s.find(".in")!=std::string::npos;
        auto powEase=[&](float power){if(inOut){float q=t*2;return q<1?0.5f*std::pow(q,power):1-0.5f*std::abs(std::pow(2-q,power));}return inOnly?std::pow(t,power):1-std::pow(1-t,power);};
        if(s.empty()||s.find("linear")!=std::string::npos)return t;
        if(s.find("quad")!=std::string::npos)return powEase(2);
        if(s.find("cube")!=std::string::npos||s.find("cubic")!=std::string::npos)return powEase(3);
        if(s.find("quart")!=std::string::npos||s.find("expo")!=std::string::npos)return powEase(4);
        if(s.find("quint")!=std::string::npos)return powEase(5);
        if(s.find("sine")!=std::string::npos){if(inOut)return -.5f*(std::cos(3.14159265359f*t)-1);if(inOnly)return 1-std::cos(t*1.57079632679f);return std::sin(t*1.57079632679f);}
        if(s.find("circ")!=std::string::npos){if(inOut){float q=t*2;return q<1?-.5f*(std::sqrt(std::max(0.0f,1-q*q))-1):.5f*(std::sqrt(std::max(0.0f,1-(q-2)*(q-2)))+1);}if(inOnly)return 1-std::sqrt(std::max(0.0f,1-t*t));float q=t-1;return std::sqrt(std::max(0.0f,1-q*q));}
        if(s.find("back")!=std::string::npos){float amount=1.7f;if(inOut){amount*=1.525f;float q=t*2;if(q<1)return .5f*q*q*((amount+1)*q-amount);q-=2;return .5f*(q*q*((amount+1)*q+amount)+2);}if(inOnly)return t*t*((amount+1)*t-amount);float q=t-1;return q*q*((amount+1)*q+amount)+1;}
        auto bounceOut=[](float q){if(q<1/2.75f)return 7.5625f*q*q;if(q<2/2.75f){q-=1.5f/2.75f;return 7.5625f*q*q+.75f;}if(q<2.5f/2.75f){q-=2.25f/2.75f;return 7.5625f*q*q+.9375f;}q-=2.625f/2.75f;return 7.5625f*q*q+.984375f;};
        if(s.find("bounce")!=std::string::npos){if(inOut)return t<.5f?.5f*(1-bounceOut(1-2*t)):.5f*bounceOut(2*t-1)+.5f;if(inOnly)return 1-bounceOut(1-t);return bounceOut(t);}
        if(s.find("elastic")!=std::string::npos){if(t==0||t==1)return t;constexpr float pi2=6.28318530718f;float period=inOut?.45f:.3f,shift=period/pi2*std::asin(1.0f);if(inOut){float q=t*2;if(q<1)return -.5f*std::pow(2.0f,10*(q-1))*std::sin(((q-1)-shift)*pi2/period);return std::pow(2.0f,-10*(q-1))*std::sin(((q-1)-shift)*pi2/period)*.5f+1;}if(inOnly){float q=t-1;return -std::pow(2.0f,10*q)*std::sin((q-shift)*pi2/period);}return std::pow(2.0f,-10*t)*std::sin((t-shift)*pi2/period)+1;}
        return t; // ParseEaseType also falls back to Linear for unknown names.
    }
    static void transitionPair(const std::string& source, float fallbackX, float fallbackY, float& x, float& y) {
        auto values = split(source, ','); x = fallbackX; y = fallbackY; if (!values.empty() && values[0] != "-") x = number(values[0], fallbackX); if (values.size() > 1 && values[1] != "-") y = number(values[1], fallbackY);
    }
    static void adjustTransitionPair(std::string& value,float dx,float dy){auto pair=split(value,',');if(pair.size()<2)pair.resize(2,"-");if(pair[0]!="-")pair[0]=std::to_string(number(pair[0])+dx);if(pair[1]!="-")pair[1]=std::to_string(number(pair[1])+dy);value=pair[0]+","+pair[1];}
    static void adjustTransitionPair(std::vector<std::string>& value,float dx,float dy){if(value.size()<2)value.resize(2,"-");if(value[0]!="-")value[0]=std::to_string(number(value[0])+dx);if(value[1]!="-")value[1]=std::to_string(number(value[1])+dy);}
    static uint32_t transitionColor(const std::string& source,uint32_t fallback=0xFFFFFF){std::string s=source;int base=10;if(!s.empty()&&s[0]=='#'){s.erase(s.begin());base=16;}else if(s.compare(0,2,"0x")==0||s.compare(0,2,"0X")==0)base=16;else for(char c:s)if((c>='a'&&c<='f')||(c>='A'&&c<='F')){base=16;break;}char* end=nullptr;unsigned long v=std::strtoul(s.c_str(),&end,base);return end&&*end==0?(uint32_t)(v&0xFFFFFF):fallback;}
    static std::string interpolateColor(const std::string& from,const std::string& to,float p){uint32_t a=transitionColor(from),b=transitionColor(to,a);int ar=(a>>16)&255,ag=(a>>8)&255,ab=a&255,br=(b>>16)&255,bg=(b>>8)&255,bb=b&255;uint32_t c=((uint32_t)std::lround(ar+(br-ar)*p)<<16)|((uint32_t)std::lround(ag+(bg-ag)*p)<<8)|(uint32_t)std::lround(ab+(bb-ab)*p);char out[8];std::snprintf(out,sizeof(out),"#%06X",c);return out;}
    static std::string overrideValue(const InstanceNode& component, const TransitionDesc& desc, const std::string& label, const std::string& fallback) {
        if (label.empty()) return fallback; auto it = component.transitionOverrides.find(desc.name + "\n" + label); if (it == component.transitionOverrides.end() || it->second.empty()) return fallback; std::string out = it->second[0]; for (size_t i=1;i<it->second.size();++i) out += "," + it->second[i]; return out;
    }
    InstanceNode* transitionTarget(InstanceNode& component, const std::string& id) const {
        if (id.empty()) return &component; Instance* inst=instanceForNode(component.nativeId);if(!inst)return nullptr;
        auto scope=inst->byOwnerAndId.find(component.nativeId);if(scope==inst->byOwnerAndId.end())return nullptr;
        auto target=scope->second.find(id);return target==scope->second.end()?nullptr:target->second;
    }
    // 步骤2(复审裁决):此队列即 fgui 域的 mutation journal——不只 transition,relations/gear/
    // pivot 补偿等 C++ 自治写全部灌入(按 instance+node+kind 合并),JS 每帧 drain 回流影子账本。
    // 不逐写点手工回流、不每帧全量扫描:变更检测收口处 queue,合并兜住流量。
    void queueTransitionProperty(InstanceNode& target, TransitionPropertyKind kind,
                                 float value0 = 0, float value1 = 0,
                                 const std::string& text = {}) {
        if (mutationJournalSuppressionDepth_ > 0) return;
        Instance* inst = instanceForNode(target.nativeId); if (!inst) return;
        ++journalEpoch_;   // 合并分支同样递增:值变了,影子仍需追平
        // Several active timelines can publish the same semantic property before JavaScript gets
        // its next frame. Only the final value is observable; coalescing here bounds an infinite
        // transition's bridge traffic without weakening the GObject shadow contract.
        for (auto it = transitionPropertySignals_.rbegin(); it != transitionPropertySignals_.rend(); ++it)
            if (it->instanceId == inst->id && it->nativeId == target.nativeId && it->kind == kind) {
                it->value0 = value0; it->value1 = value1; it->text = text; return;
            }
        transitionPropertySignals_.push_back({inst->id, target.nativeId, kind, value0, value1, text});
    }
    // A JavaScript-authored semantic write can follow a native controller/transition write before
    // the compact journal is drained (ordered-batch fallback is the important case).  The authored
    // write is already present in the JS facade, so publishing a new row would be redundant; but an
    // older queued native row must not replay its intermediate value over that newer shadow.  Only
    // replace an existing row and deliberately leave journalEpoch_ unchanged: the row's original
    // enqueue already made this generation observable.
    void supersedeQueuedTransitionProperty(InstanceNode& target, TransitionPropertyKind kind,
                                            float value0 = 0, float value1 = 0,
                                            const std::string& text = {}) {
        if (transitionPropertySignals_.empty()) return;
        Instance* inst = instanceForNode(target.nativeId); if (!inst) return;
        for (auto it = transitionPropertySignals_.rbegin(); it != transitionPropertySignals_.rend(); ++it)
            if (it->instanceId == inst->id && it->nativeId == target.nativeId && it->kind == kind) {
                it->value0 = value0; it->value1 = value1; it->text = text; return;
            }
    }
    void applyTransitionValue(InstanceNode& target, TransitionKind kind, const std::string& source, sg::SceneGraph& graph) {
        // 各 kind 的写路径自带精确标记(writeNode=GEOMETRY|LOOK、writeNodeVisibility=VISIBILITY、refreshVisual=LOOK|ATTRS);
        // 这里只置 schema 非 pristine,不再笼统排队 ATTRS|LOOK(否则 XY/Size 也会被误分类成 attrs 变化)。
        target.facadeSchemaPristine=false;
        float a=0,b=0;
        switch (kind) {
            case TransitionKind::XY: transitionPair(source,target.x,target.y,a,b);if(Instance* inst=instanceForNode(target.nativeId))moveNode(*inst,target,a,b,graph);else{target.x=a;target.y=b;writeNode(graph,target);}queueTransitionProperty(target,TransitionPropertyKind::XY,target.x,target.y);break;
            case TransitionKind::Size: transitionPair(source,target.width,target.height,a,b);if(Instance* inst=instanceForNode(target.nativeId))resizeNode(*inst,target,a,b,graph,/*compensatePivot=*/true);else{target.width=a;target.height=b;writeNode(graph,target);refreshVisual(target,graph);}queueTransitionProperty(target,TransitionPropertyKind::Size,target.width,target.height);break;
            case TransitionKind::Scale: {
                const float oldScaleX=target.scaleX,oldScaleY=target.scaleY;
                transitionPair(source,oldScaleX,oldScaleY,a,b);
                if(a!=oldScaleX||b!=oldScaleY)target.directDisplayScaleOverride=false;
                traceScaleWrite(target,"transition",oldScaleX,a);
                setSemanticScale(target,a,b);writeNode(graph,target);
                queueTransitionProperty(target,TransitionPropertyKind::Scale,target.scaleX,target.scaleY);break;
            }
            case TransitionKind::Pivot: transitionPair(source,target.pivotX,target.pivotY,a,b);setSemanticPivot(target,a,b,false);writeNode(graph,target);queueTransitionProperty(target,TransitionPropertyKind::Pivot,target.pivotX,target.pivotY);break;
            case TransitionKind::Alpha: target.alpha=number(source,target.alpha);writeNode(graph,target);queueTransitionProperty(target,TransitionPropertyKind::Alpha,target.alpha);break;
            case TransitionKind::Rotation: {const float rotation=number(source,target.rotation);if(rotation!=target.rotation)target.directDisplayRotationOverride=false;setSemanticRotation(target,rotation);writeNode(graph,target);queueTransitionProperty(target,TransitionPropertyKind::Rotation,target.rotation);break;}
            case TransitionKind::Skew: transitionPair(source,target.skewX,target.skewY,a,b);setSemanticSkew(target,a,b);writeNode(graph,target);queueTransitionProperty(target,TransitionPropertyKind::Skew,target.skewX,target.skewY);break;
            case TransitionKind::Visible: {
                const bool visible=source=="true";
                if(target.visible!=visible){
                    target.visible=visible;
                    if(Instance* inst=instanceForNode(target.nativeId))transitionVisibilitySignals_.push_back({inst->id,target.nativeId,visible});
                }
                writeNodeVisibility(graph,target);if(target.kind==ObjectKind::Group)if(Instance* inst=instanceForNode(target.nativeId))refreshGroupVisibility(*inst,graph);break;
            }
            case TransitionKind::Color: revokeTextFacadeVisual(target);target.attrs["color"]=source;refreshVisual(target,graph);queueTransitionProperty(target,TransitionPropertyKind::Color,0,0,source);break;
            case TransitionKind::Animation: {auto v=split(source,',');if(!v.empty()&&v[0]!="-")target.attrs["frame"]=v[0];if(v.size()>1)target.attrs["playing"]=(v[1]=="p"||v[1]=="true")?"true":"false";refreshVisual(target,graph);queueTransitionProperty(target,TransitionPropertyKind::Animation,number(target.attrs["frame"]),target.attrs["playing"]=="true"?1.0f:0.0f);break;}
            case TransitionKind::ColorFilter:
                // This project's bundled fairygui runtime intentionally leaves
                // GObject.updateColorComponents as a FIXME. Keep the authored value for
                // inspection/overrides, but do not invent a Native-only visual filter.
                target.attrs["colorFilter"]=source;FacadeSeqTracker::mark(graph,target.nativeId,FD_ATTRS);
                break;
            case TransitionKind::Text: revokeTextFacadeVisual(target);target.attrs["text"]=source;refreshVisual(target,graph);queueTransitionProperty(target,TransitionPropertyKind::Text,0,0,source);break;
            case TransitionKind::Icon: target.attrs["url"]=source;refreshVisual(target,graph);queueTransitionProperty(target,TransitionPropertyKind::Icon,0,0,source);break;
            default: break;
        }
    }
    void applyTransition(InstanceNode& component, const TransitionDesc& desc, float timeline, bool reversed, sg::SceneGraph& graph, int depth=0, float ownerBaseX=0, float ownerBaseY=0) {
        if(depth>12)return;
        for (const auto& item : desc.items) {
            InstanceNode* target=transitionTarget(component,item.targetId);if(!target)continue;float start=reversed?desc.maxTime-item.time-(item.tween?item.duration:0):item.time;if(timeline<start)continue;
            float local=std::max(0.0f,timeline-start);
            if(item.kind==TransitionKind::Shake){std::string value=overrideValue(component,desc,item.label,item.value);float amplitude=0,duration=0;transitionPair(value,0,0,amplitude,duration);if(local<duration&&duration>0){float decay=1-local/duration;target->shakeX=std::sin(local*83.17f)*amplitude*decay;target->shakeY=std::sin(local*111.31f+1.7f)*amplitude*decay;}else target->shakeX=target->shakeY=0;writeNode(graph,*target);continue;}
            if(item.kind==TransitionKind::Nested){std::string value=overrideValue(component,desc,item.label,item.value);auto parts=split(value,',');const TransitionDesc* nested=!parts.empty()?transition(*target,parts[0]):nullptr;int times=parts.size()>1?integer(parts[1],1):1;if(nested&&times!=0){float nestedSpan=transitionRuntimeSpan(*target,*nested,reversed,depth+1);if(!std::isfinite(nestedSpan))nestedSpan=std::max(.0001f,nested->maxTime);else nestedSpan=std::max(.0001f,nestedSpan);float nestedTime=times<0?std::fmod(local,nestedSpan):std::min(local,nestedSpan*std::max(1,times));bool finiteEnd=times>0&&nestedTime>=nestedSpan*times-.00001f;float nestedTimeline=finiteEnd?nestedSpan:std::fmod(nestedTime,nestedSpan);applyTransition(*target,*nested,nestedTimeline,reversed,graph,depth+1,target->x,target->y);}continue;}
            if(!item.tween){std::string value=overrideValue(component,desc,item.label,item.value);if(item.kind==TransitionKind::XY&&target==&component){auto xy=split(value,',');float x=xy.empty()||xy[0]=="-"?component.x:number(xy[0])+ownerBaseX,y=xy.size()<2||xy[1]=="-"?component.y:number(xy[1])+ownerBaseY;value=std::to_string(x)+","+std::to_string(y);}applyTransitionValue(*target,item.kind,value,graph);continue;}
            // repeat=-1 loops forever ($tweenRepeatComplete restarts unconditionally); pass parity
            // still drives yoyo swapping exactly like tweenTimes%2 in the stock runtime.
            float duration=std::max(0.0001f,item.duration);int pass=item.repeat<0?static_cast<int>(local/duration):std::min(item.repeat,static_cast<int>(local/duration));float p=transitionEase(item.ease,std::min(1.0f,(local-pass*duration)/duration));bool swap=reversed^((item.yoyo&&pass%2)==1);
            std::string from=overrideValue(component,desc,item.label,item.startValue),to=overrideValue(component,desc,item.label2,item.endValue);if(swap)std::swap(from,to);
            if(item.kind==TransitionKind::XY||item.kind==TransitionKind::Size||item.kind==TransitionKind::Scale||item.kind==TransitionKind::Pivot||item.kind==TransitionKind::Skew){float x0,y0,x1,y1;bool ownerXY=item.kind==TransitionKind::XY&&target==&component;transitionPair(from,item.kind==TransitionKind::XY&&!ownerXY?target->x:0,item.kind==TransitionKind::XY&&!ownerXY?target->y:0,x0,y0);transitionPair(to,x0,y0,x1,y1);float x=x0+(x1-x0)*p,y=y0+(y1-y0)*p;if(ownerXY){x+=ownerBaseX;y+=ownerBaseY;}applyTransitionValue(*target,item.kind,std::to_string(x)+","+std::to_string(y),graph);}
            else if(item.kind==TransitionKind::Alpha||item.kind==TransitionKind::Rotation){float v0=number(from),v1=number(to,v0);applyTransitionValue(*target,item.kind,std::to_string(v0+(v1-v0)*p),graph);}
            else if(item.kind==TransitionKind::Color)applyTransitionValue(*target,item.kind,interpolateColor(from,to,p),graph);
            else if(item.kind==TransitionKind::ColorFilter){auto av=split(from,','),bv=split(to,',');std::string value;for(int k=0;k<4;k++){float x=k<(int)av.size()?number(av[k]):0,y=k<(int)bv.size()?number(bv[k],x):x;if(k)value+=",";value+=std::to_string(x+(y-x)*p);}applyTransitionValue(*target,item.kind,value,graph);}
            else applyTransitionValue(*target,item.kind,p>=1?to:from,graph);
        }
    }
    static bool isBitmapText(const InstanceNode& node) {
        if (node.kind != ObjectKind::Text) return false;
        auto font = node.attrs.find("font");
        return font != node.attrs.end() && font->second.compare(0, 5, "ui://") == 0;
    }
    static bool isOrdinaryText(const InstanceNode& node) {
        return node.kind == ObjectKind::Text && !isBitmapText(node);
    }
    static bool textRenderResetsDisplayScale(const InstanceNode& node) {
        if (!isOrdinaryText(node)) return false;
        auto autoSize = node.attrs.find("autoSize");
        // GTextField.renderNow resets the Pixi display object's scale for every non-shrink
        // layout. GObject scale remains semantic state and can take ownership again through a
        // later setScale; the next text render repeats this raw display-side reset.
        return autoSize == node.attrs.end() || autoSize->second != "shrink";
    }
    bool prepareTextSurface(InstanceNode& node, TextPrepareMode mode, sg::SceneGraph& graph) {
        if (!prepareText) return false;
        node.touchFacade(FD_TEXT);
        if (isOrdinaryText(node)) node.nativeTextMetricsValid = false;
        bool ok = prepareText(node.nativeId, node, mode);
        if (!ok && isOrdinaryText(node)) node.nativeTextMetricsValid = false;
        if (ok && textRenderResetsDisplayScale(node)) {
            node.directDisplayScaleX = 1;
            node.directDisplayScaleY = 1;
            node.directDisplayScaleOverride = true;
            writeNode(graph, node);
        }
        return ok;
    }
    // 非左/非顶对齐的普通文本需要 authored 盒内对齐(align/vAlign attr):FacadeNatural 把 natural
    // 面钉在 authored x/y,语义是"等 JS NativeTextProxy 接管定位"——对 schema/mutation 直写、
    // 业务从不 setText 的字段,接管永不发生,对齐随之永久丢失(H5 侧 GTextField.renderNow 会
    // layoutAlign)。这类字段改走 AuthoredBox(盒内行对齐,≙ H5 autoSize+align 的锚点净效果)。
    static bool textAttrWantsAuthoredBox(const InstanceNode& n) {
        auto align = n.attrs.find("align");
        if (align != n.attrs.end() && !align->second.empty() && align->second != "left") return true;
        auto vertical = n.attrs.find("vAlign");
        if (vertical != n.attrs.end() && !vertical->second.empty() && vertical->second != "top") return true;
        return false;
    }

    void applyPlacedProgressBars(Instance& inst, sg::SceneGraph& graph) {
        // GProgressBar.setupAfterAdd always calls update(), even when its placement descriptor
        // contains no ProgressBar child. If that child does exist, parseInt(attr) || 0 resets a
        // missing value/max to zero. Native builds the display tree before any lazy JS facade is
        // materialised, so compile the same initial geometry here after controller gears settle.
        for (const auto& owned : inst.nodes) {
            if (!owned || owned->extension != "ProgressBar" ||
                owned->attrs.find("setup.placed") == owned->attrs.end()) continue;
            InstanceNode& progress = *owned;
            const bool hasSetup = progress.attrs.find("setup.extensionPresent") != progress.attrs.end();
            float value = 50.0f, maximum = 100.0f;
            if (hasSetup) {
                auto valueIt = progress.attrs.find("setup.value");
                auto maxIt = progress.attrs.find("setup.max");
                value = number(valueIt == progress.attrs.end() ? std::string() : valueIt->second, 0.0f);
                maximum = number(maxIt == progress.attrs.end() ? std::string() : maxIt->second, 0.0f);
                if (!std::isfinite(value)) value = 0.0f;
                if (!std::isfinite(maximum)) maximum = 0.0f;
            }
            const float percent = maximum != 0.0f ? std::min(value / maximum, 1.0f) : 0.0f;
            InstanceNode* barH = nullptr;
            InstanceNode* barV = nullptr;
            InstanceNode* title = nullptr;
            for (int childId : progress.children) if (InstanceNode* child = nodeIn(inst, childId)) {
                if (child->name == "bar") barH = child;
                else if (child->name == "bar_v") barV = child;
                else if (child->name == "title") title = child;
            }
            const bool reverse = progress.attrs.find("component.reverse") != progress.attrs.end() &&
                                 progress.attrs.at("component.reverse") == "true";
            if (barH) {
                const float fullWidth = std::max(0.0f, progress.width -
                    (progress.sourceWidth - barH->initWidth));
                const float startX = barH->x;
                resizeNode(inst, *barH, fullWidth * percent, barH->height, graph,
                           /*compensatePivot=*/true);
                if (reverse) moveNode(inst, *barH, startX + (fullWidth - barH->width), barH->y, graph);
            }
            if (barV) {
                const float fullHeight = std::max(0.0f, progress.height -
                    (progress.sourceHeight - barV->initHeight));
                const float startY = barV->y;
                resizeNode(inst, *barV, barV->width, fullHeight * percent, graph,
                           /*compensatePivot=*/true);
                if (reverse) moveNode(inst, *barV, barV->x, startY + (fullHeight - barV->height), graph);
            }
            if (title) {
                const std::string titleType = progress.attrs.count("component.titleType")
                    ? progress.attrs.at("component.titleType") : std::string("percent");
                const auto rounded = [](float input) {
                    return static_cast<long long>(std::floor(input + 0.5f));
                };
                std::string text;
                if (titleType == "valueAndmax")
                    text = std::to_string(rounded(value)) + "/" + std::to_string(rounded(maximum));
                else if (titleType == "value") text = std::to_string(rounded(value));
                else if (titleType == "max") text = std::to_string(rounded(maximum));
                else text = std::to_string(rounded(percent * 100.0f)) + "%";
                revokeTextFacadeVisual(*title);
                title->attrs["text"] = text;
                refreshVisual(*title, graph);
            }
        }
    }
    void applyAllControllers(Instance& inst, sg::SceneGraph& graph) {FacadeCommitScope __facadeCommit(*this,graph);
        struct ScopedJournalSuppression {
            int& depth;
            explicit ScopedJournalSuppression(int& value):depth(value){++depth;}
            ~ScopedJournalSuppression(){--depth;}
        } suppress(mutationJournalSuppressionDepth_);
        for (const auto& n : inst.nodes) if (n && !n->controllers.empty())
            for (size_t i=0;i<n->controllers.size();++i) applyController(inst,*n,static_cast<int>(i),graph,false);
        refreshGroupVisibility(inst,graph);
        applyPlacedProgressBars(inst,graph);

        // Controllers can replace text, color and font size. Publish facade-natural metrics only
        // after their authored default pages are final, for both reservation and live-miss builds.
        std::vector<int> schemaFailures;
        // Everything above is construction-time authored state. Publish the cheap schema contract
        // only after controllers and final text metrics have settled; every runtime write clears
        // it. Failed CoreText preparation remains ineligible so valid metrics from another row
        // can never be substituted for this node.
        for (const auto& node : inst.nodes) if (node) {
            node->facadeSchemaPristine=true;
            node->runtimeEventModeOwned=true;
        }
        for(int nativeId:schemaFailures)if(InstanceNode* node=nodeIn(inst,nativeId))node->touchFacade(FD_TEXT);
    }
    static bool controllerMatches(const std::string& reference, const ControllerState& controller, int index) {
        if (reference == controller.name) return true;
        if (reference.empty()) return false;
        char* end = nullptr; long value = std::strtol(reference.c_str(), &end, 10);
        return end && *end == 0 && value == index;
    }
    static int gearPageIndex(const std::vector<std::string>& pages, const std::string& page) {
        auto it = std::find(pages.begin(), pages.end(), page);
        return it == pages.end() ? -1 : static_cast<int>(it - pages.begin());
    }
    // Browser GObject.setXY calls updateGear(1): a business/layout write becomes the stored value
    // for the controller's current page. GearXY.apply itself is gear-locked and must not write the
    // value back. Keep that ownership boundary by invoking this helper only from the public
    // semantic position API, never from moveNode(), which controllers, transitions and relations
    // also use internally.
    void rememberCurrentXYGearValue(Instance& inst, InstanceNode& target) {
        if (!target.xyGear.present) return;
        InstanceNode* component = nodeIn(inst, target.ownerComponentId);
        if (!component) return;
        const ControllerState* controller = nullptr;
        for (size_t i = 0; i < component->controllers.size(); ++i) {
            if (!controllerMatches(target.xyGear.controller, component->controllers[i], static_cast<int>(i))) continue;
            controller = &component->controllers[i];
            break;
        }
        if (!controller || controller->selectedIndex < 0 ||
            controller->selectedIndex >= static_cast<int>(controller->pageIds.size())) return;
        const std::string& page = controller->pageIds[controller->selectedIndex];
        int index = gearPageIndex(target.xyGear.pages, page);
        if (index < 0) {
            // GearXY.updateState creates storage lazily for controller pages that were not
            // serialized in the package. Mirror that instead of mutating the authored default.
            target.xyGear.pages.push_back(page);
            target.xyGear.values.emplace_back(target.x, target.y);
            return;
        }
        if (index >= static_cast<int>(target.xyGear.values.size()))
            target.xyGear.values.resize(static_cast<size_t>(index + 1),
                                        {target.xyGear.defaultX, target.xyGear.defaultY});
        target.xyGear.values[static_cast<size_t>(index)] = {target.x, target.y};
    }
    void refreshVisual(InstanceNode& node, sg::SceneGraph& graph) {
        FacadeSeqTracker::mark(graph, node.nativeId, FD_LOOK | FD_ATTRS);
        // Once explicitly claimed, it follows the same ownership rule as ordinary Text nodes.
        if(node.nativeTextFacadeOwned){++textOwnershipStats_.preservedRefreshes;return;}
        if (node.kind == ObjectKind::Image) {
            Package* pkg = package(node.packageName);
            if (pkg) { auto r = pkg->resourcesById.find(node.sourceId); if (r != pkg->resourcesById.end()) writeImage(graph, *pkg, r->second, node); }
        } else if (node.kind == ObjectKind::MovieClip) {
            Package* pkg = package(node.packageName);
            if (pkg) { auto r = pkg->resourcesById.find(node.sourceId); if (r != pkg->resourcesById.end()) writeMovieClip(graph, *pkg, r->second, node); }
        } else if (node.kind == ObjectKind::Graph) {
            writeGraph(graph, node);
        } else if (node.kind == ObjectKind::Loader) {
            writeLoader(graph, node);
        } else if (isBitmapText(node)) {
            writeBitmapText(graph,node);
        } else if ((node.kind == ObjectKind::Text || node.kind == ObjectKind::RichText || node.kind == ObjectKind::InputText) && prepareText) {
            TextPrepareMode mode = node.nativeTextFacadeNatural ? TextPrepareMode::FacadeNatural : TextPrepareMode::AuthoredBox;
            // Natural nodes retain their host entry so same-value controller refreshes hit the
            // NativeTextSystem texture cache. Preserve the historical detach-first behavior for
            // every other component.
            if (mode == TextPrepareMode::AuthoredBox && releaseNode) releaseNode(node.nativeId);
            prepareTextSurface(node, mode, graph);
        }
    }
    void revokeTextFacadeVisual(InstanceNode& node){
        node.touchFacade(FD_TEXT);
        if(!node.nativeTextFacadeOwned)return;
        // A controller text/color gear is about to repaint from authored attributes. Natural
        // mode belongs to NativeTextProxy/GTextField and relies on its separate display offset;
        // retaining that mode after revocation paints the natural-width glyph surface at the
        // fixed text box's left edge. Restore AuthoredBox so align/vAlign are baked into the box.
        node.nativeTextFacadeOwned=false;node.nativeTextFacadeNatural=false;node.nativeTextMetricsValid=false;++textOwnershipStats_.nativeRevokes;
    }
    void applyController(Instance& inst, InstanceNode& component, int controllerIndex, sg::SceneGraph& graph,
                         bool publishSignals) {
        if (controllerIndex < 0 || controllerIndex >= static_cast<int>(component.controllers.size())) return;
        const ControllerState& c = component.controllers[controllerIndex];
        std::string page = c.selectedIndex >= 0 && c.selectedIndex < static_cast<int>(c.pageIds.size()) ? c.pageIds[c.selectedIndex] : std::string();
        for (const auto& owned : inst.nodes) {
            if (!owned || owned->ownerComponentId != component.nativeId) continue;
            InstanceNode& n = *owned;
            float oldX = n.x, oldY = n.y, oldWidth = n.width, oldHeight = n.height;
            float oldScaleX = n.scaleX, oldScaleY = n.scaleY, oldRotation = n.rotation;
            float gearWidth = oldWidth, gearHeight = oldHeight;
            bool sizeGearApplied = false;
            // 自然宽文本的对齐偏移由 writeNode 按当前盒宽/metrics 现算（见 naturalTextOffsetX/Y）。
            // 旧的"gear 前捕获 slot 偏差、gear 后原样写回"机制已移除：盒宽被 sizeGear 改变时它
            // 会把按旧盒宽算的偏移写回新盒，标题在 hover 换页后照样错位。
            bool geometryChanged = false, facadeContentChanged = false, publishTouchable = false;
            bool loaderIconAutoSized = false;
            if (n.xyGear.present && controllerMatches(n.xyGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.xyGear.pages, page); auto value = i >= 0 && i < static_cast<int>(n.xyGear.values.size()) ? n.xyGear.values[i] : std::make_pair(n.xyGear.defaultX, n.xyGear.defaultY);
                n.x = value.first; n.y = value.second; geometryChanged = true;
                if(publishSignals)queueTransitionProperty(n, TransitionPropertyKind::XY, n.x, n.y);
            }
            if (n.sizeGear.present && controllerMatches(n.sizeGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.sizeGear.pages, page); const GearSizeValue& value = i >= 0 && i < static_cast<int>(n.sizeGear.values.size()) ? n.sizeGear.values[i] : n.sizeGear.defaultValue;
                n.width = value.width; n.height = value.height;
                gearWidth = value.width; gearHeight = value.height; sizeGearApplied = true;
                if(n.scaleX!=value.scaleX||n.scaleY!=value.scaleY)n.directDisplayScaleOverride=false;
                traceScaleWrite(n,"size-gear",n.scaleX,value.scaleX);
                n.scaleX = value.scaleX; n.scaleY = value.scaleY; geometryChanged = true;
                if(publishSignals){queueTransitionProperty(n, TransitionPropertyKind::Size, n.width, n.height);queueTransitionProperty(n, TransitionPropertyKind::Scale, n.scaleX, n.scaleY);}
            }
            if (n.lookGear.present && controllerMatches(n.lookGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.lookGear.pages, page); const GearLookValue& value = i >= 0 && i < static_cast<int>(n.lookGear.values.size()) ? n.lookGear.values[i] : n.lookGear.defaultValue;
                if (n.rotation != value.rotation) n.directDisplayRotationOverride = false;
                n.alpha = value.alpha; n.rotation = value.rotation; n.touchable = value.touchable; publishTouchable = true; geometryChanged = true;
                if (value.grayed != n.grayed) { n.grayed = value.grayed; writeNodeGrayed(graph, n); }
                if(publishSignals){queueTransitionProperty(n, TransitionPropertyKind::Alpha, n.alpha);queueTransitionProperty(n, TransitionPropertyKind::Rotation, n.rotation);queueTransitionProperty(n, TransitionPropertyKind::Touchable, n.touchable ? 1.0f : 0.0f);queueTransitionProperty(n, TransitionPropertyKind::Grayed, n.grayed ? 1.0f : 0.0f);}
            }
            if (n.iconGear.present && controllerMatches(n.iconGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.iconGear.pages, page); n.attrs["url"] = i >= 0 && i < static_cast<int>(n.iconGear.values.size()) ? n.iconGear.values[i] : n.iconGear.defaultValue; geometryChanged = true;
                if(publishSignals)queueTransitionProperty(n, TransitionPropertyKind::Icon, 0, 0, n.attrs["url"]);
                // GearIcon.apply assigns GLoader.url after GearXY/GearSize. A resolved auto-size
                // image therefore performs one final ordinary setSize, including pivot
                // compensation. Recompute from the resource rather than preserving the previous
                // page's box or relying on a JS content sprite which Native intentionally omits.
                float contentWidth = 0, contentHeight = 0;
                if (resolvedLoaderAutoSize(n, contentWidth, contentHeight)) {
                    n.width = contentWidth;
                    n.height = contentHeight;
                    loaderIconAutoSized = true;
                }
            }
            if (n.textGear.present && controllerMatches(n.textGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.textGear.pages, page); n.attrs["text"] = i >= 0 && i < static_cast<int>(n.textGear.values.size()) ? n.textGear.values[i] : n.textGear.defaultValue; geometryChanged = true; facadeContentChanged = true;
                if(publishSignals)queueTransitionProperty(n, TransitionPropertyKind::Text, 0, 0, n.attrs["text"]);
            }
            if (n.colorGear.present && controllerMatches(n.colorGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.colorGear.pages, page); std::string value = i >= 0 && i < static_cast<int>(n.colorGear.values.size()) ? n.colorGear.values[i] : n.colorGear.defaultValue;
                auto colors = split(value, ','); if (!colors.empty() && !colors[0].empty() && colors[0] != "-") n.attrs["color"] = colors[0]; if (colors.size() > 1 && !colors[1].empty() && colors[1] != "-") n.attrs["strokeColor"] = colors[1]; geometryChanged = true; facadeContentChanged = true;
                auto color=n.attrs.find("color"),stroke=n.attrs.find("strokeColor");
                if(publishSignals){queueTransitionProperty(n, TransitionPropertyKind::Color, 0, 0, color==n.attrs.end()?std::string():color->second);queueTransitionProperty(n, TransitionPropertyKind::StrokeColor, 0, 0, stroke==n.attrs.end()?std::string():stroke->second);}
            }
            if (n.animationGear.present && controllerMatches(n.animationGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.animationGear.pages, page); std::string value = i >= 0 && i < static_cast<int>(n.animationGear.values.size()) ? n.animationGear.values[i] : n.animationGear.defaultValue;
                auto animation = split(value, ','); if (!animation.empty() && !animation[0].empty() && animation[0] != "-") n.attrs["frame"] = animation[0]; if (animation.size() > 1 && !animation[1].empty() && animation[1] != "-") n.attrs["playing"] = (animation[1] == "p" || animation[1] == "true" || animation[1] == "1") ? "true" : "false"; geometryChanged = true;
                auto frame=n.attrs.find("frame"),playing=n.attrs.find("playing");
                const bool isPlaying=playing==n.attrs.end()||(playing->second!="false"&&playing->second!="0");
                if(publishSignals)queueTransitionProperty(n, TransitionPropertyKind::Animation, frame==n.attrs.end()?0:number(frame->second), isPlaying?1.0f:0.0f);
            }
            if (n.fontSizeGear.present && controllerMatches(n.fontSizeGear.controller, c, controllerIndex)) {
                int i = gearPageIndex(n.fontSizeGear.pages, page); n.attrs["fontSize"] = i >= 0 && i < static_cast<int>(n.fontSizeGear.values.size()) ? n.fontSizeGear.values[i] : n.fontSizeGear.defaultValue; geometryChanged = true; facadeContentChanged = true;
                if(publishSignals)queueTransitionProperty(n, TransitionPropertyKind::FontSize, number(n.attrs["fontSize"]), 0, n.attrs["fontSize"]);
            }
            if (geometryChanged) {
                if(facadeContentChanged)revokeTextFacadeVisual(n);
                // A controller gear is a real GObject geometry mutation.  Commit it through the
                // same resize/move paths as public setters so relations that target this object
                // are notified as well.  Assigning x/y directly used to leave dependent buttons,
                // Spine slots and particles at their authored coordinates; a subsequent hover or
                // controller page could also snap a relation-adjusted node back to its old gear.
                const float x = n.x, y = n.y, width = n.width, height = n.height;
                const float scaleX = n.scaleX, scaleY = n.scaleY, rotation = n.rotation;
                const bool sizeChanged = width != oldWidth || height != oldHeight;
                const bool positionChanged = x != oldX || y != oldY;
                const bool scaleChanged = scaleX != oldScaleX || scaleY != oldScaleY;
                const bool rotationChanged = rotation != oldRotation;
                n.x = oldX; n.y = oldY; n.width = oldWidth; n.height = oldHeight;
                n.scaleX = oldScaleX; n.scaleY = oldScaleY; n.rotation = oldRotation;
                // Browser GearSize: setSize(w, h, gearXY.controller == this.$controller) — the
                // pivot shift applies only when no same-controller GearXY drives the position.
                const bool gearXYOwnsPosition = n.xyGear.present && controllerMatches(n.xyGear.controller, c, controllerIndex);
                if (loaderIconAutoSized) {
                    // Stock gear order is XY -> Size (setSize then setScale) -> Look -> Icon.
                    // Preserve the intermediate GearSize publication before the Loader URL's
                    // final auto-size; each call can update relations and retained image position.
                    if (positionChanged) moveNode(inst, n, x, y, graph);
                    if (sizeGearApplied && (gearWidth != oldWidth || gearHeight != oldHeight))
                        resizeNode(inst, n, gearWidth, gearHeight, graph,
                                   /*compensatePivot=*/!gearXYOwnsPosition);
                    if (scaleChanged) setSemanticScale(n, scaleX, scaleY);
                    if (rotationChanged) setSemanticRotation(n, rotation);
                    if (width != n.width || height != n.height)
                        resizeNode(inst, n, width, height, graph, /*compensatePivot=*/true);
                } else {
                    // handleControllerChanged applies GearXY before GearSize. This ordering is
                    // observable for GImage: setXY runs handleXYChanged, while a subsequent
                    // zero-pivot setSize does not.
                    if (positionChanged) moveNode(inst, n, x, y, graph);
                    if (sizeChanged) resizeNode(inst, n, width, height, graph, /*compensatePivot=*/!gearXYOwnsPosition);
                    if (scaleChanged) setSemanticScale(n, scaleX, scaleY);
                    if (rotationChanged) setSemanticRotation(n, rotation);
                }
                // Non-geometric gears (look/icon/text/color/animation/font-size) still need their
                // normal node publication, and this also publishes scale changes from GearSize.
                writeNode(graph, n); refreshVisual(n,graph);
                if(publishTouchable&&n.runtimeEventModeOwned)writeRuntimeTouchable(graph,n);
                // GearIcon may auto-size a Loader after GearSize ran. Re-publish the final
                // semantic values after every gear/relation side effect so the coalesced journal
                // can never restore the intermediate GearSize box into the JS facade.
                if(publishSignals){
                    if(n.xyGear.present&&controllerMatches(n.xyGear.controller,c,controllerIndex))queueTransitionProperty(n,TransitionPropertyKind::XY,n.x,n.y);
                    if((n.sizeGear.present&&controllerMatches(n.sizeGear.controller,c,controllerIndex))||loaderIconAutoSized)queueTransitionProperty(n,TransitionPropertyKind::Size,n.width,n.height);
                    if(n.sizeGear.present&&controllerMatches(n.sizeGear.controller,c,controllerIndex))queueTransitionProperty(n,TransitionPropertyKind::Scale,n.scaleX,n.scaleY);
                }
            }
            if (controllerMatches(n.displayGear.controller, c, controllerIndex)) {
                n.facadeSchemaPristine=false;   // 下面 writeNodeVisibility 立即标 FD_VISIBILITY,不再排队(一次投影一次递增)
                bool show = n.displayGear.pages.empty() || std::find(n.displayGear.pages.begin(), n.displayGear.pages.end(), page) != n.displayGear.pages.end();
                n.gearHidden = !show;   // 持久到节点;后续 writeNode 依此合成,不再被冲掉
                writeNodeVisibility(graph,n);
                if(publishSignals)queueTransitionProperty(n, TransitionPropertyKind::InternalVisible,(!n.gearHidden||n.transitionDisplayLocks>0)?1.0f:0.0f);
            }
        }
    }
    bool indexManagedNodes(Instance& inst) {
        // Validate first so a malformed instance cannot leave a partially published reverse
        // index. SceneGraph ids are globally unique while alive; an existing different owner is
        // therefore an invariant violation, not something that may safely be overwritten.
        if (inst.byNativeId.size() != inst.nodes.size()) {
            lastBuildFailure_ = "index-size:nodes=" + std::to_string(inst.nodes.size()) +
                ":ids=" + std::to_string(inst.byNativeId.size()); return false;
        }
        for (const auto& node : inst.nodes) {
            if (!node || node->nativeId <= 0) { lastBuildFailure_ = "index-invalid-node"; return false; }
            auto local = inst.byNativeId.find(node->nativeId);
            if (local == inst.byNativeId.end() || local->second != node.get()) {
                lastBuildFailure_ = "index-local:" + std::to_string(node->nativeId); return false;
            }
            auto owner = managedNodeOwners_.find(node->nativeId);
            if (owner != managedNodeOwners_.end() && owner->second != &inst) {
                lastBuildFailure_ = "index-owner-conflict:" + std::to_string(node->nativeId); return false;
            }
        }
        managedNodeOwners_.reserve(managedNodeOwners_.size() + inst.nodes.size());
        inst.byOwnerAndId.clear();
        for (const auto& node : inst.nodes) {
            managedNodeOwners_.emplace(node->nativeId,&inst);
            if(node->ownerComponentId>0&&!node->id.empty())inst.byOwnerAndId[node->ownerComponentId].emplace(node->id,node.get());
        }
        return true;
    }
    void unindexManagedNodes(Instance& inst) {
        for (const auto& node : inst.nodes) if (node && node->nativeId > 0) {
            auto owner = managedNodeOwners_.find(node->nativeId);
            if (owner != managedNodeOwners_.end() && owner->second == &inst) managedNodeOwners_.erase(owner);
        }
        inst.byOwnerAndId.clear();
    }
    void destroyTree(Instance& inst, sg::SceneGraph& graph) {
        // Remove ownership before freeNode makes the id available to an unrelated JS/native node.
        unindexManagedNodes(inst);
        for (auto it=inst.nodes.rbegin();it!=inst.nodes.rend();++it) if (*it && (*it)->nativeId>0) {
            if (releaseNode) releaseNode((*it)->nativeId); graph.freeNode((*it)->nativeId);
        }
        inst.byNativeId.clear(); inst.nodes.clear();
    }
};

} // namespace nfgui
