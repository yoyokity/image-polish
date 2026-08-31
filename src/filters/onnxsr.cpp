#include "onnxsr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// onnxruntime C++ API with manual API-table initialization: nothing is linked
// at build time, the runtime library is dlopen/LoadLibrary'd and its function
// table is handed to Ort::InitApi() before the first session is created.
#define ORT_API_MANUAL_INIT
#include "onnx/onnxruntime_cxx_api.h"
#undef ORT_API_MANUAL_INIT

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif
#endif

namespace {

using OrtGetBaseFn = const OrtApiBase *(ORT_API_CALL *)();

// Version string of the loaded onnxruntime runtime, filled when loadOrtBase()
// succeeds (OrtApiBase::GetVersionString; the C++ Ort::GetVersionString() is a
// linked export we do not have, and the OrtApi struct has no member for it).
std::string gOrtVersion;
// Handle of the loaded runtime, kept so standalone C API exports such as
// OrtSessionOptionsAppendExecutionProvider_CPU can be resolved manually.
void *gOrtHandle = nullptr;

bool fileExists(const std::string &p)
{
    FILE *f = std::fopen(p.c_str(), "rb");
    if (f)
        std::fclose(f);
    return f != nullptr;
}

// Directory of the executable, with a trailing separator; "" if unknown.
std::string exeDir()
{
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return "";
    const std::string p(buf, std::size_t(n));
    const auto s = p.find_last_of('\\');
    return s == std::string::npos ? "" : p.substr(0, s + 1);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0)
        return "";
    const std::string p(buf);
    const auto s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s + 1);
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return "";
    const std::string p(buf, std::size_t(n));
    const auto s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s + 1);
#endif
}

// Loads the onnxruntime shared library once per process and returns its API
// base, or nullptr with a reason in `err`. Only the executable's own directory
// is searched — never PATH or the library cache — so a stray onnxruntime from
// elsewhere on the system cannot be picked up. The handle is intentionally
// held for the lifetime of the process.
const OrtApiBase *loadOrtBase(std::string &err)
{
    static bool tried = false;
    static const OrtApiBase *apiBase = nullptr;
    static std::string why;
    if (!tried) {
        tried = true;
#if defined(_WIN32)
        HMODULE h = LoadLibraryA((exeDir() + "onnxruntime.dll").c_str());
        gOrtHandle = h;
        if (!h) {
            why = "onnxruntime.dll not found next to imagepolish";
        } else {
            auto getBase = reinterpret_cast<OrtGetBaseFn>(GetProcAddress(h, "OrtGetApiBase"));
            if (!getBase)
                why = "onnxruntime.dll has no OrtGetApiBase export (too old or corrupt?)";
            else
                apiBase = getBase();
        }
#else
        const std::string dir = exeDir();
        void *h = nullptr;
        for (const std::string &run : { dir + "libonnxruntime.so", dir + "libonnxruntime.dylib" })
            if ((h = dlopen(run.c_str(), RTLD_NOW)) != nullptr)
                break;
        gOrtHandle = h;
        if (!h) {
            why = "libonnxruntime.so / libonnxruntime.dylib not found or not loadable next to imagepolish";
            if (const char *d = dlerror())
                why += std::string(" (") + d + ")";
        } else {
            auto getBase = reinterpret_cast<OrtGetBaseFn>(dlsym(h, "OrtGetApiBase"));
            if (!getBase)
                why = "onnxruntime library has no OrtGetApiBase export (too old or corrupt?)";
            else
                apiBase = getBase();
        }
#endif
        if (apiBase && apiBase->GetVersionString) {
            const char *v = apiBase->GetVersionString();
            if (v)
                gOrtVersion = v;
        }
    }
    err = why;
    return apiBase;
}

#if defined(_WIN32)
// Session model paths are ORTCHAR_T (wchar_t) on Windows.
std::wstring toOrtPath(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

bool endsWithIc(const std::string &s, const char *suf)
{
    const size_t n = std::strlen(suf);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != (unsigned char)suf[i])
            return false;
    return true;
}

