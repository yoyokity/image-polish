# ImagePolish

**[English](README.md) | [简体中文](README_zh.md)**

> An image flaw fixing tool.

`imagepolish` packs the image filters commonly used in the VapourSynth / AviSynth ecosystem (anti-aliasing, denoise, dehalo, deband, sharpen, resize) into a single command-line program, implemented from scratch in C++.

![ImagePolish demo result](preview/img.webp)

**Parameters used for the image above:**

```bash
imagepolish -i input.png --denoise --deband range=12,y=60,cbcr=24 --deband range=24,y=40,cbcr=16 --aa 2 --sharpen 0.9 --dehalo --grain 10
```



## Features

- **Filters combine freely**: EEDI2 anti-aliasing, NLMeans denoise, spline36 resize, FineDehalo, CAS sharpen and neo_f3kdb deband run in command-line order; steps can be repeated, omitted, and arranged arbitrarily.
- **Lossless chroma**: chroma never goes through lossy 8-bit YUV quantization. The "luma delta" is added back to the original RGB — zero chroma loss, exactly matching the VapourSynth `ShufflePlanes` semantics.
- **Multi-file batch**: `-i` can be repeated; inputs are processed serially and each gets its own output.
- **Multithreaded**: filters are processed in parallel threads.



## Quick Start

```bash
# Deband + anti-aliasing with defaults
imagepolish -i in.png --deband --aa

# Denoise (strength 5) + resize to 1920 wide
imagepolish -i in.png --resize 1920x --denoise 5 -o out.png

# Batch process several images (-o is ignored; outputs are <name>_output.<ext>)
imagepolish -i a.png -i b.png -i c.png --deband y=40
```

Without `-o`, the output is `<input basename>_output.<same extension>` (e.g. `in.png` → `in_output.png`); with no steps at all the input passes through unchanged.



## Usage

### Basic commands

| Option | Description |
|---|---|
| `-i, --input <file>` | Input image; repeatable for batch processing. Formats: PNG / BMP / TGA / JPG / PGM / PPM / PNM, gray or RGB |
| `-o, --output <file>` | Output path; format from extension (`.png` `.bmp` `.tga` `.jpg/.jpeg` `.pgm` `.ppm`). Defaults to `<name>_output.<input ext>` |
| `--quality <q>` | JPEG output quality (`1~100`, default `80`; `92` is visually lossless). Only affects `.jpg/.jpeg` outputs |
| `-h, --help` | Print help |

### Filters

- Steps run in the order they appear on the command line, each applied to the previous result.
- The same filter may be invoked multiple times.

| Option | Meaning |
|---|---|
| `--resize WxH` | spline36 resize; one side may be omitted (`1920x` / `x1080`), the other is kept proportional |
| `--denoise [N]` | NLMeans denoise; `N` is the strength `h` (`1~8`, default `5`) |
| `--deband [name=value,...]` | neo_f3kdb deband (sample_mode=2): `range` (`0~255`, default `24`), `y` (`0~511`, default `72`), `cbcr` (`0~511`, default `32`) |
| `--aa [N]` | Anti-aliasing; `N` is the strength (`1`, `2`, default `2`) |
| `--sharpen [s]` | CAS sharpen; `s` is sharpness (`0~1`, default `0.7`) |
| `--dehalo` | Dehalo, fixed parameters |
| `--grain [h]` | Film grain; `h` is the noise variance (`0~100`, default `10`) |

Examples:

```bash
imagepolish -i in.png --deband               # defaults 24,72,32
imagepolish -i in.png --deband y=40          # luma threshold only
imagepolish -i in.png --deband range=16,cbcr=48
imagepolish -i in.png --resize 1920x --aa --deband --denoise 5 -o out.png
```

### Notes

- Order filters from top to bottom of the table above for best results (e.g. do not put anti-aliasing before denoise).

- AA strength 2 gives excellent results but slightly softens the image; a bit of sharpening compensates for it.

