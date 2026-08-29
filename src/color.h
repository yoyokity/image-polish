#pragma once

#include "common.h"

#include <cstddef>
#include <vector>

// Color helpers for the "luma-only" pipeline. RGB input is reduced to BT.601
// full-range luma for the filter steps and the processed luma is written back
// by adding the luma delta to every channel of the original pixel: exactly
// "replace luma, keep chroma" with unquantized chroma (the VS chain
// ShufflePlanes(clip,0,GRAY) ... ShufflePlanes([aa, clip], [0,1,2], YUV)).

// BT.601 full-range luma of an RGB image (8-bit).
void rgbLuma(const u8 *rgb, int n, std::vector<u8> &y);

// Reconstruct RGB after a luma-only pass: add the luma change
// (yAA - yOrig) to every channel of the original pixel.
void applyLuma(const std::vector<u8> &yAA, const u8 *origRgb, int n, std::vector<u8> &rgb);