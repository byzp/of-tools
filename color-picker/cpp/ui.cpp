#include "ui.hpp"

#include "colordlg.hpp"

#include <windows.h>
// gdiplus needs min/max; include after windows.h
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

// ── constants from ui.py ────────────────────────────────────────────────────
static const int kResizeMargin = 8;
static const int kMinWidth = 400;
static const int kMinHeight = 360;
static const int kBarWidth = 50;
static const int kBarPadTop = 6;
static const int kBarPadBottom = 20;
static const int kDragH = 24;
static const int kSpacer = 20;     // gap below bar / overlay spacer
static const int kCollapsedW = 52;

static const UINT WM_APP_RESULT = WM_APP + 1;

// ── window state (single window) ────────────────────────────────────────────
struct UiState {
    HWND hwnd = nullptr;
    int x = 200, y = 200, w = 500, h = 360;
    bool collapsed = false;
    int saved_w = 500, saved_h = 360;

    std::string target_hex = "#ffffff";
    float uvy = 0.5f;
    std::vector<std::wstring> log{L"select a target color"};
    RGBA colors[5] = {{60, 60, 60, 255}, {60, 60, 60, 255}, {60, 60, 60, 255},
                      {60, 60, 60, 255}, {60, 60, 60, 255}};
    int marked = -1;  // best slot (0-based), -1 = none

    // interaction
    bool dragging = false;
    POINT drag_off{0, 0};
    bool resizing = false;
    int resize_dir = 0;  // bitmask: 1 left, 2 right, 4 top, 8 bottom
    POINT press_pt{0, 0};
    RECT press_geom{0, 0, 0, 0};  // left/top/right(excl)/bottom(excl) as x,y,w,h-derived
    bool press_button = false;
    bool press_strip = false;
    bool hover_button = false;

    // DIB backing store
    HDC dib_dc = nullptr;
    HBITMAP dib_bmp = nullptr;
    HBITMAP dib_old = nullptr;
    void* dib_bits = nullptr;
    int dib_w = 0, dib_h = 0;

    TargetChangedCb on_target_changed;
};
static UiState g;
static ULONG_PTR g_gdip = 0;

// ── geometry helpers ────────────────────────────────────────────────────────
static int info_height(int H) {
    int ch = H - kDragH;
    int v = ch - 168;  // see layout notes
    if (v < 60) v = 60;
    if (v > 140) v = 140;
    return v;
}

struct RectI {
    int x, y, w, h;
};
static RectI bar_rect(int W, int H) {
    (void)W;
    return {0, kDragH, kBarWidth, (H - kDragH) - kSpacer};
}
static RectI strip_rect(int W, int H) {
    return {128, H - 166, W - 136, 32};
}
static RectI square_rect(int W, int H, int i) {
    float sw = (float)(W - 160) / 5.0f;
    int x = (int)(128 + i * (sw + 6));
    int wi = (int)sw;
    return {x, H - 130, wi, 120};
}
static RECT button_rect() {  // window-local
    RECT r;
    r.left = g.w - 26;
    r.top = 3;
    r.right = r.left + 22;
    r.bottom = r.top + 18;
    return r;
}

// ── rounded-rect path (selective corners) ───────────────────────────────────
static void AddRoundedRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h,
                           REAL tl, REAL tr, REAL br, REAL bl) {
    p.StartFigure();
    if (tl > 0) p.AddArc(x, y, tl * 2, tl * 2, 180, 90);
    p.AddLine(x + tl, y, x + w - tr, y);
    if (tr > 0) p.AddArc(x + w - tr * 2, y, tr * 2, tr * 2, 270, 90);
    p.AddLine(x + w, y + tr, x + w, y + h - br);
    if (br > 0) p.AddArc(x + w - br * 2, y + h - br * 2, br * 2, br * 2, 0, 90);
    p.AddLine(x + w - br, y + h, x + bl, y + h);
    if (bl > 0) p.AddArc(x, y + h - bl * 2, bl * 2, bl * 2, 90, 90);
    p.AddLine(x, y + h - bl, x, y + tl);
    p.CloseFigure();
}

