#include "voice_client.hpp"
#include <nlohmann/json.hpp>
#include <opus/opus.h>
#include <cstring>
#include <cmath>
#include <fstream>
#include <Windows.h>
#include "obf_string.hpp"
#include "anti_tamper.hpp"
#include "hwid.hpp"

using json = nlohmann::json;

#include "dbglog.hpp"
#include <cstdint>

// Cached at first auth; stays constant for the lifetime of the process.
static const std::string& get_hwid() {
    static std::string id = hwid::get();
    return id;
}

static uint64_t g_voice_session_id = 0;
static uint64_t make_session_id() {
    ULONGLONG t = GetTickCount64();
    uint64_t pid = static_cast<uint64_t>(GetCurrentProcessId());
    uint64_t tid = static_cast<uint64_t>(GetCurrentThreadId());
    return (static_cast<uint64_t>(t) << 16) ^ (pid << 32) ^ tid;
}

static std::string map_session_ended_text() {
    constexpr unsigned char k = 0x21u;
    constexpr std::array<unsigned char, 17> enc = {
        0x4c, 0x40, 0x51, 0x01, 0x52, 0x44, 0x52, 0x52, 0x48, 0x4e, 0x4f, 0x01, 0x44, 0x4f, 0x45, 0x44, 0x45
    };
    return obf::decode_ascii<enc.size(), k>(enc);
}

static void apply_mic_highpass(int16_t* pcm, size_t n, float& x1, float& y1) {
    if (!pcm || n == 0) return;

    // 1-pole DC-block / rumble cut around ~70 Hz @ 48 kHz.
    // Keeps more body in male/low voices while still trimming wind and desk rumble.
    constexpr float R = 0.9910f;
    for (size_t i = 0; i < n; ++i) {
        const float x = pcm[i] * (1.0f / 32768.0f);
        float y = x - x1 + R * y1;
        x1 = x;
        y1 = y;
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        pcm[i] = static_cast<int16_t>(y * 32767.0f);
    }
}

static void apply_soft_limiter(int16_t* pcm, size_t n) {
    if (!pcm || n == 0) return;

    constexpr float THRESH = 0.94f;
    constexpr float CEIL   = 0.99f;
    constexpr float CURVE  = 1.35f;
    const float curve_norm = std::tanh(CURVE);
    for (size_t i = 0; i < n; ++i) {
        float x = pcm[i] * (1.0f / 32768.0f);
        const float sign = (x < 0.0f) ? -1.0f : 1.0f;
        float a = std::fabs(x);
        if (a > THRESH) {
            const float t = (a - THRESH) / (1.0f - THRESH);
            a = THRESH + (CEIL - THRESH) * (std::tanh(t * CURVE) / curve_norm);
            x = sign * a;
        }
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        pcm[i] = static_cast<int16_t>(x * 32767.0f);
    }
}

// ── wstring ↔ UTF-8 helpers ───────────────────────────────────────────────────
static std::string wstr_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring utf8_to_wstr(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// TIS-620 (RO Thai client encoding) → UTF-8
static std::string tis620_to_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (c == 0) break;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c >= 0xA1) {
            uint32_t cp = 0x0E00u + (c - 0xA0u);
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

void VoiceClient::load_settings(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) { dbglog("[cfg] no settings file — using defaults"); return; }

    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    };

    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        trim(key); trim(val);
        if (key.empty()) continue;

        try {
            if      (key == "ptt_key")       ptt_key_.store(std::stoi(val));
            else if (key == "open_mic")      open_mic_.store(val == "1");
            else if (key == "muted")         muted_.store(val == "1");
            else if (key == "deafened")      deafened_.store(val == "1");
            else if (key == "channel") {
                std::lock_guard<std::mutex> lk(state_mtx_);
                channel_ = static_cast<Channel>(std::stoi(val));
            }
            else if (key == "mic_gain")      capture_.gain.store(std::stof(val));
            else if (key == "speaker_gain")  playback_.gain.store(std::stof(val));
            else if (key == "mic_device"  && !val.empty()) capture_.set_device(utf8_to_wstr(val));
            else if (key == "speaker_device" && !val.empty()) playback_.set_device(utf8_to_wstr(val));
            else if (key == "noise_suppression") noise_suppressor_.set_enabled(val == "1");
            else if (key == "agc")               agc_.set_enabled(val == "1");
            else if (key == "vad")               vad_.set_enabled(val == "1");
            else if (key == "aec")               aec_.set_enabled(val == "1");
            else if (key == "loudness_norm")     playback_.set_loudness_norm(val == "1");
            else if (key == "client_secret")     client_secret_ = val;
        } catch (...) {}
    }
    if (deafened_.load()) {
        const bool was_muted = muted_.load();
        muted_before_deafen_.store(was_muted);
        muted_.store(true);
        last_local_talk_tick_.store(0);
        reset_mic_pipeline_.store(true);
    }
    dbglog("[cfg] settings loaded");
}

void VoiceClient::save_settings(const char* path) {
    std::ofstream f(path);
    if (!f.is_open()) { dbglog("[cfg] cannot save settings"); return; }

    Channel channel = Channel::Normal;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        // Room and Whisper are transient session states — saving them causes
        // the player to reload into a broken channel (no room/peer exists).
        // Persist the pre-transient channel so the next session starts clean.
        if (channel_ == Channel::Room)
            channel = pre_room_channel_;
        else if (channel_ == Channel::Whisper)
            channel = pre_whisper_channel_;
        else
            channel = channel_;
    }

    // When deafened, deafen forces muted_=true.  Save the *pre-deafen* mute
    // state so that after reload + un-deafen the mic is correctly unmuted.
    const bool save_muted = deafened_.load() ? muted_before_deafen_.load()
                                             : muted_.load();

    f << "// Voice Client Settings — auto-saved\n"
      << "ptt_key: "        << ptt_key_.load()                       << "\n"
      << "open_mic: "       << (open_mic_.load() ? 1 : 0)           << "\n"
      << "muted: "          << (save_muted        ? 1 : 0)           << "\n"
      << "deafened: "       << (deafened_.load() ? 1 : 0)           << "\n"
      << "channel: "        << static_cast<int>(channel)            << "\n"
      << "mic_gain: "       << capture_.gain.load()                 << "\n"
      << "speaker_gain: "   << playback_.gain.load()                << "\n"
      << "mic_device: "     << wstr_to_utf8(capture_.get_device())   << "\n"
      << "speaker_device: "      << wstr_to_utf8(playback_.get_device())         << "\n"
      << "noise_suppression: "   << (noise_suppressor_.is_enabled()  ? 1 : 0) << "\n"
      << "agc: "                 << (agc_.is_enabled()               ? 1 : 0) << "\n"
      << "vad: "                 << (vad_.is_enabled()               ? 1 : 0) << "\n"
      << "aec: "                 << (aec_.is_enabled()               ? 1 : 0) << "\n"
      << "loudness_norm: "       << (playback_.is_loudness_norm()    ? 1 : 0) << "\n"
      << "client_secret: "      << client_secret_                           << "\n";

    dbglog("[cfg] settings saved");
}

