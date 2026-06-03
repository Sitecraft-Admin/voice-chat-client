#include "external_overlay.hpp"
#include "overlay.hpp"
#include "d3d9_hook.hpp"
#include "dbglog.hpp"

#include <Windows.h>
#include <d3d9.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <cfloat>

#pragma comment(lib, "d3d9.lib")

namespace ExternalOverlay {
namespace {

constexpr wchar_t kClassName[] = L"VoiceExternalOverlay";

HWND               g_overlay_hwnd = nullptr;
HWND               g_game_hwnd    = nullptr;
std::thread        g_thread;
std::atomic<bool>  g_running{ false };
std::atomic<bool>  g_owns_ui{ false };

// ── D3D9 off-screen render objects ────────────────────────────────────────────
IDirect3D9*           g_d3d    = nullptr;
IDirect3DDevice9*     g_dev    = nullptr;
IDirect3DSurface9*    g_rt     = nullptr;  // full-size render target (ImGui draws here)
int                   g_rt_w = 0, g_rt_h = 0;
// Only the UI's bounding box is copied back each frame (not the whole screen):
// StretchRect the bbox from g_rt → g_small_rt (GPU), then read back the small one.
IDirect3DSurface9*    g_small_rt     = nullptr;
IDirect3DSurface9*    g_small_sysmem = nullptr;
int                   g_small_w = 0, g_small_h = 0;
D3DPRESENT_PARAMETERS g_pp{};              // kept for device-lost Reset

// Layered-window blit objects
HDC      g_mem_dc  = nullptr;
HBITMAP  g_dib     = nullptr;
void*    g_dib_bits = nullptr;
int      g_dib_w = 0, g_dib_h = 0;

// ── Find the game's main top-level window (this process) ──────────────────────
struct FindCtx { DWORD pid; HWND best; };

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE; // skip owned popups
    RECT r{};
    GetWindowRect(hwnd, &r);
    if ((r.right - r.left) < 200 || (r.bottom - r.top) < 200) return TRUE; // skip tiny
    ctx->best = hwnd; // first big visible unowned window
    return FALSE;
}

HWND find_game_window() {
    FindCtx ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Low-level keyboard / mouse hooks ──────────────────────────────────────────
// The overlay window is never focused (WS_EX_NOACTIVATE), so it cannot receive
// keyboard or wheel messages. We grab them globally instead.
//
// CRITICAL: low-level hooks are serviced on the thread that INSTALLED them, and
// that thread must return from the callback (i.e. keep pumping messages) almost
// instantly — otherwise EVERY mouse/key event for the WHOLE system stalls. So the
// hooks live on a dedicated message-pump thread that does nothing else. The
// callbacks only read a few atomics + push events into a queue; the ImGui io is
// fed later on the render thread (ImGui input is not thread-safe).
HHOOK g_kb_hook    = nullptr;
HHOOK g_mouse_hook = nullptr;
std::thread       g_hook_thread;
DWORD             g_hook_tid = 0;
std::atomic<bool> g_hook_running{ false };

struct InEvent {
    enum Kind { Key, Char, Wheel } kind;
    int            vk    = 0;
    bool           down  = false;
    unsigned short ch    = 0;
    float          wheel = 0.f;
};
std::mutex            g_inq_mtx;
std::vector<InEvent>  g_inq;

void push_event(const InEvent& e) {
    std::lock_guard<std::mutex> lk(g_inq_mtx);
    if (g_inq.size() < 256) g_inq.push_back(e);  // bound it
}

bool input_active() {
    // Require is_inited(): the hooks can fire before the external overlay has
    // created its ImGui context — feeding io then would dereference a null
    // context. is_inited() is only true once the external overlay owns + built it.
    return g_owns_ui.load() && Overlay::is_inited() &&
           GetForegroundWindow() == g_game_hwnd;
}

LRESULT CALLBACK ll_keyboard(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && input_active()) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        const UINT vk   = k->vkCode;

        // Hotkeys (parity with the old in-game wndproc hook).
        if (down && vk == VK_SCROLL) { Overlay::toggle_badge();      return 1; }
        if (down && vk == VK_HOME)   { Overlay::toggle_call_popup(); return 1; }

        const bool want_text = Overlay::external_wants_text_input();
        if (down || up) {
            push_event({ InEvent::Key, (int)vk, down, 0, 0.f });
            if (down && want_text) {
                BYTE ks[256] = { 0 };
                if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) { ks[VK_SHIFT] = ks[VK_LSHIFT] = 0x80; }
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) { ks[VK_CONTROL] = 0x80; }
                if (GetAsyncKeyState(VK_MENU)    & 0x8000) { ks[VK_MENU] = 0x80; }
                if (GetKeyState(VK_CAPITAL) & 0x0001)      { ks[VK_CAPITAL] = 0x01; }
                WCHAR buf[8];
                int n = ToUnicode(vk, k->scanCode, ks, buf, 8, 0);
                for (int i = 0; i < n; ++i)
                    if (buf[i] >= 0x20)
                        push_event({ InEvent::Char, 0, false, (unsigned short)buf[i], 0.f });
            }
        }
        // Block keys from the game ONLY while typing into an ImGui text field.
        if (want_text) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK ll_mouse(int code, WPARAM wp, LPARAM lp) {
    // Check the message type first so plain mouse-moves cost almost nothing.
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && input_active()) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        short delta = (short)HIWORD(m->mouseData);
        push_event({ InEvent::Wheel, 0, false, 0, (float)delta / (float)WHEEL_DELTA });
        if (Overlay::external_wants_mouse()) return 1; // swallow only when over UI
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Drain queued input on the RENDER thread (where ImGui io lives). Call before
// Overlay::render().
bool drain_input_queue() {
    std::vector<InEvent> evs;
    {
        std::lock_guard<std::mutex> lk(g_inq_mtx);
        evs.swap(g_inq);
    }
    for (const auto& e : evs) {
        switch (e.kind) {
            case InEvent::Key:   Overlay::feed_key_event(e.vk, e.down); break;
            case InEvent::Char:  Overlay::feed_char_utf16(e.ch);        break;
            case InEvent::Wheel: Overlay::feed_mouse_wheel(e.wheel);    break;
        }
    }
    return !evs.empty();
}