// ── DIB management ──────────────────────────────────────────────────────────
static void ensure_dib(int W, int H) {
    if (g.dib_dc && g.dib_w == W && g.dib_h == H) return;
    if (g.dib_dc) {
        SelectObject(g.dib_dc, g.dib_old);
        DeleteObject(g.dib_bmp);
        DeleteDC(g.dib_dc);
        g.dib_dc = nullptr;
    }
    g.dib_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g.dib_bmp = CreateDIBSection(g.dib_dc, &bi, DIB_RGB_COLORS, &g.dib_bits,
                                 nullptr, 0);
    g.dib_old = (HBITMAP)SelectObject(g.dib_dc, g.dib_bmp);
    g.dib_w = W;
    g.dib_h = H;
}

static void parse_hex(const std::string& hx, int& r, int& gg, int& b) {
    unsigned rr = 255, g2 = 255, bb = 255;
    sscanf(hx.c_str(), "#%02x%02x%02x", &rr, &g2, &bb);
    r = (int)rr;
    gg = (int)g2;
    b = (int)bb;
}

// ── fonts (lazy) ────────────────────────────────────────────────────────────
static FontFamily* g_consolas = nullptr;
static FontFamily* consolas() {
    if (!g_consolas) g_consolas = new FontFamily(L"Consolas");
    return g_consolas;
}