Channel VoiceClient::get_channel() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return channel_;
}

bool VoiceClient::is_locally_talking() const {
    if (muted_.load() || !ptt_active_.load()) return false;
    const DWORD last = last_local_talk_tick_.load();
    return last != 0 && (GetTickCount() - last) <= 220;
}

VoiceClient::WhisperState VoiceClient::get_whisper_state() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return whisper_state_;
}

std::string VoiceClient::get_whisper_peer() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return whisper_peer_name_;
}

std::string VoiceClient::get_whisper_sid() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return whisper_sid_;
}

DWORD VoiceClient::get_whisper_tick() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return whisper_tick_;
}

std::string VoiceClient::get_whisper_notice() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return whisper_notice_;
}

DWORD VoiceClient::get_whisper_notice_tick() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return whisper_notice_tick_;
}

void VoiceClient::clear_whisper_notice() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    whisper_notice_.clear();
    whisper_notice_tick_ = 0;
}

void VoiceClient::set_whisper_notice(std::string notice) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    whisper_notice_ = std::move(notice);
    whisper_notice_tick_ = GetTickCount();
}

struct SpatialParams {
    float pan = 0.0f;
    float rear_attn = 1.0f;
};

static SpatialParams calc_spatial_2d(int my_x, int my_y, int sx, int sy, float max_range) {
    SpatialParams sp{};

    if (max_range <= 0.0f) max_range = 14.0f;

    const float dx = static_cast<float>(sx - my_x);
    const float dy = static_cast<float>(sy - my_y);

    sp.pan = dx / max_range;
    if (sp.pan >  1.0f) sp.pan =  1.0f;
    if (sp.pan < -1.0f) sp.pan = -1.0f;

    float front = -dy / max_range;
    if (front >  1.0f) front =  1.0f;
    if (front < -1.0f) front = -1.0f;

    if (front < 0.0f) {
        const float back = -front;
        sp.rear_attn = 1.0f - back * 0.35f;
        if (sp.rear_attn < 0.0f) sp.rear_attn = 0.0f;
    }

    return sp;
}

void VoiceClient::init_opus_encoder() {
    if (opus_enc_) return;
    int err = 0;
    opus_enc_ = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!opus_enc_) {
        char b[64]; sprintf_s(b, "[opus] encoder create FAILED err=%d", err); dbglog(b);
        return;
    }
    // 40 kbps VBR: Opus voice at this bitrate is perceptually transparent for
    // speech and saves ~65% bandwidth vs the old 112 kbps max.
    opus_encoder_ctl(opus_enc_, OPUS_SET_BITRATE(40000));
    opus_encoder_ctl(opus_enc_, OPUS_SET_VBR(1));
    opus_encoder_ctl(opus_enc_, OPUS_SET_VBR_CONSTRAINT(0));
    // Complexity 8: imperceptible quality difference vs 10 for speech,
    // saves ~20% encode CPU — important when running inside a game process.
    opus_encoder_ctl(opus_enc_, OPUS_SET_COMPLEXITY(8));
    opus_encoder_ctl(opus_enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    // Super-wideband (0–12 kHz): spreads 40 kbps more efficiently than fullband
    // while preserving speech intelligibility and presence.
    opus_encoder_ctl(opus_enc_, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_SUPERWIDEBAND));
    opus_encoder_ctl(opus_enc_, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_SUPERWIDEBAND));
    opus_encoder_ctl(opus_enc_, OPUS_SET_DTX(0));
    opus_encoder_ctl(opus_enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(opus_enc_, OPUS_SET_PACKET_LOSS_PERC(2));
    opus_encoder_ctl(opus_enc_, OPUS_SET_LSB_DEPTH(16));
    dbglog("[opus] encoder ready (40kbps SWB VBR complexity=8 FEC no-DTX)");
}

void VoiceClient::destroy_opus_encoder() {
    if (opus_enc_) { opus_encoder_destroy(opus_enc_); opus_enc_ = nullptr; }
}

void VoiceClient::reset_mic_filter_state() {
    mic_hpf_x1_ = 0.0f;
    mic_hpf_y1_ = 0.0f;
}

static void reset_opus_encoder_state(OpusEncoder* enc) {
    if (enc) opus_encoder_ctl(enc, OPUS_RESET_STATE);
}

// Push an outgoing WS message onto the queue. Safe to call from any thread
// (D3D9, capture, etc.). position_loop drains the queue so no blocking I/O
// ever runs on the game render thread.
void VoiceClient::enqueue_ws_send(std::string msg) {
    std::lock_guard<std::mutex> lk(ws_send_queue_mtx_);
    ws_send_queue_.push_back(std::move(msg));
}

