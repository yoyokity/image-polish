#include "repair.h"

#include "pfor.h"

#include <algorithm>
#include <cstring>

// Repair mode 2: clamp each pixel to [2nd smallest, 2nd largest] of the 3x3
// neighbourhood of the reference clip (vs-removegrain repairvs.cpp semantics).
// Row-parallel; the 3x3 extremes are computed with a two-pass selection
// instead of a full sort (same values, cheaper).
void repair2(const u8 *a, const u8 *b, int w, int h, u8 *out)
{
    if (h < 3 || w < 3) {
        std::memcpy(out, a, sizeof(u8) * w * h);
        return;
    }
    std::memcpy(out, a, sizeof(u8) * w);                                  // row 0
    std::memcpy(out + (h - 1) * w, a + (h - 1) * w, sizeof(u8) * w);      // row h-1

    parallelFor(1, h - 1, [&](int y) {
        const u8 *ap = a + y * w;
        const u8 *bpp = b + (y - 1) * w;
        const u8 *bpc = b + y * w;
        const u8 *bpn = b + (y + 1) * w;
        u8 *op = out + y * w;

        op[0] = ap[0];
        op[w - 1] = ap[w - 1];

        for (int x = 1; x < w - 1; x++) {
            int n[9] = {
                int(bpp[x - 1]), int(bpp[x]), int(bpp[x + 1]),
                int(bpc[x - 1]), int(bpc[x]), int(bpc[x + 1]),
                int(bpn[x - 1]), int(bpn[x]), int(bpn[x + 1])
            };
            int lo1 = 256, lo2 = 256, hi1 = -1, hi2 = -1;
            for (int k = 0; k < 9; k++) {
                const int v = n[k];
                if (v < lo1) { lo2 = lo1; lo1 = v; }
                else if (v < lo2) { lo2 = v; }
                if (v > hi1) { hi2 = hi1; hi1 = v; }
                else if (v > hi2) { hi2 = v; }
            }
            op[x] = u8(std::max(lo2, std::min(int(ap[x]), hi2)));
        }
    });
}