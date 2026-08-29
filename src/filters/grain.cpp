#include "grain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Scalar port of HolyWu VapourSynth-AddGrain (`git clone ...AddGrain`, GPLv3;
// original by Tom Barry / Firesledge / LaTo INV., VapourSynth port by HolyWu):
//
//   fastUniformRandL:  idum = 1664525 * idum + 1013904223          (32-bit)
//   fastUniformRandF:  bitcast(0x3f800000 | (0x007fffff & idum)) - 1
//   gaussianRand:      Box-Muller polar, saves the second sample
//   noise value:       (int8) round(N(0, var)), hcorr/vcorr = 0 (defaults)
//   output:            clamp(src + noise, 0, 255)
//
// The plugin computes the noise field once and rolls a per-frame window into
// it (video-oriented); a still image only ever uses the same pixel grid, so
// the port draws the field directly at stride = width.

namespace {

// Reference fastUniformRandL as unsigned 32-bit (explicit wrap, portable).
inline uint32_t lcgNext(uint32_t &u)
{
    return u = 1664525u * u + 1013904223u;
}

// Reference fastUniformRandF: mantissa from the LCG, exponent 127 -> [0,1).
inline float u01(uint32_t &u)
{
    const uint32_t bits = 0x3f800000u | (0x007fffffu & lcgNext(u));
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f - 1.0f;
}

// Reference gaussianRand (mean 0; the variance scaling is applied by the
// caller, mirroring gaussianRand's `gaussian* sqrt(variance)`): polar
// Box-Muller; the second sample is kept for the next call (isan/gset state).
struct GaussianState {
    uint32_t u;
    bool iset = false;
    float gset = 0.0f;

    float next()
    {
        if (iset) {
            iset = false;
            return gset;
        }
        float v1, v2, rsq;
        do {
            v1 = 2.0f * u01(u) - 1.0f;
            v2 = 2.0f * u01(u) - 1.0f;
            rsq = v1 * v1 + v2 * v2;
        } while (rsq >= 1.0f || rsq == 0.0f);
        const float fac = std::sqrt(-2.0f * std::log(rsq) / rsq);
        gset = v1 * fac;
        iset = true;
        return v2 * fac;
    }
};

// FNV-1a over the luma, used as the deterministic per-image seed.
inline uint32_t fnv1a(const u8 *p, std::size_t n)
{
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

} // namespace

void grain(const u8 *src, int w, int h, float var, u8 *dst)
{
    if (var <= 0.0f) {
        if (dst != src)
            std::memcpy(dst, src, std::size_t(w) * h);
        return;
    }

    GaussianState g;
    g.u = fnv1a(src, std::size_t(w) * h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float r = g.next() * std::sqrt(var);         // N(0, var)
            int d = int(std::round(r));                        // (int8) in the reference
            d = std::max(-128, std::min(127, d));
            const int v = int(src[std::size_t(y) * w + x]) + d;
            dst[std::size_t(y) * w + x] = u8(std::max(0, std::min(255, v)));
        }
    }
}