void VoiceClient::init() {
    if (running_) return;

    // Start with the cleanest/rawest mic path by default.
    // Users who need extra cleanup can enable NS/AGC/VAD/AEC later from settings,
    // but keeping them off avoids the "metallic / underwater / chopped" sound
    // on first run.
    noise_suppressor_.set_enabled(false);
    agc_.set_enabled(false);
    vad_.set_enabled(false);
    aec_.set_enabled(false);

    load_settings();   // restore PTT key, gains, devices, channel etc.

    running_ = true;
    auth_sent_ = false;
    if (g_voice_session_id == 0) g_voice_session_id = make_session_id();
    reconnecting_ = false;
    init_opus_encoder();

    std::thread([this]{
        dbglog("bg: connect_to_server start");
        connect_to_server();
        dbglog("bg: connect_to_server done");

        if (!position_thread_.joinable()) {
            dbglog("bg: starting position_thread");
            position_thread_ = std::thread([this]{ position_loop(); });
        }

        // Register AEC reference tap so the render thread feeds its pre-pan
        // mono mix of decoded voices into the echo canceller. Capture then
        // subtracts the estimated echo from the mic signal.
        playback_.set_aec_reference_callback(
            [this](const int16_t* pcm, size_t n) {
                aec_.push_reference(pcm, n);
            });

        dbglog("bg: starting audio capture");
        bool ok = capture_.start([this](const std::vector<int16_t>& pcm){
            on_audio_captured(pcm);
        });
        dbglog(ok ? "bg: audio capture started OK" : "bg: audio capture FAILED");
    }).detach();

    dbglog("VoiceClient::init returned (bg thread running)");
}

void VoiceClient::shutdown() {
    running_ = false;
    reconnecting_ = false;
    auth_sent_ = false;
    reset_mic_pipeline_.store(true);
    pcm_accum_.clear();
    reset_mic_filter_state();

    capture_.stop();
    playback_.stop_all();
    ws_.disconnect();
    destroy_opus_encoder();

    if (position_thread_.joinable())
        position_thread_.join();
}

void VoiceClient::on_ws_closed() {
    auth_sent_ = false;
    // NOTE: do NOT touch pcm_accum_ here — this function now runs on both
    // the WS recv thread (server-drop path) AND the position thread (char
    // switch path), while pcm_accum_ is mutated by the audio capture thread.
    // Clearing it cross-thread was a heap race that crashed the game on
    // char switch with PTT held. The audio thread self-clears pcm_accum_
    // on its next callback when it sees ws_.is_connected() == false, which
    // is sufficient — a stale 20 ms of audio in the buffer is harmless
    // and will be dropped before the next transmit.
    // If we were in a Room channel, reset to pre-room channel on disconnect
    // because server-side chat_room_id will be gone after reconnect
    bool restored_room_channel = false;
    bool cleared_whisper = false;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (channel_ == Channel::Whisper)
            channel_ = pre_whisper_channel_;
        if (channel_ == Channel::Room) {
            channel_ = pre_room_channel_;
            restored_room_channel = true;
        }
        cleared_whisper = (whisper_state_ != WhisperState::None) || !whisper_sid_.empty();
        whisper_state_ = WhisperState::None;
        whisper_sid_.clear();
        whisper_peer_name_.clear();
        whisper_peer_id_ = 0;
        whisper_tick_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(nearby_mtx_);
        nearby_players_.clear();
    }
    if (restored_room_channel) {
        dbglog("[ws] disconnected while in Room — restored pre-room channel");
    }
    if (cleared_whisper) {
        dbglog("[ws] disconnected during whisper — cleared local whisper state");
    }
    if (!running_) return;
    bool expected = false;
    if (!reconnecting_.compare_exchange_strong(expected, true)) return;
    dbglog("[ws] disconnected — starting reconnect loop");
    std::thread(&VoiceClient::reconnect_loop, this).detach();
}

void VoiceClient::reconnect_loop() {
    dbglog("[ws] reconnect loop start");
    // First attempt is fast (500 ms) so char-switch feels instant.
    // On repeated failures (server down) the delay backs off to 10 s.
    int delay = 500;
    while (running_ && !ws_.is_connected()) {
        Sleep(delay);
        if (!running_) break;
        if (!ws_.is_connected()) {
            dbglog("[ws] reconnecting...");
            connect_to_server();
            // Backoff: 500 → 2500 → 4500 → … → 10000 ms
            delay += 2000;
            if (delay > 10000) delay = 10000;
        }
    }
    reconnecting_ = false;
    dbglog("[ws] reconnect loop end");
}

void VoiceClient::connect_to_server() {
    ws_.on_text   = [this](const std::string& m){ on_text_message(m); };
    ws_.on_binary = [this](const std::vector<uint8_t>& d){ on_binary_message(d); };
    ws_.on_close  = [this](){ on_ws_closed(); };

    dbglog("ws_.connect start");
    bool ok = false;
    try {
        ok = ws_.connect(server_host_, server_port_, L"/");
    } catch (const std::exception& ex) {
        char b[512];
        sprintf_s(b, "ws_.connect std::exception: %s", ex.what());
        dbglog(b);
        return;
    } catch (...) {
        dbglog("ws_.connect EXCEPTION");
        return;
    }

    dbglog(ok ? "ws_.connect OK" : "ws_.connect FAILED (voice-server not running?)");
    if (!ok) return;

    auth_sent_ = false;
    try_send_auth();
}

void VoiceClient::try_send_auth() {
    if (!ws_.is_connected() || auth_sent_)
        return;

    auto st = MemoryReader::read();
    // Position comes from Map Server via UDP — DLL only needs AID + CID to auth.
    // MAP_NAME / CHAR_X / CHAR_Y offsets are not required for auth to work.
    if (!st.auth_ready) {
        char_switch_pending_ = false;  // not in game — clear stale flag
        char b[256];
        sprintf_s(b, "[auth] not ready acc=%d char=%d",
            st.account_id, st.char_id);
        dbglog(b);
        return;
    }

    // If server told us "map session ended" (char switch kick), hold off until
    // the char_id in memory actually changes.  This avoids re-authing as the
    // old char and burning an extra kick/reconnect round-trip.
    if (char_switch_pending_ && st.char_id == last_auth_char_id_.load()) {
        return;   // still showing old char — wait for new char to load
    }
    char_switch_pending_ = false;

    json auth;
    auth["type"]       = "auth";
    auth["account_id"] = st.account_id;
    auth["char_id"]    = st.char_id;
    auth["session_id"] = g_voice_session_id;
    auth["hwid"]       = get_hwid();
    auth["secret"]     = client_secret_;

    if (ws_.send_text(auth.dump())) {
        auth_sent_ = true;
        last_auth_char_id_.store(st.char_id);   // so position_loop won't see this as a char-switch
        char b[256];
        sprintf_s(b, "[auth] sent acc=%d char=%d",
            st.account_id, st.char_id);
        dbglog(b);
    } else {
        dbglog("[auth] send failed");
    }
}