// ── render the whole window ─────────────────────────────────────────────────
static void render() {
    int W = g.w, H = g.h;
    ensure_dib(W, H);

    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppARGB, (BYTE*)g.dib_bits);
        Graphics gfx(&bmp);
        gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(TextRenderingHintAntiAlias);
        gfx.Clear(Color(0, 0, 0, 0));

        // ── drag bar bg (top corners rounded) ──
        {
            GraphicsPath path;
            AddRoundedRect(path, 0, 0, (REAL)W, (REAL)kDragH, 8, 8, 0, 0);
            SolidBrush b(Color(140, 0, 0, 0));
            gfx.FillPath(&b, &path);
        }
        // ── collapse button ──
        {
            RECT br = button_rect();
            if (g.hover_button) {
                GraphicsPath hp;
                AddRoundedRect(hp, (REAL)br.left, (REAL)br.top, 22, 18, 4, 4, 4,
                               4);
                SolidBrush hb(Color(25, 255, 255, 255));
                gfx.FillPath(&hb, &hp);
            }
            Font f(consolas(), 13, FontStyleBold, UnitPixel);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            RectF rc((REAL)br.left, (REAL)br.top, 22, 18);
            SolidBrush tb(g.hover_button ? Color(255, 255, 255, 255)
                                         : Color(255, 170, 170, 170));
            const wchar_t* glyph = g.collapsed ? L"+" : L"−";
            gfx.DrawString(glyph, -1, &f, rc, &sf, &tb);
        }

        if (!g.collapsed) {
            int CH = H - kDragH;
            // ── content bg (bottom corners rounded) ──
            {
                GraphicsPath path;
                AddRoundedRect(path, 0, (REAL)kDragH, (REAL)W, (REAL)CH, 0, 0, 8,
                               8);
                SolidBrush b(Color(140, 0, 0, 0));
                gfx.FillPath(&b, &path);
            }

            // ── bar area ──
            RectI bar = bar_rect(W, H);
            {
                SolidBrush bg(Color(255, 42, 42, 48));
                gfx.FillRectangle(&bg, bar.x, bar.y, bar.w, bar.h);
                SolidBrush edge(Color(255, 65, 65, 70));
                gfx.FillRectangle(&edge, bar.x + bar.w - 1, bar.y, 1, bar.h);
                // labels (aligned to the uvy marker extremes, as in ui.py)
                Font f(consolas(), 8, FontStyleRegular, UnitPoint);
                SolidBrush lb(Color(255, 140, 140, 140));
                StringFormat sf;
                sf.SetAlignment(StringAlignmentCenter);
                RectF top((REAL)bar.x, (REAL)(bar.y + kBarPadTop), (REAL)bar.w,
                          14);
                gfx.DrawString(L"1", -1, &f, top, &sf, &lb);
                RectF bot((REAL)bar.x,
                          (REAL)(bar.y + bar.h - kBarPadBottom - 10),
                          (REAL)bar.w, 14);
                gfx.DrawString(L"0", -1, &f, bot, &sf, &lb);
                // marker
                int usable = bar.h - kBarPadTop - kBarPadBottom;
                int my = bar.y + kBarPadTop + (int)((1.0f - g.uvy) * usable);
                Pen mp(Color(200, 220, 220, 220), 2);
                gfx.DrawLine(&mp, bar.x + 4, my, bar.x + bar.w - 4, my);
            }

            // ── info log ──
            {
                int ih = info_height(H);
                RectF clip(120, (REAL)kDragH, (REAL)(W - 120), (REAL)ih);
                gfx.SetClip(clip);
                Font f(consolas(), 13, FontStyleRegular, UnitPoint);
                REAL lh = f.GetHeight(&gfx);
                SolidBrush tb(Color(255, 187, 187, 187));
                int maxlines = (int)((ih - 8) / lh);
                if (maxlines < 1) maxlines = 1;
                int total = (int)g.log.size();
                int start = total > maxlines ? total - maxlines : 0;
                REAL ty = (REAL)(kDragH + 4);
                for (int i = start; i < total; ++i) {
                    gfx.DrawString(g.log[i].c_str(), -1, &f, PointF(126, ty),
                                   &tb);
                    ty += lh;
                }
                gfx.ResetClip();
            }

            // ── color strip ──
            {
                RectI s = strip_rect(W, H);
                int r, gg, b;
                parse_hex(g.target_hex, r, gg, b);
                GraphicsPath path;
                AddRoundedRect(path, (REAL)s.x, (REAL)s.y, (REAL)s.w, (REAL)s.h,
                               4, 4, 4, 4);
                SolidBrush fill(Color(255, r, gg, b));
                gfx.FillPath(&fill, &path);
                Pen border(Color(40, 255, 255, 255), 1);
                gfx.DrawPath(&border, &path);
            }

            // ── five color squares ──
            for (int i = 0; i < 5; ++i) {
                RectI sq = square_rect(W, H, i);
                int fillh = sq.h - 18;
                SolidBrush fill(Color(255, g.colors[i].r, g.colors[i].g,
                                      g.colors[i].b));
                gfx.FillRectangle(&fill, sq.x, sq.y, sq.w, fillh);
                Pen border(Color(55, 255, 255, 255), 1);
                gfx.DrawRectangle(&border, sq.x, sq.y, sq.w - 1, fillh - 1);
                if (i == g.marked) {
                    int cx = sq.x + sq.w / 2;
                    int tri_y = sq.y + fillh - 1 + 3;  // fill_rect.bottom()+3
                    PointF pts[3] = {PointF((REAL)cx, (REAL)tri_y),
                                     PointF((REAL)(cx - 10), (REAL)(tri_y + 12)),
                                     PointF((REAL)(cx + 10), (REAL)(tri_y + 12))};
                    SolidBrush tb(Color(255, 255, 200, 50));
                    gfx.FillPolygon(&tb, pts, 3);
                }
            }

            // ── line overlay (stub + triangle + uvy number) ──
            if (W > kBarWidth + 12) {
                int usable = CH - kSpacer - kBarPadTop - kBarPadBottom;
                int y = kDragH + kBarPadTop + (int)((1.0f - g.uvy) * usable);
                int bar_end = kBarWidth;
                int line_end = bar_end + 14;
                Pen sp(Color(180, 210, 210, 210), 2);
                gfx.DrawLine(&sp, bar_end + 4, y, line_end, y);
                PointF tri[3] = {PointF((REAL)(line_end - 8), (REAL)y),
                                 PointF((REAL)line_end, (REAL)(y - 5)),
                                 PointF((REAL)line_end, (REAL)(y + 5))};
                SolidBrush tb(Color(200, 210, 210, 210));
                gfx.FillPolygon(&tb, tri, 3);
                Font f(consolas(), 9, FontStyleRegular, UnitPoint);
                wchar_t buf[32];
                swprintf(buf, 32, L"%.3f", g.uvy);
                SolidBrush nb(Color(255, 220, 220, 220));
                REAL fh = f.GetHeight(&gfx);
                gfx.DrawString(buf, -1, &f, PointF((REAL)(line_end + 4),
                                                   (REAL)y - fh / 2),
                               &nb);
            }
        }

        gfx.Flush(FlushIntentionSync);
    }

    // premultiply alpha for UpdateLayeredWindow
    {
        uint8_t* p = (uint8_t*)g.dib_bits;
        int count = W * H;
        for (int i = 0; i < count; ++i) {
            int a = p[3];
            p[0] = (uint8_t)(p[0] * a / 255);
            p[1] = (uint8_t)(p[1] * a / 255);
            p[2] = (uint8_t)(p[2] * a / 255);
            p += 4;
        }
    }

    POINT ptDst{g.x, g.y};
    SIZE sz{W, H};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g.hwnd, screen, &ptDst, &sz, g.dib_dc, &src, 0, &bf,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ── collapse / restore (ui.py _toggle_collapse) ─────────────────────────────
