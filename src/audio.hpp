#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>
#include <memory>

// Forward-declare Opus types so callers don't need to include opus.h
struct OpusDecoder;

using AudioCaptureCallback = std::function<void(const std::vector<int16_t>&)>;
using AudioPcmBuffer = std::vector<int16_t>;

// ── Device enumeration ────────────────────────────────────────────────────────
struct AudioDeviceInfo {
    std::wstring id;    // WASAPI device ID
    std::string  name;  // friendly name (UTF-8)
};

// capture=true → microphones, false → speakers/headphones
std::vector<AudioDeviceInfo> enumerate_audio_devices(bool capture);

// ── Capture ───────────────────────────────────────────────────────────────────
class AudioCapture {
public:
    bool start(AudioCaptureCallback cb);
    void stop();

    // Restart with a specific device (empty = system default)
    void set_device(const std::wstring& device_id);
    std::wstring get_device() const;

    std::atomic<float> gain{ 1.0f }; // mic input gain (0.0 – 4.0)

    // Returns the WASAPI capture stream latency in ms (0 until start() is called).
    int get_latency_ms() const { return latency_ms_.load(); }

private:
    bool start_internal();
    static DWORD WINAPI capture_thread(LPVOID param);
    std::vector<int16_t> convert_to_pcm16(const BYTE* data, UINT32 frames) const;

    AudioCaptureCallback cb_;
    std::wstring device_id_;       // empty = default
    mutable std::mutex device_mtx_;
    bool     running_      = false;
    HANDLE   thread_       = nullptr;

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice*           device_     = nullptr;
    IAudioClient*        client_     = nullptr;
    IAudioCaptureClient* capture_    = nullptr;

    WORD  fmt_tag_      = 0;
    WORD  fmt_channels_ = 0;
    DWORD fmt_rate_     = 0;
    WORD  fmt_bits_     = 0;

    std::atomic<int> latency_ms_{ 0 };
};

// ── Playback (Opus + jitter buffer) ──────────────────────────────────────────
class AudioPlayback {
public:
    ~AudioPlayback();

    // Change render device (empty = system default); restarts all streams
    void set_device(const std::wstring& device_id);

    std::atomic<float> gain{ 1.0f }; // speaker output gain (0.0 – 4.0)

    // AEC reference tap — invoked from the render thread with the mono
    // pre-pan mix of *every* decoded frame just before WASAPI write. The
    // callback must be lock-light / wait-free (it runs on the audio thread).
    // pass nullptr to detach.
    using AecRefCallback = std::function<void(const int16_t*, size_t)>;
    void set_aec_reference_callback(AecRefCallback cb);

    // Per-speaker LUFS-style loudness normalization. Off by default — stacking
    // with the server-side proximity volume falloff tends to flatten the
    // near/far dynamic range, so only enable when the group actually has very
    // loud and very quiet talkers mixed together.
    void set_loudness_norm(bool v) { loudness_norm_enabled_.store(v); }
    bool is_loudness_norm()  const { return loudness_norm_enabled_.load(); }

    // Push an Opus-encoded frame into the jitter buffer for this speaker.
    // Lazily creates the WASAPI stream and the render thread on first call.
    // lpf_alpha: 1-pole IIR low-pass coefficient (1.0 = bypass, <1 = distance muffle)
    // seq: 16-bit monotonic packet counter (from server). Used to detect loss
    //      and synthesize a recovered frame via Opus FEC (in-band redundancy
    //      encoded in the current packet from the *previous* frame).
    void play_opus(int speaker_id, const uint8_t* opus_data,
                   int opus_bytes, float volume,
                   float pan, float rear_attn,
                   float lpf_alpha = 1.0f,
                   uint16_t seq    = 0);

    uint64_t get_rx_packets() const     { return rx_packets_.load(); }
    uint64_t get_rx_loss_events() const { return rx_loss_events_.load(); }
    uint64_t get_rx_lost_frames() const { return rx_lost_frames_.load(); }
    float    get_rx_jitter_ms() const   { return rx_jitter_ms_stat_.load(); }

    // Returns the WASAPI render stream latency in ms (updated on first play).
    int get_render_latency_ms() const { return render_latency_ms_.load(); }

    void remove_speaker(int speaker_id);
    void flush_all();
    void stop_all();