void VoiceClient::on_text_message(const std::string& msg) {
    json j;
    try { j = json::parse(msg); } catch(...) { return; }

    std::string type = j.value("type", "");

    if (type == "auth_ok") {
        dbglog("[auth] auth_ok");
        // Sync current client-side TX/RX state to server after auth.
        // Why: position_loop may have sent ptt before auth completed, and the
        // server ignores pre-auth state updates. Re-sending here prevents the
        // "must toggle PTT/open mic once before others can hear me" problem.
        {
            Channel channel = get_channel();
            json msg;
            msg["type"]    = "set_channel";
            msg["channel"] = static_cast<int>(channel);
            ws_.send_text(msg.dump());
        }
        {
            json msg;
            msg["type"]  = "mute";
            msg["value"] = muted_.load();
            msg["session_id"] = g_voice_session_id;
            ws_.send_text(msg.dump());
        }
        {
            json msg;
            msg["type"]  = "deafen";
            msg["value"] = deafened_.load();
            msg["session_id"] = g_voice_session_id;
            ws_.send_text(msg.dump());
        }
        {
            const int ptt_key = ptt_key_.load();
            const bool ptt = in_map_.load() && (open_mic_.load() || ((GetAsyncKeyState(ptt_key) & 0x8000) != 0));
            ptt_active_.store(ptt);

            json msg;
            msg["type"]  = "ptt";
            msg["value"] = ptt;
            msg["session_id"] = g_voice_session_id;
            ws_.send_text(msg.dump());
        }
    }
    else if (type == "your_pos") {
        // Server echoes our own position (from map server UDP) so we don't
        // need CHAR_X / CHAR_Y memory offsets for stereo panning.
        server_pos_x_.store(j.value("x", 0));
        server_pos_y_.store(j.value("y", 0));
#ifdef VOICE_LOG
        char b[96];
        sprintf_s(b, "[your_pos] x=%d y=%d map=%s",
            server_pos_x_.load(), server_pos_y_.load(),
            j.value("map", "").c_str());
        dbglog(b);
#endif
    }
    else if (type == "pong") {
        const uint32_t sent = j.value("t", 0u);
        const uint32_t now = static_cast<uint32_t>(GetTickCount());
        if (sent != 0u && now >= sent) {
            rtt_ms_.store(now - sent);
        }
    }
    else if (type == "channel_ack") {
        const int ack_channel = j.value("channel", -1);
        const int pending = pending_channel_ack_.load();
        if (pending >= 0 && ack_channel == pending) {
            const DWORD started = pending_channel_tick_.load();
            const DWORD now = GetTickCount();
            if (started != 0 && now >= started)
                channel_switch_ack_ms_.store(now - started);
            pending_channel_ack_.store(-1);
            pending_channel_tick_.store(0);
        }
    }
    else if (type == "room_joined") {
        int room_id = j.value("room_id", 0);
        if (room_id > 0) {
            int prev_channel = 0;
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                if (channel_ != Channel::Room)
                    pre_room_channel_ = channel_;
                channel_ = Channel::Room;
                prev_channel = (int)pre_room_channel_;
            }
            char b[64]; sprintf_s(b, "[room] joined room_id=%d -> Room channel (was %d)", room_id, prev_channel);
            dbglog(b);
        }
    }
    else if (type == "room_left") {
        int restored_channel = -1;
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            if (channel_ == Channel::Room) {
                channel_ = pre_room_channel_; // restore previous channel (Party/Guild/Normal)
                restored_channel = (int)channel_;
            }
        }
        if (restored_channel >= 0) {
            char b[64]; sprintf_s(b, "[room] left room -> restored channel %d", restored_channel);
            dbglog(b);
        }
    }
    else if (type == "war_state") {
        const bool active = j.value("active", false);
        const int recommended = j.value("recommended_channel", 0);
        war_mode_.store(active);
        war_recommended_channel_.store(recommended);

        Channel switched_to = Channel::Normal;
        bool should_switch = false;
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            if (active && (channel_ == Channel::Normal || channel_ == Channel::Room)) {
                if (recommended == static_cast<int>(Channel::Guild)) {
                    switched_to = Channel::Guild;
                    should_switch = true;
                } else if (recommended == static_cast<int>(Channel::Party)) {
                    switched_to = Channel::Party;
                    should_switch = true;
                }
            }
        }

        if (should_switch) {
            set_channel(switched_to);
            set_whisper_notice("War Mode");
            char b[80]; sprintf_s(b, "[war] active -> switched channel %d", static_cast<int>(switched_to));
            dbglog(b);
        } else if (active) {
            dbglog("[war] active");
        } else {
            dbglog("[war] inactive");
        }
    }
    else if (type == "nearby_players") {
        std::lock_guard<std::mutex> lk(nearby_mtx_);
        nearby_players_.clear();
        if (j.contains("players") && j["players"].is_array()) {
            for (const auto& p : j["players"]) {
                uint32_t id = p.value("id", 0u);
                std::string name = p.value("name", "");
                if (id > 0) nearby_players_[id] = name;
            }
        }
    }
    else if (type == "error") {
        std::string err = j.value("message", "");
        if (err.empty()) err = j.value("error", "");
        if (err.empty()) err = msg;
        std::string line = "[server/error] " + err;
        dbglog(line.c_str());
        // "map session ended" = server kicked us due to char switch (auth_revoke).
        // Set flag so try_send_auth waits for the new char_id to appear in memory
        // before reconnecting — prevents a wasted round-trip where we re-auth as
        // the old char and immediately get kicked again.
        if (err == map_session_ended_text())
            char_switch_pending_ = true;
    }
    else if (type == "whisper_incoming") {
        std::lock_guard<std::mutex> lk(state_mtx_);
        whisper_sid_       = j.value("sid", "");
        whisper_peer_name_ = j.value("from_name", "");
        whisper_peer_id_   = static_cast<uint32_t>(j.value("from_char_id", 0));
        whisper_state_     = WhisperState::Incoming;
        whisper_tick_      = GetTickCount();
        dbglog("[whisper] incoming from");
    }
    else if (type == "whisper_calling") {
        std::lock_guard<std::mutex> lk(state_mtx_);
        whisper_sid_       = j.value("sid", "");
        whisper_peer_name_ = j.value("target_name", "");
        whisper_state_     = WhisperState::Calling;
        whisper_tick_      = GetTickCount();
        dbglog("[whisper] calling");
    }
    else if (type == "whisper_active") {
        std::lock_guard<std::mutex> lk(state_mtx_);
        whisper_sid_         = j.value("sid", "");
        whisper_peer_name_   = j.value("peer_name", "");
        whisper_state_       = WhisperState::Active;
        whisper_tick_        = GetTickCount();
        pre_whisper_channel_ = channel_;
        channel_             = Channel::Whisper;
        dbglog("[whisper] active");
    }
    else if (type == "whisper_rejected") {
        std::lock_guard<std::mutex> lk(state_mtx_);
        whisper_state_ = WhisperState::None;
        whisper_sid_.clear();
        whisper_peer_name_.clear();
        dbglog("[whisper] rejected");
    }
    else if (type == "whisper_ended") {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (channel_ == Channel::Whisper)
            channel_ = pre_whisper_channel_;
        whisper_state_ = WhisperState::None;
        whisper_sid_.clear();
        whisper_peer_name_.clear();
        dbglog("[whisper] ended");
    }
    else if (type == "whisper_unavailable") {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            whisper_state_ = WhisperState::None;
            whisper_sid_.clear();
            whisper_peer_name_.clear();
        }
        set_whisper_notice("offline");
        dbglog("[whisper] target unavailable");
    }
    else if (type == "whisper_lookup_fail") {
        set_whisper_notice(j.value("reason", "not found"));
        dbglog("[whisper] lookup failed");
    }
}

