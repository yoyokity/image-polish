#pragma once

#include "common.h"

// Film-grain noise on a gray8 plane (w*h -> w*h).
//
// Port of HolyWu's VapourSynth-AddGrain (`core.grain.Add`, id
// com.holywu.addgrain): every output pixel gets an independent sample from
// N(0, var) (the reference `var` noise variance, gaussianRand mean=0), the
// drawing rounded to an int8 delta and added to the source with clamp to
// [0,255]. The reference's hcorr/vcorr default to 0 (no horizontal/vertical
// correlation), so the per-pixel noise is an uncorrelated draw; `constant`
// is false (single still frame). var <= 0 passes the input through.
//
// The RNG is the reference's 32-bit LCG + Box-Muller (fastUniformRand);
// unlike the plugin (which seeds from the wall clock when seed < 0) the seed
// is derived from the luma content, so the same image always gets the same
// grain and output stays reproducible.
void grain(const u8 *src, int w, int h, float var, u8 *dst);