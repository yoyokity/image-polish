#!/usr/bin/env python3
"""Build imagepolish with zig cc.

    python build.py            build for the current platform -> out/imagepolish.exe
    python build.py --all      cross-compile for all target platforms ->
                               out/imagepolish-<version>-<target>[.exe]

The release version is read from src/version.h (single source of truth) and
baked into the cross-compiled artifact names. .pdb debug symbol files are
removed after each build.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
VERSION = re.search(
    r"IMAGEPOLISH_VERSION\s+([\d.]+)", (ROOT / "src" / "version.h").read_text()
).group(1)

SOURCES = [
    "src/main.cpp", "src/chain.cpp", "src/color.cpp", "src/imageio.cpp",
    "src/filters/eedi2.cpp", "src/filters/resample.cpp", "src/filters/repair.cpp",
    "src/filters/nlmeans.cpp", "src/filters/dehalo.cpp", "src/filters/cas.cpp",
    "src/filters/deband.cpp", "src/filters/sangnom.cpp",
]
COMMON = ["-O2", "-std=c++17", "-Wall", "-Wextra",
          "-Wno-missing-field-initializers", "-Isrc"]

# (zig target, file extension) for the --all build
TARGETS = [
    ("x86_64-windows-gnu", ".exe"),
    ("aarch64-windows-gnu", ".exe"),
    ("x86_64-linux-musl", ""),
    ("aarch64-linux-musl", ""),
    ("aarch64-macos", ""),
]


def zig(target, output):
    cmd = [sys.executable, "-m", "ziglang", "c++"] + COMMON + SOURCES + ["-o", output]
    if target:
        cmd += ["-target", target]
    subprocess.run(cmd, check=True)


def main():
    (ROOT / "out").mkdir(exist_ok=True)

    if "--all" in sys.argv:
        # remove stale version-tagged artifacts before rebuilding
        for pattern in ("imagepolish-*.exe", "imagepolish-*-linux-musl", "imagepolish-*-macos"):
            for p in (ROOT / "out").glob(pattern):
                p.unlink()
        for target, ext in TARGETS:
            zig(target, str(ROOT / "out" / f"imagepolish-{VERSION}-{target}{ext}"))
    else:
        zig(None, str(ROOT / "out" / "imagepolish.exe"))

    # drop debug symbol files produced alongside Windows targets
    for p in (ROOT / "out").glob("*.pdb"):
        p.unlink()


if __name__ == "__main__":
    main()