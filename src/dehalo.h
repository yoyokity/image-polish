#pragma once

#include "common.h"

// FineDehalo (havsfunc's haf.FineDehalo), single frame, one 8-bit luma plane.
// Faithful port of the Python reference with all parameters at their
// havsfunc defaults (not user-configurable):
//   DeHalo_alpha(rx=2.0, ry=2.0, darkstr=1.0, brightstr=1.0,
//                lowsens=50, highsens=50, ss=1.5)
//   FineDehalo(rx=2.0, ry=2.0, thmi=80, thma=128, thlimi=50, thlima=100,
//              darkstr=1.0, brightstr=1.0, excl=True, contra=0.0, edgeproc=0.0)
//
// Pipeline (luma only):
//   dehaloed = DeHalo_alpha(src)
//   edges   = Prewitt(src)                      (4 convolutions, elementwise max)
//   strong  = (edges - thmi) / (thma - thmi) * 255,  expanded (rectangle, rx, ry)
//   light   = (edges - thlimi) / (thlima - thlimi) * 255
//   shrink  = expand(light, ellipse), *4, inpand(ellipse), 2x 3x3 box blur
//   mask    = (large - max(strong, shrink)) * 2, box blur, *2
//   dst     = MaskedMerge(src, dehaloed, mask)
// Integer Expr steps follow the reference's float execution: every binary
// operation evaluates in float and rounds to nearest-even (rint) per step;
// results clamp to [0, 255].
void fineDehalo(const u8 *src, int w, int h, u8 *dst);