    std::wstring get_device() const;

private:
    // 20 ms @ 48 kHz mono
    static constexpr UINT32 FRAME_SAMPLES  = 960;
    // Absolute maximum queue depth (hard cap: 200 ms)
    static constexpr int    JITTER_MAX_ABS = 10;
    static constexpr int    MAX_CONCEAL_FRAMES = JITTER_MAX_ABS;
    // Minimum adaptive target (lowest latency we will allow)
    static constexpr int    JITTER_MIN     = 1;
    // Maximum adaptive target
    static constexpr int    JITTER_TARGET_MAX = 5;

    bool init_stream(int speaker_id);   // must be called with streams_mtx_ held

    struct PcmFrame {
        AudioPcmBuffer samples;   // always FRAME_SAMPLES elements
        float volume    = 1.0f;
        float pan       = 0.0f;         // -1.0 = left, +1.0 = right
        float rear_attn = 1.0f;         // 0.0..1.0 (behind becomes quieter)
    };

    struct SpeakerStream {
        IAudioClient*        client    = nullptr;
        IAudioRenderClient*  render    = nullptr;
        WAVEFORMATEX         format    = {};
        bool                 is_float  = false;
        WORD                 channels  = 0;
        DWORD                rate      = 0;
        OpusDecoder*         dec       = nullptr;

        std::mutex           jmtx;
        std::deque<PcmFrame> jqueue;
        bool                 buffering        = true;  // waiting for adaptive_target frames
        DWORD                last_recv        = 0;     // GetTickCount of last packet

        // Adaptive jitter buffer
        float jitter_ms_      = 0.0f;  // EWMA of |inter-arrival - 20 ms|
        int   adaptive_target_= 2;     // current target depth (frames)

        // Distance-based 1-pole IIR low-pass filter (per-speaker state)
        float lpf_z_  = 0.0f;  // IIR state
        float lpf_a_  = 1.0f;  // coefficient (1.0 = bypass)

        // Smooth volume: interpolate towards target to avoid sudden jumps
        // when a player walks in/out of proximity range.
        float smooth_vol_ = 1.0f;  // current smoothed volume (converges to target)

        // Packet sequence tracking (Opus in-band FEC recovery)
        uint16_t last_seq      = 0;
        bool     have_last_seq = false;

        // Per-speaker LUFS-style loudness normalizer state.
        // We track a running RMS of decoded speech and derive a makeup gain that
        // targets -23 LUFS (≈ -23 dBFS RMS for full-scale speech). Gain changes
        // smoothly so "loud" players stop blowing out the mix while "quiet"
        // players become intelligible.
        float loud_rms_ewma = 0.0f;  // EWMA of per-frame RMS (linear, 0..1)
        float loud_gain     = 1.0f;  // current makeup gain (smoothed)

    };

    std::wstring render_device_id_; // empty = default
    mutable std::mutex device_mtx_;

    std::mutex streams_mtx_;
    std::unordered_map<int, std::shared_ptr<SpeakerStream>> streams_;

    std::atomic<bool> render_running_{ false };
    std::thread       render_thread_;

    std::mutex      aec_cb_mtx_;
    AecRefCallback  aec_cb_;    // mono pre-pan reference tap

    std::atomic<bool> loudness_norm_enabled_{ false };
    std::atomic<int>  render_latency_ms_{ 20 };   // updated from GetStreamLatency in init_stream
    std::atomic<uint64_t> rx_packets_{ 0 };
    std::atomic<uint64_t> rx_loss_events_{ 0 };
    std::atomic<uint64_t> rx_lost_frames_{ 0 };
    std::atomic<float>    rx_jitter_ms_stat_{ 0.0f };

    void ensure_render_thread();
    void render_loop();

    // Write PCM (mono 16kHz) to WASAPI, resampling/converting as needed
    static bool write_pcm_frame(SpeakerStream& s,
                                const int16_t* pcm, UINT32 n_samples,
                                float volume, float pan, float rear_attn);

    // Post-decode DSP helpers. apply_* are stateless w.r.t. AudioPlayback
    // (operate solely on the per-speaker state) and can stay static;
    // decode_and_process has to read the loudness-norm toggle, so it's
    // an instance method.
    static void apply_distance_lpf(
        SpeakerStream& sp,
        AudioPcmBuffer& pcm,
        float lpf_alpha);
    static void apply_loudness_norm(
        SpeakerStream& sp,
        AudioPcmBuffer& pcm);
    static void destroy_stream(SpeakerStream* sp);
    bool conceal_and_process(
        SpeakerStream& sp,
        float lpf_alpha,
        AudioPcmBuffer& pcm_out);
    bool decode_and_process(
        SpeakerStream& sp,
        const uint8_t* opus_data,
        int opus_bytes,
        int decode_fec,
        float lpf_alpha,
        AudioPcmBuffer& pcm_out);
};
