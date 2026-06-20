// Entry point — wires the sniffer, search, and UI together, mirroring main.py.
// Captures OutfitColorantSelectRsp params off the wire; whenever both params and
// a target color are known, runs the uvy scan and pushes the result to the UI.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "search.hpp"
#include "sniffer.hpp"
#include "snappy_raw.hpp"
#include "swirl.hpp"
#include "texture.hpp"
#include "ui.hpp"
#include "colordlg.hpp"

// ── shared state (mirrors main.py _captured_params / _current_target) ────────
static std::mutex g_mtx;
static bool g_have_params = false;
static uint32_t g_picture_id = 1;
static std::vector<double> g_params;
static bool g_have_target = false;
static std::string g_target;  // stripped hex (no '#')
static std::string g_texdir;

// ── search worker: a single thread runs searches one at a time, coalescing
// requests (only the latest target+params matters). This avoids spawning a fresh
// thread per captured packet / color-pick, which under bursts piled up dozens of
// threads and made latency spiky. (mirrors _do_search's effect)
static std::mutex g_req_mtx;
static std::condition_variable g_req_cv;
static bool g_req_pending = false;
static std::string g_req_target;
static uint32_t g_req_pid = 1;
static std::vector<double> g_req_params;

static void request_search(std::string target, uint32_t pid,
                           std::vector<double> params) {
    {
        std::lock_guard<std::mutex> lk(g_req_mtx);
        g_req_target = std::move(target);
        g_req_pid = pid;
        g_req_params = std::move(params);
        g_req_pending = true;  // newest request supersedes any unstarted one
    }
    g_req_cv.notify_one();
}

static void search_worker() {
    for (;;) {
        std::string target;
        uint32_t pid;
        std::vector<double> params;
        {
            std::unique_lock<std::mutex> lk(g_req_mtx);
            g_req_cv.wait(lk, [] { return g_req_pending; });
            target = std::move(g_req_target);
            pid = g_req_pid;
            params = std::move(g_req_params);
            g_req_pending = false;
        }
        Result r;
        if (search(target, pid, params, g_texdir, r)) {
            printf("  %s -> %s  sim=%g%%  uvy=%g  slot=%d\n", target.c_str(),
                   r.hex.c_str(), r.sim * 100.0, r.uvy, r.slot);
            fflush(stdout);
            ui_post_result(r);
        }
    }
}

static void on_packet(uint32_t pid, const std::vector<double>& params) {
    std::string target;
    bool fire = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_picture_id = pid;
        g_params = params;
        g_have_params = true;
        if (g_have_target) {
            target = g_target;
            fire = true;
        }
    }
    if (fire) request_search(target, pid, params);
}

static void on_target_changed(const std::string& hex) {
    std::string stripped = hex;
    if (!stripped.empty() && stripped[0] == '#') stripped = stripped.substr(1);
    uint32_t pid = 1;
    std::vector<double> params;
    bool fire = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_target = stripped;
        g_have_target = true;
        if (g_have_params) {
            pid = g_picture_id;
            params = g_params;
            fire = true;
        }
    }
    if (fire) request_search(stripped, pid, params);
}

// ── resource path resolution ────────────────────────────────────────────────
static std::string exe_dir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

