/*
 * aa - EEDI2-based anti-aliasing, standalone C++ command line tool.
 *
 * Implements the well-known VapourSynth anti-aliasing chain:
 *
 *     aa = ShufflePlanes(clip,0,GRAY)
 *     aa = EEDI2(aa, field=1, mthresh=10, lthresh=20, vthresh=20, maxd=24, nt=50)
 *     aa = resample(aa, w, h, 0, -0.5).Transpose()
 *     aa = EEDI2(aa, field=1, mthresh=10, lthresh=20, vthresh=20, maxd=24, nt=50)
 *     aa = resample(aa, h, w, 0, -0.5).Transpose()
 *     aa  = ShufflePlanes([aa, clip], [0,1,2], YUV)   // luma replaced, chroma kept
 *     out = Repair(aa, clip, 2)
 *
 * Behavioral references (specification only; this is an original port):
 *   - EEDI2:   tritical / HolyWu, VapourSynth-EEDI2, EEDI2.cpp v0.9.2 (GPLv2)
 *   - Repair:  Laurent de Soras / Fredrik Mellbin, vs-removegrain repairvs.cpp
 *   - resample: Laurent de Soras, fmtconv (Scaler shift semantics + ContFirSpline36)
 * Image I/O via stb_image / stb_image_write (public domain).
 *
 * Usage:
 *   aa.exe -i input.png -o output.png [options]
 *   options: --mthresh N --lthresh N --vthresh N --maxd N --nt N --field N
 *            --repair N (0 disables Repair), -h / --help
 *
 * Build:
 *   g++ -O2 -std=c++17 -o out/aa.exe src/main.cpp src/eedi2.cpp \
 *       src/resample.cpp src/repair.cpp src/imageio.cpp
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common.h"
#include "eedi2.h"
#include "imageio.h"
#include "repair.h"
#include "resample.h"

// ---------------------------------------------------------------------------
// Color helpers (BT.601 full-range YUV444 as the "planes" of an image)
// ---------------------------------------------------------------------------
static inline u8 clamp8(int v) { return u8(std::max(0, std::min(255, v))); }

static void rgbToYuv(const u8 *rgb, int n, std::vector<u8> &y, std::vector<u8> &u, std::vector<u8> &v)
{
    y.resize(n); u.resize(n); v.resize(n);
    for (int i = 0; i < n; i++) {
        const double R = rgb[3 * i + 0], G = rgb[3 * i + 1], B = rgb[3 * i + 2];
        const double Y = 0.299 * R + 0.587 * G + 0.114 * B;
        // full-range BT.601 Cb/Cr, matched with the inverse below
        y[i] = clamp8(int(Y + 0.5));
        u[i] = clamp8(int((B - Y) / 1.772 + 128.0 + 0.5));
        v[i] = clamp8(int((R - Y) / 1.402 + 128.0 + 0.5));
    }
}

static void yuvToRgb(const std::vector<u8> &y, const std::vector<u8> &u, const std::vector<u8> &v, std::vector<u8> &rgb)
{
    const int n = int(y.size());
    rgb.resize(std::size_t(n) * 3);
    for (int i = 0; i < n; i++) {
        const double Y = y[i], U = int(u[i]) - 128, V = int(v[i]) - 128;
        int R = int(Y + 1.402 * V + 0.5);
        int G = int(Y - 0.344136 * U - 0.714136 * V + 0.5);
        int B = int(Y + 1.772 * U + 0.5);
        rgb[3 * i + 0] = clamp8(R);
        rgb[3 * i + 1] = clamp8(G);
        rgb[3 * i + 2] = clamp8(B);
    }
}

// ---------------------------------------------------------------------------
// The AA chain on a single luma plane (w*h gray8 -> w*h gray8)
// ---------------------------------------------------------------------------
static void aaChain(const u8 *gray, int w, int h, const Eedi2Params &p, std::vector<u8> &out)
{
    Eedi2 eedi2(p);

    // pass 1: vertical
    std::vector<u8> a1(std::size_t(w) * h * 2);
    eedi2.run(gray, w, h, a1.data());                       // w x 2h
    std::vector<u8> r1(std::size_t(w) * h);
    resampleV(a1.data(), w, h * 2, h, -0.5, r1.data());     // w x h

    // pass 2: horizontal (transpose -> double -> resample -> transpose back)
    std::vector<u8> t1(std::size_t(h) * w);
    transpose(r1.data(), w, h, t1.data());                  // h x w
    std::vector<u8> a2(std::size_t(h) * w * 2);
    eedi2.run(t1.data(), h, w, a2.data());                  // h x 2w
    std::vector<u8> r2(std::size_t(h) * w);
    resampleV(a2.data(), h, w * 2, w, -0.5, r2.data());     // h x w
    out.resize(std::size_t(w) * h);
    transpose(r2.data(), h, w, out.data());                 // w x h
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
static void printHelp()
{
    std::printf(
        "aa.exe - EEDI2 anti-aliasing (port of the VapourSynth eedi2+fmtc+repair chain)\n"
        "\n"
        "Usage: aa.exe -i <input> -o <output> [options]\n"
        "\n"
        "  -i, --input  <file>   input image (PNG/BMP/PNM/TGA, gray or RGB)\n"
        "  -o, --output <file>   output image (format from extension: PNG/BMP/TGA/JPG)\n"
        "\n"
        "Options (defaults match the original script):\n"
        "  --mthresh N    motion threshold         (10)\n"
        "  --lthresh N    linear interpolation th. (20)\n"
        "  --vthresh N    variance threshold       (20)\n"
        "  --maxd N       max edge search distance (24)\n"
        "  --nt N         noise threshold          (50)\n"
        "  --field N      field parity, 0 or 1     (1)\n"
        "  --repair N     Repair mode: 2 (default), 0 disables repair\n"
        "  -h, --help     show this help\n"
        "\n"
        "Examples:\n"
        "  aa.exe -i clip.png -o clip_aa.png\n"
        "  aa.exe -i in.bmp -o out.bmp --maxd 32 --nt 30\n");
}

static int parseInt(const char *s)
{
    return std::atoi(s);
}

int main(int argc, char **argv)
{
    std::string input, output;
    std::string dumpEedi2, dumpRs;
    Eedi2Params p;
    int repairMode = 2;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-i" || a == "--input") input = next("-i");
        else if (a == "-o" || a == "--output") output = next("-o");
        else if (a == "--mthresh") p.mthresh = parseInt(next("--mthresh"));
        else if (a == "--lthresh") p.lthresh = parseInt(next("--lthresh"));
        else if (a == "--vthresh") p.vthresh = parseInt(next("--vthresh"));
        else if (a == "--maxd") p.maxd = parseInt(next("--maxd"));
        else if (a == "--nt") p.nt = parseInt(next("--nt"));
        else if (a == "--field") p.field = parseInt(next("--field"));
        else if (a == "--repair") repairMode = parseInt(next("--repair"));
        else if (a == "--dump-eedi2") dumpEedi2 = next("--dump-eedi2");
        else if (a == "--dump-rs") dumpRs = next("--dump-rs");
        else if (a == "-h" || a == "--help") { printHelp(); return 0; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            printHelp();
            return 1;
        }
    }

    if (input.empty() || output.empty()) {
        std::fprintf(stderr, "error: -i and -o are required\n");
        printHelp();
        return 1;
    }

    if (p.maxd < 1 || p.maxd > 29) {
        std::fprintf(stderr, "error: maxd must be in 1..29\n");
        return 1;
    }
    if (p.field != 0 && p.field != 1) {
        std::fprintf(stderr, "error: field must be 0 or 1\n");
        return 1;
    }

    Image img;
    if (!loadImage(input, img)) {
        std::fprintf(stderr, "error: cannot read image '%s'\n", input.c_str());
        return 1;
    }
    if (img.w < 8 || img.h < 8) {
        std::fprintf(stderr, "error: image too small (need w>=8, h>=8)\n");
        return 1;
    }

    eedi2_prepare(p);

    if (!dumpEedi2.empty() && !dumpRs.empty()) {
        // debug: dump the first EEDI2 pass (w x 2h raw gray8) and its
        // resampleV result (w x h raw gray8) for comparison with eedi2/fmtc
        std::vector<u8> y, u, v;
        if (img.ch == 3)
            rgbToYuv(img.p.data(), img.w * img.h, y, u, v);
        else
            y = img.p;
        Eedi2 eedi2(p);
        std::vector<u8> h2(std::size_t(img.w) * img.h * 2);
        eedi2.run(y.data(), img.w, img.h, h2.data());
        FILE *f = std::fopen(dumpEedi2.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpEedi2.c_str()); return 1; }
        std::fwrite(h2.data(), 1, h2.size(), f);
        std::fclose(f);
        std::vector<u8> r1(std::size_t(img.w) * img.h);
        resampleV(h2.data(), img.w, img.h * 2, img.h, -0.5, r1.data());
        f = std::fopen(dumpRs.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpRs.c_str()); return 1; }
        std::fwrite(r1.data(), 1, r1.size(), f);
        std::fclose(f);
        std::printf("ok: dumped eedi2 %d x %d -> %d x %d, rs %d x %d\n",
                    img.w, img.h, img.w, img.h * 2, img.w, img.h);
        return 0;
    }

    if (!dumpEedi2.empty()) {
        // debug: dump the first EEDI2 pass (w x 2h raw gray8) for comparison
        std::vector<u8> y, u, v;
        if (img.ch == 3)
            rgbToYuv(img.p.data(), img.w * img.h, y, u, v);
        else
            y = img.p;
        Eedi2 eedi2(p);
        std::vector<u8> h2(std::size_t(img.w) * img.h * 2);
        eedi2.run(y.data(), img.w, img.h, h2.data());
        FILE *f = std::fopen(dumpEedi2.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpEedi2.c_str()); return 1; }
        std::fwrite(h2.data(), 1, h2.size(), f);
        std::fclose(f);
        std::printf("ok: dumped %d x %d -> %d x %d\n", img.w, img.h, img.w, img.h * 2);
        return 0;
    }

    std::vector<u8> lumaAA;
    std::vector<u8> grayRef;
    if (img.ch == 1) {
        grayRef = img.p;
        aaChain(img.p.data(), img.w, img.h, p, lumaAA);
    } else {
        std::vector<u8> y, u, v;
        rgbToYuv(img.p.data(), img.w * img.h, y, u, v);
        grayRef = y;
        aaChain(y.data(), img.w, img.h, p, lumaAA);
        if (repairMode != 0)
            repair2(lumaAA.data(), grayRef.data(), img.w, img.h, lumaAA.data());
        std::vector<u8> rgb;
        yuvToRgb(lumaAA, u, v, rgb);
        img.p = std::move(rgb);
    }

    if (img.ch == 1 && repairMode != 0)
        repair2(lumaAA.data(), grayRef.data(), img.w, img.h, lumaAA.data());

    if (img.ch == 1)
        img.p = std::move(lumaAA);

    if (!saveImage(output, img)) {
        std::fprintf(stderr, "error: cannot write image '%s'\n", output.c_str());
        return 1;
    }

    std::printf("ok: %d x %d -> %d x %d (%s)\n", img.w, img.h, img.w, img.h, output.c_str());
    return 0;
}