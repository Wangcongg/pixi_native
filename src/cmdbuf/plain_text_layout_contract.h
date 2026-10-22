#pragma once

#include <algorithm>
#include <cmath>

namespace plaintext {

struct VerticalFontMetrics {
    double ascent = 0;
    double descent = 0;
    double height = 0;
    bool pixiMeasured = false;
};

// PIXI does not use the font's typographic ascent/descent. TextMetrics.measureFont paints
// "|ÉqÅM" and scans the resulting Canvas pixels, which can differ by several pixels for the
// and, more importantly, the alphabetic baseline must use that scanned pair together. Falling
// back to platform metrics keeps package-factory text working before a JS facade supplies them.
inline VerticalFontMetrics verticalFontMetrics(double coreAscent, double coreDescent,
                                                double pixiAscent, double pixiDescent) {
    const bool measured = std::isfinite(pixiAscent) && std::isfinite(pixiDescent) &&
                          pixiAscent > 0 && pixiDescent >= 0 &&
                          pixiAscent + pixiDescent > 0;
    if (measured) return {pixiAscent, pixiDescent, pixiAscent + pixiDescent, true};
    return {coreAscent, coreDescent, std::ceil(coreAscent + coreDescent), false};
}

inline double defaultLineHeight(const VerticalFontMetrics& metrics, double stroke) {
    const double height = metrics.height + stroke;
    return metrics.pixiMeasured ? height : std::ceil(height);
}

// PIXI.TextMetrics returns lineHeight with style.leading already included, and Text::updateText
// centres the font metrics inside that complete advance. Keep the same split here so the extra
// leading is shared above and below the glyphs instead of accumulating below them.
inline double glyphTopOffset(double fontHeight, double baseLineHeight, double stroke,
                             double leading) {
    return stroke * 0.5 +
           std::max(0.0, (baseLineHeight + leading - fontHeight) * 0.5);
}

// drawText(..., baseline="top") adds CoreText/Android's own ascent internally. Shift its top
// coordinate by the ascent delta so the final alphabetic baseline is the one Pixi calculated.
inline double drawTopOffset(double coreAscent, const VerticalFontMetrics& metrics,
                            double baseLineHeight, double stroke, double leading) {
    return glyphTopOffset(metrics.height, baseLineHeight, stroke, leading) +
           metrics.ascent - coreAscent;
}

} // namespace plaintext
