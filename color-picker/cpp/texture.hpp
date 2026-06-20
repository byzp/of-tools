// PNG texture loading via stb_image. Replaces PIL's
// `Image.open(...).convert("RGBA")` + `np.array(...)`: produces an RGBA8 buffer,
// row-major, top-origin (row 0 == top), exactly like the numpy array the Python
// code indexes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Texture {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // size h*w*4, row-major, top-origin
    bool ok() const { return w > 0 && h > 0 && rgba.size() == (size_t)w * h * 4; }
};

bool load_texture(const std::string& path, Texture& out);
