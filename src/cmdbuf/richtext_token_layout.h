#pragma once

// H5 TaggedText token 布局语义的 C++ 复刻(修法候选 A:token 级排版,docs/text-layout-contract-v2.md)。
//
// 单一事实来源是 tests/golden/h5_taggedtext_kern_oracle.json —— 真实 TaggedText.umd_1.js 的实测输出。
// 本头的每条规则都以绝对值对数验证:tests/mac_coretext_kern_probe.mm --mode=sim(同字表同公式)必须与
// oracle 全 token 全等(content/kind/line/x/width),见 tests/mac_coretext_kern_compare.js --sim。
//
// 规则出处(game/libs/v1.0/TaggedText.umd_1.js 行号):
//  · advance   :926-947  Pixi 测量宽(词内 n-1 次 letterSpacing,ceil;词含 strokeThickness,
//                        空白显式减回净 0)+ calculateTokens 尾部再 +1×letterSpacing。
//                        即:词 = ceil(naked+(n-1)ls+strokeAdv)+ls;空白 = ceil(naked+(n-1)ls)+ls。
//                        负 letterSpacing 线性累计,允许负宽 token,禁止钳制(CoreText kern 的
//                        per-glyph max(0,·) 钳制正是丢空格根因,本布局器替代之)。
//  · 断行      :736-833  词缓冲整体搬行(cursor.x+wordWidth>maxWidth 且行非空);空白立即入行且
//                        永不触发断行;换行 token/块级图片立即换行;maxWidth=wrap>0?wrap:∞。
//  · 行尾空白  :716-734  每行尾部连续空白 token 宽度置 0(collapse),行内其余不动。
//  · 对齐      :511-522  行内 x 从 0 按(collapse 后)宽度链式重排;center/right 再加整体偏移,
//                        基准为 alignWidth(mac 现状=光栅 surface 宽;三宽度协议接入后按 frame 决议)。
//  · 行高      :非 TaggedText 语义 —— 保持 mac updateRich 现状模型(minimumLineHeight 约束+自然扩展),
//                        行高不进对数(sim 只比 line/x/width)。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace richtext {

enum class TokenKind { Word, Whitespace, Newline, Image };
enum class TokenAlign { Left, Center, Right };

struct TokenInput {
    TokenKind kind = TokenKind::Word;
    double nakedWidth = 0;        // 无 kern 属性的测量宽;Image=占位宽(icon kern 已含,不再套公式)
    int graphemeCount = 1;        // composed grapheme 数(词内 gap = n-1)
    double letterSpacing = 0;
    double strokeAdvance = 0;     // 词:visibleStroke ? strokeWidth : 0;空白/Image/换行恒 0
    bool blockImage = false;      // imgDisplay=block → 强制换行
    bool breakWords = false;      // ≙ TaggedText :753 token.style.breakWords:该 token 入词缓冲前先
                                  // flush 前词 → 相连 segment 逐个成词、可在 segment 边界断行。
                                  // oracle 铁证(segment-breakwords):CJK 单 token 仍无词内断行,
                                  // H5 同样整体溢出——breakWords 不是字符级断行。
    double ascent = 0, descent = 0;
    double naturalLineHeight = 0; // 该 token 字体的自然行高(行高聚合用)
};

struct TokenPlacement { double x = 0, width = 0; int line = 0; };
struct LinePlacement { double width = 0, x = 0, y = 0, ascent = 0, height = 0; };

struct TokenLayoutResult {
    std::vector<TokenPlacement> tokens;
    std::vector<LinePlacement> lines;
    double naturalWidth = 0, naturalHeight = 0;
};

// A flattened native label is rendered as one surface, while TaggedText on H5 keeps each token
// as an independent child.  Centring a line wider than its owner therefore produces a legitimate
// negative local x on H5.  Native surfaces cannot rasterise at a negative pixel coordinate, so the
// renderer rebases the pixels to x=0 and publishes the surface itself at the returned negative
// origin.  The two operations cancel visually and preserve the owner-relative centre.
//
// This is intentionally independent from wrapping and alignment policy: callers first apply the
// desired line offsets, then use this helper only for a surface which is allowed to overflow left.
inline double rebaseNegativeLineOrigins(TokenLayoutResult& r) {
    double minX = 0;
    for (const auto& line : r.lines) minX = std::min(minX, line.x);
    if (minX >= 0) return 0;
    const double shift = -minX;
    for (auto& line : r.lines) line.x += shift;
    for (auto& token : r.tokens) token.x += shift;
    return minX;
}