- For better banding removal across different band sizes, run deband twice:

  ```bash
  imagepolish -i in.png --deband range=12,y=60,cbcr=24 --deband range=24,y=40,cbcr=16
  ```

- Adding `--grain` at the very end is highly recommended: a layer of noise preserves detail and reduces the smeared look left behind by restoration.

- If you are not sure which filters to use, this one-liner covers everything:

  ```bash
  imagepolish -i in.png --denoise --deband range=12,y=60,cbcr=24 --deband range=24,y=40,cbcr=16 --aa --sharpen 0.9 --dehalo --quality 92
  ```



## Build

> **Prerequisites**:
>
> Python 3.x with pip. `pip install ziglang` installs the whole toolchain (zig cc) together with the runtimes for every target platform — no LLVM / MinGW installation and no PATH setup needed.

VS Code users: just run the build task (`Ctrl+Shift+B`): `build` compiles for the current platform, `build all platforms` cross-compiles every target.

**From the command line:**

```bash
python build.py          # current platform -> out/imagepolish.exe
python build.py --all    # all platforms -> out/imagepolish-<version>-<target>[.exe]
```

The version lives in a single place, `src/version.h` (check it with `--version`); the build script also removes the `.pdb` debug symbols that Windows targets produce. See `build.py` for the list of target platforms covered by `--all`.



## Implementation

This is not a plugin wrapper — every filter is an independent C++ port of the reference implementations, line by line.

### Color path

Only the BT.601 full-range luma is processed; chroma never goes through lossy 8-bit YUV quantization. The "luma delta" is added to all three RGB channels of the original — equivalent to `ShufflePlanes([aa, src], YUV)` ("replace luma, keep chroma") with zero chroma quantization error. Flat regions roundtrip exactly (1000 random colors tested; the only exception is the ~0.1% whose luma is exactly `x.5`, which shift all channels by `+1`).

### Filters

