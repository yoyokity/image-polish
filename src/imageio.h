#pragma once

#include "common.h"

#include <string>
#include <vector>

struct Image {
    int w = 0, h = 0, ch = 0;          // ch: 1 gray, 3 RGB
    std::vector<u8> p;
};

bool loadImage(const std::string &path, Image &img);

// Save an image; the format comes from the file extension. `jpegQuality`
// (1..100) applies to .jpg/.jpeg output only; all other formats ignore it.
bool saveImage(const std::string &path, const Image &img, int jpegQuality);