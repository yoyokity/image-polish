#!/usr/bin/env python3
"""Build imagepolish with zig cc.

    python build.py            build for the current platform -> out/imagepolish[.exe]
                              plus the ONNX runtime and the models/ folder
    python build.py --all      cross-compile all target platforms and zip each -> out/ zips

The release version is read from src/version.h (single source of truth). In
the --all mode every binary is built into a temporary out/<target>/... folder,
zipped straight into out/ and the folder removed again, so `out/` only holds
the zip files, each expanding to `imagepolish/imagepolish[.exe]`. .pdb debug
symbols are removed after builds.

Packaging. Every platform ships two families of zip:
  * imagepolish-<version>-<target>.zip — the plain build: the executable
    only. Nothing ONNX-related is included, so `--model` is skipped with a
    warning (no runtime, no models).
  * imagepolish-ai-<version>-<target>[-<cpu>].zip — the ONNX-enabled build:
    the executable plus the ONNX runtime listed below and the models/ folder.
    GPU-capable by default; only the CPU-only Linux variants carry `-cpu`.
      Windows (x64/arm64):  DirectML  (onnxruntime.dll + DirectML.dll +
        dxcompiler.dll + dxil.dll)
      macOS (arm64):        CoreML    (libonnxruntime.dylib)
      Linux x64:            CUDA      (libonnxruntime.so +
        libonnxruntime_providers_{cuda,shared}.so; needs a NVIDIA driver and
        matching CUDA/cuDNN on the system, otherwise the step falls back to CPU)
      Linux x64 -cpu:       CPU       (libonnxruntime.so)
      Linux arm64 -cpu:     CPU       (libonnxruntime.so)

Runtimes are fetched from npm onnxruntime-node (DirectML/CoreML/CPU binaries)
and the NuGet Microsoft.ML.OnnxRuntime.Gpu.Linux package (CUDA providers),
both for ORT_VER below, cached in .ort-cache/. If you drop your own runtime
files next to a built binary they are left untouched.
"""
import base64
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
VERSION = re.search(
    r"IMAGEPOLISH_VERSION\s+([\d.]+)", (ROOT / "src" / "version.h").read_text()
).group(1)

SOURCES = [
    "src/main.cpp", "src/chain.cpp", "src/color.cpp", "src/imageio.cpp",
    "src/filters/eedi2.cpp", "src/filters/resample.cpp", "src/filters/repair.cpp",
    "src/filters/nlmeans.cpp", "src/filters/dehalo.cpp", "src/filters/cas.cpp",
    "src/filters/deband.cpp", "src/filters/sangnom.cpp", "src/filters/grain.cpp",
    "src/filters/onnxsr.cpp",
]
COMMON = ["-O2", "-std=c++17", "-Wall", "-Wextra",
          "-Wno-missing-field-initializers", "-Isrc"]
# zig ships its own libc++ whose headers lack nullability annotations; without
# this suppression -Wall -Wextra floods the build log on some targets.
COMMON += ["-Wno-nullability-completeness"]

# (zig target, binary file name, [(zip suffix, runtime kind), ...]) — the first
# variant is the default (shipped next to the binary by "python build.py").
# Runtime kinds: dml / coreml / linux_cpu / linux_gpu.
PLATFORMS = [
    ("x86_64-windows-gnu", "imagepolish.exe", [("", "dml")]),
    ("aarch64-windows-gnu", "imagepolish.exe", [("", "dml")]),
    ("x86_64-linux-gnu", "imagepolish", [("", "linux_gpu"), ("-cpu", "linux_cpu")]),
    ("aarch64-linux-gnu", "imagepolish", [("-cpu", "linux_cpu")]),
    ("aarch64-macos", "imagepolish", [("", "coreml")]),
]

# zip DEFLATE level: 0 (store) .. 9 (smallest); 5 is a balanced default
COMPRESS_LEVEL = 5

# ---------------------------------------------------------------------------
# ONNX runtime provisioning
# ---------------------------------------------------------------------------
# Must line up with ORT_API_VERSION in src/onnx/onnxruntime_c_api.h; bump the
# headers and this together.
ORT_VER = "1.29.0"
ORT_WIN_FILES = ["onnxruntime.dll", "DirectML.dll", "dxcompiler.dll", "dxil.dll"]
ORT_MACOS_DYLIB = "libonnxruntime.dylib"
ORT_LINUX_CPU_DYLIB = "libonnxruntime.so"
ORT_LINUX_GPU_FILES = ["libonnxruntime.so", "libonnxruntime_providers_cuda.so",
                       "libonnxruntime_providers_shared.so"]