static bool is_dir(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string find_texture_dir() {
    std::string ed = exe_dir();
    const std::string sub = "/resources/swirlnoisetexture";
    std::string cands[] = {ed + sub, ed + "/." + sub, "resources/swirlnoisetexture"};
    for (const std::string& c : cands)
        if (is_dir(c)) return c;
    return ed + sub;  // default (will fail the isdir check below with a message)
}

// ── built-in self-test (no network/game needed) ─────────────────────────────
static std::vector<double> gen_params() {  // matches _parity.gen_params
    std::vector<double> p;
    uint64_t s = 123456789;
    for (int i = 0; i < 64; ++i) {
        s = (1103515245ULL * s + 12345) & 0x7fffffff;
        double v = (double)(s % 1000) / 1000.0;
        int col = i % 4;
        if (col == 2)
            p.push_back(v * 2 - 1);
        else if (col == 3)
            p.push_back(0.1 + v * 0.9);
        else
            p.push_back(v);
    }
    return p;
}

static int self_test() {
    int fail = 0;

    // snappy vectors
    struct {
        const char* name;
        std::vector<uint8_t> in;
        std::string out;
    } sv[] = {
        {"empty", {0x00}, ""},
        {"literal", {0x03, 0x08, 'a', 'b', 'c'}, "abc"},
        {"copy2", {0x06, 0x08, 'a', 'b', 'c', 0x0A, 0x03, 0x00}, "abcabc"},
        {"overlap", {0x05, 0x00, 'a', 0x0E, 0x01, 0x00}, "aaaaa"},
        {"copy1", {0x08, 0x04, 'a', 'b', 0x09, 0x02}, "abababab"},
    };
    for (auto& t : sv) {
        std::vector<uint8_t> o;
        bool ok = snap::uncompress(t.in.data(), t.in.size(), o);
        std::string got((char*)o.data(), o.size());
        bool pass = ok && got == t.out;
        printf("snappy/%-8s %s\n", t.name, pass ? "PASS" : "FAIL");
        if (!pass) fail++;
    }

    // search regression on texture 1 (expected values from the Python reference).
    // Covers tie-break cases (white/black hit sim=1.0 at multiple uvy) so the
    // parallel scan must reproduce the lowest-uvy/slot winner exactly.
    std::string texdir = find_texture_dir();
    std::vector<double> params = gen_params();
    struct Expect {
        const char* target;
        const char* hex;
        double uvy;
        int slot;
    } cases[] = {
        {"d6aa00", "#e1b500", 0.07, 3},  {"ff0000", "#ff0e01", 0.103, 2},
        {"00ff88", "#0bff88", 0.531, 5}, {"123456", "#003c64", 0.365, 1},
        {"ffffff", "#ffffff", 0.022, 1}, {"000000", "#000000", 0.287, 5},
        {"8a2be2", "#8700d1", 0.741, 1},
    };
    for (auto& e : cases) {
        Result r;
        if (!search(e.target, 1, params, texdir, r)) {
            printf("search/%s FAIL (texture not found at %s)\n", e.target,
                   texdir.c_str());
            fail++;
            continue;
        }
        bool pass = r.hex == e.hex && r.slot == e.slot &&
                    std::abs(r.uvy - e.uvy) < 1e-9;
        printf("search/%s -> %s uvy=%g slot=%d  %s\n", e.target, r.hex.c_str(),
               r.uvy, r.slot, pass ? "PASS" : "FAIL");
        if (!pass) fail++;
    }

    printf(fail ? "\nSELFTEST FAILURES: %d\n" : "\nALL SELFTESTS PASS\n", fail);
    return fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") return self_test();
    if (argc > 1 && std::string(argv[1]) == "--bench") {
        g_texdir = find_texture_dir();
        std::vector<double> params = gen_params();
        Texture tex;
        LARGE_INTEGER fr, a, b;
        QueryPerformanceFrequency(&fr);
        // texture decode cost
        int N = 50;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < N; ++i) { Texture t; load_texture(g_texdir + "/1.png", t); }
        QueryPerformanceCounter(&b);
        printf("texture decode: %.2f ms\n", (double)(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart / N);
        // full search cost
        QueryPerformanceCounter(&a);
        Result r;
        for (int i = 0; i < N; ++i) search("ff0000", 1, params, g_texdir, r);
        QueryPerformanceCounter(&b);
        printf("full search:    %.2f ms  (-> %s)\n", (double)(b.QuadPart - a.QuadPart) * 1000.0 / fr.QuadPart / N, r.hex.c_str());
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--uitest") {
        // Headless render-path check: open the window, post a real result, close.
        g_texdir = find_texture_dir();
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            Result r;
            std::vector<double> params = gen_params();
            if (search("ff0000", 1, params, g_texdir, r)) ui_post_result(r);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            ui_close();
        }).detach();
        ui_run([](const std::string&) {});
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--colordlg") {
        // Headless smoke: open the Qt-style dialog, then confirm it from a
        // worker thread; verifies create/paint/destroy don't crash.
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            HWND d = FindWindowW(L"QtStyleColorDlg", nullptr);
            if (d) PostMessageW(d, WM_COMMAND, MAKEWPARAM(1 /*IDOK*/, 0), 0);
        }).detach();
        COLORREF custom[16] = {0}, out = 0;
        bool ok = choose_color_qt(nullptr, RGB(255, 0, 0), out, custom);
        printf("colordlg ok=%d rgb=#%02x%02x%02x\n", ok, GetRValue(out),
               GetGValue(out), GetBValue(out));
        return ok ? 0 : 2;
    }

    g_texdir = find_texture_dir();
    if (!is_dir(g_texdir)) {
        fprintf(stderr, "error: texture dir not found %s\n", g_texdir.c_str());
        return 1;
    }

    std::thread(search_worker).detach();  // single coalescing search worker

    if (!start_sniffer(on_packet)) return 1;  // mirrors main.py exit-on-failure

    ui_run(on_target_changed);
    return 0;
}
