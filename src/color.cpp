#include "color.h"

#include <algorithm>
#include <cmath>

static inline u8 clamp8(int v) { return u8(std::max(0, std::min(255, v))); }

// BT.601 full-range luma of an RGB image (8-bit).
void rgbLuma(const u8 *rgb, int n, std::vector<u8> &y)
{
    y.resize(n);
    for (int i = 0; i < n; i++) {
        const double R = rgb[3 * i + 0], G = rgb[3 * i + 1], B = rgb[3 * i + 2];
        y[i] = clamp8(int(0.299 * R + 0.587 * G + 0.114 * B + 0.5));
    }
}

// Reconstruct RGB after the luma-only AA pass: keep the original chroma by
// adding the luma change (yAA - yOrig) to every channel of the original pixel.
// This is exactly "replace luma, keep chroma" with unquantized chroma, i.e.
// the same semantics as the VapourSynth ShufflePlanes([aa, src], YUV) chain;
// flat regions roundtrip with zero error (only the rare yOrig == x.5 case
// shifts all channels by +1).
void applyLuma(const std::vector<u8> &yAA, const u8 *origRgb, int n, std::vector<u8> &rgb)
{
    rgb.resize(std::size_t(n) * 3);
    for (int i = 0; i < n; i++) {
        const double R = origRgb[3 * i + 0], G = origRgb[3 * i + 1], B = origRgb[3 * i + 2];
        const double d = double(yAA[i]) - (0.299 * R + 0.587 * G + 0.114 * B);
        rgb[3 * i + 0] = clamp8(int(R + d + 0.5));
        rgb[3 * i + 1] = clamp8(int(G + d + 0.5));
        rgb[3 * i + 2] = clamp8(int(B + d + 0.5));
    }
}