def is_windows_target(target: str) -> bool:
    return target.startswith("x86_64-windows") or target.startswith("aarch64-windows")


def ort_arch(target: str) -> str:
    """npm arch name ("x64" / "arm64") for any target."""
    if target.startswith("x86_64"):
        return "x64"
    if target.startswith("aarch64"):
        return "arm64"
    raise ValueError(f"unknown target: {target}")


def _download_ort_tarball(tarball: pathlib.Path):
    """Fetch the onnxruntime-node package for ORT_VER and verify its sha512."""
    reg_url = f"https://registry.npmjs.org/onnxruntime-node/{ORT_VER}"
    with urllib.request.urlopen(reg_url, timeout=30) as r:
        meta = json.loads(r.read().decode())
    dist = meta["dist"]
    print(f"  downloading onnxruntime-node-{ORT_VER} ({dist['tarball']})")
    urllib.request.urlretrieve(dist["tarball"], tarball)
    expected = dist["integrity"]  # "sha512-<base64>"
    actual = base64.b64encode(hashlib.sha512(tarball.read_bytes()).digest()).decode()
    if expected != f"sha512-{actual}":
        raise SystemExit(f"integrity check failed for {tarball.name}")


def _ort_tarball() -> pathlib.Path:
    """Path to the cached onnxruntime-node tarball (downloads it once, with the
    package sha512 verified, when missing)."""
    cache = ROOT / ".ort-cache"
    cache.mkdir(exist_ok=True)
    tarball = cache / f"onnxruntime-node-{ORT_VER}.tgz"
    if tarball.exists():
        return tarball
    try:
        _download_ort_tarball(tarball)
    except Exception as e:  # noqa: BLE001 - surface a useful message
        raise SystemExit(
            f"cannot fetch the ONNX runtime ({e}); place the runtime files "
            f"manually next to the binary and rerun") from e
    return tarball


def _ort_gpu_nupkg() -> pathlib.Path:
    """Path to the cached Microsoft.ML.OnnxRuntime.Gpu.Linux nupkg (zip)."""
    cache = ROOT / ".ort-cache"
    cache.mkdir(exist_ok=True)
    pkg = cache / f"onnxruntime-gpu-linux-{ORT_VER}.nupkg"
    if pkg.exists():
        return pkg
    url = (f"https://api.nuget.org/v3-flatcontainer/"
           f"microsoft.ml.onnxruntime.gpu.linux/{ORT_VER}/"
           f"microsoft.ml.onnxruntime.gpu.linux.{ORT_VER}.nupkg")
    print(f"  downloading CUDA runtime ({ORT_VER})")
    try:
        urllib.request.urlretrieve(url, pkg)
    except Exception as e:  # noqa: BLE001 - surface a useful message
        raise SystemExit(
            f"cannot fetch the CUDA runtime ({e}); the GPU zip will be skipped; "
            f"download it manually from {url}") from e
    return pkg


def ort_cache_dir(platform_dir: str, arch: str, label: str) -> pathlib.Path:
    """Directory holding extracted runtime files for a platform+arch."""
    arc = ROOT / ".ort-cache" / f"onnxruntime-{ORT_VER}-{platform_dir}-{label}-{arch}"
    arc.mkdir(parents=True, exist_ok=True)
    return arc


def ort_win_cache_dir(arch: str) -> pathlib.Path:
    arc = ort_cache_dir("node", arch, "win")
    if all((arc / f).exists() for f in ORT_WIN_FILES):
        return arc
    prefix = f"package/bin/napi-v6/win32/{arch}/"
    with tarfile.open(_ort_tarball(), "r:gz") as tf:
        for name in tf.getnames():
            if name.startswith(prefix) and name.endswith(".dll"):
                dst = arc / os.path.basename(name)
                dst.write_bytes(tf.extractfile(name).read())
    missing = [f for f in ORT_WIN_FILES if not (arc / f).exists()]
    if missing:
        raise SystemExit(f"onnxruntime-node-{ORT_VER} lacks {missing} for {arch}")
    return arc


