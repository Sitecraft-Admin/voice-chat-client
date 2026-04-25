#include "memory_reader.hpp"
#include <Windows.h>
#include <cstring>
#include <cstdio>

static bool is_readable_exact(const void* ptr, size_t len) {
    if (!ptr || len == 0) return false;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end  = addr + len;
    if (end < addr) return false;

    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;

        uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= addr)
            return false;
        addr = region_end;
    }
    return true;
}

std::string MemoryReader::dump_bytes(uintptr_t offset, size_t count) {
    if (offset == 0 || count == 0) return "";
    auto* ptr = reinterpret_cast<const unsigned char*>(ROOffsets::base() + offset);
    if (!is_readable_exact(ptr, count)) return "";

    std::string out;
    out.reserve(count * 3);
    char b[8];
    for (size_t i = 0; i < count; ++i) {
        sprintf_s(b, "%02X ", ptr[i]);
        out += b;
    }
    return out;
}

std::string MemoryReader::read_str(uintptr_t offset, size_t max_len) {
    if (offset == 0 || max_len == 0)
        return "";

    auto* ptr = reinterpret_cast<const char*>(ROOffsets::base() + offset);
    if (!is_readable_exact(ptr, max_len))
        return "";

    std::string s;
    s.reserve(max_len);

    for (size_t i = 0; i < max_len; ++i) {
        unsigned char ch = static_cast<unsigned char>(ptr[i]);
        if (ch == 0)
            break;
        // Allow printable ASCII + extended (0x80-0xFF) for Thai TIS-620 names
        if (ch < 0x20)
            break;
        s.push_back(static_cast<char>(ch));
    }

    return s;
}

ROState MemoryReader::read() {
    ROState st{};

    try {
        st.x = read_mem<int>(ROOffsets::CHAR_X);
        st.y = read_mem<int>(ROOffsets::CHAR_Y);
        st.map = read_str(ROOffsets::MAP_NAME, 16);

        if (ROOffsets::PARTY_ID)
            st.party_id = read_mem<int>(ROOffsets::PARTY_ID);
        if (ROOffsets::GUILD_ID)
            st.guild_id = read_mem<int>(ROOffsets::GUILD_ID);

        st.account_id = read_mem<int>(ROOffsets::ACCOUNT_ID);
        st.char_id    = read_mem<int>(ROOffsets::CHAR_ID);
        st.char_name  = read_str(ROOffsets::CHAR_NAME, 24);
    } catch (...) {
        st = {};
    }

    st.valid = (!st.map.empty() || st.x != 0 || st.y != 0);
    st.auth_ready = (st.account_id > 0 && st.char_id > 0);
    return st;
}
