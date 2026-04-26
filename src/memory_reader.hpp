#pragma once
#include <Windows.h>
#include <cstdint>
#include <atomic>
#include <unordered_map>

namespace ROOffsets {
    inline uintptr_t base() {
        static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return b;
    }

    // Only AID + CID are read from memory.
    // X, Y, Map, Name come from the voice server (pushed by map server).
    struct Offsets { uintptr_t account_id; uintptr_t char_id; };

    inline const std::unordered_map<uint32_t, Offsets>& table() {
        static const std::unordered_map<uint32_t, Offsets> t = {
            { 20240822u, { 0x116B7ECu, 0x116B7F0u } },
            { 20250716u, { 0x011FB9A4u, 0x011FB9A8u } },
        };
        return t;
    }

    inline std::atomic<uint32_t>& client_ver() {
        static std::atomic<uint32_t> ver{0u};
        return ver;
    }

    inline const Offsets* get() {
        auto it = table().find(client_ver().load());
        return it != table().end() ? &it->second : nullptr;
    }
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
