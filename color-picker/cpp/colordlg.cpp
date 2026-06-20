#include "colordlg.hpp"

#include <commctrl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "comctl32.lib")

// ── layout constants (client coords) ────────────────────────────────────────
namespace {

const int CLIENT_W = 472;
const int CLIENT_H = 360;

const int CELL = 24;          // swatch step (22px cell + 2px gap)
const int SW = 22;            // swatch size

const int BASIC_X = 12, BASIC_Y = 30;        // 8 x 6 basic grid
const int CUSTOM_X = 12, CUSTOM_Y = 202;     // 8 x 2 custom grid

const int SVX = 224, SVY = 30, SVW = 190, SVH = 142;   // hue-sat field
const int BARX = 420, BARY = 30, BARW = 24, BARH = 142;  // value bar

const int PREV_X = 372, PREV_Y = 296, PREV_W = 74, PREV_H = 24;

// control IDs
enum {
    IDC_ADD = 1001,
    IDC_PICK = 1002,
    IDC_EDIT_H = 1010, IDC_EDIT_S, IDC_EDIT_V,
    IDC_EDIT_R, IDC_EDIT_G, IDC_EDIT_B,
    IDC_EDIT_HTML,
    IDC_SPIN_H = 1020, IDC_SPIN_S, IDC_SPIN_V,
    IDC_SPIN_R, IDC_SPIN_G, IDC_SPIN_B,
};

// 8x6 basic palette (7 hue columns light->dark + a grayscale column)
const COLORREF kBasic[48] = {
    RGB(0xFF,0xB3,0xB3), RGB(0xFF,0xD9,0xB3), RGB(0xFF,0xFF,0xB3), RGB(0xB3,0xFF,0xB3),
    RGB(0xB3,0xFF,0xFF), RGB(0xB3,0xB3,0xFF), RGB(0xFF,0xB3,0xFF), RGB(0xFF,0xFF,0xFF),
    RGB(0xFF,0x66,0x66), RGB(0xFF,0xB3,0x66), RGB(0xFF,0xFF,0x66), RGB(0x66,0xFF,0x66),
    RGB(0x66,0xFF,0xFF), RGB(0x66,0x66,0xFF), RGB(0xFF,0x66,0xFF), RGB(0xCC,0xCC,0xCC),
    RGB(0xFF,0x00,0x00), RGB(0xFF,0x80,0x00), RGB(0xFF,0xFF,0x00), RGB(0x00,0xFF,0x00),
    RGB(0x00,0xFF,0xFF), RGB(0x00,0x00,0xFF), RGB(0xFF,0x00,0xFF), RGB(0x99,0x99,0x99),
    RGB(0xB3,0x00,0x00), RGB(0xB3,0x59,0x00), RGB(0xB3,0xB3,0x00), RGB(0x00,0xB3,0x00),
    RGB(0x00,0xB3,0xB3), RGB(0x00,0x00,0xB3), RGB(0xB3,0x00,0xB3), RGB(0x66,0x66,0x66),
    RGB(0x80,0x00,0x00), RGB(0x80,0x40,0x00), RGB(0x80,0x80,0x00), RGB(0x00,0x80,0x00),
    RGB(0x00,0x80,0x80), RGB(0x00,0x00,0x80), RGB(0x80,0x00,0x80), RGB(0x33,0x33,0x33),
    RGB(0x40,0x00,0x00), RGB(0x40,0x20,0x00), RGB(0x40,0x40,0x00), RGB(0x00,0x40,0x00),
    RGB(0x00,0x40,0x40), RGB(0x00,0x00,0x40), RGB(0x40,0x00,0x40), RGB(0x00,0x00,0x00),
};

struct State {
    int h = 0, s = 0, v = 0;       // HSV: h 0..359, s/v 0..255
    int r = 0, g = 0, b = 0;       // RGB 0..255
    COLORREF* custom = nullptr;    // 16 slots (caller-owned)
    int sel_custom = 0;
    bool ok = false, done = false;
    bool updating = false;         // guards EN_CHANGE recursion
    bool picking = false;          // screen-color-pick mode
    HWND eH, eS, eV, eR, eG, eB, eHtml;
    HWND uH, uS, uV, uR, uG, uB;   // up-down buddies (kept in sync)
    HFONT font = nullptr;
};

enum SkipGroup { SKIP_NONE, SKIP_HSV, SKIP_RGB, SKIP_HTML };

// ── HSV <-> RGB (Qt conventions: H 0..359, S/V 0..255) ──────────────────────
void hsv_to_rgb(int h, int s, int v, int& r, int& g, int& b) {
    double S = s / 255.0, V = v / 255.0;
    double C = V * S;
    double Hp = (h % 360) / 60.0;
    double X = C * (1.0 - std::fabs(std::fmod(Hp, 2.0) - 1.0));
    double r1 = 0, g1 = 0, b1 = 0;
    if (Hp < 1)      { r1 = C; g1 = X; }
    else if (Hp < 2) { r1 = X; g1 = C; }
    else if (Hp < 3) { g1 = C; b1 = X; }
    else if (Hp < 4) { g1 = X; b1 = C; }
    else if (Hp < 5) { r1 = X; b1 = C; }
    else             { r1 = C; b1 = X; }
    double m = V - C;
    r = (int)std::lround((r1 + m) * 255.0);
    g = (int)std::lround((g1 + m) * 255.0);
    b = (int)std::lround((b1 + m) * 255.0);
}

void rgb_to_hsv(int r, int g, int b, int& h, int& s, int& v) {
    double R = r / 255.0, G = g / 255.0, B = b / 255.0;
    double mx = (R > G ? R : G); if (B > mx) mx = B;
    double mn = (R < G ? R : G); if (B < mn) mn = B;
    double d = mx - mn;
    v = (int)std::lround(mx * 255.0);
    if (mx <= 0.0) { s = 0; return; }     // black: keep hue
    s = (int)std::lround(d / mx * 255.0);
    if (d <= 0.0) return;                  // gray: keep hue
    double hh;
    if (mx == R)      hh = std::fmod((G - B) / d, 6.0);
    else if (mx == G) hh = (B - R) / d + 2.0;
    else              hh = (R - G) / d + 4.0;
    hh *= 60.0;
    if (hh < 0) hh += 360.0;
    h = ((int)std::lround(hh)) % 360;
}

inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Qt QColorPicker mapping: x -> hue (360 at left, 0 at right), y -> sat (255 top)
inline int hue_from_x(int x) { return 360 - x * 360 / (SVW - 1); }
inline int sat_from_y(int y) { return 255 - y * 255 / (SVH - 1); }
inline int x_from_hue(int h) { return (360 - h) * (SVW - 1) / 360; }
inline int y_from_sat(int s) { return (255 - s) * (SVH - 1) / 255; }
inline int val_from_y(int y) { return 255 - y * 255 / (BARH - 1); }
inline int y_from_val(int v) { return (255 - v) * (BARH - 1) / 255; }

State* get_state(HWND h) {
    return (State*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

int read_int(HWND e) {
    wchar_t buf[16] = {0};
    GetWindowTextW(e, buf, 16);
    return _wtoi(buf);
}

void refresh(HWND dlg, State* st, SkipGroup skip) {
    st->updating = true;
    // Drive the up-down controls (UDS_SETBUDDYINT updates the edit text), so the
    // spin arrows always start from the current value.
    if (skip != SKIP_HSV) {
        SendMessageW(st->uH, UDM_SETPOS32, 0, st->h);
        SendMessageW(st->uS, UDM_SETPOS32, 0, st->s);
        SendMessageW(st->uV, UDM_SETPOS32, 0, st->v);
    }
    if (skip != SKIP_RGB) {
        SendMessageW(st->uR, UDM_SETPOS32, 0, st->r);
        SendMessageW(st->uG, UDM_SETPOS32, 0, st->g);
        SendMessageW(st->uB, UDM_SETPOS32, 0, st->b);
    }
    if (skip != SKIP_HTML) {
        wchar_t buf[8];
        wsprintfW(buf, L"#%02x%02x%02x", st->r, st->g, st->b);
        SetWindowTextW(st->eHtml, buf);
    }
    st->updating = false;
    RECT rc{SVX - 2, SVY - 2, BARX + BARW + 8, SVY + SVH + 8};
    InvalidateRect(dlg, &rc, FALSE);
    RECT rp{PREV_X, PREV_Y, PREV_X + PREV_W, PREV_Y + PREV_H};
    InvalidateRect(dlg, &rp, FALSE);
}

// ── painting ────────────────────────────────────────────────────────────────
void blit_field(HDC dc) {  // hue-sat field at val=255 (constant)
    static std::vector<uint32_t> img;
    if (img.empty()) {
        img.resize((size_t)SVW * SVH);
        for (int y = 0; y < SVH; ++y)
            for (int x = 0; x < SVW; ++x) {
                int rr, gg, bb;
                hsv_to_rgb(hue_from_x(x), sat_from_y(y), 255, rr, gg, bb);
                img[(size_t)y * SVW + x] = (rr << 16) | (gg << 8) | bb;
            }
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = SVW;
    bi.bmiHeader.biHeight = -SVH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, SVX, SVY, SVW, SVH, 0, 0, 0, SVH, img.data(), &bi,
                      DIB_RGB_COLORS);
}

void blit_bar(HDC dc, State* st) {  // value gradient of current hue/sat
    std::vector<uint32_t> img((size_t)BARW * BARH);
    for (int y = 0; y < BARH; ++y) {
        int rr, gg, bb;
        hsv_to_rgb(st->h, st->s, val_from_y(y), rr, gg, bb);
        uint32_t px = (rr << 16) | (gg << 8) | bb;
        for (int x = 0; x < BARW; ++x) img[(size_t)y * BARW + x] = px;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = BARW;
    bi.bmiHeader.biHeight = -BARH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, BARX, BARY, BARW, BARH, 0, 0, 0, BARH, img.data(), &bi,
                      DIB_RGB_COLORS);
}

void draw_grid(HDC dc, const COLORREF* colors, int n, int ox, int oy, int cols,
               int rows) {
    HBRUSH gray = CreateSolidBrush(RGB(128, 128, 128));
    for (int i = 0; i < n; ++i) {
        int c = i % cols, r = i / cols;
        if (r >= rows) break;
        int x = ox + c * CELL, y = oy + r * CELL;
        RECT rc{x, y, x + SW, y + SW};
        HBRUSH br = CreateSolidBrush(colors[i]);
        FillRect(dc, &rc, br);          // color fill
        DeleteObject(br);
        FrameRect(dc, &rc, gray);       // 1px border (no interior fill)
    }
    DeleteObject(gray);
}

void on_paint(HWND dlg, State* st) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(dlg, &ps);

    // grids
    draw_grid(dc, kBasic, 48, BASIC_X, BASIC_Y, 8, 6);
    draw_grid(dc, st->custom, 16, CUSTOM_X, CUSTOM_Y, 8, 2);
    // highlight selected custom slot
    {
        int c = st->sel_custom % 8, r = st->sel_custom / 8;
        int x = CUSTOM_X + c * CELL, y = CUSTOM_Y + r * CELL;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x - 1, y - 1, x + SW + 1, y + SW + 1);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }

    // hue-sat field + value bar
    blit_field(dc);
    blit_bar(dc, st);

    // field crosshair
    {
        int cx = SVX + x_from_hue(st->h);
        int cy = SVY + y_from_sat(st->s);
        HPEN wp = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HPEN bp = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(dc, bp);
        Ellipse(dc, cx - 5, cy - 5, cx + 5, cy + 5);
        SelectObject(dc, wp);
        Ellipse(dc, cx - 4, cy - 4, cx + 4, cy + 4);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(wp);
        DeleteObject(bp);
    }
    // bar indicator (arrows on both sides)
    {
        int y = BARY + y_from_val(st->v);
        HBRUSH blk = (HBRUSH)GetStockObject(BLACK_BRUSH);
        POINT lt[3] = {{BARX - 1, y}, {BARX - 6, y - 4}, {BARX - 6, y + 4}};
        POINT rt[3] = {{BARX + BARW + 1, y}, {BARX + BARW + 6, y - 4}, {BARX + BARW + 6, y + 4}};
        HGDIOBJ op = SelectObject(dc, GetStockObject(BLACK_PEN));
        HGDIOBJ ob = SelectObject(dc, blk);
        Polygon(dc, lt, 3);
        Polygon(dc, rt, 3);
        SelectObject(dc, op);
        SelectObject(dc, ob);
    }

    // border around field
    {
        HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, RGB(128, 128, 128));
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, SVX - 1, SVY - 1, SVX + SVW + 1, SVY + SVH + 1);
        SelectObject(dc, op);
        SelectObject(dc, ob);
    }

    // preview swatch
    {
        HBRUSH br = CreateSolidBrush(RGB(st->r, st->g, st->b));
        RECT rc{PREV_X, PREV_Y, PREV_X + PREV_W, PREV_Y + PREV_H};
        FillRect(dc, &rc, br);
        DeleteObject(br);
        FrameRect(dc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));
    }

    EndPaint(dlg, &ps);
}

