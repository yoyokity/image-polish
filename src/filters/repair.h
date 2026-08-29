#pragma once

#include "common.h"

// Repair mode 2: clamp each pixel to [2nd smallest, 2nd largest] of the 3x3
// neighbourhood of the reference clip (vs-removegrain repairvs.cpp semantics).
void repair2(const u8 *a, const u8 *b, int w, int h, u8 *out);