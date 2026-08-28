#pragma once

#include "common.h"

// CAS (contrast adaptive sharpening), port of HolyWu/VapourSynth-CAS
// (FidelityFX CAS) applied to one 8-bit luma plane.
// sharpness is in [0, 1] (the CLI default is 0.7).
// Border handling and the per-pixel float32 chain follow the reference
// filter_c exactly: edge rows are duplicated, the left/right edges mirror
// column 1 / column w-2, and the result is rounded half-up into [0, 255].
void cas(const u8 *src, int w, int h, float sharpness, u8 *dst);