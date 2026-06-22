#pragma once
#include <Windows.h>
#include <cstdint>

namespace ROOffsets {
    inline uintptr_t base() {
        static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return b;
    }

    // Only AID + CID (and optionally LOGIN_ID1) are read from memory.
    // X, Y, Map, Name come from the voice server (pushed by map server).
    //
    // LOGIN_ID1 is optional anti-spoof hardening: a random per-login session key
    // that only the real client process holds in memory. The map server forwards
    // the authoritative value to the voice server via auth_advisory, so when this
    // is set (non-zero) the DLL sends it on auth and the voice server rejects any
    // session whose login_id1 doesn't match — knowing just a victim's AID/CID plus
    // the shared client secret is no longer enough to spoof.
    //   0 = disabled (backward compatible): the DLL omits login_id1 from auth and
    //   the server stays on the AID/CID-only check.
    // Find offsets with tools/ro-mem-scanner. For LOGIN_ID1 it reads the current
    // value from the voice server log line "l1=<value>" (log_level 3).
    //
    // Switch the active block to match your client version.

    // ── Client 20240822 ──────────────────────────────────────────────────────
    constexpr uintptr_t ACCOUNT_ID = 0x116B7EC;
    constexpr uintptr_t CHAR_ID    = 0x116B7F0;
    constexpr uintptr_t LOGIN_ID1  = 0x0116B07C;

    // ── Client 20250716 ──────────────────────────────────────────────────────
    //constexpr uintptr_t ACCOUNT_ID = 0x011FB9A4;
    //constexpr uintptr_t CHAR_ID    = 0x011FB9A8;
    //constexpr uintptr_t LOGIN_ID1  = 0x011FB244;
}

struct ROState {
    int      account_id = 0;
    int      char_id    = 0;
    uint32_t login_id1  = 0;
    bool     auth_ready = false;
    bool     valid      = false;
};

class MemoryReader {
public:
    static ROState read();

private:
    template<typename T>
    static T read_mem(uintptr_t offset) {
        if (offset == 0) return T{};
        auto* ptr = reinterpret_cast<T*>(ROOffsets::base() + offset);
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return T{};
        if (mbi.State != MEM_COMMIT) return T{};
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return T{};
        __try {
            return *ptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return T{};
        }
    }
};
