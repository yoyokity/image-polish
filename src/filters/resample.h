#pragma once

#include "common.h"

// vertical resize of a w*srcH image to w*dstH
void resampleV(const u8 *src, int w, int srcH, int dstH, double sy, u8 *dst);

void transpose(const u8 *src, int w, int h, u8 *dst);

// resize a w*h image to nw*nh with the spline36 kernel, pixel centres
// aligned (shift = 0); implemented as two separable passes
void resample2D(const u8 *src, int w, int h, int nw, int nh, u8 *dst);

// same, with independent horizontal/vertical half-pixel shifts (fmtconv
// sx/sy semantics, in input pixels); resample2D is resample2DShift(...,0,0)
void resample2DShift(const u8 *src, int w, int h, int nw, int nh,
                     double sx, double sy, u8 *dst);