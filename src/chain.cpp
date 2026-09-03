#include "chain.h"

#include "color.h"
#include "filters/cas.h"
#include "filters/deband.h"
#include "filters/dehalo.h"
#include "filters/eedi2.h"
#include "filters/grain.h"
#include "filters/nlmeans.h"
#include "filters/onnxsr.h"
#include "filters/repair.h"
#include "filters/resample.h"
#include "filters/sangnom.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Strong AA chain (--aa 2), port of the VapourSynth reference:
//   aa = fmtc.resample(clip, dw, dh)                       // 3/4 downscale
//   aa = eedi2.EEDI2(aa)                                   // double height
//   aa = fmtc.resample(aa, w, uh, sy=-0.5).Transpose()
//   aa = eedi2.EEDI2(aa)                                   // double height
//   aa = fmtc.resample(aa, uh, uw, sy=-0.5)                // 8-bit passthrough
//   aa = sangnom.SangNom(aa, aa=48, dh=False).Transpose()
//   aa = sangnom.SangNom(aa, aa=48, dh=False)
//   aa = fmtc.resample(aa, clip.width, clip.height)        // back to w x h
//   (bitdepth to 8 is a no-op: the whole chain runs in gray8 here)
//
// ---------------------------------------------------------------------------
static int workSize(int n)
{
    // 3/2 scale, rounded up to an even number so both SangNom passes (which
    // reject odd heights) work for any w,h; even sizes pass through unchanged.
    const int s = n * 3 / 2;
    return s + (s & 1);
}

static void aaChain2(const u8 *gray, int w, int h, const Eedi2Params &p, std::vector<u8> &out)
{
    const int uw = workSize(w);
    const int uh = workSize(h);
    // 3/4 downscale keeps the floor division of the reference; the resamplers
    // absorb the residue.
    const int dw = w * 3 / 4;
    const int dh = h * 3 / 4;

    Eedi2 eedi2(p);

    std::vector<u8> d1(std::size_t(dw) * dh);
    resample2D(gray, w, h, dw, dh, d1.data());                      // dw x dh

    std::vector<u8> a1(std::size_t(dw) * 2 * dh);
    eedi2.run(d1.data(), dw, dh, a1.data());                        // dw x 2dh
    std::vector<u8> r1(std::size_t(w) * uh);
    resample2DShift(a1.data(), dw, 2 * dh, w, uh, 0.0, -0.5, r1.data()); // w x uh

    std::vector<u8> t1(std::size_t(uh) * w);
    transpose(r1.data(), w, uh, t1.data());                         // uh x w

    std::vector<u8> a2(std::size_t(uh) * 2 * w);
    eedi2.run(t1.data(), uh, w, a2.data());                         // uh x 2w
    std::vector<u8> r2(std::size_t(uh) * uw);
    resample2DShift(a2.data(), uh, 2 * w, uh, uw, 0.0, -0.5, r2.data()); // uh x uw

    std::vector<u8> s1(std::size_t(uh) * uw);
    sangnom(r2.data(), uh, uw, 48, s1.data());                      // uh x uw
    std::vector<u8> t2(std::size_t(uw) * uh);
    transpose(s1.data(), uh, uw, t2.data());                        // uw x uh
    std::vector<u8> s2(std::size_t(uw) * uh);
    sangnom(t2.data(), uw, uh, 48, s2.data());                      // uw x uh

    out.resize(std::size_t(w) * h);
    resample2D(s2.data(), uw, uh, w, h, out.data());                // w x h
}

// Level-2 AA step: the chain above plus Repair(2) against the original luma.
static void aaStep2(const Eedi2Params &p, std::vector<u8> &plane, int w, int h)
{
    std::vector<u8> ref = plane;                    // Repair reference = pre-AA luma
    std::vector<u8> aa;
    aaChain2(plane.data(), w, h, p, aa);
    repair2(aa.data(), ref.data(), w, h, aa.data());
    plane = std::move(aa);
}

// ---------------------------------------------------------------------------
// Per-step CLI parsing
// ---------------------------------------------------------------------------

// Parse "--resize WxH | Wx | xH"; a missing side (-1) is kept proportional.
void parseResize(const char *s, int &rw, int &rh)
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

// Parameter index for a --deband name: 0 range, 1 y, 2 cbcr; -1 if unknown.
static int debandIndex(const std::string &name)
{
    if (name == "range")
        return 0;
    if (name == "y")
        return 1;
    if (name == "cbcr")
        return 2;
    return -1;
}

// Parse "--deband [<name>=<value>,...]" with names range (0..255), y and
// cbcr (0..511). Omitted names keep their defaults (24, 72, 32); any order is
// allowed. Unknown names, duplicates, missing '=' or empty values are errors.
void parseDeband(const char *s, int &range, int &y, int &cbcr)
{
    bool seen[3] = {};
    const std::string spec = s;
    for (size_t pos = 0;;) {
        const size_t comma = spec.find(',', pos);
        const std::string tok = spec.substr(pos, comma == std::string::npos
                                                ? std::string::npos : comma - pos);
        if (tok.empty()) {
            std::fprintf(stderr, "error: --deband expects <name>=<value> pairs (e.g. y=40)\n");
            std::exit(1);
        }
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "error: --deband '%s' is not a <name>=<value> pair (e.g. y=40)\n", tok.c_str());
            std::exit(1);
        }
        const std::string name = tok.substr(0, eq);
        const std::string val = tok.substr(eq + 1);
        if (val.empty()) {
            std::fprintf(stderr, "error: --deband missing value after '%s='\n", name.c_str());
            std::exit(1);
        }
        const int n = std::atoi(val.c_str());
        const int idx = debandIndex(name);
        if (idx < 0) {
            std::fprintf(stderr, "error: --deband unknown parameter '%s' (expect range, y or cbcr)\n", name.c_str());
            std::exit(1);
        }
        if (seen[idx]) {
            std::fprintf(stderr, "error: --deband parameter '%s' given more than once\n", name.c_str());
            std::exit(1);
        }
        seen[idx] = true;
        switch (idx) {
        case 0: range = n; break;
        case 1: y = n; break;
        case 2: cbcr = n; break;
        }
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
}

