#include "nlmeans.h"

#include "pfor.h"

#if defined(__clang__)
#pragma clang fp contract(off)   // keep the port bit-identical to the reference
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

inline int clampX(int x, int w) { return std::max(0, std::min(w - 1, x)); }
inline int clampY(int y, int h) { return std::max(0, std::min(h - 1, y)); }

// positive difference: x > y ? x - y : 0
inline float fdim(float x, float y) { return (x > y) ? x - y : 0.0f; }

// weight modes of the reference: 0 welsch, 1..3 bisquare A/B/C
inline float weight(int wmode, float t)
{
    if (wmode == 0)
        return std::exp(-t);                 // welsch
    float v = fdim(1.0f, t);                 // bisquare A
    if (wmode == 1)
        return v;
    v *= v;                                  // bisquare B
    if (wmode == 2)
        return v;
    v *= v;                                  // bisquare C
    v *= v;
    return v;
}

} // namespace

// NLMeans with vs-nlm-ispc semantics, d = 0 (single frame), channels = "Y".
// Threading: the four per-pixel stages are split by rows; the vertical box is
// the reference's rolling window, kept column-major per column (the operation
// sequence inside a column is identical to the serial version, so the output
// stays bit-identical) and parallelized over columns.
void nlmeans(const u8 *src, int w, int h, const NlmeansParams &p, u8 *dst)
{
    const int a = p.a;
    const int s = p.s;
    const int stride = w;
    const int size = w * h;

    // grouping matches vsnlm.cpp exactly: 255^2 / (3 * h^2 * (2s+1)^2)
    const float h2_inv_norm = (255.0f * 255.0f) /
                              (3.0f * (p.h * p.h) * ((2 * s + 1) * (2 * s + 1)));
    const float sq_inv_divisor = (1.0f / 255.0f) * (1.0f / 255.0f);

    std::vector<float> srcf(size);
    for (int i = 0; i < size; i++)
        srcf[i] = float(src[i]);

    std::vector<float> weightp(size, 0.0f);                                  // sum of weights
    std::vector<float> wdstp(size, 0.0f);                                    // sum of weight * value
    std::vector<float> max_weightp(size, std::numeric_limits<float>::epsilon());
    std::vector<float> temp(size);                                           // horizontal box result
    std::vector<float> temp_bwd(size);                                       // distances, later weights

    // iterate the upper-left half of the patch (mirror offsets are folded into
    // the accumulation, the center is handled by wref in the finish step)
    for (int oy = -a; oy <= a; oy++) {
        for (int ox = -a; ox <= a; ox++) {
            if (oy * (2 * a + 1) + ox >= 0)
                continue;

            // 1. per-pixel squared difference vs the neighbour at (ox, oy)
            parallelFor(0, h, [&](int y) {
                const int ny = clampY(y + oy, h);
                for (int x = 0; x < w; x++) {
                    const float u1 = srcf[y * stride + x];
                    const float u1_pq = srcf[ny * stride + clampX(x + ox, w)];
                    const float d = u1 - u1_pq;
                    temp_bwd[y * stride + x] = 3.0f * (d * d) * sq_inv_divisor;
                }
            });

            // 2. horizontal box sum over j in [-s, s]
            parallelFor(0, h, [&](int y) {
                for (int x = 0; x < w; x++) {
                    float sum = 0.0f;
                    for (int j = -s; j <= s; j++)
                        sum += temp_bwd[y * stride + clampX(x + j, w)];
                    temp[y * stride + x] = sum;
                }
            });

            // 3. vertical box sum over the same radius, then map to a weight.
            // The sliding window folds the top edge over row 0 exactly like
            // the reference; reordered column-major so the per-column rolling
            // sequence is unchanged and columns run in parallel.
            parallelFor(0, w, [&](int x) {
                float b = s * temp[x];
                for (int y = 0; y < s; y++)
                    b += temp[clampY(y, h) * stride + x];
                for (int y = 0; y < std::min(s, h); y++) {
                    b += temp[clampY(y + s, h) * stride + x];
                    temp_bwd[y * stride + x] = weight(p.wmode, b * h2_inv_norm);
                    b -= temp[x];                       // NB: row 0, as in the reference
                }
                if (h > s) {
                    for (int y = s; y < h - s; y++) {
                        b += temp[(y + s) * stride + x];
                        temp_bwd[y * stride + x] = weight(p.wmode, b * h2_inv_norm);
                        b -= temp[(y - s) * stride + x];
                    }
                    for (int y = std::max(h - s, s); y < h; y++) {
                        b += temp[clampY(y + s, h) * stride + x];
                        temp_bwd[y * stride + x] = weight(p.wmode, b * h2_inv_norm);
                        b -= temp[(y - s) * stride + x];
                    }
                }
            });

            // 4. accumulate the offset and its mirror
            parallelFor(0, h, [&](int y) {
                const int ry1 = clampY(y + oy, h);
                const int ry0 = clampY(y - oy, h);
                for (int x = 0; x < w; x++) {
                    const int idx = y * stride + x;
                    const float u4 = temp_bwd[idx];
                    const float u4_mq = temp_bwd[ry0 * stride + clampX(x - ox, w)];
                    weightp[idx] += u4 + u4_mq;
                    if (u4 > max_weightp[idx]) max_weightp[idx] = u4;
                    if (u4_mq > max_weightp[idx]) max_weightp[idx] = u4_mq;
                    const float u1_pq = srcf[ry1 * stride + clampX(x + ox, w)];
                    const float u1_mq = srcf[ry0 * stride + clampX(x - ox, w)];
                    wdstp[idx] += u4 * u1_pq + u4_mq * u1_mq;
                }
            });
        }
    }

    // 5. finish: blend with the reference (current pixel) weight
    parallelFor(0, h, [&](int y) {
        for (int x = 0; x < w; x++) {
            const int idx = y * stride + x;
            const float multiplier = p.wref * max_weightp[idx];
            const float denominator = multiplier + weightp[idx];
            const int v = int(std::round((multiplier * srcf[idx] + wdstp[idx]) / denominator));
            dst[idx] = u8(std::max(0, std::min(255, v)));
        }
    });
}