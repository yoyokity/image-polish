#pragma once

#include "../common.h"

#include <string>
#include <vector>

// ONNX-model super-resolution (onnxsr.cpp). The model runs on an RGB image
// (u8, w*h*3) fed as a 4D NCHW tensor in [0,1] (float32 or float16 input,
// whichever the model declares) and returns the upscaled RGB. The output size
// is read back from the model, so the scale of the model does not need to be
// known in advance (all bundled models upscale 2x).
//
// onnxruntime is NOT linked at build time: the runtime library
// (onnxruntime.dll / libonnxruntime.so / libonnxruntime.dylib) is loaded
// dynamically from the executable's own directory only. When it is missing or
// too old the step is *skipped* (see OnnxSrStatus) so the image continues
// through the rest of the chain rather than aborting.
//
// Sessions are cached per model path (created on first use, reused for every
// image) and the DirectML execution provider is used on Windows when the
// bundled onnxruntime.dll supports it, with CPU as automatic fallback; see
// build.py for the runtime DLL set that is shipped next to the executable.
enum class OnnxSrStatus { Ok, Skip, Error };

// Runs the model on an RGB image (u8, w*h*3) fed as a 4D NCHW tensor in
// [0,1] (float32 or float16 input, whichever the model declares) and returns
// the upscaled RGB; the output size is read back from the model, so the scale
// of the model does not need to be known in advance (all bundled models
// upscale 2x).
//
// Returns Ok with out/outW/outH filled; Skip when the onnxruntime library is
// not available (err holds the reason, nothing else happens); Error for model
// or inference failures (err holds a human-readable reason).
OnnxSrStatus runOnnxSr(const std::string &modelPath, const u8 *rgb, int w, int h,
                       std::vector<u8> &out, int &outW, int &outH, std::string &err);

// Print a warning line to stderr in yellow (ANSI on POSIX, console attribute
// on Windows), falling back to plain text when stderr is not a console.
void onnxWarn(const std::string &msg);

// True when an onnxruntime library is loadable next to the executable. The
// plain packages are shipped without one, so --model is skipped there instead
// of failing on model resolution.
bool onnxRuntimeAvailable();

// Resolve a "--model <name>" argument to an onnx file. A value containing a
// path separator or ending in ".onnx" is used as a path; otherwise it is a
// model name tried as models/<name>.onnx, <name>.onnx and models/<name>, then
// matched by prefix against the *.onnx files in models/ (an exact match wins,
// an ambiguous prefix is an error). Returns "" and fills `err` (including the
// list of available models) when nothing matches.
std::string resolveOnnxModel(const std::string &name, std::string &err);