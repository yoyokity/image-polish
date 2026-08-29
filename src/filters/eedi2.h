#pragma once

#include "common.h"

// ---------------------------------------------------------------------------
// EEDI2 parameters and setup (defaults identical to the original script)
// ---------------------------------------------------------------------------
struct Eedi2Params {
    int field    = 1;      // 1: original lines land on even rows, new lines on odd
    int mthresh  = 10;
    int lthresh  = 20;
    int vthresh  = 20;
    int estr     = 2;
    int dstr     = 4;
    int maxd     = 24;
    int nt       = 50;
    int pp       = 1;

    int mthresh2 = 0;      // mthresh^2
    int vthresh9 = 0;      // vthresh * 81
    unsigned nt4 = 0, nt7 = 0, nt8 = 0, nt13 = 0, nt19 = 0;
    int8_t  limlut[33];
    int16_t limlut2[33];
};

void eedi2_prepare(Eedi2Params &p);

// ---------------------------------------------------------------------------
// EEDI2 core: doubles the height with edge-directed interpolation (gray8)
// ---------------------------------------------------------------------------
class Eedi2
{
public:
    Eedi2(const Eedi2Params &p);

    // in: w*h gray8 -> out: w*2h gray8.
    // Original lines land on rows with parity (1-field), interpolated lines
    // on rows with parity field.
    void run(const u8 *src, int w, int h, u8 *out);

private:
    static constexpr u8 NEUTRAL = 128;
    static constexpr u8 PEAK = 255;
    Eedi2Params d;

    // ---- 1. edge mask: mark pixels with enough local structure ------------
    void buildEdgeMask(const u8 *src, int w, int h, u8 *msk);
    static void erode(const u8 *msk, int w, int h, u8 *dst);
    static void dilate(const u8 *msk, int w, int h, u8 *dst);
    static void removeSmallHorzGaps(const u8 *msk, int w, int h, u8 *dst);

    // ---- 2. direction map at original resolution --------------------------
    void calcDirections(const u8 *src, const u8 *msk, int w, int h, u8 *dst);

    // ---- 3. direction map cleaning (original resolution) -------------------
    void filterDirMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst);
    void expandDirMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst);
    void filterMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst);

    // ---- 4. 2x stage -------------------------------------------------------
    void upscaleBy2(const u8 *src, int w, int h, u8 *dst);
    void markDirections2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst);
    void filterDirMap2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst);
    void expandDirMap2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst);
    void fillGaps2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst);

    // The actual edge-directed interpolation: fills the odd rows of dst.
    void interpolateLattice(const u8 *omsk, u8 *dmsk, int w, int H2, u8 *dst);
    void postProcess(const u8 *nmsk, const u8 *omsk, int w, int H2, u8 *dst);

    // ---- helpers ----------------------------------------------------------
    int maxLut2(int dirMapValue) const;
};