void VoiceClient::whisper_lookup(const std::string& name) {
    if (!ws_.is_connected() || !auth_sent_) {
        set_whisper_notice("not connected");
        return;
    }
    json msg;
    msg["type"] = "whisper_lookup";
    msg["name"] = name;
    set_whisper_notice("calling...");
    enqueue_ws_send(msg.dump());
}

void VoiceClient::whisper_request(uint32_t target_char_id) {
    if (!ws_.is_connected() || !auth_sent_) {
        set_whisper_notice("not connected");
        return;
    }
    json msg;
    msg["type"]           = "whisper_request";
    msg["target_char_id"] = target_char_id;
    set_whisper_notice("calling...");
    enqueue_ws_send(msg.dump());
}

void VoiceClient::whisper_accept() {
    std::string sid;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (whisper_state_ != WhisperState::Incoming || !ws_.is_connected()) return;
        sid = whisper_sid_;
    }
    json msg;
    msg["type"] = "whisper_accept";
    msg["sid"]  = sid;
    enqueue_ws_send(msg.dump());
}

void VoiceClient::whisper_reject() {
    std::string sid;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (whisper_state_ != WhisperState::Incoming || !ws_.is_connected()) return;
        sid = whisper_sid_;
        whisper_state_ = WhisperState::None;
        whisper_sid_.clear();
        whisper_peer_name_.clear();
    }
    json msg;
    msg["type"] = "whisper_reject";
    msg["sid"]  = sid;
    enqueue_ws_send(msg.dump());
}

void VoiceClient::whisper_end() {
    std::string sid;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (whisper_sid_.empty() || !ws_.is_connected()) return;
        sid = whisper_sid_;
        if (channel_ == Channel::Whisper)
            channel_ = pre_whisper_channel_;
        whisper_state_ = WhisperState::None;
        whisper_sid_.clear();
        whisper_peer_name_.clear();
    }
    json msg;
    msg["type"] = "whisper_end";
    msg["sid"]  = sid;
    enqueue_ws_send(msg.dump());
}

void VoiceClient::on_binary_message(const std::vector<uint8_t>& data) {
    // Header: [4 char_id BE][4 vol BE][4 x BE][4 y BE][24 name][2 seq BE] = 42 bytes minimum
    if (data.size() < 42) return;

    uint32_t sender_id = (static_cast<uint32_t>(data[0]) << 24)
                       | (static_cast<uint32_t>(data[1]) << 16)
                       | (static_cast<uint32_t>(data[2]) << 8)
                       |  static_cast<uint32_t>(data[3]);

    uint32_t vol_bits  = (static_cast<uint32_t>(data[4]) << 24)
                       | (static_cast<uint32_t>(data[5]) << 16)
                       | (static_cast<uint32_t>(data[6]) << 8)
                       |  static_cast<uint32_t>(data[7]);

    uint32_t sx_bits   = (static_cast<uint32_t>(data[8]) << 24)
                       | (static_cast<uint32_t>(data[9]) << 16)
                       | (static_cast<uint32_t>(data[10]) << 8)
                       |  static_cast<uint32_t>(data[11]);

    uint32_t sy_bits   = (static_cast<uint32_t>(data[12]) << 24)
                       | (static_cast<uint32_t>(data[13]) << 16)
                       | (static_cast<uint32_t>(data[14]) << 8)
                       |  static_cast<uint32_t>(data[15]);

    float volume = 0.0f;
    std::memcpy(&volume, &vol_bits, 4);
    if (volume <= 0.0f) return;

    const int sender_x = static_cast<int>(sx_bits);
    const int sender_y = static_cast<int>(sy_bits);

    // Name: bytes 16-39 (24 bytes, null-terminated)
    char name_buf[25] = {};
    std::memcpy(name_buf, data.data() + 16, 24);
    std::string sender_name(name_buf);

    // Seq: bytes 40-41 (2 bytes BE) — used by play_opus for FEC recovery on gap
    const uint16_t seq = static_cast<uint16_t>((data[40] << 8) | data[41]);

    // Server strips channel+gid+seq fields before forwarding opus — opus bytes start at offset 42
    const uint8_t* opus_data  = data.data() + 42;
    int            opus_bytes = (int)(data.size() - 42);

    if (!sender_name.empty()) {
        std::lock_guard<std::mutex> lk(name_cache_mtx_);
        name_cache_[sender_id] = sender_name;
    }

    {
        std::lock_guard<std::mutex> lk(active_spk_mtx_);
        active_speakers_[sender_id] = GetTickCount();
    }

    if (!deafened_.load() && !is_player_muted(sender_id)) {
        // Spatial panning only applies to Proximity (Normal) channel.
        // Party / Guild / Room / Whisper play flat (center, no distance effect).
        const Channel ch = get_channel();
        const bool use_spatial = (ch == Channel::Normal);

        SpatialParams sp{};   // pan=0, rear_attn=1 by default (flat/center)
        if (use_spatial) {
            const int my_x = server_pos_x_.load();
            const int my_y = server_pos_y_.load();
            sp = calc_spatial_2d(my_x, my_y, sender_x, sender_y, 14.0f);
        }

        // Distance LPF: only for proximity channel — muffles distant voices.
        // fc ranges from 2000 Hz (far) to 16000 Hz (near). Flat for other channels.
        constexpr float FS = 48000.f;
        float lpf_a = 1.f;   // flat (no LPF) for party/guild/room
        if (use_spatial) {
            float fc = 2000.f + 14000.f * volume;
            lpf_a = 1.f - expf(-6.2832f * fc / FS);
        }

        playback_.play_opus(static_cast<int>(sender_id), opus_data, opus_bytes,
                            volume, sp.pan, sp.rear_attn, lpf_a, seq);
    }
}

