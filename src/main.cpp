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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common.h"
#include "dehalo.h"
#include "eedi2.h"
#include "imageio.h"
#include "nlmeans.h"
#include "repair.h"
#include "resample.h"

// ---------------------------------------------------------------------------
// Color helpers (BT.601 luma; chroma of the original image is kept exact)
// ---------------------------------------------------------------------------
static inline u8 clamp8(int v) { return u8(std::max(0, std::min(255, v))); }

// BT.601 full-range luma of an RGB image (8-bit).
static void rgbLuma(const u8 *rgb, int n, std::vector<u8> &y)
{
    y.resize(n);
    for (int i = 0; i < n; i++) {
        const double R = rgb[3 * i + 0], G = rgb[3 * i + 1], B = rgb[3 * i + 2];
        y[i] = clamp8(int(0.299 * R + 0.587 * G + 0.114 * B + 0.5));
    }
}

// Reconstruct RGB after the luma-only AA pass: keep the original chroma by
// adding the luma change (yAA - yOrig) to every channel of the original pixel.
// This is exactly "replace luma, keep chroma" with unquantized chroma, i.e.
// the same semantics as the VapourSynth ShufflePlanes([aa, src], YUV) chain;
// flat regions roundtrip with zero error (only the rare yOrig == x.5 case
// shifts all channels by +1).
static void applyLuma(const std::vector<u8> &yAA, const u8 *origRgb, int n, std::vector<u8> &rgb)
{
    rgb.resize(std::size_t(n) * 3);
    for (int i = 0; i < n; i++) {
        const double R = origRgb[3 * i + 0], G = origRgb[3 * i + 1], B = origRgb[3 * i + 2];
        const double d = double(yAA[i]) - (0.299 * R + 0.587 * G + 0.114 * B);
        rgb[3 * i + 0] = clamp8(int(R + d + 0.5));
        rgb[3 * i + 1] = clamp8(int(G + d + 0.5));
        rgb[3 * i + 2] = clamp8(int(B + d + 0.5));
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
// Processing steps, applied to the luma plane in command-line order
// ---------------------------------------------------------------------------
enum class StepKind { AA, Denoise, Resize, Dehalo };

struct Step {
    StepKind kind;
    float h = 0.0f;      // NLMeans strength for Denoise steps
    int rw = -1, rh = -1; // Resize targets (-1: keep proportional)
};

// Anti-aliasing step: the EEDI2 chain plus Repair(2), parameters fixed to the
// defaults matching the original script.
static void aaStep(const Eedi2Params &p, std::vector<u8> &plane, int w, int h)
{
    std::vector<u8> ref = plane;                    // Repair reference = pre-AA luma
    std::vector<u8> aa;
    aaChain(plane.data(), w, h, p, aa);
    repair2(aa.data(), ref.data(), w, h, aa.data());
    plane = std::move(aa);
}

// Parse "--resize WxH | Wx | xH"; a missing side (-1) is kept proportional.
static void parseResize(const char *s, int &rw, int &rh)
{
    std::string spec = s;
    std::transform(spec.begin(), spec.end(), spec.begin(),
                   [](char c) { return char(std::tolower((unsigned char)c)); });
    const auto x = spec.find('x');
    if (x == std::string::npos) {
        std::fprintf(stderr, "error: --resize expects WxH, Wx or xH (e.g. 1920x1080)\n");
        std::exit(1);
    }
    const std::string a = spec.substr(0, x), b = spec.substr(x + 1);
    if (a.empty() && b.empty()) {
        std::fprintf(stderr, "error: --resize needs at least one dimension\n");
        std::exit(1);
    }
    auto side = [](const std::string &v) -> int {
        if (v.empty())
            return -1;
        const int n = std::atoi(v.c_str());
        if (n <= 0) {
            std::fprintf(stderr, "error: --resize dimensions must be positive\n");
            std::exit(1);
        }
        return n;
    };
    rw = side(a);
    rh = side(b);
}

static void runSteps(const Eedi2Params &p, std::vector<u8> &plane, int &w, int &h,
                     const std::vector<Step> &steps, std::vector<u8> *refRgb)
{
    for (const Step &st : steps) {
        switch (st.kind) {
        case StepKind::AA:
            if (w < 8 || h < 8) {
                std::fprintf(stderr, "error: --aa needs w,h >= 8 (current %d x %d)\n", w, h);
                std::exit(1);
            }
            aaStep(p, plane, w, h);
            break;
        case StepKind::Denoise: {
            NlmeansParams np;               // fixed config: a=2, s=4, wmode=3, wref=1
            np.h = st.h;
            std::vector<u8> out(std::size_t(w) * h);
            nlmeans(plane.data(), w, h, np, out.data());
            plane = std::move(out);
            break;
        }
        case StepKind::Resize: {
            int nw = st.rw, nh = st.rh;
            if (nw < 0) nw = std::max(1, int(std::llround(double(nh) * w / h)));
            if (nh < 0) nh = std::max(1, int(std::llround(double(nw) * h / w)));
            std::vector<u8> out(std::size_t(nw) * nh);
            resample2D(plane.data(), w, h, nw, nh, out.data());
            plane = std::move(out);
            if (refRgb) {
                // keep the chroma reference size in lockstep so applyLuma stays paired
                std::vector<u8> ch(std::size_t(w) * h), gen(std::size_t(nw) * nh);
                std::vector<u8> scaled(std::size_t(nw) * nh * 3);
                for (int c = 0; c < 3; c++) {
                    for (int i = 0; i < w * h; i++)
                        ch[i] = (*refRgb)[3 * i + c];
                    resample2D(ch.data(), w, h, nw, nh, gen.data());
                    for (int i = 0; i < nw * nh; i++)
                        scaled[3 * i + c] = gen[i];
                }
                *refRgb = std::move(scaled);
            }
            w = nw;
            h = nh;
            break;
        }
        case StepKind::Dehalo: {
            std::vector<u8> out(std::size_t(w) * h);
            fineDehalo(plane.data(), w, h, out.data());
            plane = std::move(out);
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
static void printHelp()
{
    std::printf(
        "aa.exe - EEDI2 anti-aliasing + NLMeans denoise + resize\n"
        "\n"
        "Usage: aa.exe -i <input> [-o <output>] [--aa] [--denoise <h>] [--resize <W>x<H>]\n"
        "\n"
        "  -i, --input <file>    input image (PNG/BMP/PNM/TGA, gray or RGB)\n"
        "  -o, --output <file>   output image (format from extension);\n"
        "                        omitted: <input basename>_output.<same ext>\n"
        "  --aa                  anti-aliasing (EEDI2 chain, parameters fixed)\n"
        "  --denoise <h>         NLMeans denoise, <h> is the filter strength\n"
        "  --resize <W>x<H>      resize with spline36; one side may be omitted\n"
        "                        (1920x or x1080, the other side is proportional)\n"
        "  --dehalo              dehalo (FineDehalo, havsfunc defaults, fixed)\n"
        "\n"
        "Steps are executed in the order they appear on the command line;\n"
        "with no step the image is passed through unchanged.\n"
        "\n"
        "Examples:\n"
        "  aa.exe -i in.png --denoise 5 --aa\n"
        "  aa.exe -i in.png --resize 1920x --aa\n"
        "  aa.exe -i in.png -o out.png --aa --denoise 3\n");
}

int main(int argc, char **argv)
{
    std::string input, output;
    std::string dumpEedi2, dumpRs;
    std::vector<Step> steps;

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
        else if (a == "--aa") steps.push_back({StepKind::AA, 0.0f, -1, -1});
        else if (a == "--denoise") steps.push_back({StepKind::Denoise, static_cast<float>(std::atof(next("--denoise"))), -1, -1});
        else if (a == "--resize") {
            int rw = 0, rh = 0;
            parseResize(next("--resize"), rw, rh);
            steps.push_back({StepKind::Resize, 0.0f, rw, rh});
        }
        else if (a == "--dehalo") steps.push_back({StepKind::Dehalo, 0.0f, -1, -1});
        else if (a == "--dump-eedi2") dumpEedi2 = next("--dump-eedi2");
        else if (a == "--dump-rs") dumpRs = next("--dump-rs");
        else if (a == "-h" || a == "--help") { printHelp(); return 0; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            printHelp();
            return 1;
        }
    }

    if (input.empty()) {
        std::fprintf(stderr, "error: -i is required\n");
        printHelp();
        return 1;
    }

    if (output.empty()) {
        // default: <input basename>_output.<same extension>
        const auto pos = input.find_last_of('.');
        if (pos == std::string::npos)
            output = input + "_output";
        else
            output = input.substr(0, pos) + "_output" + input.substr(pos);
    }

    for (const Step &st : steps)
        if (st.kind == StepKind::Denoise && st.h <= 0.0f) {
            std::fprintf(stderr, "error: --denoise h must be positive\n");
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

    Eedi2Params p;                       // fixed AA parameters (script defaults)
    eedi2_prepare(p);

    if (!dumpEedi2.empty() && !dumpRs.empty()) {
        // debug: dump the first EEDI2 pass (w x 2h raw gray8) and its
        // resampleV result (w x h raw gray8) for comparison with eedi2/fmtc
        std::vector<u8> y;
        if (img.ch == 3)
            rgbLuma(img.p.data(), img.w * img.h, y);
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
        std::vector<u8> y;
        if (img.ch == 3)
            rgbLuma(img.p.data(), img.w * img.h, y);
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

    if (!steps.empty()) {
        if (img.ch == 1) {
            runSteps(p, img.p, img.w, img.h, steps, nullptr);
        } else {
            std::vector<u8> y;
            rgbLuma(img.p.data(), img.w * img.h, y);
            std::vector<u8> refRgb = img.p;              // chroma reference, size kept in lockstep
            runSteps(p, y, img.w, img.h, steps, &refRgb);
            std::vector<u8> rgb;
            applyLuma(y, refRgb.data(), img.w * img.h, rgb);
            img.p = std::move(rgb);
        }
    }

    if (!saveImage(output, img)) {
        std::fprintf(stderr, "error: cannot write image '%s'\n", output.c_str());
        return 1;
    }

    std::printf("ok: %d x %d -> %d x %d (%s)\n", img.w, img.h, img.w, img.h, output.c_str());
    return 0;
}