struct HorizontalContentBounds {
    double x = 0;
    double width = 0;
};

struct ContentBounds {
    double x = 0, y = 0, width = 0, height = 0;
};

// Measure visible token geometry in owner coordinates. `surfaceOriginX` is the placement returned
// by rebaseNegativeLineOrigins (zero for an ordinary in-box surface).
inline HorizontalContentBounds horizontalContentBounds(const TokenLayoutResult& r,
                                                        double surfaceOriginX = 0) {
    if (r.lines.empty()) return {};
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    for (const auto& line : r.lines) {
        minX = std::min(minX, surfaceOriginX + line.x);
        maxX = std::max(maxX, surfaceOriginX + line.x + line.width);
    }
    if (!std::isfinite(minX) || !std::isfinite(maxX)) return {};
    return {minX, std::max(0.0, maxX - minX)};
}

// Dynamic MultiStyleText retains paragraph coordinates even with a content-sized raster.
// Keep its original placement when cropping either positive blank space or negative overflow.
inline double compactLineOrigins(TokenLayoutResult& r) {
    const double origin = horizontalContentBounds(r).x;
    for (auto& line : r.lines) line.x -= origin;
    for (auto& token : r.tokens) token.x -= origin;
    return origin;
}

// center: width + x + left; right: width + x; left: width.
inline double taggedAnchorWidth(double contentX, double contentWidth, TokenAlign align) {
    if (align == TokenAlign::Center) return contentWidth + 2 * contentX;
    if (align == TokenAlign::Right) return contentWidth + contentX;
    return contentWidth;
}

// TextWrapperUtils.setPixiTextAlignCorrect can add a per-line Y offset and then centre the
// adjusted line centres inside the FairyGUI field's authored height.  H5 performs that operation
// by mutating every retained token child.  The native rich-text facade flattens those children into
// one surface, so apply the equivalent transform to the semantic lines before rasterisation.
//
// Negative content is rebased into the texture exactly like rebaseNegativeLineOrigins does for X;
// the returned value is the owner-relative surface origin which the renderer must publish.
inline double applyLineVerticalCorrection(TokenLayoutResult& r, double perLineOffset,
                                          double centreHeight,
                                          const std::vector<TokenInput>* tokenBoxes = nullptr) {
    if (r.lines.empty()) { r.naturalHeight = 0; return 0; }
    if (std::isfinite(perLineOffset) && perLineOffset != 0) {
        for (size_t i = 1; i < r.lines.size(); ++i)
            r.lines[i].y += static_cast<double>(i) * perLineOffset;
    }
    if (std::isfinite(centreHeight) && centreHeight > 0 && r.lines.size() > 1) {
        double minCentre = std::numeric_limits<double>::infinity();
        double maxCentre = -std::numeric_limits<double>::infinity();
        for (size_t li = 0; li < r.lines.size(); ++li) {
            const auto& line = r.lines[li];
            double centre = line.y + line.height * .5;
            // H5's helper groups a line around its first visible text child. In a mixed-size
            // line that child's centre can differ from the tallest token's centre.
            if (tokenBoxes) {
                for (size_t ti = 0; ti < r.tokens.size() && ti < tokenBoxes->size(); ++ti) {
                    const auto& token = (*tokenBoxes)[ti];
                    if (r.tokens[ti].line == static_cast<int>(li) && token.kind == TokenKind::Word) {
                        centre = line.y + line.ascent - token.ascent + token.naturalLineHeight * .5;
                        break;
                    }
                }
            }
            minCentre = std::min(minCentre, centre);
            maxCentre = std::max(maxCentre, centre);
        }
        const double shift = centreHeight * .5 - (minCentre + maxCentre) * .5;
        for (auto& line : r.lines) line.y += shift;
    }
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const auto& line : r.lines) {
        minY = std::min(minY, line.y);
        maxY = std::max(maxY, line.y + line.height);
    }
    if (!std::isfinite(minY) || !std::isfinite(maxY)) { r.naturalHeight = 0; return 0; }
    const double surfaceOriginY = std::min(0.0, minY);
    if (surfaceOriginY < 0)
        for (auto& line : r.lines) line.y -= surfaceOriginY;
    r.naturalHeight = std::max(0.0, maxY - surfaceOriginY);
    return surfaceOriginY;
}

