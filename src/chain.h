#pragma once

#include "common.h"

#include <string>
#include <vector>

struct Eedi2Params;

// Processing steps, applied to the luma plane in command-line order. The
// static factories keep the CLI parsing independent of the field layout.
enum class StepKind { AA, Denoise, Resize, Dehalo, Sharpen, Deband, Grain };

struct Step {
    StepKind kind;
    float h = 0.0f;       // NLMeans strength (Denoise) or grain variance (Grain)
    int rw = -1, rh = -1; // Resize targets (-1: keep proportional)
    int dbr = 24, dby = 72, dbc = 32; // Deband range / luma / chroma thresholds
    int aaLevel = 2;      // 2: EEDI2 + SangNom strong AA (default); 1: EEDI2 chain

    static Step aa(int level = 2) { return {StepKind::AA, 0.0f, -1, -1, 24, 72, 32, level}; }
    static Step denoise(float h) { return {StepKind::Denoise, h}; }
    static Step sharpen(float s) { return {StepKind::Sharpen, s}; }
    static Step resize(int w, int h) { return {StepKind::Resize, 0.0f, w, h}; }
    static Step dehalo() { return {StepKind::Dehalo}; }
    static Step deband(int range, int y, int cbcr) { return {StepKind::Deband, 0.0f, -1, -1, range, y, cbcr}; }
    static Step grain(float h) { return {StepKind::Grain, h}; }
};

// Parse "--resize WxH | Wx | xH"; a missing side (-1) is kept proportional.
void parseResize(const char *s, int &rw, int &rh);

// Parse "--deband [<name>=<value>,...]" with names range (0..255), y and
// cbcr (0..511). Omitted names keep their defaults (24, 72, 32); any order is
// allowed. Unknown names, duplicates, missing '=' or empty values are errors.
void parseDeband(const char *s, int &range, int &y, int &cbcr);

// Runs `steps` on the plane. In batch mode (accMs != nullptr) the per-step
// wall time is accumulated into stepMs/stepNames instead of printed per file;
// the totals are reported once by main() after all inputs.
void runSteps(const Eedi2Params &p, std::vector<u8> &plane, int &w, int &h,
              const std::vector<Step> &steps, std::vector<u8> *refRgb,
              std::vector<double> *accMs = nullptr,
              std::vector<std::string> *accNames = nullptr);