// Dedicated thread: install the hooks here and pump messages so callbacks are
// serviced instantly (never blocked by the overlay's rendering).
void hook_pump_loop() {
    g_hook_tid = GetCurrentThreadId();
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    g_kb_hook    = SetWindowsHookExW(WH_KEYBOARD_LL, ll_keyboard, hInst, 0);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL,    ll_mouse,    hInst, 0);
    dbglog(g_kb_hook && g_mouse_hook ? "[extov] input hooks installed"
                                     : "[extov] input hooks FAILED");

    MSG msg;
    while (g_hook_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_kb_hook)    { UnhookWindowsHookEx(g_kb_hook);    g_kb_hook = nullptr; }
    if (g_mouse_hook) { UnhookWindowsHookEx(g_mouse_hook); g_mouse_hook = nullptr; }
    dbglog("[extov] input hooks removed");
}

void start_input_hooks() {
    if (g_hook_running.exchange(true)) return;
    g_hook_thread = std::thread(hook_pump_loop);
}

void stop_input_hooks() {
    if (!g_hook_running.exchange(false)) return;
    // Wake the pump thread out of GetMessage. Retry in case its message queue
    // isn't created yet (PostThreadMessage fails until the first GetMessage).
    for (int i = 0; i < 100; ++i) {
        if (g_hook_tid && PostThreadMessageW(g_hook_tid, WM_QUIT, 0, 0)) break;
        Sleep(10);
    }
    if (g_hook_thread.joinable()) g_hook_thread.join();
    g_hook_tid = 0;
}

// ── DIB used as the UpdateLayeredWindow source ────────────────────────────────
void release_dib() {
    if (g_mem_dc) { DeleteDC(g_mem_dc); g_mem_dc = nullptr; }
    if (g_dib)    { DeleteObject(g_dib); g_dib = nullptr; }
    g_dib_bits = nullptr; g_dib_w = g_dib_h = 0;
}