static void toggle_collapse() {
    if (g.collapsed) {
        int new_w = g.saved_w, new_h = g.saved_h;
        int old_w = g.w;
        g.w = new_w;
        g.h = new_h;
        g.x = g.x + old_w - new_w;
        g.collapsed = false;
    } else {
        g.saved_w = g.w;
        g.saved_h = g.h;
        int old_w = g.w;
        g.w = kCollapsedW;
        g.h = kDragH;
        g.x = g.x + old_w - kCollapsedW;
        g.collapsed = true;
    }
    render();
}

// ── color picker (Qt-QColorDialog-style; see colordlg.cpp) ──────────────────
static void pick_color() {
    int r, gg, b;
    parse_hex(g.target_hex, r, gg, b);
    static COLORREF custom[16];
    static bool custom_init = false;
    if (!custom_init) {  // Qt shows empty custom slots as white
        for (int i = 0; i < 16; ++i) custom[i] = RGB(255, 255, 255);
        custom_init = true;
    }
    COLORREF out;
    if (choose_color_qt(g.hwnd, RGB(r, gg, b), out, custom)) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", GetRValue(out),
                 GetGValue(out), GetBValue(out));
        g.target_hex = buf;
        render();
        if (g.on_target_changed) g.on_target_changed(g.target_hex);
    }
}

// ── edge detection (ui.py _get_edges) ───────────────────────────────────────
static int get_edges(int x, int y) {
    int d = 0;
    if (x <= kResizeMargin) d |= 1;
    if (x >= g.w - kResizeMargin) d |= 2;
    if (y <= kResizeMargin) d |= 4;
    if (y >= g.h - kResizeMargin) d |= 8;
    return d;
}
static HCURSOR cursor_for(int x, int y) {
    if (g.collapsed) return LoadCursor(nullptr, IDC_ARROW);
    int d = g.resizing ? g.resize_dir : get_edges(x, y);
    if (((d & 1) && (d & 4)) || ((d & 2) && (d & 8)))
        return LoadCursor(nullptr, IDC_SIZENWSE);
    if (((d & 2) && (d & 4)) || ((d & 1) && (d & 8)))
        return LoadCursor(nullptr, IDC_SIZENESW);
    if ((d & 1) || (d & 2)) return LoadCursor(nullptr, IDC_SIZEWE);
    if ((d & 4) || (d & 8)) return LoadCursor(nullptr, IDC_SIZENS);
    return LoadCursor(nullptr, IDC_ARROW);
}

