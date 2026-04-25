#pragma once

#include <array>
#include <string>
#include <string_view>

namespace obf {

template <size_t N, unsigned char Key>
inline std::string decode_ascii(const std::array<unsigned char, N>& data) {
    std::string out;
    out.resize(N);
    for (size_t i = 0; i < N; ++i)
        out[i] = static_cast<char>(data[i] ^ Key);
    return out;
}

template <size_t N, unsigned char Key>
inline std::wstring decode_wide(const std::array<unsigned char, N>& data) {
    std::wstring out;
    out.resize(N);
    for (size_t i = 0; i < N; ++i)
        out[i] = static_cast<wchar_t>(data[i] ^ Key);
    return out;
}

} // namespace obf