void VoiceClient::position_loop() {
    {
        char b[48];
        sprintf_s(b, "[pos] loop started tid=%lu", GetCurrentThreadId());
        dbglog(b);
    }

    int auth_wait_counter = 0;
    bool last_ptt = false;
    DWORD last_ping_tick = 0;

    while (running_) {
        Sleep(33);

        // ── Drain outgoing WS queue ───────────────────────────────────────
        // D3D9 / UI thread pushes messages here instead of calling
        // ws_.send_text() directly, so the game render thread never blocks
        // on WinHTTP I/O.  We drain here under no lock — safe because only
        // position_loop calls ws_.send_text() (recv_thread only receives).
        {
            std::vector<std::string> pending;
            {
                std::lock_guard<std::mutex> lk(ws_send_queue_mtx_);
                pending.swap(ws_send_queue_);
            }
            if (ws_.is_connected()) {
                for (auto& m : pending)
                    ws_.send_text(m);
            }
        }

        try {
            // Read memory before syncing PTT so char-select/loading screens
            // cannot keep the old character's server-side TX state open.
            auto state = MemoryReader::read();
            // Position comes from Map Server via UDP — only AID + CID needed.
            // on_map = auth_ready (account_id > 0 && char_id > 0).
            // MAP_NAME / CHAR_X / CHAR_Y offsets are optional (overlay/debug only).
            const bool on_map = state.auth_ready;
            in_map_ = on_map;

            const int ptt_key = ptt_key_.load();
            bool ptt = on_map && (open_mic_.load() || ((GetAsyncKeyState(ptt_key) & 0x8000) != 0));
            if (ptt != last_ptt) {
                last_ptt = ptt;
                set_ptt(ptt);
            }

            // Keep in_map_ current even while disconnected.
            // This makes is_in_game() return false during char select (map="")
            // immediately — no need to wait for the server to close the WS.
            if (!ws_.is_connected())
                continue;

            // Whisper timeout — auto-reject/cancel after 30s
            WhisperState whisper_state = WhisperState::None;
            DWORD whisper_tick = 0;
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                whisper_state = whisper_state_;
                whisper_tick = whisper_tick_;
            }
            if (whisper_state == WhisperState::Calling || whisper_state == WhisperState::Incoming) {
                if (GetTickCount() - whisper_tick > 30000) {
                    if (whisper_state == WhisperState::Incoming)
                        whisper_reject();
                    else
                        whisper_end();
                }
            }

            if (!state.auth_ready) {
                if ((++auth_wait_counter % 100) == 0) {
                    char b[256];
                    sprintf_s(b, "[auth] still not ready acc=%d char=%d",
                        state.account_id,
                        state.char_id);
                    dbglog(b);
                }
                continue;
            }

            // Detect character switch.
            //
            // We deliberately do NOT call ws_.disconnect() here. Doing so from
            // position_loop races with recv_thread, which may be simultaneously
            // handling a close frame from the server's own auth_revoke kick —
            // both threads touching the same WinHTTP handles → heap corruption
            // → game crash.
            //
            // Instead we let the server be the sole authority:
            //   • Normal path: map server sends auth_revoke UDP → voice server
            //     kicks WS → DLL recv gets close → on_ws_closed → reconnect →
            //     auth with new char_id.
            //   • Race path (DLL sees char change before server kicks): we reset
            //     auth_sent_ and try_send_auth re-fires on the SAME connection.
            //     Server rejects re-auth ("re-auth on same connection not allowed")
            //     → kicks WS → same clean reconnect.
            const int last_auth_char_id = last_auth_char_id_.load();
            if (last_auth_char_id != 0 &&
                state.char_id != last_auth_char_id &&
                auth_sent_) {
                dbglog("[auth] char changed — waiting for server kick to reconnect");
                auth_sent_ = false;
            }

            if (!auth_sent_)
                try_send_auth();
            if (!auth_sent_)
                continue;

            DWORD now = GetTickCount();
            if (ws_.is_connected() && now - last_ping_tick >= 5000) {
                json ping;
                ping["type"] = "ping";
                ping["t"] = static_cast<uint32_t>(now);
                ws_.send_text(ping.dump());
                last_ping_tick = now;
                last_ping_sent_tick_.store(now);
            }

            // Position is now sent by Map Server via UDP
            // DLL only sends auth + audio + party/guild updates

            // Position (x/y/map) comes from Map Server via UDP — not read from memory.

            // party/guild are managed server-side via DB refresh — no need to send from DLL
        }
        catch (const std::exception& ex) {
            char b[512];
            sprintf_s(b, "[pos] std::exception: %s", ex.what());
            dbglog(b);
        }
        catch (...) {
            dbglog("[pos] unknown C++ exception");
        }
    }

    dbglog("[pos] loop ended");
}