// *.onnx file names in a directory, sorted case-insensitively; empty if the
std::vector<std::string> onnxFilesIn(const std::string &dir)
{
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.onnx").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            out.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            if (endsWithIc(e->d_name, ".onnx"))
                out.push_back(e->d_name);
        }
        closedir(d);
    }
#endif
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
        for (size_t i = 0; i < a.size() && i < b.size(); i++) {
            const unsigned char ca = (unsigned char)std::tolower((unsigned char)a[i]);
            const unsigned char cb = (unsigned char)std::tolower((unsigned char)b[i]);
            if (ca != cb)
                return ca < cb;
        }
        return a.size() < b.size();
    });
    return out;
}

std::string join(const std::vector<std::string> &v, const char *sep)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? sep : "") + v[i];
    return s;
}

} // namespace

std::string resolveOnnxModel(const std::string &name, std::string &err)
{
    const bool asIs = name.find('/') != std::string::npos ||
                      name.find('\\') != std::string::npos ||
                      endsWithIc(name, ".onnx");
    if (asIs) {
        if (fileExists(name))
            return name;
        err = "model file not found: " + name;
        return "";
    }

    // bare model name: exact candidates first ...
    const char *candidates[] = { "models/<name>.onnx", "<name>.onnx", "models/<name>" };
    for (const char *c : candidates) {
        std::string p = c;
        p.replace(p.find("<name>"), 6, name);
        if (fileExists(p))
            return p;
    }

    // ... then a unique prefix match against models/*.onnx
    const std::vector<std::string> avail = onnxFilesIn("models");
    std::vector<std::string> hits;
    for (const std::string &m : avail)
        if (m.compare(0, name.size(), name) == 0)
            hits.push_back(m);
    if (hits.size() == 1)
        return "models/" + hits[0];
    if (hits.size() > 1) {
        err = "model name '" + name + "' is ambiguous, matches: " + join(hits, ", ");
        return "";
    }

    err = "no ONNX model '" + name + "' found; models/ contains: " +
          (avail.empty() ? "nothing" : join(avail, ", "));
    return "";
}

void onnxWarn(const std::string &msg)
{
#if defined(_WIN32)
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &bi)) {
        SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN);
        std::fprintf(stderr, "%s\n", msg.c_str());
        SetConsoleTextAttribute(h, bi.wAttributes);
        return;
    }
#endif
    std::fprintf(stderr, "\033[33m%s\033[0m\n", msg.c_str());
}

bool onnxRuntimeAvailable()
{
    std::string err;
    return loadOrtBase(err) != nullptr;
}