// ── interaction ─────────────────────────────────────────────────────────────
void apply_rgb(HWND dlg, State* st, int r, int g, int b, SkipGroup skip) {
    st->r = clampi(r, 0, 255);
    st->g = clampi(g, 0, 255);
    st->b = clampi(b, 0, 255);
    rgb_to_hsv(st->r, st->g, st->b, st->h, st->s, st->v);
    refresh(dlg, st, skip);
}
void apply_hsv(HWND dlg, State* st, int h, int s, int v, SkipGroup skip) {
    st->h = ((h % 360) + 360) % 360;
    st->s = clampi(s, 0, 255);
    st->v = clampi(v, 0, 255);
    hsv_to_rgb(st->h, st->s, st->v, st->r, st->g, st->b);
    refresh(dlg, st, skip);
}

bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void handle_mouse(HWND dlg, State* st, int x, int y, bool down) {
    if (in_rect(x, y, SVX, SVY, SVW, SVH)) {
        int h = hue_from_x(clampi(x - SVX, 0, SVW - 1));
        int s = sat_from_y(clampi(y - SVY, 0, SVH - 1));
        apply_hsv(dlg, st, h, s, st->v, SKIP_NONE);
        SetCapture(dlg);
        return;
    }
    if (in_rect(x, y, BARX, BARY, BARW, BARH)) {
        int v = val_from_y(clampi(y - BARY, 0, BARH - 1));
        apply_hsv(dlg, st, st->h, st->s, v, SKIP_NONE);
        SetCapture(dlg);
        return;
    }
    if (down) {
        // basic grid
        if (in_rect(x, y, BASIC_X, BASIC_Y, 8 * CELL, 6 * CELL)) {
            int c = (x - BASIC_X) / CELL, r = (y - BASIC_Y) / CELL;
            if ((x - BASIC_X) % CELL < SW && (y - BASIC_Y) % CELL < SW &&
                c < 8 && r < 6) {
                COLORREF cr = kBasic[r * 8 + c];
                apply_rgb(dlg, st, GetRValue(cr), GetGValue(cr), GetBValue(cr),
                          SKIP_NONE);
            }
        }
        // custom grid
        if (in_rect(x, y, CUSTOM_X, CUSTOM_Y, 8 * CELL, 2 * CELL)) {
            int c = (x - CUSTOM_X) / CELL, r = (y - CUSTOM_Y) / CELL;
            if ((x - CUSTOM_X) % CELL < SW && (y - CUSTOM_Y) % CELL < SW &&
                c < 8 && r < 2) {
                st->sel_custom = r * 8 + c;
                COLORREF cr = st->custom[st->sel_custom];
                apply_rgb(dlg, st, GetRValue(cr), GetGValue(cr), GetBValue(cr),
                          SKIP_NONE);
            }
        }
    }
}