// Update AEC system delay from measured WASAPI latencies.
// Called at most once every ~2 s to avoid thrashing; safe to call from any thread.
void VoiceClient::update_aec_system_delay() {
    int cap_ms    = capture_.get_latency_ms();
    int render_ms = playback_.get_render_latency_ms();
    int total_ms  = cap_ms + render_ms;
    if (total_ms == 0) return;

    int current_ms = aec_.get_system_delay_ms();
    if (std::abs(total_ms - current_ms) >= 5) {  // only update if meaningfully different
        aec_.set_system_delay_ms(total_ms);
        char b[80];
        sprintf_s(b, "[aec] system delay updated: cap=%d render=%d total=%d ms",
                  cap_ms, render_ms, total_ms);
        dbglog(b);
    }
}

void VoiceClient::on_audio_captured(const std::vector<int16_t>& pcm) {
    // Periodically sync AEC system delay from real WASAPI latencies.
    // Runs once every ~100 frames (2 s) so it catches device switches too.
    {
        static int s_frame_ctr = 0;
        if (++s_frame_ctr >= 100) { s_frame_ctr = 0; update_aec_system_delay(); }
    }

    // Always update mic RMS for overlay (even when not transmitting)
    if (!pcm.empty()) {
        constexpr float kUiTalkRmsThreshold = 0.018f;
        float sum = 0.0f;
        for (int16_t s : pcm) { float f = s / 32768.0f; sum += f * f; }
        float rms = sqrtf(sum / pcm.size());
        // Smooth with simple low-pass so the bar doesn't flicker
        float prev = mic_rms_.load();
        float smooth = prev * 0.6f + rms * 0.4f;
        mic_rms_.store(smooth);
        if (ptt_active_.load() && smooth >= kUiTalkRmsThreshold) {
            last_local_talk_tick_.store(GetTickCount());
        }
    }

    if (reset_mic_pipeline_.exchange(false)) {
        pcm_accum_.clear();
        reset_mic_filter_state();
        // Opus encoder and TX sequence must be reset on the capture thread
        // (the only thread that touches them) to avoid data races with
        // set_channel() / set_deafen() which run on the D3D9 render thread.
        reset_opus_encoder_state(opus_enc_);
        tx_seq_.store(0, std::memory_order_relaxed);
    }

    if (!ws_.is_connected() || !in_map_.load() || muted_.load() || deafened_.load() || !ptt_active_.load() || !auth_sent_ || !opus_enc_) {
        // Always clear accumulator when not transmitting so stale audio
        // cannot leak into the next PTT press or reconnect.
        if (!pcm_accum_.empty()) {
            pcm_accum_.clear();
            reset_mic_filter_state();
        }
        return;
    }

    pcm_accum_.insert(pcm_accum_.end(), pcm.begin(), pcm.end());

    constexpr size_t FRAME = 960; // 20ms @ 48kHz
    while (pcm_accum_.size() >= FRAME) {
        // ── AEC: subtract far-end speaker leakage from mic ──────────────
        // Runs first so every downstream stage (VAD, AGC, noise suppressor)
        // sees the echo-removed signal rather than the raw mic.
        aec_.process(pcm_accum_.data(), FRAME);
        apply_mic_highpass(pcm_accum_.data(), FRAME, mic_hpf_x1_, mic_hpf_y1_);

        // ── Feature-based VAD (energy + ZCR + SFM + hangover) ─────────
        // Runs BEFORE AGC / noise suppressor so those don't skew the
        // spectral/energy features that the VAD is judging.
        const bool is_speech = vad_.is_speech(pcm_accum_.data(), FRAME);

        // ── AGC: normalize input level to -20 dBFS ───────────────────
        agc_.process(pcm_accum_.data(), FRAME);

        // ── Noise suppression ────────────────────────────────────────
        noise_suppressor_.process(pcm_accum_.data(), FRAME);
        apply_soft_limiter(pcm_accum_.data(), FRAME);

        // If VAD says non-speech, drop the frame before Opus encode.
        // Opus DTX would still produce a comfort-noise packet, but we'd
        // rather not transmit at all while the user isn't speaking — saves
        // bandwidth at scale (3000 players) and reduces background-noise
        // leakage when PTT is not the gating mechanism (open-mic mode).
        if (!is_speech) {
            pcm_accum_.erase(pcm_accum_.begin(), pcm_accum_.begin() + FRAME);
            continue;
        }

        // ── Opus encode ───────────────────────────────────────────────
        // Buffer sized for 96 kbps VBR peaks (≈1280 bytes / frame max)
        uint8_t opus_buf[1280];
        int opus_bytes = opus_encode(opus_enc_,
                                     pcm_accum_.data(), (int)FRAME,
                                     opus_buf, (int)sizeof(opus_buf));

        pcm_accum_.erase(pcm_accum_.begin(), pcm_accum_.begin() + FRAME);

        if (opus_bytes <= 0) continue; // encoder error or DTX silence packet dropped

        // ── Build packet: [1 channel][4 gid BE][2 seq BE][N opus bytes] ─
        uint32_t gid = 0;
        Channel tx_channel = Channel::Normal;
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            tx_channel = channel_;
        }
        // party_id / guild_id come from the server (DB refresh via map server).
        // No need to read from memory — voice server already tracks them.

        const uint16_t seq = tx_seq_++;   // wraps naturally at 65535 → 0

        // ── Anti-tamper gate ───────────────────────────────────────────
        // If an analysis tool was detected (debugger / reverse-engineering
        // tool), silently discard the encoded packet. The DLL keeps running,
        // the overlay shows correctly, and the user hears nothing wrong on
        // their own end — but no audio is transmitted to the server.
        // This is intentionally harder to spot than an explicit crash or error.
        if (anti_tamper::is_tampered()) {
            // pcm_accum_ was already erased above (line 1138) — do NOT erase again.
            continue;
        }

        std::vector<uint8_t> packet;
        packet.reserve(7 + opus_bytes);
        packet.push_back(static_cast<uint8_t>(tx_channel));
        packet.push_back((gid >> 24) & 0xFF);
        packet.push_back((gid >> 16) & 0xFF);
        packet.push_back((gid >> 8)  & 0xFF);
        packet.push_back(gid & 0xFF);
        packet.push_back((seq >> 8) & 0xFF);
        packet.push_back(seq & 0xFF);
        packet.insert(packet.end(), opus_buf, opus_buf + opus_bytes);

        bool ok = ws_.send_binary(packet);
        if (!ok) dbglog("[audio/tx] send_binary FAILED");
    }
}