namespace {

// ---------------------------------------------------------------------------
// Execution providers and session cache
// ---------------------------------------------------------------------------
// The session is created once per model path and reused for every image (the
// Node reference caches by model path too); the previous behaviour of
// re-creating Env + Session per image re-initialized the GPU device and
// reloaded the model graph every frame. On each platform the fastest GPU
// execution provider is appended first (DirectML on Windows, CoreML on macOS,
// CUDA on Linux) with the CPU EP last as fallback; when the GPU provider
// cannot initialize — the runtime lacks it, its companion DLLs are missing,
// or the machine has no compatible accelerator — creation is retried with
// CPU-only options.

#if defined(_WIN32)
// DirectML is not in the base OrtApi; it is fetched through
// OrtApi::GetExecutionProviderApi("DML", ORT_API_VERSION), which returns an
// OrtDmlApi*. Only the leading member is declared here: the append function is
// the first, oldest entry of that struct and sits at the same offset on every
// ORT build that supports DirectML, so the rest of the struct is never read.
struct OrtDmlApi {
    OrtStatusPtr(ORT_API_CALL *SessionOptionsAppendExecutionProvider_DML)(
        OrtSessionOptions *options, int device_id);
};
#endif

// One loaded model: an Env that outlives its session, the session (DML + CPU
// fallback), graph input names/types read once at load time, and the execution
// provider actually in use (informational).
struct OnnxSrModel {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::string inName;
    std::string outName;
    int inType = 0;        // ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT / _FLOAT16
    std::string provider;  // "DirectML" or "CPU" (informational)
    std::string version;   // onnxruntime version string (informational)
};

std::mutex gModelMutex;
std::map<std::string, std::shared_ptr<const OnnxSrModel>> gModels;

std::string baseName(const std::string &p)
{
    const auto s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static bool appendCpuFallback(Ort::SessionOptions &so)
{
    using Fn = OrtStatusPtr(ORT_API_CALL *)(OrtSessionOptions *, int);
    static Fn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
#if defined(_WIN32)
        if (HMODULE h = GetModuleHandleA("onnxruntime.dll"))
            fn = reinterpret_cast<Fn>(GetProcAddress(h, "OrtSessionOptionsAppendExecutionProvider_CPU"));
#else
        if (gOrtHandle)
            fn = reinterpret_cast<Fn>(dlsym(gOrtHandle, "OrtSessionOptionsAppendExecutionProvider_CPU"));
#endif
    }
    if (!fn)
        return false;
    OrtStatusPtr st = fn(so, 1);
    if (st) {
        Ort::GetApi().ReleaseStatus(st);
        return false;
    }
    return true;
}

// Appends the fastest GPU execution provider for the platform with the CPU EP
// registered last for operators the GPU cannot run (the same provider order as
// the Node reference: [gpu, 'cpu']):
//   Windows  DirectML — D3D12, NVIDIA/AMD/Intel, statically built into the
//            onnxruntime.dll shipped by build.py
//   macOS    CoreML   — Apple Neural Engine / GPU, in the darwin dylib shipped
//            by build.py; reached through the generic append (ORT has no
//            dedicated CoreML C API entry)
//   Linux    CUDA     — only enabled when the runtime the user placed next to
//            the exe supports it (CUDA/cuDNN are never bundled)
// Returns the provider label ("DirectML" / "CoreML" / "CUDA"), or "" when the
// GPU provider could not be appended (the session then runs CPU-only); `why`
// holds the failure reason for a one-time warning.
static std::string appendGpuProvider(Ort::SessionOptions &so, std::string &why)
{
    why.clear();
#if defined(_WIN32)
    const void *dmlPtr = nullptr;
    OrtStatusPtr st = Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, &dmlPtr);
    if (st) {
        const char *m = Ort::GetApi().GetErrorMessage(st);
        why = m && *m ? m : "DirectML provider is not in this onnxruntime build";
        Ort::GetApi().ReleaseStatus(st);
    } else if (dmlPtr) {
        const auto *dml = static_cast<const OrtDmlApi *>(dmlPtr);
        st = dml->SessionOptionsAppendExecutionProvider_DML(so, 0);
        if (!st) {
            appendCpuFallback(so);  // DML first, CPU last
            return "DirectML";
        }
        const char *m = Ort::GetApi().GetErrorMessage(st);
        why = m && *m ? m : "DirectML provider is not usable";
        Ort::GetApi().ReleaseStatus(st);
    } else {
        why = "DirectML provider is not in this onnxruntime build";
    }
#elif defined(__APPLE__)
    try {
        so.AppendExecutionProvider("CoreML", {});
        appendCpuFallback(so);
        return "CoreML";
    } catch (const Ort::Exception &e) {
        why = e.what();
    }
#elif defined(__linux__)
    try {
        Ort::CUDAProviderOptions cuda;
        so.AppendExecutionProvider_CUDA_V2(*cuda);
        appendCpuFallback(so);
        return "CUDA";
    } catch (const Ort::Exception &e) {
        why = e.what();
    }
#endif
    return "";
}

