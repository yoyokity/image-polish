#!/usr/bin/env python3
"""Build imagepolish with zig cc.

    python build.py            build for the current platform -> out/imagepolish.exe
    python build.py --all      cross-compile all target platforms and zip each ->
                               out/imagepolish-<version>-<target>.zip  (-all also works)

The release version is read from src/version.h (single source of truth). In
the --all mode every binary is built into a temporary out/<target>/imagepolish/
folder (binary named `imagepolish`), zipped straight into out/ and the folder
removed again, so `out/` only holds the zip files, each expanding to
`imagepolish/imagepolish[.exe]`. .pdb debug symbols are removed after builds.
"""
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time
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
]
COMMON = ["-O2", "-std=c++17", "-Wall", "-Wextra",
          "-Wno-missing-field-initializers", "-Isrc"]

# (zig target, binary file name) per platform
TARGETS = [
    ("x86_64-windows-gnu", "imagepolish.exe"),
    ("aarch64-windows-gnu", "imagepolish.exe"),
    ("x86_64-linux-musl", "imagepolish"),
    ("aarch64-linux-musl", "imagepolish"),
    ("aarch64-macos", "imagepolish"),
]

# zip DEFLATE level: 0 (store) .. 9 (smallest); 5 is a balanced default
COMPRESS_LEVEL = 5


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
    return f"{arch}-linux-musl"


def build_and_zip(target, name, out):
    tmp = out / target
    bindir = tmp / "imagepolish"
    bindir.mkdir(parents=True)

    zig(target, str(bindir / name))

    zip_path = out / f"imagepolish-{VERSION}-{target}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED,
                         compresslevel=COMPRESS_LEVEL) as z:
        z.write(bindir / name, arcname=f"imagepolish/{name}")
    print(f"  {target}: {zip_path.name}")

    rm_retry(tmp)


def main():
    out = ROOT / "out"
    out.mkdir(exist_ok=True)

    if "--all" in sys.argv or "-all" in sys.argv:
        # clean slate: out/ only ever holds the zips produced below
        for p in out.glob("*"):
            rm_retry(p)
        for target, name in TARGETS:
            build_and_zip(target, name, out)
    else:
        target = host_target()
        name = "imagepolish.exe" if os.name == "nt" else "imagepolish"
        zig(target, str(out / name))
        print(f"  {target}: {out / name}")

    # drop debug symbol files produced alongside Windows targets
    for p in out.glob("*.pdb"):
        p.unlink()


if __name__ == "__main__":
    main()