void VoiceClient::set_ptt(bool active) {
    if (ptt_active_.exchange(active) == active) return;
    if (!active) {
        last_local_talk_tick_.store(0);
        reset_mic_pipeline_.store(true);
    }

    if (!ws_.is_connected()) return;

    json msg;
    msg["type"]  = "ptt";
    msg["value"] = active;
    msg["session_id"] = g_voice_session_id;
    ws_.send_text(msg.dump());

    dbglog(active ? "[ptt] ON" : "[ptt] OFF");
}

void VoiceClient::set_mute(bool muted) {
    if (!muted && deafened_.load()) {
        muted_.store(true);
        last_local_talk_tick_.store(0);
        reset_mic_pipeline_.store(true);
        return;
    }

    muted_.store(muted);
    if (muted) {
        last_local_talk_tick_.store(0);
        reset_mic_pipeline_.store(true);
    }

    if (!ws_.is_connected()) return;

    json msg;
    msg["type"]  = "mute";
    msg["value"] = muted;
    msg["session_id"] = g_voice_session_id;
    enqueue_ws_send(msg.dump());
}

void VoiceClient::set_deafen(bool v) {
    const bool was_deafened = deafened_.load();
    if (was_deafened == v) return;

    deafened_.store(v);
    if (v) {
        // Discord-style behavior: deafen mutes the mic, but remembers whether
        // the user had already muted manually so undeafen can restore safely.
        const bool was_muted = muted_.load();
        muted_before_deafen_.store(was_muted);
        if (!was_muted) {
            set_mute(true);
        }
    } else if (!muted_before_deafen_.load() && muted_.load()) {
        set_mute(false);
    }

    if (!ws_.is_connected()) return;

    json msg;
    msg["type"]  = "deafen";
    msg["value"] = v;
    msg["session_id"] = g_voice_session_id;
    enqueue_ws_send(msg.dump());
}

void VoiceClient::set_channel(Channel ch) {
    // Room is server-controlled (assigned via map server chat_join/chat_leave).
    // Reject manual Room switches — caller must wait for room_joined event.
    if (ch == Channel::Room) {
        dbglog("[chan] ignoring manual set_channel(Room) — server-controlled");
        return;
    }
    if (war_mode_.load() && (ch == Channel::Normal || ch == Channel::Room)) {
        set_whisper_notice("War Mode");
        return;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        changed = (channel_ != ch);
        channel_ = ch;
    }
    if (changed) {
        // Drop old jitter/decoder state so packets from the previous channel
        // don't smear into the newly selected one for a frame or two.
        playback_.flush_all();
        // Signal capture thread to reset opus encoder + tx_seq safely on its
        // own thread — avoids the data race with opus_encode() running there.
        reset_mic_pipeline_.store(true);
        pending_channel_ack_.store(static_cast<int>(ch));
        pending_channel_tick_.store(GetTickCount());
        channel_switch_ack_ms_.store(0);
    }
    // set_channel is a critical control message: the server's rx_channel check
    // (should_forward ch<=2) blocks audio until it arrives.  Send directly
    // instead of via the 33ms queue so the server updates rx_channel before
    // the first audio packet on the new channel is transmitted.
    // ws_.send_text() is protected by send_mtx_ so it is safe to call from
    // the D3D9 render thread.
    if (ws_.is_connected() && auth_sent_) {
        json msg;
        msg["type"]    = "set_channel";
        msg["channel"] = static_cast<int>(ch);
        ws_.send_text(msg.dump());
    }
}

// join_room() removed — room membership is managed by the map server via UDP
// (chat_join / chat_leave).  The voice server updates chat_room_id and sends
// room_joined / room_left WS events to the DLL; the DLL does not initiate joins.

std::vector<std::string> VoiceClient::get_speakers() {
    std::lock_guard lock(speakers_mtx_);
    return speakers_;
}

std::vector<uint32_t> VoiceClient::get_nearby_players() {
    std::lock_guard<std::mutex> lk(nearby_mtx_);
    std::vector<uint32_t> out;
    out.reserve(nearby_players_.size());
    for (const auto& kv : nearby_players_) out.push_back(kv.first);
    return out;
}

std::string VoiceClient::get_speaker_name(uint32_t char_id) {
    // Check nearby_players_ first (most up-to-date name from server), fall back to name_cache_
    {
        std::lock_guard<std::mutex> lk(nearby_mtx_);
        auto it = nearby_players_.find(char_id);
        if (it != nearby_players_.end()) return it->second;
    }
    std::lock_guard<std::mutex> lk(name_cache_mtx_);
    auto it = name_cache_.find(char_id);
    return (it != name_cache_.end()) ? it->second : std::string{};
}

void VoiceClient::mute_player(uint32_t char_id) {
    std::lock_guard<std::mutex> lk(muted_players_mtx_);
    muted_players_.insert(char_id);
}

void VoiceClient::unmute_player(uint32_t char_id) {
    std::lock_guard<std::mutex> lk(muted_players_mtx_);
    muted_players_.erase(char_id);
}

bool VoiceClient::is_player_muted(uint32_t char_id) const {
    std::lock_guard<std::mutex> lk(muted_players_mtx_);
    return muted_players_.count(char_id) > 0;
}

std::vector<uint32_t> VoiceClient::get_muted_players() const {
    std::lock_guard<std::mutex> lk(muted_players_mtx_);
    return std::vector<uint32_t>(muted_players_.begin(), muted_players_.end());
}

std::vector<uint32_t> VoiceClient::get_active_speakers() {
    std::lock_guard<std::mutex> lk(active_spk_mtx_);
    DWORD now = GetTickCount();
    std::vector<uint32_t> out;
    for (auto it = active_speakers_.begin(); it != active_speakers_.end(); ) {
        if (now - it->second < 800) {
            out.push_back(it->first);
            ++it;
        } else {
            it = active_speakers_.erase(it);
        }
    }
    return out;
}