// Loads (and caches) the model for a path: Env + Session are created once per
// distinct path, input names/types are read at load time, and on the DirectML
// path a session-creation failure (no DirectML.dll, no D3D12 GPU, ...) is
// retried with CPU-only options. Throws Ort::Exception on real failures; a
// freshly created model prints one informational line naming the provider.
static std::shared_ptr<const OnnxSrModel> loadModel(const std::string &modelPath)
{
    {
        std::lock_guard<std::mutex> lock(gModelMutex);
        const auto it = gModels.find(modelPath);
        if (it != gModels.end())
            return it->second;
    }

    auto model = std::make_shared<OnnxSrModel>();
    model->env.reset(new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "imagepolish"));

#if defined(_WIN32)
    const std::wstring op = toOrtPath(modelPath);
#else
    const std::string &op = modelPath;
#endif

    std::string why;
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    const std::string gpu = appendGpuProvider(so, why);
    if (!gpu.empty())
        so.DisableMemPattern();  // GPU EPs do their own memory planning

    try {
        model->session.reset(new Ort::Session(*model->env, op.c_str(), so));
        model->provider = gpu.empty() ? "CPU" : gpu;
    } catch (const Ort::Exception &e) {
        if (gpu.empty())
            throw;  // CPU-only session failed: that is a real error
        // The GPU provider could not initialize (missing runtime pieces, no
        // accelerator, driver mismatch, ...): retry with CPU-only options and
        // warn once below.
        std::string note = gpu + " unavailable";
        if (!why.empty())
            note += " (" + why + ")";
        note += "; retrying on CPU: ";
        note += e.what();
        onnxWarn("warning: --model: " + note);
        Ort::SessionOptions cpuSo;
        cpuSo.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        model->session.reset(new Ort::Session(*model->env, op.c_str(), cpuSo));
        model->provider = "CPU";
    }

    Ort::AllocatorWithDefaultOptions alloc;
    model->inName = model->session->GetInputNameAllocated(0, alloc).get();
    model->outName = model->session->GetOutputNameAllocated(0, alloc).get();
    model->inType = model->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType();
    if (model->inType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
        model->inType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
        throw Ort::Exception("model input must be tensor(float) or tensor(float16)",
                             ORT_INVALID_GRAPH);
    model->version = gOrtVersion;

    {
        std::lock_guard<std::mutex> lock(gModelMutex);
        const auto [it, inserted] = gModels.emplace(modelPath, model);
        if (!inserted)
            return it->second;          // lost a creation race: our copy is discarded
    }

    std::fprintf(stderr, "[onnx] %s: %s execution provider (onnxruntime %s)\n",
                 baseName(modelPath).c_str(), model->provider.c_str(), model->version.c_str());
    return model;
}

} // namespace

