#include "d3d9_hook.hpp"
#include "overlay.hpp"
#include "dbglog.hpp"
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// D3D9 Device vtable indices
constexpr int VTI_RESET    = 16;
constexpr int VTI_PRESENT  = 17;
constexpr int VTI_ENDSCENE = 42;

typedef HRESULT(APIENTRY* EndScene_t)(LPDIRECT3DDEVICE9);
typedef HRESULT(APIENTRY* Present_t)(LPDIRECT3DDEVICE9, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(APIENTRY* Reset_t)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*);

// These hold the real function addresses (saved from the game's vtable).
// Called directly — no trampoline needed.
static EndScene_t oEndScene = nullptr;
static Present_t  oPresent  = nullptr;
static Reset_t    oReset    = nullptr;

static std::atomic<bool> g_installed{ false };
static LPDIRECT3DDEVICE9 g_game_device = nullptr;
static void** g_game_vtable = nullptr; // saved for uninstall / device replacement

HRESULT APIENTRY hkEndScene(LPDIRECT3DDEVICE9 pDevice);
HRESULT APIENTRY hkPresent(LPDIRECT3DDEVICE9 pDevice,
                           const RECT* pSrcRect, const RECT* pDstRect,
                           HWND hWnd, const RGNDATA* pDirtyRegion);
HRESULT APIENTRY hkReset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPP);

// ── Vtable patch helper ───────────────────────────────────────────

static void vtable_patch(void** vt, int idx, void* hook) {
    DWORD old = 0;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

static void patch_game_vtable(void** vt) {
    if (!vt) return;
    vtable_patch(vt, VTI_ENDSCENE, reinterpret_cast<void*>(&hkEndScene));
    vtable_patch(vt, VTI_PRESENT,  reinterpret_cast<void*>(&hkPresent));
    vtable_patch(vt, VTI_RESET,    reinterpret_cast<void*>(&hkReset));
    g_game_vtable = vt;
}

static void restore_game_vtable() {
    if (!g_game_vtable) return;
    if (oEndScene) vtable_patch(g_game_vtable, VTI_ENDSCENE, reinterpret_cast<void*>(oEndScene));
    if (oPresent)  vtable_patch(g_game_vtable, VTI_PRESENT,  reinterpret_cast<void*>(oPresent));
    if (oReset)    vtable_patch(g_game_vtable, VTI_RESET,    reinterpret_cast<void*>(oReset));
    g_game_vtable = nullptr;
}

// ── Inline JMP hook — used ONLY to discover the game's vtable ────
// No trampoline: we restore original bytes inside the hook, then
// switch to pure vtable hooking so all future calls need no trampoline.

struct TempHook {
    void*         target = nullptr;
    unsigned char original[5] = {};
    bool          removed = false;
};

static TempHook g_temp;

static bool temp_hook_install(void* target, void* hook_fn) {
    g_temp.target  = target;
    g_temp.removed = false;
    memcpy(g_temp.original, target, 5);

    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        char b[80]; sprintf_s(b, "[D3D9Hook] VirtualProtect FAILED err=%lu", GetLastError());
        dbglog(b);
        return false;
    }
    auto* t = static_cast<unsigned char*>(target);
    t[0] = 0xE9;
    auto rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(hook_fn) -
        (reinterpret_cast<uintptr_t>(target) + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    return true;
}

// Must be called while NOT executing through the hook (i.e. before calling original).
static void temp_hook_remove() {
    if (!g_temp.target || g_temp.removed) return;
    DWORD old = 0;
    VirtualProtect(g_temp.target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_temp.target, g_temp.original, 5);
    VirtualProtect(g_temp.target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_temp.target, 5);
    g_temp.removed = true;
    dbglog("[D3D9Hook] temp hook removed, switched to vtable hook");
}

// ── Final hooks (vtable-based, no trampoline) ─────────────────────

static bool g_rendered_this_frame = false;

static void do_frame(LPDIRECT3DDEVICE9 pDevice) {
    if (!g_game_device) {
        dbglog("[D3D] first device, init overlay");
        pDevice->AddRef();
        g_game_device = pDevice;
        Overlay::init(pDevice);
    } else if (pDevice != g_game_device) {
        char b[96];
        sprintf_s(b, "[D3D] new device %p (old %p), re-init", pDevice, g_game_device);
        dbglog(b);
        Overlay::shutdown();
        LPDIRECT3DDEVICE9 old_device = g_game_device;
        void** new_vt = *reinterpret_cast<void***>(pDevice);
        restore_game_vtable();
        if (old_device)
            old_device->Release();
        pDevice->AddRef();
        g_game_device = pDevice;
        patch_game_vtable(new_vt);
        dbglog("[D3D9Hook] vtable moved to replacement device");
        Overlay::init(pDevice);
    }
    Overlay::render(pDevice);
}

HRESULT APIENTRY hkEndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (g_installed && !g_rendered_this_frame) {
        g_rendered_this_frame = true;
        do_frame(pDevice);
    }
    HRESULT hr = oEndScene(pDevice); // direct call — no trampoline
    g_rendered_this_frame = false;
    return hr;
}

