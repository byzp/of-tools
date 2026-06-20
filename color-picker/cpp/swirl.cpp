#include "swirl.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

// ── helpers ───────────────────────────────────────────────────────────────
namespace {

// np.clip(x, 0, 1) == min(max(x, 0), 1)
inline float clamp01f(float x) {
    float lo = x < 0.0f ? 0.0f : x;
    return lo > 1.0f ? 1.0f : lo;
}

inline long long clampll(long long x, long long lo, long long hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

}  // namespace

void SwirlNoiseGenHelper::set_swirl_params(const std::vector<double>& params,
                                           const Texture& tex) {
    if (params.size() != 64) {  // matches algo.py: arr.size != 64 -> no-op
        configured_ = false;
        return;
    }
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 4; ++j)
            swirl_[i][j] = (float)params[i * 4 + j];
    tex_ = tex.rgba;
    tex_w_ = tex.w;
    tex_h_ = tex.h;
    configured_ = tex.ok();
}

std::vector<RGBA> SwirlNoiseGenHelper::get_color_array(float uv_y,
                                                       int count) const {
    std::vector<RGBA> out(count);
    get_colors(uv_y, count, out.data());
    return out;
}

void SwirlNoiseGenHelper::get_colors(float uv_y, int n, RGBA* out) const {
    if (!configured_) {
        for (int i = 0; i < n; ++i) out[i] = RGBA{0, 0, 0, 255};
        return;
    }

    const int w = tex_w_;
    const int h = tex_h_;
    const float rotate_coef = rotate_coef_;
    const float radius_coef = radius_coef_;

    // ColorPower exponent: lerp(y0, y1, clamp(uv_y,0,1)). Computed once per call.
    float t = clamp01f(uv_y);
    float color_exp = (color_power_y1_ - color_power_y0_) * t + color_power_y0_;

    for (int idx = 0; idx < n; ++idx) {
        // u = arange(1, n+1)/(n+1); v = uv_y   (all float32)
        float u = (float)(idx + 1) / (float)(n + 1);
        float v = uv_y;

        // 16 swirl transforms
        for (int k = 0; k < 16; ++k) {
            float cx = swirl_[k][0];
            float cy = swirl_[k][1];
            float z = swirl_[k][2];
            float wv = swirl_[k][3];

            float angle = std::fabs(z) * rotate_coef;
            float radius = wv * radius_coef;
            float sign = z < 0.0f ? -1.0f : 1.0f;

            float ox = u - cx;
            float oy = v - cy;
            // Vector2.magnitude: dot in float64 -> sqrt -> back to float32
            float dist = (float)std::sqrt((double)(ox * ox + oy * oy));
            float rot = angle * std::exp(-dist / radius);  // expf (float args)
            float mn = dist < radius ? dist : radius;
            float blend = clamp01f(mn / radius);
            float final_ = ((0.0f - rot) * blend + rot) * sign;
            float ca = std::cos(final_);
            float sa = std::sin(final_);
            u = ox * ca - sa * oy + cx;
            v = ox * sa + oy * ca + cy;
        }

        // UVToPixel: fraction -> floor -> clamp (Unity origin bottom-left)
        float fu = u - std::floor(u);
        float fv = v - std::floor(v);
        long long px = (long long)std::floor((float)w * fu);
        long long py = (long long)std::floor((float)h * fv);
        px = clampll(px, 0, w - 1);
        py = clampll(py, 0, h - 1);

        // Texture lookup (row flipped: (h-1)-py), normalize to [0,1]
        long long row = (long long)(h - 1) - py;
        long long col = px;
        const uint8_t* texel = &tex_[(size_t)(row * w + col) * 4];
        float tr = (float)texel[0] / 255.0f;
        float tg = (float)texel[1] / 255.0f;
        float tb = (float)texel[2] / 255.0f;
        float ta = (float)texel[3] / 255.0f;

        // ColorPower on RGB; alpha passes through. Channels rounded half-to-even
        // (Python int(round(x)) / numba round) via llrint (default FE_TONEAREST).
        float pr = std::pow(tr, color_exp);
        float pg = std::pow(tg, color_exp);
        float pb = std::pow(tb, color_exp);
        out[idx].r = (int)std::llrint((double)pr * 255.0);
        out[idx].g = (int)std::llrint((double)pg * 255.0);
        out[idx].b = (int)std::llrint((double)pb * 255.0);
        out[idx].a = (int)std::llrint((double)ta * 255.0);
    }
}

// ── color helpers ──────────────────────────────────────────────────────────
std::tuple<int, int, int> hex_to_rgb(const std::string& h_in) {
    std::string h = h_in;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    auto hx = [](const std::string& s) -> int {
        return (int)strtol(s.c_str(), nullptr, 16);
    };
    int r = hx(h.substr(0, 2));
    int g = hx(h.substr(2, 2));
    int b = hx(h.substr(4, 2));
    return {r, g, b};
}

std::string rgb_to_hex(int r, int g, int b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r & 0xff, g & 0xff, b & 0xff);
    return std::string(buf);
}

double cie76(int ar, int ag, int ab, int br, int bg, int bb) {
    auto linearize = [](double c) -> double {
        c /= 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto to_xyz = [](double lr, double lg, double lb, double& x, double& y,
                     double& z) {
        x = lr * 0.4124564 + lg * 0.3575761 + lb * 0.1804375;
        y = lr * 0.2126729 + lg * 0.7151522 + lb * 0.0721750;
        z = lr * 0.0193339 + lg * 0.1191920 + lb * 0.9503041;
    };
    auto f = [](double tt) -> double {
        double d = 6.0 / 29.0;
        // Python uses t ** (1/3); use pow (not cbrt) to match bit-for-bit.
        return tt > d * d * d ? std::pow(tt, 1.0 / 3.0)
                              : tt / (3.0 * d * d) + 4.0 / 29.0;
    };
    auto lab = [&](double x, double y, double z, double& L, double& a, double& b) {
        const double xn = 0.95047, yn = 1.0, zn = 1.08883;
        L = 116.0 * f(y / yn) - 16.0;
        a = 500.0 * (f(x / xn) - f(y / yn));
        b = 200.0 * (f(y / yn) - f(z / zn));
    };

    double lar = linearize(ar), lag = linearize(ag), lab_ = linearize(ab);
    double lbr = linearize(br), lbg = linearize(bg), lbb = linearize(bb);
    double xa, ya, za, xb, yb, zb;
    to_xyz(lar, lag, lab_, xa, ya, za);
    to_xyz(lbr, lbg, lbb, xb, yb, zb);
    double L1, a1, b1, L2, a2, b2;
    lab(xa, ya, za, L1, a1, b1);
    lab(xb, yb, zb, L2, a2, b2);
    double d = std::sqrt((L1 - L2) * (L1 - L2) + (a1 - a2) * (a1 - a2) +
                         (b1 - b2) * (b1 - b2));
    double sim = 1.0 - d / 100.0;
    return sim < 0.0 ? 0.0 : sim;
}
