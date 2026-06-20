// Swirl-noise color generator + color math, ported line-for-line from algo.py
// and the color helpers in main.py. The float32 path is preserved exactly
// (numba used np.float32 throughout) so output matches the Python reference.
#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "texture.hpp"

struct RGBA {
    int r, g, b, a;
};

class SwirlNoiseGenHelper {
public:
    // params must be 64 doubles (16x4, row-major). If not, the helper stays
    // unconfigured and get_color_array returns black (matching algo.py's guard).
    void set_swirl_params(const std::vector<double>& params, const Texture& tex);

    std::vector<RGBA> get_color_array(float uv_y, int count) const;
    // Same as get_color_array but writes into a caller-provided buffer of `count`
    // entries (no heap allocation; for the hot search loop).
    void get_colors(float uv_y, int count, RGBA* out) const;

    bool configured() const { return configured_; }

private:
    bool configured_ = false;
    float swirl_[16][4] = {};  // stored as float (numba cast each to f32 on use)
    std::vector<uint8_t> tex_;
    int tex_w_ = 0, tex_h_ = 0;

    // Coefficients (algo.py constructor defaults).
    float rotate_coef_ = 5.0f;
    float radius_coef_ = 0.5f;
    float color_power_y1_ = 0.2f;  // exponent at uv_y == 1
    float color_power_y0_ = 1.5f;  // exponent at uv_y == 0
};

// ── color helpers (main.py) ──────────────────────────────────────────────
std::tuple<int, int, int> hex_to_rgb(const std::string& h);
std::string rgb_to_hex(int r, int g, int b);
// CIE76 similarity in [0,1]: max(0, 1 - deltaE/100). Args are 0..255 sRGB.
double cie76(int ar, int ag, int ab, int br, int bg, int bb);