def ort_darwin_cache_dir() -> pathlib.Path:
    arc = ort_cache_dir("node", "arm64", "darwin")
    dylib = arc / ORT_MACOS_DYLIB
    if dylib.exists():
        return arc
    name = f"package/bin/napi-v6/darwin/arm64/libonnxruntime.{ORT_VER}.dylib"
    with tarfile.open(_ort_tarball(), "r:gz") as tf:
        dylib.write_bytes(tf.extractfile(name).read())
    return arc


def ort_linux_cpu_cache_dir(arch: str) -> pathlib.Path:
    arc = ort_cache_dir("node", arch, "linux-cpu")
    so = arc / ORT_LINUX_CPU_DYLIB
    if so.exists():
        return arc
    name = f"package/bin/napi-v6/linux/{arch}/libonnxruntime.so.1"
    with tarfile.open(_ort_tarball(), "r:gz") as tf:
        so.write_bytes(tf.extractfile(name).read())
    return arc


def ort_linux_gpu_cache_dir() -> pathlib.Path:
    arc = ort_cache_dir("gpu", "x64", "linux")
    if all((arc / f).exists() for f in ORT_LINUX_GPU_FILES):
        return arc
    pkg = _ort_gpu_nupkg()
    prefix = "runtimes/linux-x64/native/"
    with zipfile.ZipFile(pkg) as zf:
        for f in ORT_LINUX_GPU_FILES:
            arc.joinpath(f).write_bytes(zf.read(prefix + f))
    return arc


def install_ort_windows(bindir: pathlib.Path, target: str):
    """Copy the DirectML runtime next to the built exe (unless the user already
    provided their own onnxruntime.dll there)."""
    if (bindir / "onnxruntime.dll").exists():
        print("  onnxruntime.dll already present, not overwriting")
        return
    arc = ort_win_cache_dir(ort_arch(target))
    for f in ORT_WIN_FILES:
        shutil.copy2(arc / f, bindir / f)
    print(f"  {target}: shipped {', '.join(ORT_WIN_FILES)}")


def install_ort_macos(bindir: pathlib.Path):
    """Copy the CoreML-capable onnxruntime dylib next to the built binary
    (unless the user already provided their own)."""
    if (bindir / ORT_MACOS_DYLIB).exists():
        print(f"  {ORT_MACOS_DYLIB} already present, not overwriting")
        return
    shutil.copy2(ort_darwin_cache_dir() / ORT_MACOS_DYLIB, bindir / ORT_MACOS_DYLIB)
    print(f"  shipped {ORT_MACOS_DYLIB}")


def install_ort_linux_cpu(bindir: pathlib.Path, target: str):
    """Copy the CPU onnxruntime .so next to the built binary (unless the user
    already provided their own)."""
    if (bindir / ORT_LINUX_CPU_DYLIB).exists():
        print("  libonnxruntime.so already present, not overwriting")
        return
    shutil.copy2(ort_linux_cpu_cache_dir(ort_arch(target)) / ORT_LINUX_CPU_DYLIB,
                 bindir / ORT_LINUX_CPU_DYLIB)
    print(f"  {target}: shipped libonnxruntime.so")


def install_ort_linux_gpu(bindir: pathlib.Path, target: str):
    """Copy the CUDA-enabled runtime (main lib + CUDA/shared providers) next to
    the built binary (unless the user already provided their own). The three
    files must be kept together. CUDA is used only when the system has a NVIDIA
    driver and matching CUDA/cuDNN; the step falls back to CPU otherwise."""
    if (bindir / "libonnxruntime.so").exists():
        print("  libonnxruntime.so already present, not overwriting")
        return
    arc = ort_linux_gpu_cache_dir()
    for f in ORT_LINUX_GPU_FILES:
        shutil.copy2(arc / f, bindir / f)
    print(f"  {target}: shipped {', '.join(ORT_LINUX_GPU_FILES)}")


def install_runtime(bindir: pathlib.Path, target: str, kind: str):
    if kind == "dml":
        install_ort_windows(bindir, target)
    elif kind == "coreml":
        install_ort_macos(bindir)
    elif kind == "linux_cpu":
        install_ort_linux_cpu(bindir, target)
    elif kind == "linux_gpu":
        install_ort_linux_gpu(bindir, target)
    else:
        raise ValueError(f"unknown runtime kind: {kind}")


