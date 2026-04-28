#pragma once
#include "ws_client.hpp"
#include "audio.hpp"
#include "memory_reader.hpp"
#include "noise_suppressor.hpp"
#include "agc.hpp"
#include "voice_activity.hpp"
#include "echo_canceller.hpp"
#include "obf_string.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

struct OpusEncoder;  // forward-declare; opus.h included only in voice_client.cpp

enum class Channel : uint8_t {
    Normal  = 0x00,
    Party   = 0x01,
    Guild   = 0x02,
    Room    = 0x03,
    Whisper = 0x04
};

class VoiceClient {
public:
    static VoiceClient& get() {
        static VoiceClient instance;
        return instance;
    }

    void init();
    void shutdown();

    bool        is_connected()  const { return ws_.is_connected(); }
    bool        is_in_game()    const { return auth_sent_ && in_map_; }  // true = authed & on a map
    bool        is_on_map()     const { return in_map_.load(); }
    bool        is_muted()      const { return muted_.load(); }
    bool        is_ptt_active() const { return ptt_active_.load(); }
    Channel     get_channel()   const;
    bool        is_open_mic()   const { return open_mic_.load(); }
    void        set_open_mic(bool v)  { open_mic_.store(v); }
    bool        is_noise_suppression() const { return noise_suppressor_.is_enabled(); }
    void        set_noise_suppression(bool v){ noise_suppressor_.set_enabled(v); }
    bool        is_agc()               const { return agc_.is_enabled(); }
    void        set_agc(bool v)              { agc_.set_enabled(v); }
    bool        is_vad()               const { return vad_.is_enabled(); }
    void        set_vad(bool v)              { vad_.set_enabled(v); }
    bool        is_aec()               const { return aec_.is_enabled(); }
    void        set_aec(bool v)              { aec_.set_enabled(v); }
    bool        is_loudness_norm()     const { return playback_.is_loudness_norm(); }
    void        set_loudness_norm(bool v)    { playback_.set_loudness_norm(v); }
    bool        is_deafened()   const { return deafened_.load(); }
    bool        is_war_mode()   const { return war_mode_.load(); }
    int         get_war_recommended_channel() const { return war_recommended_channel_.load(); }
    void        set_deafen(bool v);
    int         get_ptt_key()   const { return ptt_key_.load(); }
    void        set_ptt_key(int vk)   { ptt_key_.store(vk); }
    float       get_mic_rms()   const { return mic_rms_.load(); }
    uint32_t    get_rtt_ms()    const { return rtt_ms_.load(); }
    float       get_rx_jitter_ms() const { return playback_.get_rx_jitter_ms(); }
    uint64_t    get_rx_packets() const { return playback_.get_rx_packets(); }
    uint64_t    get_rx_loss_events() const { return playback_.get_rx_loss_events(); }
    uint64_t    get_rx_lost_frames() const { return playback_.get_rx_lost_frames(); }
    uint32_t    get_channel_switch_ack_ms() const { return channel_switch_ack_ms_.load(); }
    bool        is_locally_talking() const;
    std::vector<std::string> get_speakers();

    // Gain controls
    float get_mic_gain()      const { return capture_.gain.load(); }
    float get_speaker_gain()  const { return playback_.gain.load(); }
    void  set_mic_gain(float v)     { capture_.gain.store(v); }
    void  set_speaker_gain(float v) { playback_.gain.store(v); }

    // Device controls
    std::vector<AudioDeviceInfo> get_mic_devices()     { return enumerate_audio_devices(true); }
    std::vector<AudioDeviceInfo> get_speaker_devices() { return enumerate_audio_devices(false); }
    void set_mic_device(const std::wstring& id)     { capture_.set_device(id); }
    void set_speaker_device(const std::wstring& id) { playback_.set_device(id); }

    // Returns char_ids that sent audio within the last 800 ms
    std::vector<uint32_t> get_active_speakers();

    // Returns all char_ids the server reported as nearby (within proximity range)
    std::vector<uint32_t> get_nearby_players();

    // Returns cached name for char_id, or empty string if unknown
    std::string get_speaker_name(uint32_t char_id);

    // Per-player mute (client-side only)
    void mute_player(uint32_t char_id);
    void unmute_player(uint32_t char_id);
    bool is_player_muted(uint32_t char_id) const;
    std::vector<uint32_t> get_muted_players() const;

    void set_ptt(bool active);
    void set_mute(bool muted);
    void set_channel(Channel ch);
    // join_room() removed — rooms are managed by map server (UDP chat_join/chat_leave).
    // The DLL receives room_joined/room_left WS events and switches channel accordingly.

    // ── Whisper ───────────────────────────────────────────────────────────────
    enum class WhisperState { None, Calling, Incoming, Active };
    WhisperState    get_whisper_state()       const;
    std::string     get_whisper_peer()        const;
    std::string     get_whisper_sid()         const;
    DWORD           get_whisper_tick()        const;
    std::string     get_whisper_notice()      const;
    DWORD           get_whisper_notice_tick() const;
    void            clear_whisper_notice();