static int round_up(int v, int a) { return (v + a - 1) / a * a; }

// Grow-only: never shrink/recreate when the UI box gets a little smaller — that
// would thrash GDI/D3D allocations every frame as e.g. the ping counter changes
// width (which degrades smoothness over time).
bool ensure_dib(int need_w, int need_h) {
    if (g_dib && g_dib_w >= need_w && g_dib_h >= need_h) return true;
    int cap_w = round_up(need_w, 64); if (g_dib_w > cap_w) cap_w = g_dib_w;
    int cap_h = round_up(need_h, 64); if (g_dib_h > cap_h) cap_h = g_dib_h;
    release_dib();

    HDC screen = GetDC(nullptr);
    g_mem_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!g_mem_dc) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cap_w;
    bmi.bmiHeader.biHeight      = -cap_h;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_dib = CreateDIBSection(g_mem_dc, &bmi, DIB_RGB_COLORS, &g_dib_bits, nullptr, 0);
    if (!g_dib) { release_dib(); return false; }
    SelectObject(g_mem_dc, g_dib);
    g_dib_w = cap_w; g_dib_h = cap_h;
    return true;
}

// ── D3D9 device + render targets ──────────────────────────────────────────────
void release_small_targets() {
    if (g_small_rt)     { g_small_rt->Release();     g_small_rt = nullptr; }
    if (g_small_sysmem) { g_small_sysmem->Release(); g_small_sysmem = nullptr; }
    g_small_w = g_small_h = 0;
}

void release_targets() {
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
    g_rt_w = g_rt_h = 0;
    release_small_targets();
}

bool create_targets(int w, int h) {
    release_targets();
    HRESULT hr = g_dev->CreateRenderTarget(
        w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
        FALSE, &g_rt, nullptr);
    if (FAILED(hr)) { dbgloghr("[extov] CreateRenderTarget", hr); return false; }
    g_rt_w = w; g_rt_h = h;
    return true;
}

// Right-size (grow-only) the small readback surfaces to the UI bounding box.
bool ensure_small_targets(int need_w, int need_h) {
    if (g_small_rt && g_small_w >= need_w && g_small_h >= need_h) return true;
    int cap_w = round_up(need_w, 64); if (g_small_w > cap_w) cap_w = g_small_w;
    int cap_h = round_up(need_h, 64); if (g_small_h > cap_h) cap_h = g_small_h;
    release_small_targets();
    HRESULT hr = g_dev->CreateRenderTarget(
        cap_w, cap_h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_small_rt, nullptr);
    if (FAILED(hr)) { dbgloghr("[extov] small CreateRenderTarget", hr); return false; }
    hr = g_dev->CreateOffscreenPlainSurface(
        cap_w, cap_h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_small_sysmem, nullptr);
    if (FAILED(hr)) { dbgloghr("[extov] small CreateOffscreenPlainSurface", hr); return false; }
    g_small_w = cap_w; g_small_h = cap_h;
    return true;
}

bool d3d_init(int w, int h) {
    Overlay::set_external_mode(true);   // skip Win32 input backend; we feed input
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { dbglog("[extov] Direct3DCreate9 failed"); return false; }

    g_pp = D3DPRESENT_PARAMETERS{};
    g_pp.Windowed               = TRUE;
    g_pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    g_pp.BackBufferWidth        = 16;   // unused — we render to our own RT
    g_pp.BackBufferHeight       = 16;
    g_pp.hDeviceWindow          = g_overlay_hwnd;
    g_pp.EnableAutoDepthStencil = FALSE;
    g_pp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_overlay_hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &g_pp, &g_dev);
    if (FAILED(hr)) {
        dbgloghr("[extov] CreateDevice(HAL,HW)", hr);
        hr = g_d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_overlay_hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &g_pp, &g_dev);
        if (FAILED(hr)) { dbgloghr("[extov] CreateDevice(HAL,SW)", hr); return false; }
    }

    if (!create_targets(w, h)) return false;
    dbglog("[extov] D3D9 device + targets created");
    return true;
}