// Bounds of the flattened label in owner coordinates.  `textOnly` models TaggedText's first
// retained child (`textContainer`): inline Sprite tokens participate in line layout but are not
// themselves children of that container.  Text before and after an inline image still spans the
// intervening gap, matching Pixi Container bounds.
inline ContentBounds tokenContentBounds(const TokenLayoutResult& r,
                                        const std::vector<TokenInput>& inputs,
                                        double surfaceOriginX = 0,
                                        double surfaceOriginY = 0,
                                        double top = 0,
                                        bool textOnly = false) {
    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    const size_t count = std::min(r.tokens.size(), inputs.size());
    for (size_t i = 0; i < count; ++i) {
        const TokenInput& input = inputs[i];
        if (input.kind == TokenKind::Newline || (textOnly && input.kind == TokenKind::Image)) continue;
        const int lineIndex = r.tokens[i].line;
        if (lineIndex < 0 || static_cast<size_t>(lineIndex) >= r.lines.size()) continue;
        const LinePlacement& line = r.lines[static_cast<size_t>(lineIndex)];
        const double tokenHeight = std::max(0.0, input.naturalLineHeight);
        const double x = surfaceOriginX + r.tokens[i].x;
        const double y = surfaceOriginY + top + line.y + line.ascent - input.ascent;
        minX = std::min(minX, x); minY = std::min(minY, y);
        maxX = std::max(maxX, x + std::max(0.0, r.tokens[i].width));
        maxY = std::max(maxY, y + tokenHeight);
    }
    if (!std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY)) return {};
    return {minX, minY, std::max(0.0, maxX - minX), std::max(0.0, maxY - minY)};
}

inline double tokenAdvance(const TokenInput& t) {
    switch (t.kind) {
        case TokenKind::Newline: return 0;
        case TokenKind::Image: return t.nakedWidth;
        case TokenKind::Whitespace:
        case TokenKind::Word: {
            const double ls = std::isfinite(t.letterSpacing) ? t.letterSpacing : 0;
            const double stroke = t.kind == TokenKind::Word ? std::max(0.0, t.strokeAdvance) : 0.0;
            const double inner = t.nakedWidth + (std::max(1, t.graphemeCount) - 1) * ls + stroke;
            return std::ceil(inner) + ls;
        }
    }
    return 0;
}