- **EEDI2** (line-by-line port of tritical / HolyWu v0.9.2): 14-stage pipeline — edge mask → erode/dilate/remove tiny horizontal gaps → direction calc (5 candidates, median with limlut clamps) → direction filtering/dilate/gap fill → per-row assembly → 2× upsampling → 2× direction processing → lattice interpolation (nt8 main search + nt7 fallback) → post-processing.
- **Resample** (fmtconv conventions): `srcPos(o) = (o+0.5)·ratio - 0.5 + shift` (pixel-center aligned), spline36 kernel scaled by `max(ratio, 1)` and normalized; `--resize` uses the same kernel with shift=0 (separable: transpose → vertical → transpose).
- **Repair mode 2**: `clamp(a, n[1], n[7])` over the sorted 3×3 neighborhood of reference `b`; border rows/cols copy `a`.
- **NLMeans** (port of vs-nlm-ispc / KNLMeansCL drop-in; d=0, a=2, s=4, wmode=3, wref=1): single-frame spatial denoise (d=0, no temporal). For each of the 12 offsets in the upper triangle of the patch: `3·Δ²/255²`, box-filtered over `|j| <= s` horizontally then vertically into window distance `d²`; weight `w = max(1 - d²·κ, 0)⁸` with `κ = 255²/(3·h²·81)`, `h` from `--denoise`; output `(wref·maxw·src + Σw·v)/(wref·maxw + Σw)`.
- **FineDehalo** (port of havsfunc `haf.FineDehalo`, havsfunc defaults: DeHalo_alpha rx=2/ry=2, darkstr=brightstr=1, lowsens=highsens=50, ss=1.5; FineDehalo thmi=80/thma=128, thlimi=50/thlima=100, excl=True): DeHalo_alpha dehaloring (Bicubic(1/3,1/3) blurred halo layer minus source, 1.5× Lanczos upsampled min/max clamped, blended in luma) → Prewitt edges + two luma thresholds (thmi/thma main, thlimi/thlima weak) + rect/diamond morphology + 3×3 box "exclusion mask" → `MaskedMerge` replaces only mask-255 pixels. Expr steps reproduce the reference's per-op float + round-half-even (rint) semantics.
- **CAS** (port of HolyWu / VapourSynth-CAS, i.e. AMD FidelityFX CAS): 3×3 soft min/max drives contrast-adaptive amplitude `amp = sqrt(clamp(min(mn, limit-mx)/mx))`, weight `w = amp·(-1/lerp(16,5,s))`, cross filter `((b+d+f+h)·w+e)/(1+4w)`.
- **Deband** (port of neo_f3kdb `Deband(range,y,cb,cr,grainy=0,grainc=0, output_depth=16)`, i.e. default sample_mode=2, blur_first=true, random_algo_ref=UNIFORM, no grain/dither): 8-bit samples are upsampled to the 16-bit internal domain (`<<8`, thresholds `y<<2` with the reference's `scale=false`); per pixel, random distances `ref1/ref2 ∈ [0, cur_range]` (borders clamped, RNG sequence reproducing `init_frame_luts` exactly: one LCG seed advanced per pixel in Y/ref1/ref2/Cb/Cr order) pick 4 diagonal references, averaged by the reference's SSE `avg_4` (first pair minus 1, then averaged with the second); the pixel is kept only if `|avg - px| >= threshold`, otherwise `avg` replaces it; finally `>>8` back to 8-bit. Like every other filter it acts on the luma plane only (`cb/cr` accepted for reference compatibility).
- **Grain** (port of VapourSynth-AddGrain, i.e. `core.grain.Add`; original AddGrain by Tom Barry / Firesledge, VapourSynth port by HolyWu): a 32-bit LCG (`idum = 1664525·idum + 1013904223`) and polar Box-Muller (the second sample cached in the `gset` state for the next call) produce a standard normal, scaled by `sqrt(var)` to `N(0, var)`; each pixel draws independently (the reference's `hcorr/vcorr` default to 0 = no horizontal/vertical correlation), and the drawn value is rounded to an int8 delta, added to the source and finally clamped to `[0,255]`. Deviation from the reference: the plugin seeds from the wall clock by default and pre-generates a 2× tall noise field into which it rolls a per-frame window; this port instead derives the seed from an FNV-1a hash of the image and draws the field directly per pixel (equivalent for a single still frame), so the same input is reproducible. `var <= 0` passes through.

### Verification

Pixel-compared against a real VapourSynth installation (vapoursynth pip package + eedi2 / fmtconv / RemoveGrainVS / nlm_ispc / cas / neo-f3kdb plugins):

| Filter | Compared against | Result |
|---|---|---|
| EEDI2 (8bit) | eedi2.dll output directly | 100% pixel-identical |
| Repair (mode 2, 8bit) | RemoveGrainVS.dll output directly | 100% pixel-identical |
| spline36 resample | fmtconv 16bit output >>8 | 100% within 1 |
| Full AA chain | VS full 16bit >>8 vs this tool 8bit | 100% within 2, 99.9% within 1 |
| NLMeans (8bit) | nlm_ispc.dll directly (multiple params/sizes) | 100% pixel-identical |
| FineDehalo (8bit) | havsfunc (all defaults) directly | masks match; halo-region pixels within 24 (float detail in internal resize kernels) |
| CAS (8bit) | cas.dll (sharpness=0.7) directly | 99.3% byte-identical, rest within 2 (plugin AVX2/AVX512 reassociation) |
| Deband (8bit) | neo-f3kdb.dll r7 (several range/y, GRAY16 output) >>8 | 100% pixel-identical |
| Grain (8bit) | addgrain.dll (`core.grain.Add`, var=40) noise statistics | mean/std match within 0.1% (seeds differ, so pixels are not comparable) |

Deband was verified at 480×320, 386×277, 300×220 and 333×241 with several parameter sets (including `y=0`, where the output must equal the input); the rest of the chain matches at the same level.



## License

The project code is released under the **MIT License**; see [LICENSE](LICENSE).

The filters are independent ports of open-source implementations from the VapourSynth / AviSynth ecosystem (including VapourSynth-EEDI2 and others): they follow the reference behavior, with sources noted in code comments, but the code itself is written from scratch for this project.