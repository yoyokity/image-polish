#pragma once

#include "common.h"

// NLMeans denoise, single frame (spatial only), one 8-bit plane.
// Faithful port of vs-nlm-ispc (drop-in of KNLMeansCL), channels = "Y".
//
// Parameter semantics are those of the reference filter
//   NLMeans(clip, d, a, s, h, wmode, wref)
// with d fixed to 0: the reference's d is a *temporal* radius over frames,
// so a single-frame call uses only the current frame (no temporal
// neighbours). This tool always denoises with a = 2, s = 4, wmode = 3,
// wref = 1.0 and h from --denoise.
//
// Algorithm per patch offset (the loop covers the upper-left half of the
// (2a+1)^2 patch: the mirrored offsets are folded into the accumulation and
// the pixel itself enters through wref, exactly like the reference):
//   1. per-pixel squared difference vs the neighbour at (ox, oy):
//        dist = 3 * (src - src[+off])^2 / 255^2
//   2. window distance = horizontal then vertical box sum over |j| <= s
//      (the box filters are the reference's sliding implementation, which
//      folds the top edge over row 0)
//   3. weight from the window distance t:
//        wmode 0  welsch      exp(-t)
//        wmode 1  bisquare A  fdim(1, t)
//        wmode 2  bisquare B  fdim(1, t)^2
//        wmode 3  bisquare C  fdim(1, t)^8
//      with t = distance * 255^2 / (3 * h^2 * (2s+1)^2)
//   4. accumulate sum of weights, sum of weight * value, max weight
//   5. out = (wref * maxw * src + sum(w*v)) / (wref * maxw + sum(w)),
//      rounded to nearest and clamped to [0, 255]
//
// All float expressions reproduce the reference bit for bit (the module is
// compiled without FMA contraction); src/dst are w*h gray8 planes.

struct NlmeansParams {
    int   a     = 2;      // patch search radius
    int   s     = 4;      // distance window half-width
    float h     = 1.2f;   // filter strength
    int   wmode = 3;      // weight mode, 0..3 (see above)
    float wref  = 1.0f;   // weight of the pixel itself
};

void nlmeans(const u8 *src, int w, int h, const NlmeansParams &p, u8 *dst);