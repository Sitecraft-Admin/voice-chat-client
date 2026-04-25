#pragma once
#include <Windows.h>
#include <cstdint>

namespace ROOffsets {
    inline uintptr_t base() {
        static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return b;
    }

    // Only AID + CID are read from memory.
    // X, Y, Map, Name come from the voice server (pushed by map server).
    // Client 20240822
    //constexpr uintptr_t ACCOUNT_ID = 0x116B7EC;
    //constexpr uintptr_t CHAR_ID    = 0x116B7F0;
    // Client 20250716
    constexpr uintptr_t ACCOUNT_ID = 0x011FB9A4;
    constexpr uintptr_t CHAR_ID    = 0x011FB9A8;
}

struct ROState {
    int  account_id = 0;
    int  char_id    = 0;
    bool auth_ready = false;
    bool valid      = false;
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
