#pragma once

#include <algorithm>
#include <cmath>

namespace richtext {

// 两个独立概念(docs/text-layout-contract-v2.md 第三部分;数值以 tests/golden/ 的 oracle 为准):
//  · token 内部 grapheme 间隙 = letterSpacing(词内 n-1 次,TaggedText 尾部再 +1 次——
//    见 richtext_token_layout.h::tokenAdvance 的完整公式);
//  · 词 token 的描边 advance 补偿 = strokeWidth(Pixi 测量宽含 stroke padding;空白 token
//    被 TaggedText 显式减回,净 0)。
inline double tokenInternalGapKern(double letterSpacing) {
    return std::isfinite(letterSpacing) ? letterSpacing : 0;
}
inline double tokenStrokeAdvanceCorrection(double strokeWidth, bool whitespace, bool visibleStroke) {
    if (whitespace || !visibleStroke || !std::isfinite(strokeWidth)) return 0;
    return std::max(0.0, strokeWidth);
}

// 旧 kern-附加实现(整串 kCTKern + 词尾覆盖)的混合值。mac 已迁移到 token 级排版
// (richtext_token_layout.h),不再使用;iOS 未迁移前保留。经 oracle 对数,其 advance 设计与 H5
// 等量,但 kCTKern 负值会被 CoreText per-glyph max(0,·) 钳制(丢空格根因),且显式 0 会禁用
// pair kerning —— 语义缺陷在附加方式,不在数值。
inline double taggedTextTrailingKern(double letterSpacing, double strokeWidth,
                                     bool whitespace, bool visibleStroke) {
    return tokenInternalGapKern(letterSpacing)
         + tokenStrokeAdvanceCorrection(strokeWidth, whitespace, visibleStroke);
}

} // namespace richtext
