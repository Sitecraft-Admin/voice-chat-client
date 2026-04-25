#pragma once
#include <Windows.h>
#include <string>
#include <cstdint>

namespace ROOffsets {
    inline uintptr_t base() {
        static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return b;
    }

    constexpr uintptr_t CHAR_X     = 0x1156ef4;
    constexpr uintptr_t CHAR_Y     = 0x1156ef8;
    constexpr uintptr_t MAP_NAME   = 0x116b7f4;
    constexpr uintptr_t CHAR_NAME  = 0x116807C;
    constexpr uintptr_t ACCOUNT_ID = 0x116B7EC;
    constexpr uintptr_t CHAR_ID    = 0x116B7F0;

    constexpr uintptr_t PARTY_ID   = 0x000000;   // TODO: ยังไม่ได้หา
    constexpr uintptr_t GUILD_ID   = 0x110C030;
}

struct ROState {
    int         x          = 0;
    int         y          = 0;
    std::string map;
    int         account_id = 0;
    int         char_id    = 0;
    std::string char_name;
    int         party_id   = 0;
    int         guild_id   = 0;
    bool        valid      = false;
    bool        auth_ready = false;
};

class MemoryReader {
public:
    static ROState read();
    static std::string dump_bytes(uintptr_t offset, size_t count);

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

    static std::string read_str(uintptr_t offset, size_t max_len);
};
