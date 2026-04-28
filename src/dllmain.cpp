#include "overlay.hpp"
#include "voice_client.hpp"
#include "d3d9_hook.hpp"
#include "dbglog.hpp"
#include "anti_tamper.hpp"
#include <Windows.h>
#include <cstdio>

extern "C" __declspec(dllexport) void VoiceAttach() {}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;

    HMODULE hMod = nullptr;
    char modname[MAX_PATH] = "<unknown>";
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &hMod))
        GetModuleFileNameA(hMod, modname, MAX_PATH);

    // Also log the exception info (access violation = read/write + bad address)
    char extra[64] = "";
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
        sprintf_s(extra, " [%s addr=%p]",
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write",
            reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));

    char b[512];
    sprintf_s(b, "[CRASH] code=%08lX addr=%p tid=%lu mod=%s%s",
        code, addr, GetCurrentThreadId(), modname, extra);
    dbglog(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_initialized = false;

// ── Anti-tamper background thread ─────────────────────────────────────────
// Runs every 30 s; each tick does a full analysis-tool scan.
// We don't check more often — process enumeration is expensive and visible
// in profilers. 30 s is short enough to catch tools opened after injection.
static std::atomic<bool> g_at_running{ false };

static DWORD WINAPI AntiTamperThread(LPVOID) {
    // Initial check right at startup (catches tools that were already open
    // before the DLL was injected).
    anti_tamper::periodic_check();

    while (g_at_running.load(std::memory_order_relaxed)) {
        Sleep(30000);  // 30 seconds
        anti_tamper::periodic_check();
    }
    return 0;
}

static DWORD WINAPI MainThread(LPVOID) {
    if (g_initialized) return 0; // ไม่ init ซ้ำ
    g_initialized = true;

    dbglog("MainThread started");
    Sleep(1000);

    // Start anti-tamper monitor before any real work begins.
    g_at_running.store(true);
    CreateThread(nullptr, 0, AntiTamperThread, nullptr, 0, nullptr);

    dbglog("Calling D3D9Hook::install");
    bool hook_ok = D3D9Hook::install();
    dbglog(hook_ok ? "D3D9Hook::install OK" : "D3D9Hook::install FAILED");

    dbglog("Calling VoiceClient::init");
    VoiceClient::get().init();
    dbglog("VoiceClient::init done");

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpvReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        AddVectoredExceptionHandler(1, CrashHandler);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        dbglog("DLL_PROCESS_DETACH");
        // lpvReserved != NULL means process is terminating — OS kills all threads,
        // joining them here deadlocks or crashes. Skip cleanup in that case.
        if (lpvReserved == nullptr) {
            g_at_running.store(false);
            VoiceClient::get().shutdown();
            D3D9Hook::uninstall();
        }
    }
    return TRUE;
}