// ---------------------------------------------------------------------------
// Step dispatch
// ---------------------------------------------------------------------------
void runSteps(const Eedi2Params &p, std::vector<u8> &plane, int &w, int &h,
              const std::vector<Step> &steps, std::vector<u8> *refRgb,
              std::vector<double> *accMs, std::vector<std::string> *accNames)
{
    int idx = 0;
    for (const Step &st : steps) {
        const auto t0 = std::chrono::steady_clock::now();
        char name[40];
        bool skipped = false;
        switch (st.kind) {
        case StepKind::AA:
            if (w < 8 || h < 8) {
                std::fprintf(stderr, "error: --aa needs w,h >= 8 (current %d x %d)\n", w, h);
                std::exit(1);
            }
            if (st.aaLevel == 2) {
                // odd 3/2-scaled working heights are rounded to even inside the
                // chain, so any w,h >= 8 is accepted
                aaStep2(p, plane, w, h);
                std::snprintf(name, sizeof name, "AA(2)");
            } else {
                aaStep(p, plane, w, h);
                std::snprintf(name, sizeof name, "AA");
            }
            break;
        case StepKind::Denoise: {
            NlmeansParams np;               // fixed config: a=2, s=4, wmode=3, wref=1
            np.h = st.h;
            std::vector<u8> out(std::size_t(w) * h);
            nlmeans(plane.data(), w, h, np, out.data());
            plane = std::move(out);
            std::snprintf(name, sizeof name, "NLMeans h=%g", st.h);
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
            std::snprintf(name, sizeof name, "Resize %dx%d", nw, nh);
            break;
        }
        case StepKind::Dehalo: {
            std::vector<u8> out(std::size_t(w) * h);
            fineDehalo(plane.data(), w, h, out.data());
            plane = std::move(out);
            std::snprintf(name, sizeof name, "FineDehalo");
            break;
        }
        case StepKind::Sharpen: {
            std::vector<u8> out(std::size_t(w) * h);
            cas(plane.data(), w, h, st.h, out.data());
            plane = std::move(out);
            std::snprintf(name, sizeof name, "CAS s=%g", st.h);
            break;
        }
        case StepKind::Grain: {
            std::vector<u8> out(std::size_t(w) * h);
            grain(plane.data(), w, h, st.h, out.data());
            plane = std::move(out);
            std::snprintf(name, sizeof name, "Grain var=%g", st.h);
            break;
        }
        case StepKind::Deband: {
            DebandParams dp;
            dp.range = st.dbr;
            dp.y = st.dby;
            dp.cbcr = st.dbc;
            std::vector<u8> out(std::size_t(w) * h);
            deband(plane.data(), w, h, dp, out.data());
            plane = std::move(out);
            std::snprintf(name, sizeof name, "Deband r=%d y=%d", st.dbr, st.dby);
            break;
        }
        case StepKind::OnnxSr: {
            // ONNX SR models are trained on RGB, so the step runs on the full
            // color image: luma plane + chroma reference are recombined to RGB
            // (or the gray plane is replicated), the model runs, and the
            // result replaces both the luma plane and the chroma reference.
            std::vector<u8> rgb;
            if (refRgb) {
                applyLuma(plane, refRgb->data(), w * h, rgb);
            } else {
                const std::size_t n = std::size_t(w) * h;
                rgb.resize(n * 3);
                for (std::size_t i = 0; i < n; i++) {
                    const u8 v = plane[i];
                    rgb[3 * i + 0] = v;
                    rgb[3 * i + 1] = v;
                    rgb[3 * i + 2] = v;
                }
            }
            std::vector<u8> sr;
            int nw = 0, nh = 0;
            std::string oerr;
            const OnnxSrStatus rc = runOnnxSr(st.model, rgb.data(), w, h, sr, nw, nh, oerr);
            if (rc == OnnxSrStatus::Skip) {
                onnxWarn("warning: --model: " + oerr + "; step skipped");
                skipped = true;
                break;
            }
            if (rc == OnnxSrStatus::Error) {
                std::fprintf(stderr, "error: --model (%s): %s\n", st.model.c_str(), oerr.c_str());
                std::exit(1);
            }
            if (refRgb) {
                rgbLuma(sr.data(), nw * nh, plane);
                *refRgb = std::move(sr);
            } else {
                rgbLuma(sr.data(), nw * nh, plane);   // gray stays gray: keep the luma
            }
            w = nw;
            h = nh;
            std::string mname = st.model;
            const auto slash = mname.find_last_of("/\\");
            if (slash != std::string::npos)
                mname = mname.substr(slash + 1);
            std::snprintf(name, sizeof name, "ONNX-SR%s%s", mname.empty() ? "" : " ",
                          mname.c_str());
            break;
        }
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        ++idx;
        if (skipped)
            continue;                       // warned above; keep step numbering
        if (accMs) {
            (*accMs)[idx - 1] += ms;
            if (accNames)
                (*accNames)[idx - 1] = name;
        } else {
            std::printf("  step %d  %-16s %8.2f ms\n", idx, name, ms);
        }
    }
}