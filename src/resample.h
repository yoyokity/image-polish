#pragma once

#include "common.h"

// vertical resize of a w*srcH image to w*dstH
void resampleV(const u8 *src, int w, int srcH, int dstH, double sy, u8 *dst);

void transpose(const u8 *src, int w, int h, u8 *dst);