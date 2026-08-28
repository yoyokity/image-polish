#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "imageio.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

bool loadImage(const std::string &path, Image &img)
{
    int w, h, ch;
    u8 *data = stbi_load(path.c_str(), &w, &h, &ch, 0);
    if (!data)
        return false;
    img.w = w; img.h = h;
    if (ch == 1 || ch == 2) {
        img.ch = 1;
        img.p.assign(data, data + std::size_t(w) * h);
    } else {
        img.ch = 3;
        img.p.resize(std::size_t(w) * h * 3);
        for (int i = 0; i < w * h; i++) {
            img.p[3 * i + 0] = data[ch * i + 0];
            img.p[3 * i + 1] = data[ch * i + 1];
            img.p[3 * i + 2] = data[ch * i + 2];
        }
    }
    stbi_image_free(data);
    return true;
}

bool saveImage(const std::string &path, const Image &img)
{
    const std::string ext = [&]() {
        const auto pos = path.find_last_of('.');
        std::string e = (pos == std::string::npos) ? "" : path.substr(pos + 1);
        std::transform(e.begin(), e.end(), e.begin(), [](char c) { return char(std::tolower((unsigned char)c)); });
        return e;
    }();
    const int stride = img.w * img.ch;
    if (ext == "png")
        return stbi_write_png(path.c_str(), img.w, img.h, img.ch, img.p.data(), stride) != 0;
    if (ext == "bmp")
        return stbi_write_bmp(path.c_str(), img.w, img.h, img.ch, img.p.data()) != 0;
    if (ext == "tga")
        return stbi_write_tga(path.c_str(), img.w, img.h, img.ch, img.p.data()) != 0;
    if (ext == "jpg" || ext == "jpeg")
        return stbi_write_jpg(path.c_str(), img.w, img.h, img.ch, img.p.data(), 95) != 0;
    if (ext == "pgm" || ext == "pnm" || ext == "ppm") {
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const char magic = (img.ch == 3) ? '6' : '5';
        std::fprintf(f, "P%c\n%d %d\n255\n", magic, img.w, img.h);
        const bool ok = std::fwrite(img.p.data(), 1, img.p.size(), f) == img.p.size();
        std::fclose(f);
        return ok;
    }
    // default: PNG
    return stbi_write_png(path.c_str(), img.w, img.h, img.ch, img.p.data(), stride) != 0;
}