// ── control creation ────────────────────────────────────────────────────────
HWND mk(HWND p, const wchar_t* cls, const wchar_t* txt, DWORD style, int x,
        int y, int w, int h, int id, State* st) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w,
                             h, p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    if (st->font) SendMessageW(c, WM_SETFONT, (WPARAM)st->font, TRUE);
    return c;
}

HWND mk_spin(HWND p, int editId, int spinId, int x, int y, int lo, int hi,
             State* st, HWND* spinOut) {
    HWND e = mk(p, L"EDIT", L"0", WS_TABSTOP | WS_BORDER | ES_NUMBER, x, y, 50,
                22, editId, st);
    HWND u = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                             WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT |
                                 UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                             0, 0, 0, 0, p, (HMENU)(INT_PTR)spinId,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(u, UDM_SETBUDDY, (WPARAM)e, 0);
    SendMessageW(u, UDM_SETRANGE32, lo, hi);
    *spinOut = u;
    return e;
}

void create_controls(HWND dlg, State* st) {
    auto label = [&](const wchar_t* t, int x, int y, int w, DWORD extra = 0) {
        mk(dlg, L"STATIC", t, extra, x, y, w, 16, -1, st);
    };
    label(L"基本颜色", BASIC_X, 10, 120);
    label(L"自定义颜色", CUSTOM_X, 182, 140);
    mk(dlg, L"BUTTON", L"添加到自定义颜色(&A)", WS_TABSTOP | BS_PUSHBUTTON,
       CUSTOM_X, 256, 190, 26, IDC_ADD, st);

    mk(dlg, L"BUTTON", L"拾取屏幕颜色(&P)", WS_TABSTOP | BS_PUSHBUTTON, SVX, 184,
       150, 26, IDC_PICK, st);

    // HSV column
    label(L"色调(H):", SVX, 218, 48, SS_RIGHT);
    st->eH = mk_spin(dlg, IDC_EDIT_H, IDC_SPIN_H, SVX + 54, 216, 0, 359, st, &st->uH);
    label(L"饱和度(S):", SVX, 244, 48, SS_RIGHT);
    st->eS = mk_spin(dlg, IDC_EDIT_S, IDC_SPIN_S, SVX + 54, 242, 0, 255, st, &st->uS);
    label(L"明度(V):", SVX, 270, 48, SS_RIGHT);
    st->eV = mk_spin(dlg, IDC_EDIT_V, IDC_SPIN_V, SVX + 54, 268, 0, 255, st, &st->uV);
    // RGB column
    label(L"红(R):", SVX + 120, 218, 40, SS_RIGHT);
    st->eR = mk_spin(dlg, IDC_EDIT_R, IDC_SPIN_R, SVX + 164, 216, 0, 255, st, &st->uR);
    label(L"绿(G):", SVX + 120, 244, 40, SS_RIGHT);
    st->eG = mk_spin(dlg, IDC_EDIT_G, IDC_SPIN_G, SVX + 164, 242, 0, 255, st, &st->uG);
    label(L"蓝(B):", SVX + 120, 270, 40, SS_RIGHT);
    st->eB = mk_spin(dlg, IDC_EDIT_B, IDC_SPIN_B, SVX + 164, 268, 0, 255, st, &st->uB);

    label(L"HTML:", SVX, 298, 40, SS_RIGHT);
    st->eHtml = mk(dlg, L"EDIT", L"#000000", WS_TABSTOP | WS_BORDER, SVX + 44,
                   296, 96, 22, IDC_EDIT_HTML, st);

    mk(dlg, L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 300, 326, 78, 26,
       IDOK, st);
    mk(dlg, L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 386, 326, 78, 26,
       IDCANCEL, st);
}

