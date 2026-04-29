#include <winsock2.h>
#include <ws2tcpip.h>
#include "ws_client.hpp"
#include "dbglog.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint16_t RAW_MAGIC = 0x5654; // "VT"
constexpr uint8_t RAW_VERSION = 1;
constexpr uint32_t RAW_MAX_PAYLOAD = 1024 * 1024;

enum class RawPacketType : uint8_t {
    Text = 1,
    Binary = 2,
    Close = 3
};

static void write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

static void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

static uint16_t read_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

static bool send_all(SOCKET s, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(std::min<size_t>(len - sent, 64 * 1024)), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool recv_all(SOCKET s, uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = ::recv(s, reinterpret_cast<char*>(data + got),
                       static_cast<int>(std::min<size_t>(len - got, 64 * 1024)), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

static bool send_frame(SOCKET s, RawPacketType type, const uint8_t* data, uint32_t len) {
    std::vector<uint8_t> frame(8 + len);
    write_u16_be(frame.data(), RAW_MAGIC);
    frame[2] = RAW_VERSION;
    frame[3] = static_cast<uint8_t>(type);
    write_u32_be(frame.data() + 4, len);
    if (len && data)
        std::memcpy(frame.data() + 8, data, len);
    return send_all(s, frame.data(), frame.size());
}

}

bool WsClient::connect(const std::wstring& host, INTERNET_PORT port, const std::wstring&) {
    std::lock_guard<std::mutex> conn_lock(conn_mtx_);

    connected_ = false;
    SOCKET old_socket = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> send_lock(send_mtx_);
        old_socket = socket_;
        socket_ = INVALID_SOCKET;
        if (old_socket != INVALID_SOCKET)
            shutdown(old_socket, SD_BOTH);
    }
    if (recv_thread_.joinable())
        recv_thread_.join();
    if (old_socket != INVALID_SOCKET)
        closesocket(old_socket);
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        dbglog("[rawtcp] WSAStartup FAILED");
        return false;
    }
    wsa_started_ = true;

    const std::string host8 = wide_to_utf8(host);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string port_s = std::to_string(port);
    if (getaddrinfo(host8.c_str(), port_s.c_str(), &hints, &result) != 0 || !result) {
        dbglog("[rawtcp] getaddrinfo FAILED");
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ptr = result; ptr; ptr = ptr->ai_next) {
        s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (::connect(s, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (s == INVALID_SOCKET) {
        dbglog("[rawtcp] connect FAILED");
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> send_lock(send_mtx_);
        socket_ = s;
        connected_ = true;
    }

    dbglog("[rawtcp] connect OK");
    recv_thread_ = std::thread([this] { recv_loop(); });
    return true;
}

void WsClient::recv_loop() {
    dbglog("[rawtcp] recv loop started");

    while (connected_) {
        SOCKET s = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(send_mtx_);
            s = socket_;
        }
        if (s == INVALID_SOCKET)
            break;

        uint8_t header[8] = {};
        if (!recv_all(s, header, sizeof(header)))
            break;
        if (read_u16_be(header) != RAW_MAGIC || header[2] != RAW_VERSION)
            break;

        const auto type = static_cast<RawPacketType>(header[3]);
        const uint32_t len = read_u32_be(header + 4);
        if (len > RAW_MAX_PAYLOAD)
            break;

        std::vector<uint8_t> payload(len);
        if (len && !recv_all(s, payload.data(), len))
            break;

        if (type == RawPacketType::Text) {
            if (on_text)
                on_text(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        } else if (type == RawPacketType::Binary) {
            if (on_binary)
                on_binary(payload);
        } else if (type == RawPacketType::Close) {
            break;
        }
    }

    if (connected_.exchange(false)) {
        if (on_close)
            on_close();
    }

    dbglog("[rawtcp] recv loop ended");
}

bool WsClient::send_text(const std::string& msg) {
    std::lock_guard<std::mutex> lock(send_mtx_);
    if (!connected_ || socket_ == INVALID_SOCKET)
        return false;
    return send_frame(socket_, RawPacketType::Text,
                      reinterpret_cast<const uint8_t*>(msg.data()),
                      static_cast<uint32_t>(msg.size()));
}

bool WsClient::send_binary(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(send_mtx_);
    if (!connected_ || socket_ == INVALID_SOCKET)
        return false;
    return send_frame(socket_, RawPacketType::Binary,
                      data.data(),
                      static_cast<uint32_t>(data.size()));
}

void WsClient::disconnect() {
    std::lock_guard<std::mutex> conn_lock(conn_mtx_);

    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(send_mtx_);
        connected_ = false;
        s = socket_;
        socket_ = INVALID_SOCKET;
        if (s != INVALID_SOCKET) {
            send_frame(s, RawPacketType::Close, nullptr, 0);
            shutdown(s, SD_BOTH);
        }
    }

    if (recv_thread_.joinable())
        recv_thread_.join();

    if (s != INVALID_SOCKET)
        closesocket(s);
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }

    dbglog("[rawtcp] disconnect done");
}
