#include "cas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// CAS (FidelityFX contrast adaptive sharpening), faithful port of the
// filter_c() core of HolyWu/VapourSynth-CAS for one 8-bit plane.
void cas(const u8 *src, int w, int h, float sharpness, u8 *dst)
{
    const float sharpScale = -1.0f / (16.0f + (5.0f - 16.0f) * sharpness);   // -1/lerp(16,5,s)
    const int limit = 511;                                           // (1 << 9) - 1
    const int peak = 255;

    const auto sharpen = [&](int a, int b, int c, int d, int e, int f,
                             int g, int hh, int i) -> float {
        // soft min / max (2.0x, factored out)
        int mn = std::min({ d, e, f, b, hh });
        const int mn2 = std::min({ mn, a, c, g, i });
        mn += mn2;

        int mx = std::max({ d, e, f, b, hh });
        const int mx2 = std::max({ mx, a, c, g, i });
        mx += mx2;

        // smooth minimum distance to signal limit divided by smooth max
        float amp = std::clamp(std::min(mn, limit - mx) / static_cast<float>(mx), 0.0f, 1.0f);
        amp = std::sqrt(amp);

        // filter shape: cross taps on the center, weighted by amp * sharpness
        const float weight = amp * sharpScale;
        return ((b + d + f + hh) * weight + e) / (1.0f + 4.0f * weight);
    };

    for (int y = 0; y < h; y++) {
        const u8 *above = src + (y == 0 ? 0 : -w);
        const u8 *below = src + (y == h - 1 ? -w : w);

        {
            const float r = sharpen(above[1], above[0], above[1],
                                    src[1], src[0], src[1],
                                    below[1], below[0], below[1]);
            dst[0] = u8(std::clamp(int(r + 0.5f), 0, peak));
        }
        for (int x = 1; x < w - 1; x++) {
            const float r = sharpen(above[x - 1], above[x], above[x + 1],
                                    src[x - 1], src[x], src[x + 1],
                                    below[x - 1], below[x], below[x + 1]);
            dst[x] = u8(std::clamp(int(r + 0.5f), 0, peak));
        }
        {
            const float r = sharpen(above[w - 2], above[w - 1], above[w - 2],
                                    src[w - 2], src[w - 1], src[w - 2],
                                    below[w - 2], below[w - 1], below[w - 2]);
            dst[w - 1] = u8(std::clamp(int(r + 0.5f), 0, peak));
        }

        src += w;
        dst += w;
    }
}