LRESULT CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = get_state(dlg);
    switch (msg) {
        case WM_NCCREATE:
            st = (State*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)st);
            return DefWindowProcW(dlg, msg, wp, lp);
        case WM_CREATE:
            st->updating = true;   // ignore EN_CHANGE while building controls
            create_controls(dlg, st);
            st->updating = false;
            apply_rgb(dlg, st, st->r, st->g, st->b, SKIP_NONE);
            return 0;
        case WM_PAINT:
            on_paint(dlg, st);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(dlg, &rc);
            FillRect((HDC)wp, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
            return 1;
        }
        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if (st->picking) {  // confirm screen pick
                st->picking = false;
                ReleaseCapture();
                return 0;
            }
            handle_mouse(dlg, st, x, y, true);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (st->picking) {
                POINT p;
                GetCursorPos(&p);
                HDC sdc = GetDC(nullptr);
                COLORREF c = GetPixel(sdc, p.x, p.y);
                ReleaseDC(nullptr, sdc);
                if (c != CLR_INVALID)
                    apply_rgb(dlg, st, GetRValue(c), GetGValue(c), GetBValue(c),
                              SKIP_NONE);
                return 0;
            }
            if (wp & MK_LBUTTON) {
                int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                handle_mouse(dlg, st, x, y, false);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (GetCapture() == dlg && !st->picking) ReleaseCapture();
            return 0;
        case WM_SETCURSOR:
            if (st->picking) {
                SetCursor(LoadCursor(nullptr, IDC_CROSS));
                return TRUE;
            }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (code == EN_CHANGE && !st->updating) {
                if (id == IDC_EDIT_H || id == IDC_EDIT_S || id == IDC_EDIT_V)
                    apply_hsv(dlg, st, read_int(st->eH), read_int(st->eS),
                              read_int(st->eV), SKIP_HSV);
                else if (id == IDC_EDIT_R || id == IDC_EDIT_G || id == IDC_EDIT_B)
                    apply_rgb(dlg, st, read_int(st->eR), read_int(st->eG),
                              read_int(st->eB), SKIP_RGB);
                else if (id == IDC_EDIT_HTML) {
                    wchar_t buf[16] = {0};
                    GetWindowTextW(st->eHtml, buf, 16);
                    unsigned rr, gg, bb;
                    const wchar_t* p = buf[0] == L'#' ? buf + 1 : buf;
                    if (swscanf(p, L"%02x%02x%02x", &rr, &gg, &bb) == 3)
                        apply_rgb(dlg, st, rr, gg, bb, SKIP_HTML);
                }
                return 0;
            }
            if (code == BN_CLICKED) {
                if (id == IDOK) { st->ok = true; st->done = true; }
                else if (id == IDCANCEL) { st->ok = false; st->done = true; }
                else if (id == IDC_ADD) {
                    st->custom[st->sel_custom] = RGB(st->r, st->g, st->b);
                    st->sel_custom = (st->sel_custom + 1) % 16;
                    InvalidateRect(dlg, nullptr, FALSE);
                } else if (id == IDC_PICK) {
                    st->picking = true;
                    SetCapture(dlg);
                    SetCursor(LoadCursor(nullptr, IDC_CROSS));
                }
            }
            return 0;
        }
        case WM_CLOSE:
            st->ok = false;
            st->done = true;
            return 0;
    }
    return DefWindowProcW(dlg, msg, wp, lp);
}

}  // namespace

bool choose_color_qt(HWND owner, COLORREF initial, COLORREF& out,
                     COLORREF custom[16]) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    static bool registered = false;
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QtStyleColorDlg";
        RegisterClassExW(&wc);
        registered = true;
    }

    State st;
    st.custom = custom;
    st.r = GetRValue(initial);
    st.g = GetGValue(initial);
    st.b = GetBValue(initial);
    rgb_to_hsv(st.r, st.g, st.b, st.h, st.s, st.v);
    st.font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT rc{0, 0, CLIENT_W, CLIENT_H};
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int x = 200, y = 200;
    RECT orc;
    if (owner && GetWindowRect(owner, &orc))
        { x = orc.left + 20; y = orc.top + 20; }

    HWND dlg = CreateWindowExW(ex, L"QtStyleColorDlg", L"选择颜色", style, x, y,
                               W, H, owner, nullptr, hinst, &st);
    if (!dlg) { if (st.font) DeleteObject(st.font); return false; }

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    DestroyWindow(dlg);
    if (st.font) DeleteObject(st.font);

    if (st.ok) out = RGB(st.r, st.g, st.b);
    return st.ok;
}
