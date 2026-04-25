#pragma once
// Anti-tamper / anti-debug protection.
//
// Strategy: detect analysis tools via 6 independent methods, mark a tamper
// flag, and let the audio pipeline silently break — no obvious crash that
// the reverser can find and patch out.  Quiet failure is far harder to
// diagnose than a conspicuous abort().
//
// All checks are __forceinline so they don't appear as separate functions
// in the import table (harder to locate and NOP out individually).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>   // PROCESSENTRY32A, Process32FirstA, Process32NextA
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <cwchar>
#include <cstring>

namespace anti_tamper {

// ── Detection checks ──────────────────────────────────────────────────────

// 1. PEB.BeingDebugged — simplest check; catches most injectors that don't
//    clear the flag.
__forceinline bool chk_peb_flag() {
    return IsDebuggerPresent() != FALSE;
}

// 2. CheckRemoteDebuggerPresent — catches external debuggers (x64dbg, OllyDbg)
//    even after they've cleared PEB.BeingDebugged.
__forceinline bool chk_remote_dbg() {
    BOOL present = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &present);
    return present != FALSE;
}

// 3. NtQueryInformationProcess — ProcessDebugPort (class 7).
//    Returns a non-NULL port handle when a kernel debugger is attached.
//    Not exported by Win32 API, so we resolve dynamically to avoid an
//    obvious import entry.
__forceinline bool chk_debug_port() {
    using NtQIPFn = LONG(__stdcall*)(HANDLE, UINT, PVOID, ULONG, PULONG);
    static auto fn = reinterpret_cast<NtQIPFn>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"),
                       "NtQueryInformationProcess"));
    if (!fn) return false;
    HANDLE port = nullptr;
    // 0 == STATUS_SUCCESS
    return fn(GetCurrentProcess(), 7, &port, sizeof(port), nullptr) == 0
        && port != nullptr;
}

// 4. Heap debug flags — Windows heap manager sets extra flags when a
//    process-creation debugger is present (GFlags, appverif, etc.).
//    32-bit offsets: PEB at FS:[0x30], ProcessHeap at PEB+0x18,
//    Flags at heap+0x40, ForceFlags at heap+0x44 (Vista+/Win7+/Win10).
__forceinline bool chk_heap_flags() {
    auto* pPEB  = reinterpret_cast<BYTE*>(__readfsdword(0x30));
    auto* pHeap = *reinterpret_cast<BYTE**>(pPEB + 0x18);
    DWORD flags      = *reinterpret_cast<DWORD*>(pHeap + 0x40);
    DWORD forceFlags = *reinterpret_cast<DWORD*>(pHeap + 0x44);
    // Normal flags value is 0x00000002 (HEAP_GROWABLE). Anything else → debugger.
    return (flags & ~0x2u) != 0u || forceFlags != 0u;
}

// 5. PEB.NtGlobalFlag — debugger sets bits 0x70 (heap tail/free checking +
//    heap parameter validation). At PEB+0x68 for 32-bit processes.
__forceinline bool chk_nt_global_flag() {
    auto* pPEB = reinterpret_cast<BYTE*>(__readfsdword(0x30));
    DWORD flag = *reinterpret_cast<DWORD*>(pPEB + 0x68);
    return (flag & 0x70u) != 0u;
}

// 6. Blacklisted process names — scan the running process list for known
//    analysis tools.  Case-insensitive.  The list is intentionally not sorted
//    or grouped so it's less obvious what's being checked.
//    Uses wide-string API (Process32FirstW) since the project builds with UNICODE.
__forceinline bool chk_blacklist() {
    static const wchar_t* const s_bad[] = {
        L"x32dbg.exe",
        L"x64dbg.exe",
        L"ollydbg.exe",
        L"idaq.exe",
        L"idaq64.exe",
        L"ida.exe",
        L"ida64.exe",
        L"idafree.exe",
        L"idafree64.exe",
        L"radare2.exe",
        L"r2.exe",
        L"cutter.exe",
        L"ghidra.exe",
        L"binaryninja.exe",
        L"cheatengine-i386.exe",
        L"cheatengine-x86_64.exe",
        L"cheatengine-x86_64-sse4-avx2.exe",
        L"processhacker.exe",
        L"processhacker2.exe",
        L"systeminformer.exe",
        L"procmon.exe",
        L"procmon64.exe",
        L"procexp.exe",
        L"procexp64.exe",
        L"wireshark.exe",
        L"fiddler.exe",
        L"fiddler4.exe",
        L"charleskipper.exe",
        L"charles.exe",
        L"scylla.exe",
        L"scylla_x86.exe",
        L"scylla_x64.exe",
        L"pe-sieve32.exe",
        L"pe-sieve64.exe",
        L"apimonitor-x86.exe",
        L"apimonitor-x64.exe",
        L"api-monitor.exe",
        L"dnspy.exe",
        L"dotpeek64.exe",
        L"ilspy.exe",
        L"regshot.exe",
        L"lordpe.exe",
        L"petools.exe",
        L"importrec.exe",
        L"imprec.exe",
        L"reshacker.exe",
        L"resource_hacker.exe",
        nullptr
    };

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            // Lower-case the exe name in a local buffer
            wchar_t lower[MAX_PATH] = {};
            for (int i = 0; i < MAX_PATH - 1 && pe.szExeFile[i]; ++i)
                lower[i] = static_cast<wchar_t>(
                    towlower(static_cast<wint_t>(pe.szExeFile[i])));

            for (int k = 0; s_bad[k]; ++k) {
                if (wcscmp(lower, s_bad[k]) == 0) {
                    found = true;
                    break;
                }
            }
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ── Combined check ────────────────────────────────────────────────────────
// Returns true if any sign of debugging / analysis is detected.
inline bool is_being_analyzed() {
#ifdef _DEBUG
    // Debug builds always trigger heap-flag and NtGlobalFlag checks because
    // MSVC Debug CRT sets those flags itself — skip all checks in debug.
    return false;
#else
    return chk_peb_flag()
        || chk_remote_dbg()
        || chk_debug_port()
        || chk_heap_flags()
        || chk_nt_global_flag()
        || chk_blacklist();
#endif
}

// ── Tamper state ──────────────────────────────────────────────────────────
// Once set, never cleared.  The audio pipeline checks this and silently
// stops transmitting — the DLL keeps running so there's no obvious crash
// for the reverser to find and bypass.

inline std::atomic<bool>& tampered_ref() {
    static std::atomic<bool> s{ false };
    return s;
}

inline void mark_tampered() {
    tampered_ref().store(true, std::memory_order_relaxed);
}

// Called once per periodic tick (from a background thread).
inline void periodic_check() {
    if (!tampered_ref().load(std::memory_order_relaxed)) {
        if (is_being_analyzed())
            mark_tampered();
    }
}

// Query from audio/TX pipeline — must be fast (called every 20 ms).
inline bool is_tampered() {
    return tampered_ref().load(std::memory_order_relaxed);
}

} // namespace anti_tamper