static bool pt_in(RECT r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ── window proc ─────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_RESULT: {
            Result* pr = (Result*)lp;
            g.uvy = (float)pr->uvy;
            // format like ui.py _on_result
            wchar_t line[160];
            std::wstring th(pr->target_hex.begin(), pr->target_hex.end());
            std::wstring mh(pr->hex.begin(), pr->hex.end());
            swprintf(line, 160, L"%ls → %ls  sim=%.1f%%  uvy=%.3f  slot=%d",
                     th.c_str(), mh.c_str(), pr->sim * 100.0, pr->uvy, pr->slot);
            if (g.log.size() == 1 && g.log[0] == L"select a target color")
                g.log[0] = line;
            else
                g.log.push_back(line);
            for (int i = 0; i < 5; ++i) g.colors[i] = pr->colors[i];
            g.marked = pr->slot - 1;
            delete pr;
            render();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            SetCapture(hwnd);
            if (!g.collapsed && get_edges(x, y)) {
                g.resizing = true;
                g.resize_dir = get_edges(x, y);
                POINT p{x, y};
                ClientToScreen(hwnd, &p);
                g.press_pt = p;
                g.press_geom.left = g.x;
                g.press_geom.top = g.y;
                g.press_geom.right = g.x + g.w - 1;   // inclusive (Qt-style)
                g.press_geom.bottom = g.y + g.h - 1;  // inclusive
            } else if (pt_in(button_rect(), x, y)) {
                g.press_button = true;
            } else if (!g.collapsed && pt_in({strip_rect(g.w, g.h).x,
                                              strip_rect(g.w, g.h).y,
                                              strip_rect(g.w, g.h).x +
                                                  strip_rect(g.w, g.h).w,
                                              strip_rect(g.w, g.h).y +
                                                  strip_rect(g.w, g.h).h},
                                             x, y)) {
                g.press_strip = true;
            } else if (y < kDragH) {
                g.dragging = true;
                POINT p{x, y};
                ClientToScreen(hwnd, &p);
                g.drag_off.x = p.x - g.x;
                g.drag_off.y = p.y - g.y;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            bool hov = pt_in(button_rect(), x, y);
            if (hov != g.hover_button) {
                g.hover_button = hov;
                render();
            }
            if (g.resizing) {
                POINT p{x, y};
                ClientToScreen(hwnd, &p);
                int dx = p.x - g.press_pt.x, dy = p.y - g.press_pt.y;
                int L = g.press_geom.left, T = g.press_geom.top;
                int R = g.press_geom.right, B = g.press_geom.bottom;
                if (g.resize_dir & 1) {
                    int v = L + dx, lim = R - kMinWidth + 1;
                    L = v < lim ? v : lim;
                }
                if (g.resize_dir & 2) {
                    int v = R + dx, lim = L + kMinWidth - 1;
                    R = v > lim ? v : lim;
                }
                if (g.resize_dir & 4) {
                    int v = T + dy, lim = B - kMinHeight + 1;
                    T = v < lim ? v : lim;
                }
                if (g.resize_dir & 8) {
                    int v = B + dy, lim = T + kMinHeight - 1;
                    B = v > lim ? v : lim;
                }
                g.x = L;
                g.y = T;
                g.w = R - L + 1;
                g.h = B - T + 1;
                render();
            } else if (g.dragging) {
                POINT p{x, y};
                ClientToScreen(hwnd, &p);
                g.x = p.x - g.drag_off.x;
                g.y = p.y - g.drag_off.y;
                SetWindowPos(hwnd, nullptr, g.x, g.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            ReleaseCapture();
            bool was_button = g.press_button, was_strip = g.press_strip;
            g.dragging = g.resizing = false;
            g.resize_dir = 0;
            g.press_button = g.press_strip = false;
            if (was_button && pt_in(button_rect(), x, y)) {
                toggle_collapse();
            } else if (was_strip) {
                RectI s = strip_rect(g.w, g.h);
                if (pt_in({s.x, s.y, s.x + s.w, s.y + s.h}, x, y)) pick_color();
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(cursor_for(pt.x, pt.y));
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── public API ──────────────────────────────────────────────────────────────
void ui_run(TargetChangedCb cb) {
    g.on_target_changed = cb;
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdip, &in, nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = nullptr;  // we manage the cursor
    wc.lpszClassName = L"OfColorPickerWnd";
    RegisterClassExW(&wc);

    g.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
        L"of-color-picker", WS_POPUP, g.x, g.y, g.w, g.h, nullptr, nullptr,
        wc.hInstance, nullptr);

    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    render();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    GdiplusShutdown(g_gdip);
}

void ui_post_result(const Result& r) {
    if (!g.hwnd) return;
    Result* pr = new Result(r);
    PostMessage(g.hwnd, WM_APP_RESULT, 0, (LPARAM)pr);
}

void ui_close() {
    if (g.hwnd) PostMessage(g.hwnd, WM_CLOSE, 0, 0);
}