def linux_runtime_paths(kind: str):
    if kind == "linux_cpu":
        return [ORT_LINUX_CPU_DYLIB]
    if kind == "linux_gpu":
        return ORT_LINUX_GPU_FILES
    return []


def zig(target, output):
    cmd = [sys.executable, "-m", "ziglang", "c++"] + COMMON + SOURCES + ["-o", output]
    if target:
        cmd += ["-target", target]
    subprocess.run(cmd, check=True)


def rm_retry(path, tries=8):
    """Remove a file or directory, retrying briefly: freshly written binaries
    can be held for a moment by antivirus/indexing on Windows."""
    for _ in range(tries):
        try:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
            return
        except PermissionError:
            time.sleep(0.5)
    raise


def host_target():
    arch = {"amd64": "x86_64", "x86_64": "x86_64",
            "aarch64": "aarch64", "arm64": "aarch64"}.get(
        platform.machine().lower(), platform.machine().lower())
    if os.name == "nt":
        return f"{arch}-windows-gnu"
    if sys.platform == "darwin":
        return f"{arch}-macos"
    return f"{arch}-linux-gnu"


def build_and_zip(target, name, variants, out):
    tmp = out / target
    builddir = tmp / "build"
    builddir.mkdir(parents=True)

    zig(target, str(builddir / name))

    # plain build: the executable only — no ONNX runtime, no models
    plain = tmp / "imagepolish"
    plain.mkdir(parents=True)
    shutil.copy2(builddir / name, plain / name)
    plain_zip = out / f"imagepolish-{VERSION}-{target}.zip"
    with zipfile.ZipFile(plain_zip, "w", zipfile.ZIP_DEFLATED,
                         compresslevel=COMPRESS_LEVEL) as z:
        z.write(plain / name, arcname=f"imagepolish/{name}")
    print(f"  {target}: {plain_zip.name}")

    # AI builds: ONNX runtime + models/
    models = sorted((ROOT / "models").glob("*.onnx"))
    for suffix, kind in variants:
        vdir = tmp / ("imagepolish-ai" + suffix)
        vdir.mkdir(parents=True)
        shutil.copy2(builddir / name, vdir / name)
        install_runtime(vdir, target, kind)

        zip_path = out / f"imagepolish-ai-{VERSION}-{target}{suffix}.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED,
                             compresslevel=COMPRESS_LEVEL) as z:
            z.write(vdir / name, arcname=f"imagepolish/{name}")
            if kind == "dml":
                for f in ORT_WIN_FILES:
                    z.write(vdir / f, arcname=f"imagepolish/{f}")
            elif kind == "coreml":
                z.write(vdir / ORT_MACOS_DYLIB, arcname=f"imagepolish/{ORT_MACOS_DYLIB}")
            elif kind == "linux_cpu" or kind == "linux_gpu":
                for f in linux_runtime_paths(kind):
                    z.write(vdir / f, arcname=f"imagepolish/{f}")
            for m in models:
                z.write(m, arcname=f"imagepolish/models/{m.name}")
        print(f"  {target}{suffix}: {zip_path.name}")

    rm_retry(tmp)


def main():
    out = ROOT / "out"
    out.mkdir(exist_ok=True)

    if "--all" in sys.argv or "-all" in sys.argv:
        # clean slate: out/ only ever holds the zips produced below
        for p in out.glob("*"):
            rm_retry(p)
        for target, name, variants in PLATFORMS:
            build_and_zip(target, name, variants, out)
    else:
        spec = next((p for p in PLATFORMS if p[0] == host_target()), None)
        if spec is None:
            raise SystemExit(f"this host has no build target: {host_target()}")
        target, name, variants = spec
        zig(target, str(out / name))
        install_runtime(out, target, variants[0][1])   # default = GPU-capable
        # ship the models/ folder next to the binary (like the -ai zips)
        models = sorted((ROOT / "models").glob("*.onnx"))
        dest = out / "models"
        if dest.exists():
            rm_retry(dest)
        dest.mkdir(parents=True)
        for m in models:
            shutil.copy2(m, dest / m.name)
        print(f"  {target}: {out / name} (+ models/)")

    # drop debug symbol files produced alongside Windows targets
    for p in out.glob("*.pdb"):
        p.unlink()


if __name__ == "__main__":
    main()