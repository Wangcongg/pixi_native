#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace richtext {

// A FairyGUI text box controls the retained surface size and alignment; it does not implicitly
// enable TaggedText word wrapping. Only the final default style's positive wordWrapWidth may
// constrain CoreText line breaking. This distinction matters when customData.default replaced the
// authored XML style and deliberately removed wordWrap while business code inserted line breaks.
inline double coreTextLineBreakWidth(double wordWrapWidth) {
    return std::isfinite(wordWrapWidth) && wordWrapWidth > 0
        ? wordWrapWidth
        : std::numeric_limits<double>::max();
}

struct InlineAttachmentMetrics {
    double ascent = 0;
    double descent = 0;
    double placementAscent = 0;
};

// TaggedText aligns every token in a line against the same font baseline. CoreText attachments,
// however, default to putting their entire height above that baseline. Keep the layout extents
// non-negative while retaining a separate placement value: small top/bottom-aligned images may
// legitimately render wholly above or below the baseline without enlarging the opposite extent.
inline InlineAttachmentMetrics inlineAttachmentMetrics(double imageHeight, double fontAscent,
                                                        double fontDescent,
                                                        std::string_view verticalAlign) {
    const double height = std::isfinite(imageHeight) ? std::max(0.0, imageHeight) : 0.0;
    const double ascent = std::isfinite(fontAscent) ? std::max(0.0, fontAscent) : 0.0;
    const double descent = std::isfinite(fontDescent) ? std::max(0.0, fontDescent) : 0.0;

    double placementAscent = height;
    if (verticalAlign == "middle")
        placementAscent = (height + ascent - descent) * 0.5;
    else if (verticalAlign == "top")
        placementAscent = ascent;
    else if (verticalAlign == "bottom")
        placementAscent = height - descent;

    InlineAttachmentMetrics out;
    out.placementAscent = placementAscent;
    out.ascent = std::max(0.0, placementAscent);
    out.descent = std::max(0.0, height - placementAscent);
    return out;
}

} // namespace richtext