HRESULT APIENTRY hkPresent(LPDIRECT3DDEVICE9 pDevice,
                           const RECT* pSrcRect, const RECT* pDstRect,
                           HWND hWnd, const RGNDATA* pDirtyRegion) {
    if (g_installed && !g_rendered_this_frame) {
        g_rendered_this_frame = true;
        do_frame(pDevice);
    }
    HRESULT hr = oPresent(pDevice, pSrcRect, pDstRect, hWnd, pDirtyRegion); // direct
    g_rendered_this_frame = false;
    return hr;
}

HRESULT APIENTRY hkReset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPP) {
    bool is_game = g_installed && (pDevice == g_game_device);
    if (is_game) Overlay::on_reset_before();
    HRESULT hr = oReset(pDevice, pPP); // direct
    if (is_game && SUCCEEDED(hr)) Overlay::on_reset_after(pDevice);
    return hr;
}

// ── One-shot temp hook: fires once, then switches to vtable ──────

HRESULT APIENTRY hkEndSceneTemp(LPDIRECT3DDEVICE9 pDevice) {
    // 1. Get the game device's actual vtable
    void** vt = *reinterpret_cast<void***>(pDevice);

    char b[128];
    sprintf_s(b, "[D3D9Hook] game vtable=%p ES=%p PR=%p RST=%p",
        vt, vt[VTI_ENDSCENE], vt[VTI_PRESENT], vt[VTI_RESET]);
    dbglog(b);

    // 2. Save original pointers from THIS specific vtable
    oEndScene = reinterpret_cast<EndScene_t>(vt[VTI_ENDSCENE]);
    oPresent  = reinterpret_cast<Present_t>(vt[VTI_PRESENT]);
    oReset    = reinterpret_cast<Reset_t>(vt[VTI_RESET]);

    // 3. Restore original function bytes (removes the temp inline hook)
    temp_hook_remove();

    // 4. Patch the game device's vtable with our final hooks
    patch_game_vtable(vt);
    dbglog("[D3D9Hook] vtable patched on game device");

    g_installed.store(true, std::memory_order_release);

    // 5. Call original EndScene directly (bytes restored, safe to call)
    return oEndScene(pDevice);
}

// ── Install ───────────────────────────────────────────────────────

bool D3D9Hook::install() {
    {
        char modpath[MAX_PATH] = "<not loaded>";
        HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
        if (hD3D9) GetModuleFileNameA(hD3D9, modpath, MAX_PATH);
        char b[320];
        sprintf_s(b, "[D3D9Hook] d3d8=%d d3d9=%d path=%s",
            GetModuleHandleA("d3d8.dll") != nullptr, hD3D9 != nullptr, modpath);
        dbglog(b);
    }

    HWND tmpWnd = CreateWindowA("STATIC", "tmp", WS_POPUP, 0, 0, 2, 2,
                                nullptr, nullptr, nullptr, nullptr);
    if (!tmpWnd) { dbglog("[D3D9Hook] CreateWindow FAILED"); return false; }

    void* pFnEndScene = nullptr;

    LPDIRECT3D9 pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (pD3D) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = tmpWnd;
        LPDIRECT3DDEVICE9 pDev = nullptr;
        HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            tmpWnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDev);
        if (SUCCEEDED(hr) && pDev) {
            void** vt = *reinterpret_cast<void***>(pDev);
            pFnEndScene = vt[VTI_ENDSCENE];
            char b[128];
            sprintf_s(b, "[D3D9Hook] temp vtable=%p EndScene=%p", vt, pFnEndScene);
            dbglog(b);
            pDev->Release();
        } else {
            char b[64]; sprintf_s(b, "[D3D9Hook] CreateDevice hr=%08lX", (unsigned long)hr);
            dbglog(b);
        }
        pD3D->Release();
    }

    DestroyWindow(tmpWnd);

    if (!pFnEndScene) {
        dbglog("[D3D9Hook] could not get EndScene address — FAILED");
        return false;
    }

    // Install one-shot inline hook on EndScene.
    // When it fires: discovers game vtable, switches to vtable hook, removes itself.
    if (!temp_hook_install(pFnEndScene, reinterpret_cast<void*>(&hkEndSceneTemp))) {
        return false;
    }

    dbglog("[D3D9Hook] temp hook installed — waiting for first EndScene");
    return true;
}

void D3D9Hook::uninstall() {
    // Remove temp hook if not yet triggered
    temp_hook_remove();

    if (!g_installed) return;

    restore_game_vtable();

    // Stop render hooks from entering do_frame before device is released.
    g_installed.store(false, std::memory_order_release);

    Overlay::shutdown();
    if (g_game_device) {
        g_game_device->Release();
        g_game_device = nullptr;
    }
    dbglog("[D3D9Hook] uninstalled");
}

bool D3D9Hook::is_installed() { return g_installed.load(); }
