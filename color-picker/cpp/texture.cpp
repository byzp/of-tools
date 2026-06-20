#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_DEPRECATED
#include "stb_image.h"

#include "texture.hpp"

bool load_texture(const std::string& path, Texture& out) {
    int w = 0, h = 0, n = 0;
    // Force 4 channels (RGBA), matching PIL .convert("RGBA").
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return out.ok();
}
