import numpy as np
import numba as nb


# ── numba-accelerated core: 16 swirls + texture lookup + color power ──
@nb.njit(cache=True)
def _compute_colors_nb(
    u,
    v,
    swirl_params,
    source_texture,
    h,
    w,
    rotate_coef,
    radius_coef,
    color_power_y0,
    color_power_y1,
    n,
    uv_y,
):
    """Apply all 16 swirl transforms, texture lookup, and color power in one JIT call."""
    f32 = np.float32
    i64 = np.int64

    # 16 swirl iterations
    for i in range(16):
        cx = f32(swirl_params[i, 0])
        cy = f32(swirl_params[i, 1])
        z_val = f32(swirl_params[i, 2])
        w_val = f32(swirl_params[i, 3])

        angle = abs(z_val) * rotate_coef
        radius = w_val * radius_coef
        sign_val = f32(1.0)
        if z_val < f32(0.0):
            sign_val = f32(-1.0)

        ox = u - cx
        oy = v - cy
        # Vector2.magnitude: dot in float64 → sqrt → float32
        dist = np.sqrt((ox * ox + oy * oy).astype(np.float64)).astype(f32)
        rot = angle * np.exp((-dist / radius).astype(f32))
        blend = np.clip(np.minimum(dist, radius) / radius, f32(0.0), f32(1.0))
        final = ((f32(0.0) - rot) * blend + rot) * sign_val
        cos_a = np.cos(final)
        sin_a = np.sin(final)
        u = ox * cos_a - sin_a * oy + cx
        v = ox * sin_a + oy * cos_a + cy

    # UVToPixel: fraction → floor → clamp (Unity origin at bottom-left)
    px = np.clip(np.floor(f32(w) * (u - np.floor(u))).astype(i64), 0, w - 1)
    py = np.clip(np.floor(f32(h) * (v - np.floor(v))).astype(i64), 0, h - 1)

    # Texture lookup (element-wise for numba)
    texel = np.zeros((n, 4), dtype=f32)
    for i in range(n):
        row = (h - 1) - py[i]
        col = px[i]
        texel[i, 0] = f32(source_texture[row, col, 0]) / f32(255.0)
        texel[i, 1] = f32(source_texture[row, col, 1]) / f32(255.0)
        texel[i, 2] = f32(source_texture[row, col, 2]) / f32(255.0)
        texel[i, 3] = f32(source_texture[row, col, 3]) / f32(255.0)

    # ColorPower: exp = lerp(color_power_y0, color_power_y1, clamp(uv_y, 0, 1))
    t = min(max(uv_y, f32(0.0)), f32(1.0))
    exp = (color_power_y1 - color_power_y0) * t + color_power_y0
    rgb = texel[:, :3] ** exp

    # Assemble result as (r, g, b, a) int tuples
    result = np.zeros((n, 4), dtype=i64)
    for i in range(n):
        result[i, 0] = int(round(rgb[i, 0] * 255.0))
        result[i, 1] = int(round(rgb[i, 1] * 255.0))
        result[i, 2] = int(round(rgb[i, 2] * 255.0))
        result[i, 3] = int(round(texel[i, 3] * 255.0))

    return result


class SwirlNoiseGenHelper:
    def __init__(self):
        self._swirl_params = None
        self._rotate_coef = 5.0  # field +14: angle = rotate_coef * |z|
        self._radius_coef = 0.5  # field +15: radius = radius_coef * w
        # ColorPower 指数随 uv_y 在两端之间插值 (field +12 / +13):
        #   exp = (power_y1 - power_y0) * clamp(uv_y, 0, 1) + power_y0
        self._color_power_y1 = 0.2  # uv_y == 1 时的指数
        self._color_power_y0 = 1.5  # uv_y == 0 时的指数
        self._source_texture = None

    def set_swirl_params(self, swirl_params, texture):
        arr = np.array(swirl_params, dtype=np.float64)
        if arr.size != 64:
            return
        self._swirl_params = arr.reshape(16, 4).copy()
        arr = np.array(texture).astype(np.uint8)
        self._source_texture = arr
        self._source_height, self._source_width = arr.shape[:2]

    def get_color_array(self, uv_y, output_color_count):
        if self._swirl_params is None or self._source_texture is None:
            return [(0, 0, 0, 255)] * output_color_count
        n = int(output_color_count)
        f32 = np.float32
        u = np.arange(1, n + 1, dtype=f32) / f32(n + 1.0)
        v = np.full(n, f32(uv_y), dtype=f32)

        result = _compute_colors_nb(
            u,
            v,
            self._swirl_params,
            self._source_texture,
            self._source_height,
            self._source_width,
            f32(self._rotate_coef),
            f32(self._radius_coef),
            f32(self._color_power_y0),
            f32(self._color_power_y1),
            n,
            f32(uv_y),
        )

        return [
            (int(result[i, 0]), int(result[i, 1]), int(result[i, 2]), int(result[i, 3]))
            for i in range(n)
        ]
