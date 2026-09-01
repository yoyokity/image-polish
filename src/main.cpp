/*
 * imagepolish - EEDI2-based anti-aliasing, standalone C++ command line tool.
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
 *   imagepolish -i input.png [-i input2.png ...] [-o output.png] [options]
 *   Multiple -i inputs run as a serial batch; -o is then ignored and each
 *   output is <input basename>_output.<same ext>.
 *   options: --aa [1|2] (2 = strong AA, EEDI2 + 3/2 supersample + SangNom),
 *            --mthresh N --lthresh N --vthresh N --maxd N --nt N --field N
 *            --repair N (0 disables Repair), --deband [range=,y=,cbcr=],
 *            --grain [h] (AddGrain noise variance, default 10),
 *            --model <name> (ONNX super-resolution from models/),
 *            --quality N (JPEG output quality, 1..100, default 80),
 *            -v / --version, -h / --help
 *
 * Build (see build.py):
 *   python build.py            build for the current platform -> out/imagepolish.exe
 *   python build.py --all      cross-compile all platforms and zip each ->
 *                              out/imagepolish-<version>-<target>.zip
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "chain.h"
#include "color.h"
#include "filters/eedi2.h"
#include "filters/onnxsr.h"
#include "imageio.h"
#include "filters/resample.h"
#include "version.h"

// Stringify a macro token (version.h keeps the version unquoted so that build
// scripts can extract it; here it becomes the "--version" string).
#define IMAGEPOLISH_STR_(x) #x
#define IMAGEPOLISH_STR(x) IMAGEPOLISH_STR_(x)

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
static void printHelp()
{
    std::printf(
        "imagepolish - EEDI2 anti-aliasing + NLMeans denoise + resize + deband\n"
        "\n"
        "Usage: imagepolish -i <input> [-i <input> ...] [-o <output>] [options]\n"
        "\n"
        "  -i, --input <file>    input image (PNG/BMP/PNM/TGA, gray or RGB); may be\n"
        "                        repeated to process a batch of files serially;\n"
        "                        -o is then ignored and each output is\n"
        "                        <input basename>_output.<same ext>\n"
        "  -o, --output <file>   output image (format from extension);\n"
        "                        omitted: <input basename>_output.<same ext>\n"
        "  --aa [<level>]        anti-aliasing; <level> is 2 (default) or 1:\n"
        "                        2 = strong AA: EEDI2 + 3/2 supersample + SangNom,\n"
        "                        1 = EEDI2 chain (fixed parameters)\n"
        "  --denoise [<h>]        NLMeans denoise; <h> is the filter strength\n"
        "                        (default 5)\n"
        "  --sharpen [<s>]        CAS sharpening; <s> in 0.0..1.0 (default 0.7)\n"
        "  --resize <W>x<H>      resize with spline36; one side may be omitted\n"
        "                        (1920x or x1080, the other side is proportional)\n"
        "  --dehalo              dehalo (FineDehalo, havsfunc defaults, fixed)\n"
        "  --deband [<name>=<value>,...]\n"
        "                        deband (neo_f3kdb, sample_mode=2, no grain);\n"
        "                        names: range (0..255), y (0..511), cbcr\n"
        "                        (0..511), defaults 24,72,32; omitted names\n"
        "                        keep their defaults, any order\n"
        "  --grain [<h>]         film grain (AddGrain); <h> is the noise\n"
        "                        variance std=h^0.5 (default 10), e.g. 20~100 for\n"
        "                        visible grain, 0 disables; seed derived from image\n"
        "  --model <name>       super-resolution via an ONNX model (all bundled\n"
        "                        models upscale x2, RGB); <name> is a path or a\n"
        "                        bare name looked up in models/ (e.g. --model\n"
        "                        2xGTv6 or --model 2x_HSR_V3_compact_fp16_op18);\n"
        "                        odd input sides are padded to even and the\n"
        "                        output is cropped accordingly; needs\n"
        "                        onnxruntime.dll (Windows) or\n"
        "                        libonnxruntime.so (Linux/macOS) next to the\n"
        "                        executable\n"
        "  --quality <q>       JPEG output quality 1..100 (default 80);\n"
        "                        only affects .jpg/.jpeg outputs\n"
        "  -v, --version        print version and exit\n"
        "\n"
        "Steps are executed in the order they appear on the command line;\n"
        "with no step the image is passed through unchanged.\n"
        "\n"
        "Examples:\n"
        "  imagepolish -i in.png --denoise 5 --aa\n"
        "  imagepolish -i in.png --resize 1920x --aa 2\n"
        "  imagepolish -i in.png -o out.png --aa 2 --denoise 3\n"
        "  imagepolish -i a.png -i b.png -i c.png --aa --denoise 5\n");
}

// Default output path: <input basename>_output.<same extension>
static std::string defaultOutput(const std::string &input)
{
    const auto pos = input.find_last_of('.');
    if (pos == std::string::npos)
        return input + "_output";
    return input.substr(0, pos) + "_output" + input.substr(pos);
}

int main(int argc, char **argv)
{
    std::vector<std::string> inputs;
    std::string output;
    std::string dumpEedi2, dumpRs;
    std::vector<Step> steps;
    int jpegQuality = 80;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-i" || a == "--input") inputs.push_back(next("-i"));
        else if (a == "-o" || a == "--output") output = next("-o");
        else if (a == "--aa") {
            int level = 2;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                level = std::atoi(argv[++i]);
            if (level < 1 || level > 2) {
                std::fprintf(stderr, "error: --aa level must be 1 or 2\n");
                return 1;
            }
            steps.push_back(Step::aa(level));
        }
        else if (a == "--denoise") {
            float hv = 5.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                hv = static_cast<float>(std::atof(argv[++i]));
            steps.push_back(Step::denoise(hv));
        } else if (a == "--sharpen") {
            float sv = 0.7f;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                sv = static_cast<float>(std::atof(argv[++i]));
            steps.push_back(Step::sharpen(sv));
        } else if (a == "--resize") {
            int rw = 0, rh = 0;
            parseResize(next("--resize"), rw, rh);
            steps.push_back(Step::resize(rw, rh));
        }
        else if (a == "--dehalo") steps.push_back(Step::dehalo());
        else if (a == "--grain") {
            float gv = 10.0f;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                gv = static_cast<float>(std::atof(argv[++i]));
            if (gv < 0.0f) {
                std::fprintf(stderr, "error: --grain variance must be >= 0\n");
                return 1;
            }
            steps.push_back(Step::grain(gv));
        }
        else if (a == "--deband") {
            int r = 24, y = 72, c = 32;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                parseDeband(next("--deband"), r, y, c);
            steps.push_back(Step::deband(r, y, c));
        }
        else if (a == "--model") {
            std::string err;
            const std::string path = resolveOnnxModel(next("--model"), err);
            if (path.empty()) {
                if (onnxRuntimeAvailable()) {
                    std::fprintf(stderr, "error: --model: %s\n", err.c_str());
                    return 1;
                }
                // The plain packages ship without the onnxruntime library, so
                // the ONNX feature is not part of them: warn and skip instead
                // of failing on model resolution.
                onnxWarn("warning: --model: ONNX is not included in this package "
                         "(onnxruntime library missing); step skipped");
                continue;
            }
            steps.push_back(Step::onnxSr(path));
        }
        else if (a == "--quality") {
            jpegQuality = std::atoi(next("--quality"));
        }
        else if (a == "--dump-eedi2") dumpEedi2 = next("--dump-eedi2");
        else if (a == "--dump-rs") dumpRs = next("--dump-rs");
        else if (a == "-v" || a == "--version") {
            std::printf("imagepolish %s\n", IMAGEPOLISH_STR(IMAGEPOLISH_VERSION));
            return 0;
        }
        else if (a == "-h" || a == "--help") { printHelp(); return 0; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            printHelp();
            return 1;
        }
    }

    if (inputs.empty()) {
        std::fprintf(stderr, "error: -i is required\n");
        printHelp();
        return 1;
    }

    const bool batch = inputs.size() > 1;
    if (batch && !output.empty()) {
        std::fprintf(stderr, "warning: -o ignored with multiple -i inputs\n");
        output.clear();
    }

    for (const Step &st : steps) {
        if (st.kind == StepKind::Denoise && st.h <= 0.0f) {
            std::fprintf(stderr, "error: --denoise h must be positive\n");
            return 1;
        }
        if (st.kind == StepKind::Sharpen && (st.h < 0.0f || st.h > 1.0f)) {
            std::fprintf(stderr, "error: --sharpen must be in 0.0..1.0\n");
            return 1;
        }
        if (st.kind == StepKind::Deband) {
            if (st.dbr < 0 || st.dbr > 255) {
                std::fprintf(stderr, "error: --deband range must be in 0..255\n");
                return 1;
            }
            if (st.dby < 0 || st.dby > 511) {
                std::fprintf(stderr, "error: --deband y must be in 0..511\n");
                return 1;
            }
            if (st.dbc < 0 || st.dbc > 511) {
                std::fprintf(stderr, "error: --deband cbcr must be in 0..511\n");
                return 1;
            }
        }
    }
    if (jpegQuality < 1 || jpegQuality > 100) {
        std::fprintf(stderr, "error: --quality must be in 1..100\n");
        return 1;
    }

    Eedi2Params p;                       // fixed AA parameters (script defaults)
    eedi2_prepare(p);

    if (!dumpEedi2.empty()) {
        // debug: dump the first input's EEDI2 pass (and optionally the
        // resampleV result) as raw gray8, then exit
        const std::string &input = inputs[0];
        Image img;
        if (!loadImage(input, img)) {
            std::fprintf(stderr, "error: cannot read image '%s'\n", input.c_str());
            return 1;
        }
        if (img.w < 8 || img.h < 8) {
            std::fprintf(stderr, "error: image too small (need w>=8, h>=8)\n");
            return 1;
        }
        std::vector<u8> y;
        if (img.ch == 3)
            rgbLuma(img.p.data(), img.w * img.h, y);
        else
            y = img.p;
        Eedi2 eedi2(p);
        std::vector<u8> h2(std::size_t(img.w) * img.h * 2);
        eedi2.run(y.data(), img.w, img.h, h2.data());
        if (!dumpRs.empty()) {
            std::vector<u8> r1(std::size_t(img.w) * img.h);
            resampleV(h2.data(), img.w, img.h * 2, img.h, -0.5, r1.data());
            FILE *f = std::fopen(dumpRs.c_str(), "wb");
            if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpRs.c_str()); return 1; }
            std::fwrite(r1.data(), 1, r1.size(), f);
            std::fclose(f);
        }
        FILE *f = std::fopen(dumpEedi2.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpEedi2.c_str()); return 1; }
        std::fwrite(h2.data(), 1, h2.size(), f);
        std::fclose(f);
        std::printf("ok: dumped eedi2 %d x %d -> %d x %d%s\n",
                    img.w, img.h, img.w, img.h * 2, dumpRs.empty() ? "" : " (+ rs)");
        return 0;
    }

    // per-step wall time, summed over all inputs in batch mode
    std::vector<double> stepMs(steps.size(), 0.0);
    std::vector<std::string> stepNames(steps.size());

    for (const std::string &input : inputs) {
        Image img;
        if (!loadImage(input, img)) {
            std::fprintf(stderr, "error: cannot read image '%s'\n", input.c_str());
            return 1;
        }
        if (img.w < 8 || img.h < 8) {
            std::fprintf(stderr, "error: image too small (need w>=8, h>=8)\n");
            return 1;
        }

        if (!steps.empty()) {
            if (img.ch == 1) {
                runSteps(p, img.p, img.w, img.h, steps, nullptr,
                         batch ? &stepMs : nullptr, batch ? &stepNames : nullptr);
            } else {
                std::vector<u8> y;
                rgbLuma(img.p.data(), img.w * img.h, y);
                std::vector<u8> refRgb = img.p;              // chroma reference, size kept in lockstep
                runSteps(p, y, img.w, img.h, steps, &refRgb,
                         batch ? &stepMs : nullptr, batch ? &stepNames : nullptr);
                std::vector<u8> rgb;
                applyLuma(y, refRgb.data(), img.w * img.h, rgb);
                img.p = std::move(rgb);
            }
        }

        std::string out = output;
        if (out.empty())
            out = defaultOutput(input);
        if (!saveImage(out, img, jpegQuality)) {
            std::fprintf(stderr, "error: cannot write image '%s'\n", out.c_str());
            return 1;
        }
        std::printf("ok: %d x %d -> %d x %d (%s)\n", img.w, img.h, img.w, img.h, out.c_str());
        std::fflush(stdout);  // emit the ok line immediately, also in batch mode
    }

    if (batch && !steps.empty()) {
        std::printf("step times summed over %zu input(s):\n", inputs.size());
        for (size_t k = 0; k < steps.size(); k++)
            std::printf("  step %zu  %-16s %8.2f ms\n", k + 1, stepNames[k].c_str(), stepMs[k]);
    }
    return 0;
}