#pragma once

#include "common.h"

// SangNom directional anti-aliasing on a gray8 plane (w*h -> w*h).
//
// Port of the VapourSynth sangnom plugin behaviour used by the level-2 AA
// chain: dh=false, order=1 (top field kept), so each output pixel on an
// interpolated line blends the two source lines around it along the local
// direction of minimum difference; `aa` (0..255) is the blend threshold.
// The source height must be even (the reference plugin rejects odd heights
// in this mode).
void sangnom(const u8 *src, int w, int h, int aa, u8 *out);