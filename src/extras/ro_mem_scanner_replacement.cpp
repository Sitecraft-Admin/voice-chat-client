#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <algorithm>

static uintptr_t get_base() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
}

static bool is_readable_region(const void* ptr, size_t len) {
    if (!ptr || len == 0) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end = addr + len;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= addr) return false;
        addr = region_end;
    }
    return true;
}

static std::string read_ascii(uintptr_t addr, size_t max_len) {
    auto* p = reinterpret_cast<const unsigned char*>(addr);
    if (!is_readable_region(p, max_len)) return "";
    std::string s;
    for (size_t i = 0; i < max_len; ++i) {
        unsigned char ch = p[i];
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7e) return "";
        s.push_back((char)ch);
    }
    return s;
}

static std::vector<uintptr_t> find_ascii_string(const std::string& target) {
    std::vector<uintptr_t> out;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end  = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            auto* buf = reinterpret_cast<const char*>(addr);
            SIZE_T sz = mbi.RegionSize;
            for (SIZE_T i = 0; i + target.size() < sz; ++i) {
                if (memcmp(buf + i, target.c_str(), target.size()) == 0 && buf[i + target.size()] == '\0') {
                    out.push_back(addr + i);
                    if (out.size() >= 16) return out;
                }
            }
        }
        addr += mbi.RegionSize;
    }
    return out;
}

static std::string hex_dump(uintptr_t addr, size_t len) {
    std::ostringstream oss;
    auto* p = reinterpret_cast<const unsigned char*>(addr);
    if (!is_readable_region(p, len)) return "<unreadable>";
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (unsigned)p[i] << ' ';
    }
    return oss.str();
}

static void print_candidate_fields(uintptr_t name_addr) {
    uintptr_t base = get_base();
    std::cout << "Name candidate @ 0x" << std::hex << name_addr
              << "  RVA=0x" << (name_addr - base) << std::dec << "\n";
    std::cout << "  raw[32] = " << hex_dump(name_addr, 32) << "\n";

    for (int back = 4; back <= 0x80; back += 4) {
        uintptr_t a = name_addr - back;
        if (!is_readable_region(reinterpret_cast<void*>(a), 4)) continue;
        int v = *reinterpret_cast<int*>(a);
        if (v > 0 && v < 2000000000) {
            std::cout << "  int @ name-0x" << std::hex << back << std::dec
                      << " = " << v
                      << "  abs=0x" << std::hex << a
                      << "  rva=0x" << (a - base) << std::dec << "\n";
        }
    }
}

int main() {
    std::cout << "RO local-player helper scanner\n";
    std::cout << "Process base: 0x" << std::hex << get_base() << std::dec << "\n\n";

    std::string name;
    std::cout << "Enter your exact char name: ";
    std::getline(std::cin, name);
    if (name.empty()) {
        std::cout << "No name entered.\n";
        return 1;
    }

    auto hits = find_ascii_string(name);
    if (hits.empty()) {
        std::cout << "No exact name hits found.\n";
        return 1;
    }

    std::cout << "Found " << hits.size() << " candidate(s).\n\n";
    for (auto addr : hits) {
        print_candidate_fields(addr);
        std::cout << "\n";
    }

    std::cout << "Tip: use the candidate whose surrounding ints match your real account_id/char_id.\n";
    return 0;
}
