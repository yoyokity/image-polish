#pragma once

#include "common.h"

// neo_f3kdb deband, single frame, one 8-bit plane.
// Faithful port of HomeOfVapourSynthEvolution/VapourSynth-f3kdb (the algorithm
// is identical to HomeOfAviSynthPlusEvolution/neo_f3kdb, same core) at the
// parameter combination
//   Deband(clip, range, y, grainy=0, grainc=0, output_depth=16)
// i.e. sample_mode=2, blur_first=true, random_algo_ref=UNIFORM, no grain and
// no dither. The pipeline runs in the reference's internal 16-bit domain
// (8-bit samples are upsampled by << 8) and the plane is recovered by >> 8;
// for an 8-bit source that is exactly what output_depth=16 plus a raw shift
// back produces.
//
//   luma threshold  = y << 2   (the reference scales user thresholds by 4 when
//                              scale=false, matching its 14-bit-era ranges)
//   sample positions: (x+ref1,y+ref2), (x-ref1,y-ref2), (x+ref2,y-ref1),
//                     (x-ref2,y+ref1), the refs being per-pixel random
//                     distances in [0, cur_range], cur_range clamped to the
//                     plane borders like the reference LUT
//   avg             = avg_4() of the four 16-bit samples, exactly as the
//                     reference's SSE routine: (r1+r2+1)>>1 saturating-minus-1
//                     then averaged with (r3+r4+1)>>1 (only the first pair is
//                     reduced by 1; the plugin ships the SSE path on SSE4 CPUs)
//   out16           = |avg - px16| >= threshold ? px16 : avg
//   out             = clamp(out16, 0, 65535) >> 8
//
// The per-pixel random offsets reproduce the reference RNG sequence
// (LCG 1664525*x+1013904223, 32-bit wrap, uniform) bit for bit; the grain
// draws (grainy=grainc=0 -> value 0) still advance the same seed, and the
// input is taken as 4:4:4/gray (every pixel also draws the two chroma grain
// values, matching init_frame_luts for SSW=SSH=0).
//
// Parameter semantics follow the reference filter Deband(range, y, cb, cr, ...).
// This tool processes the luma plane only, like every other step (main.cpp
// keeps the chroma of the original image exact), so only `range` and `y`
// affect the output; `cbcr` is accepted and range-checked for CLI
// compatibility but unused here.
struct DebandParams {
    int range = 24;   // banding detection radius, 0..255 (f3kdb default is 15)
    int y     = 72;   // luma threshold, 0..511 (16-bit domain: y << 2)
    int cbcr  = 32;   // chroma threshold, 0..511 (unused: luma-only pipeline)
};

void deband(const u8 *src, int w, int h, const DebandParams &p, u8 *dst);