inline TokenLayoutResult layoutTokens(const std::vector<TokenInput>& tokens,
                                      double wrapWidth, double alignWidth, TokenAlign align,
                                      double requestedLineHeight, double defaultLineHeight,
                                      double defaultAscent) {
    TokenLayoutResult out;
    out.tokens.resize(tokens.size());
    const double maxWidth = wrapWidth > 0 ? wrapWidth : std::numeric_limits<double>::infinity();

    // ---- 断行(≙ layout :736-833):cursor 只用于断行判定,最终 x 由 alignLeft 链式重排决定 ----
    double cursorX = 0, wordWidth = 0;
    std::vector<size_t> word;                 // 词缓冲(token 索引)
    int lineIndex = 0;
    bool lineHasContent = false;              // ≙ line.length>0
    auto flushWord = [&]() {
        for (size_t idx : word) {
            out.tokens[idx].line = lineIndex;
            cursorX += out.tokens[idx].width;
            lineHasContent = true;
        }
        word.clear();
        wordWidth = 0;
    };
    auto newLine = [&]() { lineIndex++; cursorX = 0; lineHasContent = false; };
    for (size_t i = 0; i < tokens.size(); ++i) {
        const TokenInput& t = tokens[i];
        out.tokens[i].width = tokenAdvance(t);
        const bool isWs = t.kind == TokenKind::Whitespace;
        const bool isNl = t.kind == TokenKind::Newline;
        const bool isImg = t.kind == TokenKind::Image;
        if (isWs || isImg || isNl || t.breakWords) flushWord();
        word.push_back(i);
        wordWidth += out.tokens[i].width;
        if (isWs || isNl) flushWord();
        if (isNl || (isImg && t.blockImage)) newLine();
        else if (cursorX + wordWidth > maxWidth && lineHasContent) { lineIndex++; cursorX = 0; lineHasContent = false; }
    }
    flushWord();
    // 行数按 token 实际分布计(≙ TaggedText:newline token 留在它结束的行,newLine 后的新行只有
    // 收到 token 才实体化)。oracle 铁证:"A\n"=1 行、"\n\n"=2 行——尾随换行不得多计空行
    // (旧 lineIndex+1 让每个 </p>/尾 \n 的富文本高度多一个默认行高)。
    int lineCount = 0;
    for (size_t i = 0; i < tokens.size(); ++i) lineCount = std::max(lineCount, out.tokens[i].line + 1);
    out.lines.resize((size_t)std::max(0, lineCount));

    // ---- 行尾空白 collapse(≙ collapseWhitespacesOnEndOfLines :716-734):每行独立,尾部连续空白置 0 宽 ----
    for (int li = 0; li < lineCount; ++li) {
        // 找该行 token 范围(顺序分配,行内索引连续)
        std::ptrdiff_t last = -1;
        for (size_t i = 0; i < tokens.size(); ++i) if (out.tokens[i].line == li) last = (std::ptrdiff_t)i;
        for (std::ptrdiff_t i = last; i >= 0 && out.tokens[(size_t)i].line == li; --i) {
            const TokenKind k = tokens[(size_t)i].kind;
            if (k == TokenKind::Whitespace || k == TokenKind::Newline) out.tokens[(size_t)i].width = 0;
            else break;
        }
    }

    // ---- 行内 x 链式重排 + 对齐(≙ alignLeft/Center/Right :511-522) ----
    for (int li = 0; li < lineCount; ++li) {
        double x = 0, maxAscent = 0, maxNaturalLh = 0;
        bool any = false;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (out.tokens[i].line != li) continue;
            out.tokens[i].x = x;
            x += out.tokens[i].width;
            if (tokens[i].kind != TokenKind::Newline) {
                maxAscent = std::max(maxAscent, tokens[i].ascent);
                maxNaturalLh = std::max(maxNaturalLh, tokens[i].naturalLineHeight);
            }
            any = true;
        }
        LinePlacement& line = out.lines[(size_t)li];
        line.width = x;
        line.ascent = any && maxAscent > 0 ? maxAscent : defaultAscent;
        const double natural = any && maxNaturalLh > 0 ? maxNaturalLh : defaultLineHeight;
        line.height = std::max(requestedLineHeight > 0 ? requestedLineHeight : 0.0, natural);
        double offset = 0;
        if (std::isfinite(alignWidth) && alignWidth > 0) {
            if (align == TokenAlign::Center) offset = (alignWidth - line.width) / 2;
            else if (align == TokenAlign::Right) offset = alignWidth - line.width;
        }
        line.x = offset;
        if (offset != 0)
            for (size_t i = 0; i < tokens.size(); ++i)
                if (out.tokens[i].line == li) out.tokens[i].x += offset;
        out.naturalWidth = std::max(out.naturalWidth, line.width);
    }
    double y = 0;
    for (auto& line : out.lines) { line.y = y; y += line.height; }
    out.naturalHeight = y;
    return out;
}

// 对齐基准宽(如 surface 宽 = max(natural, box))在断行之后才确定时的二段式对齐:
// 先用 TokenAlign::Left 调 layoutTokens,再以最终基准宽整体偏移各行。
inline void applyLineAlignOffsets(TokenLayoutResult& r, double alignWidth, TokenAlign align) {
    if (align == TokenAlign::Left || !std::isfinite(alignWidth) || alignWidth <= 0) return;
    for (size_t li = 0; li < r.lines.size(); ++li) {
        const double offset = align == TokenAlign::Center ? (alignWidth - r.lines[li].width) / 2
                                                          : alignWidth - r.lines[li].width;
        r.lines[li].x += offset;
        for (auto& t : r.tokens)
            if ((size_t)t.line == li) t.x += offset;
    }
}

} // namespace richtext
