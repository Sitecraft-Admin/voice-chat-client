#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

using INTERNET_PORT = unsigned short;

using WsTextCallback   = std::function<void(const std::string&)>;
using WsBinaryCallback = std::function<void(const std::vector<uint8_t>&)>;
using WsCloseCallback  = std::function<void()>;

class WsClient {
public:
    bool connect(const std::wstring& host, INTERNET_PORT port, const std::wstring& path);
    void disconnect();

    bool send_text(const std::string& msg);
    bool send_binary(const std::vector<uint8_t>& data);

    bool is_connected() const { return connected_; }

    WsTextCallback   on_text;
    WsBinaryCallback on_binary;
    WsCloseCallback  on_close;

private:
    UINT_PTR socket_ = ~static_cast<UINT_PTR>(0);
    bool     wsa_started_ = false;

    std::atomic<bool> connected_{ false };
    std::thread       recv_thread_;
    std::mutex        conn_mtx_;
    std::mutex        send_mtx_; // ป้องกัน send จากหลาย thread พร้อมกัน

    void recv_loop();
};