    void whisper_lookup(const std::string& name); // lookup by char name → server does request
    void whisper_request(uint32_t target_char_id);
    void whisper_accept();
    void whisper_reject();
    void whisper_end();

    void load_settings(const char* path = "voice_client.conf");
    void save_settings(const char* path = "voice_client.conf");

private:
    VoiceClient() = default;

    WsClient ws_;
    void connect_to_server();
    void on_ws_closed();
    void reconnect_loop();
    void on_text_message(const std::string& msg);
    void on_binary_message(const std::vector<uint8_t>& data);
    void try_send_auth();

    std::thread position_thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> reconnecting_{ false };
    std::atomic<bool> auth_sent_{ false };
    std::atomic<bool> in_map_{ false };     // true only when on a map (hides overlay on char select)
    void position_loop();

    AudioCapture  capture_;
    AudioPlayback playback_;
    void on_audio_captured(const std::vector<int16_t>& pcm);

    // TX pipeline
    std::vector<int16_t> pcm_accum_;   // accumulate 320 samples (20ms)
    OpusEncoder*         opus_enc_ = nullptr;
    std::atomic<uint16_t> tx_seq_{ 0 };   // monotonic packet counter (wraps at 65535)
    std::atomic<int>     last_auth_char_id_{ 0 }; // char_id we last successfully authed as
                                                  // (set in try_send_auth, read by position_loop
                                                  // to avoid spurious char-switch detection)
    std::atomic<bool>    char_switch_pending_{ false }; // server told us "map session ended"
                                                        // → wait for char_id to change in memory
                                                        // before reconnecting (avoids double-auth)

    void init_opus_encoder();
    void destroy_opus_encoder();
    void set_whisper_notice(std::string notice);
    void reset_mic_filter_state();
    void update_aec_system_delay();

    // Thread-safe outgoing WS message queue.
    // D3D9 / UI thread pushes here; position_loop drains on its own thread
    // so the game render thread never blocks on WinHTTP network I/O.
    std::mutex              ws_send_queue_mtx_;
    std::vector<std::string> ws_send_queue_;
    void enqueue_ws_send(std::string msg);

    mutable std::mutex state_mtx_;
    Channel channel_         = Channel::Normal;
    Channel pre_room_channel_ = Channel::Normal; // channel to restore when leaving a room
    NoiseSuppressor        noise_suppressor_;
    AutoGainControl        agc_;
    VoiceActivityDetector  vad_;
    EchoCanceller          aec_;
    std::atomic<bool> muted_{ false };
    std::atomic<bool> ptt_active_{ false };
    std::atomic<bool> open_mic_{ false };
    std::atomic<bool> deafened_{ false };
    std::atomic<bool> muted_before_deafen_{ false };
    std::atomic<bool> war_mode_{ false };
    std::atomic<int>  war_recommended_channel_{ 0 };
    std::atomic<bool> reset_mic_pipeline_{ false };
    std::atomic<int> ptt_key_{ 'V' };   // default PTT key
    std::atomic<float> mic_rms_{ 0.0f };
    std::atomic<DWORD> last_local_talk_tick_{ 0 };
    std::atomic<DWORD> last_ping_sent_tick_{ 0 };
    std::atomic<uint32_t> rtt_ms_{ 0 };
    std::atomic<int>   pending_channel_ack_{ -1 };
    std::atomic<DWORD> pending_channel_tick_{ 0 };
    std::atomic<uint32_t> channel_switch_ack_ms_{ 0 };
    float mic_hpf_x1_ = 0.0f;
    float mic_hpf_y1_ = 0.0f;

    std::mutex speakers_mtx_;
    std::vector<std::string> speakers_;

    std::mutex active_spk_mtx_;
    std::unordered_map<uint32_t, DWORD> active_speakers_; // char_id → last tick

    std::mutex name_cache_mtx_;
    std::unordered_map<uint32_t, std::string> name_cache_; // char_id → name

    std::mutex nearby_mtx_;
    std::unordered_map<uint32_t, std::string> nearby_players_; // char_id → name (server-reported)

    mutable std::mutex muted_players_mtx_;
    std::unordered_set<uint32_t> muted_players_; // char_ids muted locally

    // Own position pushed by server from map_pos — no memory offsets needed.
    std::atomic<int> server_pos_x_{ 0 };
    std::atomic<int> server_pos_y_{ 0 };

    std::wstring server_host_ = L"127.0.0.1";
    INTERNET_PORT server_port_ = 7000;
    std::string  client_secret_ = "";

    // Whisper state
    WhisperState whisper_state_     = WhisperState::None;
    std::string  whisper_sid_;
    std::string  whisper_peer_name_;
    uint32_t     whisper_peer_id_   = 0;
    DWORD        whisper_tick_      = 0;   // GetTickCount of state change (for timeout)
    Channel      pre_whisper_channel_ = Channel::Normal;

    std::string  whisper_notice_;
    DWORD        whisper_notice_tick_ = 0;
};
