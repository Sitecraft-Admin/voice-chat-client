#include "ws_client.hpp"
#include "dbglog.hpp"
#include "obf_string.hpp"
#include <vector>

namespace {

std::wstring default_user_agent() {
    constexpr unsigned char k = 0x33u;
    constexpr std::array<unsigned char, 12> enc = {
        0x61, 0x7c, 0x1e, 0x65, 0x5c, 0x5a, 0x50, 0x56, 0x1c, 0x02, 0x1d, 0x03
    };
    return obf::decode_wide<enc.size(), k>(enc);
}

}

bool WsClient::connect(const std::wstring& host, INTERNET_PORT port, const std::wstring& path) {
    // Clean up any leftover handles from previous connection before reconnecting
    if (websocket_) { WinHttpCloseHandle(websocket_); websocket_ = nullptr; }
    if (request_)   { WinHttpCloseHandle(request_);   request_   = nullptr; }
    if (connect_)   { WinHttpCloseHandle(connect_);   connect_   = nullptr; }
    if (session_)   { WinHttpCloseHandle(session_);   session_   = nullptr; }
    if (recv_thread_.joinable()) recv_thread_.join();

    dbglog("WinHttpOpen");
    const std::wstring user_agent = default_user_agent();
    session_ = WinHttpOpen(user_agent.c_str(),
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) { dbglog("WinHttpOpen FAILED"); return false; }

    DWORD timeout = 3000;
    WinHttpSetOption(session_, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(session_, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout, sizeof(timeout));
    WinHttpSetOption(session_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    dbglog("WinHttpConnect");
    connect_ = WinHttpConnect(session_, host.c_str(), port, 0);
    if (!connect_) { dbglog("WinHttpConnect FAILED"); return false; }

    dbglog("WinHttpOpenRequest");
    request_ = WinHttpOpenRequest(connect_, L"GET", path.c_str(),
                                  nullptr, nullptr, nullptr, 0);
    if (!request_) { dbglog("WinHttpOpenRequest FAILED"); return false; }

    dbglog("WinHttpSetOption UPGRADE");
    WinHttpSetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

    dbglog("WinHttpSendRequest calling...");
    BOOL sendOk = WinHttpSendRequest(request_, nullptr, 0, nullptr, 0, 0, 0);
    if (!sendOk) {
        char errbuf[64];
        sprintf_s(errbuf, "WinHttpSendRequest FAILED err=%lu", GetLastError());
        dbglog(errbuf);
        return false;
    }
    dbglog("WinHttpSendRequest OK");

    dbglog("WinHttpReceiveResponse");
    if (!WinHttpReceiveResponse(request_, nullptr)) {
        dbglog("WinHttpReceiveResponse FAILED");
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request_,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    char scbuf[64];
    sprintf_s(scbuf, "HTTP status: %lu", statusCode);
    dbglog(scbuf);

    if (statusCode != 101) {
        dbglog("Not 101 — voice-server not running?");
        return false;
    }

    dbglog("WinHttpWebSocketCompleteUpgrade");
    websocket_ = WinHttpWebSocketCompleteUpgrade(request_, 0);
    if (!websocket_) {
        dbglog("WinHttpWebSocketCompleteUpgrade FAILED");
        return false;
    }

    dbglog("WinHttpCloseHandle request");
    WinHttpCloseHandle(request_);
    request_ = nullptr;

    connected_ = true;

    dbglog("start recv_thread");
    recv_thread_ = std::thread([this]{ recv_loop(); });
    dbglog("connect done");
    return true;
}

void WsClient::recv_loop() {
    {
        char b[48];
        sprintf_s(b, "[recv] loop started tid=%lu", GetCurrentThreadId());
        dbglog(b);
    }

    std::vector<uint8_t> buf(64 * 1024);
    DWORD bytes_read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE buf_type;

    while (connected_) {
        dbglog("[recv] calling WinHttpWebSocketReceive");

        HINTERNET ws = websocket_;
        if (!ws) break;

        DWORD ret = WinHttpWebSocketReceive(
            ws, buf.data(), static_cast<DWORD>(buf.size()),
            &bytes_read, &buf_type);

        if (!connected_) break;

        if (ret != ERROR_SUCCESS) {
            char errbuf[64];
            sprintf_s(errbuf, "[recv] receive error=%lu", ret);
            dbglog(errbuf);

            connected_ = false;
            if (on_close) on_close();
            break;
        }

        dbglog("[recv] got message");

        if (buf_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            dbglog("[recv] text message");
            if (on_text) {
                std::string msg(reinterpret_cast<char*>(buf.data()), bytes_read);
                on_text(msg);
            }
        }
        else if (buf_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (on_binary) {
                std::vector<uint8_t> data(buf.begin(), buf.begin() + bytes_read);
                on_binary(data);
            }
        }
        else if (buf_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            dbglog("[recv] close frame");
            connected_ = false;
            if (on_close) on_close();
            break;
        }
    }

    dbglog("[recv] loop ended");
}

bool WsClient::send_text(const std::string& msg) {
    if (!connected_ || !websocket_) return false;

    std::lock_guard<std::mutex> lock(send_mtx_);
    if (!connected_ || !websocket_) return false;

    DWORD ret = WinHttpWebSocketSend(
        websocket_,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<void*>(static_cast<const void*>(msg.data())),
        static_cast<DWORD>(msg.size()));

    return ret == ERROR_SUCCESS;
}

bool WsClient::send_binary(const std::vector<uint8_t>& data) {
    if (!connected_ || !websocket_) return false;

    std::lock_guard<std::mutex> lock(send_mtx_);
    if (!connected_ || !websocket_) return false;

    DWORD ret = WinHttpWebSocketSend(
        websocket_,
        WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        const_cast<void*>(static_cast<const void*>(data.data())),
        static_cast<DWORD>(data.size()));

    return ret == ERROR_SUCCESS;
}

void WsClient::disconnect() {
    bool was_connected = connected_.exchange(false);
    if (!was_connected && !websocket_ && !request_ && !connect_ && !session_) return;

    dbglog("[ws] disconnect start");

    HINTERNET ws = websocket_;
    if (ws) {
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }

    // Hold send_mtx_ before nulling/freeing the handle so any send() that passed
    // the inner connected_ check while holding the lock can finish before we free.
    {
        std::lock_guard<std::mutex> lock(send_mtx_);
        websocket_ = nullptr;
    }
    if (ws)       WinHttpCloseHandle(ws);
    if (request_) { WinHttpCloseHandle(request_); request_ = nullptr; }
    if (connect_) { WinHttpCloseHandle(connect_); connect_ = nullptr; }
    if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }

    dbglog("[ws] disconnect done");
}