OnnxSrStatus runOnnxSr(const std::string &modelPath, const u8 *rgb, int w, int h,
                       std::vector<u8> &out, int &outW, int &outH, std::string &err)
{
    const OrtApiBase *base = loadOrtBase(err);
    if (!base)
        return OnnxSrStatus::Skip;
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    if (!api) {
        err = err.empty() ? "onnxruntime library is older than the API version this build needs"
                          : err + " (or too old for this build)";
        return OnnxSrStatus::Skip;
    }
    Ort::InitApi(api);

    std::shared_ptr<const OnnxSrModel> model;
    try {
        model = loadModel(modelPath);
    } catch (const Ort::Exception &e) {
        err = e.what();
        return OnnxSrStatus::Error;
    }

    // Some SR models are exported with divisibility assumptions (2xGTv6 needs
    // an even input height); pad both sides to even with a replicated border,
    // run, and crop the output back to the region of the original input. The
    // crop is scale*original, with the scale taken from what the model
    // actually produced for the padded input (2x for all bundled models).
    const int pw = w + (w & 1);
    const int ph = h + (h & 1);
    std::vector<u8> padRgb;
    if (pw != w || ph != h) {
        padRgb.resize(std::size_t(pw) * ph * 3);
        for (int y = 0; y < ph; y++) {
            const int sy = y < h ? y : h - 1;
            for (int x = 0; x < pw; x++) {
                const int sx = x < w ? x : w - 1;
                const u8 *s = rgb + (std::size_t(sy) * w + sx) * 3;
                u8 *d = &padRgb[(std::size_t(y) * pw + x) * 3];
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
        rgb = padRgb.data();
    }
    const size_t npx = std::size_t(pw) * ph;

    try {
        const bool fp16 = model->inType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;

        // NCHW planes -> [0,1] values. All three bundled models are trained on
        // 0..1 RGB; if a model expected 0..255 this is the single place to
        // change. fp16 buffers hold raw binary16 bits (header guarantees
        // sizeof(Ort::Float16_t) == 2 and IEEE layout).
        std::vector<float> fin;
        std::vector<uint16_t> hin;
        if (fp16)
            hin.resize(npx * 3);
        else
            fin.resize(npx * 3);
        for (int y = 0; y < ph; y++) {
            const u8 *row = rgb + std::size_t(y) * pw * 3;
            for (int x = 0; x < pw; x++) {
                const size_t i = std::size_t(y) * pw + x;   // NCHW: plane-major
                for (int c = 0; c < 3; c++) {
                    const float v = row[3 * x + c] * (1.0f / 255.0f);
                    if (fp16) {
                        const Ort::Float16_t f(v);
                        uint16_t bits;
                        std::memcpy(&bits, &f, sizeof bits);
                        hin[c * npx + i] = bits;
                    } else {
                        fin[c * npx + i] = v;
                    }
                }
            }
        }

        const int64_t dims[4] = { 1, 3, ph, pw };
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inVal = fp16
            ? Ort::Value::CreateTensor(mem, (void *)hin.data(), hin.size() * sizeof(uint16_t),
                                       dims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
            : Ort::Value::CreateTensor(mem, (void *)fin.data(), fin.size() * sizeof(float),
                                       dims, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);

        Ort::RunOptions ro;
        const char *inCStr = model->inName.c_str();
        const char *outCStr = model->outName.c_str();
        std::vector<Ort::Value> rv =
            model->session->Run(ro, &inCStr, &inVal, 1, &outCStr, 1);

        const auto shape = rv[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 || shape[1] != 3) {
            err = "model output must be a 4D NCHW RGB tensor";
            return OnnxSrStatus::Error;
        }
        const int mw = int(shape[3]);
        const int mh = int(shape[2]);

        // center crop of the model output for the original (pre-pad) image
        const int tw = (int)std::lround(double(mw) * w / pw);
        const int th = (int)std::lround(double(mh) * h / ph);
        outW = tw < 1 ? 1 : tw > mw ? mw : tw;
        outH = th < 1 ? 1 : th > mh ? mh : th;
        const int ox = (mw - outW) / 2;
        const int oy = (mh - outH) / 2;
        const size_t onpx = std::size_t(mw) * mh;
        out.resize(std::size_t(outW) * outH * 3);

        const auto outType = rv[0].GetTensorTypeAndShapeInfo().GetElementType();
        const void *raw = rv[0].GetTensorRawData();
        if (outType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            outType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            err = "model output must be tensor(float) or tensor(float16)";
            return OnnxSrStatus::Error;
        }
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                const size_t si = std::size_t(y + oy) * mw + (x + ox);  // NCHW plane-major
                for (int c = 0; c < 3; c++) {
                    float v = outType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16
                        ? float(Ort::Float16_t::FromBits(((const uint16_t *)raw)[c * onpx + si]))
                        : ((const float *)raw)[c * onpx + si];
                    // models slightly overshoot [0,1] (e.g. -0.16); clamp to u8
                    int t = (int)(v * 255.0f + 0.5f);
                    out[(std::size_t(y) * outW + x) * 3 + c] = (u8)(t < 0 ? 0 : t > 255 ? 255 : t);
                }
            }
        }
        return OnnxSrStatus::Ok;
    } catch (const Ort::Exception &e) {
        err = e.what();
        return OnnxSrStatus::Error;
    }
}