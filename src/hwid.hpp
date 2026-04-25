#pragma once
// Hardware fingerprint — a 64-bit hash derived from:
//   1. CPU vendor + feature bits (CPUID leaves 0 and 1)
//   2. System volume serial number (C:\)
//   3. First non-virtual physical MAC address
//
// The result is included in every "auth" WebSocket message so the server
// can bind each authorized character to a specific machine.  An attacker
// who extracts the DLL binary cannot authenticate with a different machine
// unless the server explicitly allows it.
//
// Requires iphlpapi.lib (already added to the project).

#pragma comment(lib, "iphlpapi.lib")

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace hwid {

namespace detail {

// FNV-1a 64-bit — fast, good avalanche, no external dependency.
inline uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= UINT64_C(0x00000100000001B3);
    }
    return h;
}

} // namespace detail

// Returns a 16-character lowercase hex string (64-bit fingerprint).
// The value is stable across reboots as long as hardware doesn't change.
// Returns an empty string on catastrophic failure (should not happen).
inline std::string get() {
    uint64_t h = UINT64_C(0xcbf29ce484222325);  // FNV-1a offset basis

    // ── 1. CPU identification ─────────────────────────────────────────────
    // Leaf 0: max leaf + vendor string ("GenuineIntel" / "AuthenticAMD")
    // Leaf 1: family/model/stepping + feature flags
    {
        int r[4] = {};
        __cpuid(r, 0);
        h = detail::fnv1a(h, r, sizeof(r));
        __cpuid(r, 1);
        h = detail::fnv1a(h, r, sizeof(r));
    }

    // ── 2. System volume serial number ───────────────────────────────────
    {
        DWORD serial = 0;
        GetVolumeInformationA("C:\\", nullptr, 0, &serial,
                              nullptr, nullptr, nullptr, 0);
        h = detail::fnv1a(h, &serial, sizeof(serial));
    }

    // ── 3. First physical (non-virtual / non-loopback) MAC address ────────
    // We pick the numerically smallest valid MAC for stability when multiple
    // adapters are present (NIC ordering can change after driver updates).
    {
        ULONG buflen = 15u * sizeof(IP_ADAPTER_INFO);
        auto  buf    = std::make_unique<BYTE[]>(buflen);

        if (GetAdaptersInfo(
                reinterpret_cast<PIP_ADAPTER_INFO>(buf.get()),
                &buflen) == ERROR_BUFFER_OVERFLOW) {
            buf = std::make_unique<BYTE[]>(buflen);
            GetAdaptersInfo(
                reinterpret_cast<PIP_ADAPTER_INFO>(buf.get()), &buflen);
        }

        uint64_t best_mac = UINT64_MAX;
        auto* ai = reinterpret_cast<PIP_ADAPTER_INFO>(buf.get());
        for (; ai; ai = ai->Next) {
            if (ai->AddressLength != 6) continue;

            // Skip all-zeros or all-0xFF (loopback / broadcast placeholders)
            bool all0 = true, allFF = true;
            for (UINT i = 0; i < 6; ++i) {
                if (ai->Address[i] != 0x00) all0  = false;
                if (ai->Address[i] != 0xFF) allFF = false;
            }
            if (all0 || allFF) continue;

            uint64_t mac = 0;
            for (UINT i = 0; i < 6; ++i)
                mac = (mac << 8) | ai->Address[i];

            if (mac < best_mac) best_mac = mac;
        }

        if (best_mac != UINT64_MAX)
            h = detail::fnv1a(h, &best_mac, sizeof(best_mac));
    }

    // Format as 16-char lowercase hex
    char out[17] = {};
    snprintf(out, sizeof(out), "%016llx",
             static_cast<unsigned long long>(h));
    return std::string(out, 16);
}

} // namespace hwid
