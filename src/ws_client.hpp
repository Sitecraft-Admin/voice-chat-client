#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>

using INTERNET_PORT = unsigned short;

using WsTextCallback   = std::function<void(const std::string&)>;
using WsBinaryCallback = std::function<void(const std::vector<uint8_t>&)>;
using WsCloseCallback  = std::function<void()>;

class WsClient {
public:
    bool connect(const std::wstring& host, INTERNET_PORT port, const std::wstring& path);
    void disconnect();

    bool send_text(const std::string& msg);
    bool send_text_priority(const std::string& msg); // push to front of queue (bypass audio backlog)
    bool send_binary(const std::vector<uint8_t>& data);

    bool is_connected() const { return connected_; }

    WsTextCallback   on_text;
    WsBinaryCallback on_binary;
    WsCloseCallback  on_close;

private:
    UINT_PTR socket_ = ~static_cast<UINT_PTR>(0);

    std::atomic<bool> connected_{ false };
    std::thread       recv_thread_;
    std::thread       send_thread_;
    std::mutex        conn_mtx_;
    std::mutex        socket_mtx_;
    std::mutex        queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<std::vector<uint8_t>> send_queue_;
    std::atomic<size_t> queued_bytes_{ 0 };
    static constexpr size_t MAX_QUEUED_BYTES = 1024 * 1024;

    void recv_loop();
    void send_loop();
    bool enqueue_frame(uint8_t type, const uint8_t* data, uint32_t len);
    bool enqueue_frame_priority(uint8_t type, const uint8_t* data, uint32_t len);
};
