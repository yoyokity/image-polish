#include "deband.h"

#include "pfor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Reference RNG (neo_f3kdb src/random.cpp, uniform algorithm). random_algo_ref
// defaults to RANDOM_ALGORITHM_UNIFORM:
//   seed = 1664525 * seed + 1013904223          (full 32-bit wrap)
//   rand_to_double: reinterpret the seed as a double mantissa pattern scaled
//   into [1, 2), then map to [-1, 1). The union bit trick reproduces the
//   reference's bit pattern exactly.
// round(r) = r > 0 ? floor(r + 0.5) : ceil(r - 0.5), as in the reference.
// ---------------------------------------------------------------------------

static double randToDouble(uint32_t seed)
{
    union { uint64_t i; double d; } u;
    u.i = uint64_t(seed) & 0xffffffffULL;
    u.i = (u.i << 20) | (u.i >> 12);
    u.i |= 0x3ff0000000000000ULL;
    return (u.d - 1.0) * 2.0 - 1.0;
}

// random(algo, seed, range): one uniform draw, result in [-range, range].
// The seed advances even when the value is unused (grain draws are 0 but must
// keep the sequence in lockstep for the ref draws that follow).
static int randomUniform(int32_t &seed, int range)
{
    seed = int32_t(1664525 * int64_t(seed) + 1013904223);
    const double num = randToDouble(uint32_t(seed));       // [-1, 1)
    const double r = num * double(range);
    return r > 0.0 ? int(std::floor(r + 0.5)) : int(std::ceil(r - 0.5));
}

// avg_4 from the reference's SSE path (r7 flash3kyuu_deband_sse_base.h,
// process_pixels_mode12_high_part): _mm_avg_epu16(ref1,ref2), saturating
// subtract 1, then average with _mm_avg_epu16(ref3,ref4). Note only the FIRST
// pair is reduced by 1 -- the plain C core reduces both, but the shipped
// plugin runs the SSE/AVX routine on SSE4 CPUs, so this is the behavior that
// matches the reference output (verified pixel-identical).
static inline int avg4(int r1, int r2, int r3, int r4)
{
    int a1 = (r1 + r2 + 1) >> 1;
    if (a1 > 0)
        a1 -= 1;
    int a2 = (r3 + r4 + 1) >> 1;
    return (a1 + a2 + 1) >> 1;
}

void deband(const u8 *src, int w, int h, const DebandParams &p, u8 *dst)
{
    // LUT seed, exactly init_frame_luts() for a single-frame input
    // (seed = 0x92D68CA2 - seed_param; seed ^= (w<<16)^h; seed ^= (fr<<16)^fr):
    int32_t seed = 0x92D68CA2;
    seed ^= (w << 16) ^ h;
    seed ^= (1 << 16) ^ 1;

    const int range = p.range;
    const int threshold = p.y << 2;          // reference scale=false: y * 4

    // Per-pixel random sample distances, mirroring the reference LUT: for
    // every pixel the seed advances once for the luma grain draw, and for
    // cur_range > 0 two ref draws follow (sample_mode == 2 reads ref1+ref2).
    // As 4:4:4/gray (SSW=SSH=0) every pixel then also draws the two chroma
    // grain values, which keeps the sequence in step with the reference.
    std::vector<int8_t> ref1(std::size_t(w) * h), ref2(std::size_t(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int yr = std::min({ range, y, h - y - 1 });
            const int xr = std::min({ range, x, w - x - 1 });
            const int cur = std::min(xr, yr);

            randomUniform(seed, 0);          // info_y.change (grainy=0 -> 0)
            int r1 = 0, r2 = 0;
            if (cur > 0) {
                r1 = std::abs(randomUniform(seed, cur));
                r2 = std::abs(randomUniform(seed, cur));
            }
            ref1[std::size_t(y) * w + x] = int8_t(r1);
            ref2[std::size_t(y) * w + x] = int8_t(r2);

            randomUniform(seed, 0);          // info_cb.change (444: every pixel)
            randomUniform(seed, 0);          // info_cr.change
        }
    }

    // Process rows in parallel; each row reads only its own LUT entries and
    // source rows, so the result is identical to a serial loop.
    parallelFor(0, h, [&](int y) {
        const u8 *row = src + std::size_t(y) * w;
        const int8_t *r1row = ref1.data() + std::size_t(y) * w;
        const int8_t *r2row = ref2.data() + std::size_t(y) * w;
        for (int x = 0; x < w; x++) {
            const int d1 = r1row[x], d2 = r2row[x];
            const int p16 = row[x] << 8;
            const int s1 = src[std::size_t(y + d2) * w + (x + d1)] << 8;
            const int s2 = src[std::size_t(y - d2) * w + (x - d1)] << 8;
            const int s3 = src[std::size_t(y - d1) * w + (x + d2)] << 8;
            const int s4 = src[std::size_t(y + d1) * w + (x - d2)] << 8;
            const int avg = avg4(s1, s2, s3, s4);
            const int val = std::abs(avg - p16) >= threshold ? p16 : avg;
            dst[std::size_t(y) * w + x] = u8(std::clamp(val, 0, 65535) >> 8);
        }
    });
}