void d3d_shutdown() {
    g_owns_ui.store(false);
    // Only tear down the overlay if WE own it (external mode). If the UI was
    // handed to the in-process overlay (exclusive-fullscreen fallback), let the
    // D3D9 hook's uninstall path release it instead — don't free another
    // thread's/device's ImGui objects from here.
    if (Overlay::is_external_mode())
        Overlay::shutdown();  // releases ImGui + UI textures bound to g_dev
    release_targets();
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
    release_dib();
}

// Toggle the overlay between click-through (clicks pass to the game) and
// interactive (clicks land on the overlay's UI) without re-creating the window.
void set_click_through(bool on) {
    LONG ex = GetWindowLongW(g_overlay_hwnd, GWL_EXSTYLE);
    const bool is_on = (ex & WS_EX_TRANSPARENT) != 0;
    if (on == is_on) return;
    if (on) ex |= WS_EX_TRANSPARENT;
    else    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(g_overlay_hwnd, GWL_EXSTYLE, ex);
}

bool g_layer_visible = false;
bool g_input_seen    = false;  // set when input events were processed this frame

void hide_layer() {
    if (g_layer_visible) { ShowWindow(g_overlay_hwnd, SW_HIDE); g_layer_visible = false; }
}

// Render the ImGui UI to the off-screen RT, then read back + blit ONLY the
// bounding box that was drawn (not the whole screen) into the layered window.
// (ox,oy) = game client-area top-left in screen coordinates.
void render_frame(int w, int h, int ox, int oy) {
    // Handle a lost device (exclusive fullscreen by another app, RDP, lock screen).
    HRESULT co = g_dev->TestCooperativeLevel();
    if (co == D3DERR_DEVICELOST) { return; }            // not ready — skip frame
    if (co == D3DERR_DEVICENOTRESET) {
        if (Overlay::is_inited()) Overlay::on_reset_before();
        release_targets();
        if (FAILED(g_dev->Reset(&g_pp))) return;
        Overlay::on_reset_after(g_dev);
        if (!create_targets(w, h)) return;
    }

    if (w != g_rt_w || h != g_rt_h) {
        if (!create_targets(w, h)) return;
    }

    // ── Feed input (full game-client coordinates; works while click-through) ───
    static ULONGLONG last_tick = GetTickCount64();
    ULONGLONG now = GetTickCount64();
    float dt = (now - last_tick) / 1000.f;
    last_tick = now;
    if (dt <= 0.f) dt = 1.f / 60.f;

    POINT cur{};
    GetCursorPos(&cur);
    float mx = float(cur.x - ox);
    float my = float(cur.y - oy);
    const bool inside = (mx >= 0 && my >= 0 && mx < w && my < h);
    if (!inside) { mx = -FLT_MAX; my = -FLT_MAX; }
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    Overlay::feed_external_input(float(w), float(h), mx, my, lmb, rmb, dt);

    // Apply queued keyboard/wheel events (pushed by the hook thread).
    if (drain_input_queue()) g_input_seen = true;

    g_dev->SetRenderTarget(0, g_rt);
    g_dev->SetDepthStencilSurface(nullptr);
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

    if (SUCCEEDED(g_dev->BeginScene())) {
        // Premultiplied alpha for UpdateLayeredWindow (ImGui sets the colour
        // blend; we add the separate alpha-channel blend, which it leaves alone).
        g_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        g_dev->SetRenderState(D3DRS_SRCBLENDALPHA,  D3DBLEND_ONE);
        g_dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);

        Overlay::render(g_dev);   // inits on first call (fonts+textures on g_dev)

        g_dev->EndScene();
    }

    // Only the UI bounding box needs to be copied back + composited. Round the box
    // outward to a 16px grid so tiny per-frame width changes (e.g. the ping
    // counter) don't resize/move the layered window every frame.
    int bx0, by0, bx1, by1;
    if (!Overlay::get_draw_bounds(bx0, by0, bx1, by1)) { hide_layer(); return; }
    bx0 = (bx0 / 16) * 16;          by0 = (by0 / 16) * 16;
    bx1 = round_up(bx1, 16);        by1 = round_up(by1, 16);
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
    if (bx1 > w) bx1 = w; if (by1 > h) by1 = h;
    const int bw = bx1 - bx0, bh = by1 - by0;
    if (bw <= 0 || bh <= 0) { hide_layer(); return; }

    if (!ensure_small_targets(bw, bh) || !ensure_dib(bw, bh)) return;

    RECT srcRect{ bx0, by0, bx1, by1 };
    RECT dstRect{ 0, 0, bw, bh };
    if (FAILED(g_dev->StretchRect(g_rt, &srcRect, g_small_rt, &dstRect, D3DTEXF_NONE)))
        return;
    if (FAILED(g_dev->GetRenderTargetData(g_small_rt, g_small_sysmem))) return;

    D3DLOCKED_RECT lr{};
    if (FAILED(g_small_sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    auto* dst = static_cast<BYTE*>(g_dib_bits);
    auto* src = static_cast<BYTE*>(lr.pBits);
    const int dst_pitch = g_dib_w * 4;   // DIB is capacity-sized (grow-only)
    const int row_bytes = bw * 4;
    for (int y = 0; y < bh; ++y)
        memcpy(dst + y * dst_pitch, src + y * lr.Pitch, row_bytes);
    g_small_sysmem->UnlockRect();

    POINT dpt{ ox + bx0, oy + by0 };   // screen position of the UI box
    SIZE  sz{ bw, bh };
    POINT spt{ 0, 0 };
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_overlay_hwnd, screen, &dpt, &sz, g_mem_dc, &spt, 0,
                        &bf, ULW_ALPHA);   // also moves/resizes the window
    ReleaseDC(nullptr, screen);

    if (!g_layer_visible) {
        SetWindowPos(g_overlay_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(g_overlay_hwnd, SW_SHOWNOACTIVATE);
        g_layer_visible = true;
    }

    // Re-assert topmost occasionally (cheap; not every frame).
    static int topmost_tick = 0;
    if (++topmost_tick >= 120) {
        topmost_tick = 0;
        SetWindowPos(g_overlay_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Interactive only while ImGui is hovering UI; otherwise pass clicks through.
    set_click_through(!Overlay::external_wants_mouse());
}

void run_loop() {
    g_game_hwnd = find_game_window();
    if (!g_game_hwnd) {
        dbglog("[extov] game window not found");
        g_owns_ui.store(false);   // let the in-process overlay take over
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // Without a class cursor, hovering the window while it's interactive (click-
    // through off) leaves the cursor undefined → Windows shows the busy/loading
    // cursor. Use the standard arrow.
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT gr{};
    GetClientRect(g_game_hwnd, &gr);
    POINT tl{ 0, 0 };
    ClientToScreen(g_game_hwnd, &tl);
    int w = gr.right - gr.left;
    int h = gr.bottom - gr.top;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    g_overlay_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClassName, L"", WS_POPUP,
        tl.x, tl.y, w, h,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_overlay_hwnd) {
        dbglog("[extov] CreateWindowEx failed");
        g_owns_ui.store(false);
        UnregisterClassW(kClassName, wc.hInstance);
        return;
    }
    // Stays hidden until render_frame has UI content to show (it manages
    // visibility + position/size via UpdateLayeredWindow).
    dbglog("[extov] overlay window created");

    if (!d3d_init(w, h)) {
        dbglog("[extov] d3d_init failed — external overlay disabled, falling back to in-process");
        d3d_shutdown();               // also clears g_owns_ui
        Overlay::set_external_mode(false); // in-process must use its Win32 input path
        DestroyWindow(g_overlay_hwnd); g_overlay_hwnd = nullptr;
        UnregisterClassW(kClassName, wc.hInstance);
        return;
    }
    // g_owns_ui already true (set in start()) — in-process overlay stays silent.

    // Global keyboard + wheel hooks run on their OWN message-pump thread so they
    // are serviced instantly (never blocked by this thread's rendering).
    start_input_hooks();

    bool relinquished = false; // gave UI to in-process overlay (exclusive FS)
    while (g_running.load()) {
        if (!IsWindow(g_game_hwnd)) break;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // ── Exclusive-fullscreen fallback ────────────────────────────────────
        // A layered window cannot draw over an exclusive-fullscreen swapchain, so
        // hand the UI back to the in-process overlay (which renders inside the
        // game's pipeline). Reclaim it when the game returns to windowed.
        //
        // Debounce the windowed/fullscreen signal: a windowed game can issue a
        // transient Reset with Windowed=FALSE during startup/resolution changes,
        // and we must NOT relinquish on those (it caused needless in-process
        // re-init). Only act once the state has been stable for several frames.
        static bool stable_windowed = true;
        static int  windowed_streak = 0;
        const bool windowed_now = D3D9Hook::is_game_windowed();
        if (windowed_now == stable_windowed) windowed_streak = 0;
        else if (++windowed_streak >= 20) { stable_windowed = windowed_now; windowed_streak = 0; }
        const bool windowed = stable_windowed;
        if (!windowed && !relinquished) {
            dbglog("[extov] exclusive fullscreen — handing UI to in-process overlay");
            Overlay::shutdown();              // tear down our (external) ImGui here
            Overlay::set_external_mode(false);
            g_owns_ui.store(false);           // in-process overlay now renders
            hide_layer();
            relinquished = true;
        } else if (windowed && relinquished) {
            dbglog("[extov] back to windowed — reclaiming UI");
            g_owns_ui.store(true);            // stop in-process rendering
            // Wait for the game thread to tear down its in-process ImGui (it does
            // this in do_frame once it sees owns_ui && !external_mode).
            for (int i = 0; i < 120 && Overlay::is_inited(); ++i) Sleep(16);
            Overlay::set_external_mode(true); // external lazy re-inits next frame
            relinquished = false;
        }
        if (relinquished) { Sleep(50); continue; } // in-process draws; we idle

        const bool game_active = (GetForegroundWindow() == g_game_hwnd) &&
                                 !IsIconic(g_game_hwnd);
        if (!game_active) { hide_layer(); Sleep(50); continue; }

        RECT cr{};
        GetClientRect(g_game_hwnd, &cr);
        POINT p{ 0, 0 };
        ClientToScreen(g_game_hwnd, &p);
        int cw = cr.right - cr.left;
        int ch = cr.bottom - cr.top;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;

        // render_frame positions/sizes the layered window itself (via
        // UpdateLayeredWindow), blitting only the UI bounding box.
        g_input_seen = false;
        render_frame(cw, ch, p.x, p.y);

        // Keep a HIGH, steady update rate. A topmost layered window forces DWM to
        // composite the game + overlay; if the overlay updates slowly (idle), the
        // composition cadence desyncs from the scrolling game and the game itself
        // visibly stutters during walking. The per-frame cost is tiny now that we
        // only blit the UI bounding box, so a constant high rate is cheap.
        // (Idle-throttling to ~30 Hz caused exactly that walk stutter.)
        Sleep(5);  // ~165–200 Hz, steady
    }

    stop_input_hooks();
    d3d_shutdown();
    if (g_overlay_hwnd) { DestroyWindow(g_overlay_hwnd); g_overlay_hwnd = nullptr; }
    UnregisterClassW(kClassName, wc.hInstance);
    dbglog("[extov] overlay loop ended");
}

} // namespace

bool start() {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true))
        return true;
    // Claim UI ownership *immediately* (before the D3D9 hook renders its first
    // frame) so the in-process overlay never inits on the game device. If the
    // external path later fails, run_loop clears this and the in-process overlay
    // takes over as the fallback.
    g_owns_ui.store(true);
    g_thread = std::thread(run_loop);
    return true;
}

void stop() {
    g_running.store(false);
    if (g_thread.joinable())
        g_thread.join();
}

bool is_running() { return g_running.load(); }
bool owns_ui()    { return g_owns_ui.load(); }

} // namespace ExternalOverlay
