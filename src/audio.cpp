#include "audio.hpp"
#include "dbglog.hpp"

#include <opus/opus.h>
#include <avrt.h>
#include <timeapi.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <propvarutil.h>
#pragma comment(lib, "Propsys.lib")

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

using AudioPcmBuffer = std::vector<int16_t>;

namespace {

enum class SampleKind { Unknown = 0, Float32, Int16, Int32 };

SampleKind detect_kind(const WAVEFORMATEX* fmt) {
    if (!fmt) return SampleKind::Unknown;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32)
        return SampleKind::Float32;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        if (fmt->wBitsPerSample == 16) return SampleKind::Int16;
        if (fmt->wBitsPerSample == 32) return SampleKind::Int32;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && ex->Format.wBitsPerSample == 32)
            return SampleKind::Float32;
        if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            if (ex->Format.wBitsPerSample == 16) return SampleKind::Int16;
            if (ex->Format.wBitsPerSample == 32) return SampleKind::Int32;
        }
    }
    return SampleKind::Unknown;
}

// Cubic Hermite resample helper — smoother than linear, no external deps.
// Reduces aliasing artefacts when src_rate != dst_rate (e.g. 48k↔44.1k).
static inline float sample_at(const int16_t* src, UINT32 size, int i) {
    if (i < 0) i = 0;
    if (i >= (int)size) i = (int)size - 1;
    return static_cast<float>(src[i]) / 32768.0f;
}

static float lerp_pcm(const int16_t* src, UINT32 src_size,
                       DWORD src_rate, DWORD dst_rate, UINT32 dst_idx) {
    if (src_rate == dst_rate) {
        UINT32 i = dst_idx < src_size ? dst_idx : src_size - 1;
        return static_cast<float>(src[i]) / 32768.0f;
    }
    double pos = static_cast<double>(dst_idx) * src_rate / dst_rate;
    int    i1  = static_cast<int>(pos);
    float  t   = static_cast<float>(pos - i1);  // fractional part [0,1)

    // 4-point cubic Hermite (Catmull-Rom)
    float p0 = sample_at(src, src_size, i1 - 1);
    float p1 = sample_at(src, src_size, i1    );
    float p2 = sample_at(src, src_size, i1 + 1);
    float p3 = sample_at(src, src_size, i1 + 2);

    float a = -0.5f*p0 + 1.5f*p1 - 1.5f*p2 + 0.5f*p3;
    float b =       p0 - 2.5f*p1 + 2.0f*p2 - 0.5f*p3;
    float c = -0.5f*p0            + 0.5f*p2;
    float d =                  p1;

    return ((a*t + b)*t + c)*t + d;
}

static void calc_pan_gains(float pan, float rear_attn, float& gl, float& gr) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    if (rear_attn < 0.0f) rear_attn = 0.0f;
    if (rear_attn > 1.0f) rear_attn = 1.0f;

    const float angle = (pan + 1.0f) * 0.78539816339f; // 0..pi/2
    gl = std::cos(angle) * rear_attn;
    gr = std::sin(angle) * rear_attn;
}

std::mutex       g_capfmt_mtx;
std::unordered_map<const AudioCapture*, SampleKind> g_capfmt;

class ScopedCoInit {
public:
    ScopedCoInit() : initialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ScopedCoInit() {
        if (initialized_) CoUninitialize();
    }

private:
    bool initialized_ = false;
};

bool can_open_render_device(const std::wstring& device_id) {
    ScopedCoInit co_init;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    WAVEFORMATEX* mix = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&enumerator);
    if (FAILED(hr)) return false;

    if (!device_id.empty()) {
        hr = enumerator->GetDevice(device_id.c_str(), &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (FAILED(hr)) {
        enumerator->Release();
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) {
        device->Release();
        enumerator->Release();
        return false;
    }

    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        client->Release();
        device->Release();
        enumerator->Release();
        return false;
    }

    mix->nChannels       = 2;
    mix->nBlockAlign     = mix->nChannels * mix->wBitsPerSample / 8;
    mix->nAvgBytesPerSec = mix->nSamplesPerSec * mix->nBlockAlign;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
        ex->Format.nChannels       = 2;
        ex->Format.nBlockAlign     = ex->Format.nChannels * ex->Format.wBitsPerSample / 8;
        ex->Format.nAvgBytesPerSec = ex->Format.nSamplesPerSec * ex->Format.nBlockAlign;
        ex->dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        ex->Samples.wValidBitsPerSample = ex->Format.wBitsPerSample;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr);

    CoTaskMemFree(mix);
    client->Release();
    device->Release();
    enumerator->Release();
    return SUCCEEDED(hr);
}

} // namespace

// ── Device enumeration ────────────────────────────────────────────────────────

std::vector<AudioDeviceInfo> enumerate_audio_devices(bool capture) {
    std::vector<AudioDeviceInfo> result;

    ScopedCoInit co_init;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator)))
        return result;

    EDataFlow flow = capture ? eCapture : eRender;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    col->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;

        LPWSTR id_str = nullptr;
        dev->GetId(&id_str);

        IPropertyStore* props = nullptr;
        std::string friendly;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
                // wstring → utf8
                int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) { friendly.resize(len - 1); WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, &friendly[0], len, nullptr, nullptr); }
            }
            PropVariantClear(&var);
            props->Release();
        }

        AudioDeviceInfo info;
        if (id_str) { info.id = id_str; CoTaskMemFree(id_str); }
        info.name = friendly.empty() ? "Device " + std::to_string(i) : friendly;
        result.push_back(std::move(info));
        dev->Release();
    }

    col->Release();
    enumerator->Release();
    return result;
}

// ── AudioCapture ──────────────────────────────────────────────────────────────

void AudioCapture::set_device(const std::wstring& device_id) {
    bool was_running = running_;
    AudioCaptureCallback saved_cb = cb_;
    std::wstring old_device_id;
    {
        std::lock_guard<std::mutex> lk(device_mtx_);
        old_device_id = device_id_;
    }
    if (was_running) stop();
    {
        std::lock_guard<std::mutex> lk(device_mtx_);
        device_id_ = device_id;
    }
    if (was_running && !start(saved_cb)) {
        dbglog("[audio] capture device switch failed - restoring previous device");
        {
            std::lock_guard<std::mutex> lk(device_mtx_);
            device_id_ = old_device_id;
        }
        if (!start(saved_cb)) {
            dbglog("[audio] capture rollback FAILED");
        }
    }
}

std::wstring AudioCapture::get_device() const {
    std::lock_guard<std::mutex> lk(device_mtx_);
    return device_id_;
}

bool AudioCapture::start(AudioCaptureCallback cb) {
    cb_ = cb;

    ScopedCoInit co_init;
    std::wstring device_id;
    {
        std::lock_guard<std::mutex> lk(device_mtx_);
        device_id = device_id_;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                     CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { dbgloghr("[audio] FAILED CoCreateInstance", hr); return false; }

    if (!device_id.empty()) {
        hr = enumerator->GetDevice(device_id.c_str(), &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    }
    if (FAILED(hr)) {
        dbgloghr("[audio] FAILED GetAudioEndpoint", hr);
        enumerator->Release();
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) {
        dbgloghr("[audio] FAILED Activate", hr);
        device->Release();
        enumerator->Release();
        return false;
    }

    WAVEFORMATEX* pMixFmt = nullptr;
    hr = client->GetMixFormat(&pMixFmt);
    if (FAILED(hr)) {
        dbgloghr("[audio] FAILED GetMixFormat", hr);
        client->Release();
        device->Release();
        enumerator->Release();
        return false;
    }

    const WORD  new_fmt_tag      = pMixFmt->wFormatTag;
    const WORD  new_fmt_channels = pMixFmt->nChannels;
    const DWORD new_fmt_rate     = pMixFmt->nSamplesPerSec;
    const WORD  new_fmt_bits     = pMixFmt->wBitsPerSample;

    SampleKind kind = detect_kind(pMixFmt);

    char buf[160];
    sprintf_s(buf, "[audio] cap fmt: tag=%u ch=%u rate=%lu bits=%u kind=%d",
              new_fmt_tag, new_fmt_channels, new_fmt_rate, new_fmt_bits, (int)kind);
    dbglog(buf);

    // Balanced shared-mode capture buffer. 100ms is far safer across devices
    // than ultra-low 30ms, while avoiding the old 1s delay.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pMixFmt, nullptr);
    CoTaskMemFree(pMixFmt);
    if (FAILED(hr)) {
        dbgloghr("[audio] FAILED Initialize", hr);
        client->Release();
        device->Release();
        enumerator->Release();
        return false;
    }

    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr)) {
        dbgloghr("[audio] FAILED GetService", hr);
        client->Release();
        device->Release();
        enumerator->Release();
        return false;
    }

    // Measure capture latency — used by AEC to align reference delay
    {
        REFERENCE_TIME lat = 0;
        int ms = 20; // fallback
        if (SUCCEEDED(client->GetStreamLatency(&lat)))
            ms = static_cast<int>(lat / 10000);
        latency_ms_.store(ms);
        char lb[48]; sprintf_s(lb, "[audio] capture latency=%d ms", ms); dbglog(lb);
    }

    const WORD  old_fmt_tag      = fmt_tag_;
    const WORD  old_fmt_channels = fmt_channels_;
    const DWORD old_fmt_rate     = fmt_rate_;
    const WORD  old_fmt_bits     = fmt_bits_;

    fmt_tag_      = new_fmt_tag;
    fmt_channels_ = new_fmt_channels;
    fmt_rate_     = new_fmt_rate;
    fmt_bits_     = new_fmt_bits;

    enumerator_ = enumerator;
    device_ = device;
    client_ = client;
    capture_ = capture;
    {
        std::lock_guard<std::mutex> lk(g_capfmt_mtx);
        g_capfmt[this] = kind;
    }

    running_ = true;
    thread_  = CreateThread(nullptr, 0, capture_thread, this, 0, nullptr);
    if (!thread_) {
        dbglog("[audio] FAILED CreateThread");
        running_ = false;
        fmt_tag_      = old_fmt_tag;
        fmt_channels_ = old_fmt_channels;
        fmt_rate_     = old_fmt_rate;
        fmt_bits_     = old_fmt_bits;
        if (capture_)   { capture_->Release();    capture_    = nullptr; }
        if (client_)    { client_->Release();     client_     = nullptr; }
        if (device_)    { device_->Release();     device_     = nullptr; }
        if (enumerator_){ enumerator_->Release(); enumerator_ = nullptr; }
        std::lock_guard<std::mutex> lk(g_capfmt_mtx);
        g_capfmt.erase(this);
        return false;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        dbgloghr("[audio] FAILED Start", hr);
        running_ = false;
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
        fmt_tag_      = old_fmt_tag;
        fmt_channels_ = old_fmt_channels;
        fmt_rate_     = old_fmt_rate;
        fmt_bits_     = old_fmt_bits;
        if (capture_)   { capture_->Release();    capture_    = nullptr; }
        if (client_)    { client_->Release();     client_     = nullptr; }
        if (device_)    { device_->Release();     device_     = nullptr; }
        if (enumerator_){ enumerator_->Release(); enumerator_ = nullptr; }
        std::lock_guard<std::mutex> lk(g_capfmt_mtx);
        g_capfmt.erase(this);
        return false;
    }
    dbglog("[audio] capture started");
    return true;
}

DWORD WINAPI AudioCapture::capture_thread(LPVOID param) {
    auto* self = static_cast<AudioCapture*>(param);
    ScopedCoInit co_init;

    DWORD task_idx;
    HANDLE task = AvSetMmThreadCharacteristicsA("Pro Audio", &task_idx);

    while (self->running_) {
        UINT32 frames_available = 0;
        HRESULT hr = self->capture_->GetNextPacketSize(&frames_available);
        if (FAILED(hr) || frames_available == 0) { Sleep(5); continue; }

        BYTE*  data  = nullptr;
        DWORD  flags = 0;
        hr = self->capture_->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames_available > 0) {
                auto pcm = self->convert_to_pcm16(data, frames_available);
                if (!pcm.empty()) self->cb_(pcm);
            }
            self->capture_->ReleaseBuffer(frames_available);
        }
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    return 0;
}

std::vector<int16_t> AudioCapture::convert_to_pcm16(const BYTE* data, UINT32 frames) const {
    SampleKind kind = SampleKind::Unknown;
    { std::lock_guard<std::mutex> lk(g_capfmt_mtx); auto it = g_capfmt.find(this); if (it != g_capfmt.end()) kind = it->second; }

    // Mix down to mono float
    std::vector<float> mono(frames);
    if (kind == SampleKind::Float32) {
        const float* f = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < frames; i++) {
            float s = 0.0f;
            for (WORD c = 0; c < fmt_channels_; c++) s += f[i * fmt_channels_ + c];
            mono[i] = s / fmt_channels_;
        }
    } else if (kind == SampleKind::Int32) {
        const int32_t* s = reinterpret_cast<const int32_t*>(data);
        for (UINT32 i = 0; i < frames; i++) {
            double sum = 0;
            for (WORD c = 0; c < fmt_channels_; c++) sum += (double)s[i * fmt_channels_ + c] / 2147483648.0;
            mono[i] = (float)(sum / fmt_channels_);
        }
    } else {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        for (UINT32 i = 0; i < frames; i++) {
            float sum = 0.0f;
            for (WORD c = 0; c < fmt_channels_; c++) sum += s[i * fmt_channels_ + c] / 32768.0f;
            mono[i] = sum / fmt_channels_;
        }
    }

    // Resample to 48kHz with linear interpolation
    constexpr DWORD OUT_RATE = 48000;
    UINT32 out_frames = (fmt_rate_ == OUT_RATE)
        ? frames
        : static_cast<UINT32>((double)frames * OUT_RATE / fmt_rate_);

    // Cubic Hermite resample from device rate → OUT_RATE
    auto mono_at = [&](int i) -> float {
        if (i < 0) i = 0;
        if (i >= (int)frames) i = (int)frames - 1;
        return mono[i];
    };

    std::vector<int16_t> pcm(out_frames);
    for (UINT32 i = 0; i < out_frames; i++) {
        float v;
        if (fmt_rate_ == OUT_RATE) {
            v = mono[i < frames ? i : frames - 1];
        } else {
            double pos = (double)i * fmt_rate_ / OUT_RATE;
            int    i1  = (int)pos;
            float  t   = (float)(pos - i1);

            float p0 = mono_at(i1 - 1), p1 = mono_at(i1);
            float p2 = mono_at(i1 + 1), p3 = mono_at(i1 + 2);
            float a = -0.5f*p0 + 1.5f*p1 - 1.5f*p2 + 0.5f*p3;
            float b =       p0 - 2.5f*p1 + 2.0f*p2 - 0.5f*p3;
            float c = -0.5f*p0            + 0.5f*p2;
            v = ((a*t + b)*t + c)*t + p1;
        }
        v *= gain.load();
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = static_cast<int16_t>(v * 32767.0f);
    }
    return pcm;
}

void AudioCapture::stop() {
    ScopedCoInit co_init;
    running_ = false;
    // Stop WASAPI *before* joining the capture thread so GetNextPacketSize
    // immediately returns an error and the thread exits cleanly rather than
    // spinning on 5 ms sleeps until the join timeout fires.
    if (client_) client_->Stop();
    if (thread_) { WaitForSingleObject(thread_, 2000); CloseHandle(thread_); thread_ = nullptr; }
    if (client_)    { client_->Release();    client_    = nullptr; }
    if (capture_)   { capture_->Release();                    capture_   = nullptr; }
    if (device_)    { device_->Release();                     device_    = nullptr; }
    if (enumerator_){ enumerator_->Release();                 enumerator_= nullptr; }
    std::lock_guard<std::mutex> lk(g_capfmt_mtx);
    g_capfmt.erase(this);
}

// ── AudioPlayback ─────────────────────────────────────────────────────────────

AudioPlayback::~AudioPlayback() {
    stop_all();
}

void AudioPlayback::set_device(const std::wstring& device_id) {
    if (!can_open_render_device(device_id)) {
        dbglog("[audio/rx] render device switch rejected - keeping previous device");
        return;
    }

    render_running_ = false;
    if (render_thread_.joinable())
        render_thread_.join();

    // Tear down all existing streams — they'll be recreated on next play_opus call
    std::lock_guard<std::mutex> lk(streams_mtx_);
    {
        std::lock_guard<std::mutex> device_lk(device_mtx_);
        render_device_id_ = device_id;
    }
    streams_.clear();
}

std::wstring AudioPlayback::get_device() const {
    std::lock_guard<std::mutex> lk(device_mtx_);
    return render_device_id_;
}

void AudioPlayback::ensure_render_thread() {
    bool expected = false;
    if (render_running_.compare_exchange_strong(expected, true)) {
        render_thread_ = std::thread([this] { render_loop(); });
    }
}

void AudioPlayback::set_aec_reference_callback(AecRefCallback cb) {
    std::lock_guard<std::mutex> lk(aec_cb_mtx_);
    aec_cb_ = std::move(cb);
}

void AudioPlayback::destroy_stream(SpeakerStream* sp) {
    if (!sp) return;
    ScopedCoInit co_init;
    if (sp->client) {
        sp->client->Stop();
        sp->client->Release();
    }
    if (sp->render) sp->render->Release();
    if (sp->dec) opus_decoder_destroy(sp->dec);
    delete sp;
}

// Post-decode DSP helpers: distance low-pass + LUFS-style loudness normalizer.
// These run over a decoded frame in-place and mutate the SpeakerStream state
// so consecutive frames stay continuous (filter memory, EWMAs, gain ramps).
void AudioPlayback::apply_distance_lpf(SpeakerStream& sp,
                                       AudioPcmBuffer& pcm, float lpf_alpha) {
    sp.lpf_a_ = lpf_alpha;
    if (lpf_alpha >= 0.98f) return;
    float z = sp.lpf_z_;
    for (size_t i = 0; i < pcm.size(); i++) {
        float x = pcm[i] * (1.f / 32768.f);
        z = lpf_alpha * x + (1.f - lpf_alpha) * z;
        pcm[i] = static_cast<int16_t>(z * 32767.f);
    }
    sp.lpf_z_ = z;
}

// Per-speaker loudness normalization — targets a steady perceived level so
// loud shouters don't clip and soft talkers stay intelligible.
// Uses a running RMS EWMA (~1 s window) against a -23 dBFS (0.0708 linear)
// reference; gain ramps towards the derived makeup gain at attack/release
// rates that sound natural for speech.
//
// Note: this intentionally fights against the server-side proximity volume
// falloff (since it re-boosts quiet/distant voices back up to target), so it
// is OFF by default and only a user-opt-in toggle from the Settings UI.
void AudioPlayback::apply_loudness_norm(SpeakerStream& sp,
                                        AudioPcmBuffer& pcm) {
    // Gated by user toggle — most groups prefer the untouched proximity mix.
    // Ref lives on AudioPlayback; we'd need `this` here, but this is a
    // static member. Caller (decode_and_process) checks the flag instead.
    // Instantaneous RMS for this frame (linear, 0..1)
    double sumsq = 0.0;
    for (size_t i = 0; i < pcm.size(); i++) {
        double s = pcm[i] * (1.0 / 32768.0);
        sumsq += s * s;
    }
    float rms = static_cast<float>(sqrt(sumsq / (double)pcm.size()));

    // EWMA of RMS — roughly 1-second window at 20 ms frames (50 frames).
    // Gate updates on very quiet frames so silence doesn't pull the estimate down.
    if (rms > 0.003f) {
        sp.loud_rms_ewma = 0.95f * sp.loud_rms_ewma + 0.05f * rms;
    }

    // Derive target gain. -23 dBFS RMS is the EBU R 128 program target.
    constexpr float TARGET_RMS = 0.0708f;  // 10^(-23/20)
    constexpr float MIN_RMS    = 0.01f;    // don't amplify near-silence (would be ~30 dB)
    constexpr float MAX_GAIN   = 4.0f;     // +12 dB cap
    constexpr float MIN_GAIN   = 0.25f;    // -12 dB cap

    float target_gain = 1.0f;
    if (sp.loud_rms_ewma > MIN_RMS) {
        target_gain = TARGET_RMS / sp.loud_rms_ewma;
        if (target_gain > MAX_GAIN) target_gain = MAX_GAIN;
        if (target_gain < MIN_GAIN) target_gain = MIN_GAIN;
    }

    // Smooth gain ramp — slower release so we don't "pump" during pauses.
    float ramp = (target_gain < sp.loud_gain) ? 0.20f : 0.05f;
    sp.loud_gain += ramp * (target_gain - sp.loud_gain);

    // Apply gain in float, clamp back to int16.
    for (size_t i = 0; i < pcm.size(); i++) {
        float s = pcm[i] * sp.loud_gain;
        if (s >  32767.f) s =  32767.f;
        if (s < -32768.f) s = -32768.f;
        pcm[i] = static_cast<int16_t>(s);
    }
}

// Decode one opus packet into a normalized, distance-filtered frame ready to
// enqueue. Used both for the regular packet and for FEC-recovered prior frames.
// Returns false if decode failed (caller should skip enqueue).
bool AudioPlayback::decode_and_process(SpeakerStream& sp,
                                       const uint8_t* opus_data, int opus_bytes,
                                       int decode_fec, float lpf_alpha,
                                       AudioPcmBuffer& pcm_out) {
    pcm_out.assign(FRAME_SAMPLES, 0);
    int decoded = opus_decode(sp.dec,
                              opus_data, opus_bytes,
                              pcm_out.data(),
                              (int)FRAME_SAMPLES,
                              decode_fec);
    if (decoded <= 0) return false;
    pcm_out.resize((size_t)decoded);

    // Loudness norm runs FIRST (on the un-muffled signal) so the EWMA tracks
    // the speaker's natural level rather than the distance-attenuated level,
    // and so the subsequent LPF still shapes the "far" character of the voice.
    if (loudness_norm_enabled_.load(std::memory_order_relaxed))
        apply_loudness_norm(sp, pcm_out);

    apply_distance_lpf(sp, pcm_out, lpf_alpha);
    return true;
}

bool AudioPlayback::conceal_and_process(SpeakerStream& sp,
                                        float lpf_alpha,
                                        AudioPcmBuffer& pcm_out) {
    pcm_out.assign(FRAME_SAMPLES, 0);
    int decoded = opus_decode(sp.dec, nullptr, 0,
                              pcm_out.data(),
                              (int)FRAME_SAMPLES,
                              0);
    if (decoded <= 0) return false;
    pcm_out.resize((size_t)decoded);

    if (loudness_norm_enabled_.load(std::memory_order_relaxed))
        apply_loudness_norm(sp, pcm_out);
    apply_distance_lpf(sp, pcm_out, lpf_alpha);
    return true;
}

void AudioPlayback::play_opus(int speaker_id, const uint8_t* opus_data,
                               int opus_bytes, float volume,
                               float pan, float rear_attn,
                               float lpf_alpha,
                               uint16_t seq) {
    ensure_render_thread();

    std::shared_ptr<SpeakerStream> sp;
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        auto it = streams_.find(speaker_id);
        if (it == streams_.end()) {
            if (!init_stream(speaker_id)) return;
            it = streams_.find(speaker_id);
        }
        sp = it->second;
    }

    // ── Sequence / FEC recovery ───────────────────────────────────────────
    // Compute seq gap vs last seen packet. uint16 arithmetic wraps naturally,
    // and casting the delta to int16_t gives a signed distance — negative means
    // the packet is older than what we've already seen (reorder; drop).
    int gap = 1; // default: treat first packet as no-loss
    if (sp->have_last_seq) {
        // The sender resets tx_seq_ to 0 on every PTT release (set_ptt(false),
        // set_mute, etc.) via reset_mic_pipeline_.  If the speaker has been
        // silent for > 500 ms they almost certainly released PTT and started a
        // fresh transmission — clear have_last_seq so the new stream is accepted
        // immediately rather than being silently dropped until seq wraps past the
        // old high-water mark (which takes up to ~22 minutes at 50 pkt/s).
        {
            DWORD now_ck = GetTickCount();
            DWORD lr;
            {
                std::lock_guard<std::mutex> lk(sp->jmtx);
                lr = sp->last_recv;
            }
            if (lr > 0 && (now_ck - lr) > 500) {
                sp->have_last_seq = false;
                sp->last_seq = 0;
            }
        }
    }
    if (sp->have_last_seq) {
        int16_t delta = static_cast<int16_t>(seq - sp->last_seq);
        if (delta <= 0) {
            // Stale / duplicate / reordered — ignore.
            return;
        }
        gap = delta;
    }

    // If exactly one packet was lost, the current packet's in-band FEC carries
    // a coarse copy of the previous frame. Decode it first, push it, then
    // decode the normal frame. Two-or-more losses → rely on PLC.
    std::vector<std::vector<int16_t>> concealed_frames;
    concealed_frames.reserve((size_t)(std::min)(gap, MAX_CONCEAL_FRAMES));

    const int missing_frames = (gap > 1) ? (gap - 1) : 0;
    rx_packets_.fetch_add(1, std::memory_order_relaxed);
    if (missing_frames > 0) {
        rx_loss_events_.fetch_add(1, std::memory_order_relaxed);
        rx_lost_frames_.fetch_add(static_cast<uint64_t>(missing_frames), std::memory_order_relaxed);
    }
    const int recoverable_missing = (std::min)(missing_frames, MAX_CONCEAL_FRAMES);
    const int plc_frames = (recoverable_missing > 1)
        ? (recoverable_missing - 1)
        : 0;
    for (int i = 0; i < plc_frames; ++i) {
        std::vector<int16_t> plc_pcm;
        if (!conceal_and_process(*sp, lpf_alpha, plc_pcm))
            break;
        concealed_frames.push_back(std::move(plc_pcm));
    }

    if (missing_frames > 0) {
        std::vector<int16_t> recovered;
        // decode_fec=1 reconstructs the frame *before* the current packet from
        // redundancy embedded in the current packet's payload.
        if (decode_and_process(*sp, opus_data, opus_bytes,
                               /*decode_fec=*/1, lpf_alpha,
                               recovered)) {
            concealed_frames.push_back(std::move(recovered));
        } else if (recoverable_missing == missing_frames) {
            std::vector<int16_t> plc_pcm;
            if (conceal_and_process(*sp, lpf_alpha, plc_pcm))
                concealed_frames.push_back(std::move(plc_pcm));
        }
    }

    // Decode current frame normally (or PLC on decoder error).
    std::vector<int16_t> pcm;
    if (!decode_and_process(*sp, opus_data, opus_bytes,
                            /*decode_fec=*/0, lpf_alpha, pcm)) {
        if (!conceal_and_process(*sp, lpf_alpha, pcm)) return;
    }

    if (missing_frames > recoverable_missing) {
        sp->last_seq = static_cast<uint16_t>(seq - (missing_frames - recoverable_missing));
    } else {
        sp->last_seq = seq;
    }
    sp->have_last_seq = true;

    {
        std::lock_guard<std::mutex> lk(sp->jmtx);

        // ── Adaptive jitter buffer ────────────────────────────────────────────
        // Measure inter-arrival jitter and adapt the target depth accordingly.
        DWORD now = GetTickCount();
        if (sp->last_recv > 0) {
            float iat_ms  = static_cast<float>(now - sp->last_recv);
            if (iat_ms > 200.f) {
                // Long gap — reset jitter so re-entry doesn't stall on max pre-buffer.
                sp->jitter_ms_       = 0.0f;
                sp->adaptive_target_ = 2;
            } else {
                float jitter = (iat_ms > 20.f) ? (iat_ms - 20.f) : (20.f - iat_ms);
                // EWMA: smooth jitter estimate
                sp->jitter_ms_ = 0.90f * sp->jitter_ms_ + 0.10f * jitter;
                float prev_stat = rx_jitter_ms_stat_.load(std::memory_order_relaxed);
                rx_jitter_ms_stat_.store(prev_stat * 0.90f + sp->jitter_ms_ * 0.10f, std::memory_order_relaxed);

                // target = ceil(jitter / 20ms) + 1, clamped [JITTER_MIN, JITTER_TARGET_MAX]
                int new_target = static_cast<int>(sp->jitter_ms_ / 20.f) + 1;
                if (new_target < JITTER_MIN)        new_target = JITTER_MIN;
                if (new_target > JITTER_TARGET_MAX) new_target = JITTER_TARGET_MAX;
                sp->adaptive_target_ = new_target;
            }
        }
        sp->last_recv = now;

        // Hard cap: drop oldest frame if queue is too deep
        int hard_max = sp->adaptive_target_ * 3;
        if (hard_max > JITTER_MAX_ABS) hard_max = JITTER_MAX_ABS;
        // When we push 2 frames (recovery + normal), make room for both.
        int to_push = (int)concealed_frames.size() + 1;
        while ((int)sp->jqueue.size() + to_push > hard_max && !sp->jqueue.empty())
            sp->jqueue.pop_front();

        // ── Smooth volume ─────────────────────────────────────────────────────
        // Interpolate towards the incoming volume each frame to avoid clicks
        // when a player walks in or out of proximity range.
        // Fast attack (0.3): volume rises quickly as player approaches
        // Slow release (0.05): volume fades gradually as player moves away
        float alpha = (volume > sp->smooth_vol_) ? 0.30f : 0.05f;
        sp->smooth_vol_ += alpha * (volume - sp->smooth_vol_);

        for (auto& concealed : concealed_frames)
            sp->jqueue.push_back({ std::move(concealed), sp->smooth_vol_, pan, rear_attn });
        sp->jqueue.push_back({ std::move(pcm), sp->smooth_vol_, pan, rear_attn });

        // Start playing once we have enough pre-buffered frames
        if (sp->buffering && (int)sp->jqueue.size() >= sp->adaptive_target_)
            sp->buffering = false;
    }
}

void AudioPlayback::render_loop() {
    ScopedCoInit co_init;
    timeBeginPeriod(1);

    DWORD task_idx;
    HANDLE task = AvSetMmThreadCharacteristicsA("Pro Audio", &task_idx);

    while (render_running_) {
        Sleep(5);

        // Snapshot stream list without holding the map lock during WASAPI calls
        std::vector<std::shared_ptr<SpeakerStream>> active;
        {
            std::lock_guard<std::mutex> lk(streams_mtx_);
            active.reserve(streams_.size());
            for (auto& kv : streams_) active.push_back(kv.second);
        }

        for (const auto& sp : active) {
            PcmFrame frame;
            bool have_frame = false;
            {
                std::lock_guard<std::mutex> lk(sp->jmtx);
                if (!sp->buffering && !sp->jqueue.empty()) {
                    frame = sp->jqueue.front();
                    have_frame = true;
                } else if (!sp->buffering && sp->jqueue.empty()) {
                    sp->buffering = true;
                }
            }

            if (have_frame) {
                // Tap frame for AEC reference BEFORE panning/volume — the
                // canceller wants the dry mono mix because the physical
                // speaker→mic path already contains the real panning of the
                // listener's speakers, which we can't predict.
                {
                    std::lock_guard<std::mutex> lk(aec_cb_mtx_);
                    if (aec_cb_)
                        aec_cb_(frame.samples.data(), frame.samples.size());
                }

                if (write_pcm_frame(*sp, frame.samples.data(),
                                    (UINT32)frame.samples.size(),
                                    frame.volume * gain.load(),
                                    frame.pan, frame.rear_attn)) {
                    std::lock_guard<std::mutex> lk(sp->jmtx);
                    if (!sp->jqueue.empty())
                        sp->jqueue.pop_front();
                }
            }
        }
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    timeEndPeriod(1);
}

bool AudioPlayback::write_pcm_frame(SpeakerStream& s,
                                     const int16_t* pcm, UINT32 n_samples,
                                     float volume, float pan, float rear_attn) {
    constexpr DWORD  IN_RATE = 48000;
    const     float  GAIN    = 1.10f;  // mild trim boost; keeps playback cleaner at peaks

    float gain_l = 1.0f, gain_r = 1.0f;
    calc_pan_gains(pan, rear_attn, gain_l, gain_r);

    UINT32 out_frames = (s.rate == IN_RATE)
        ? n_samples
        : static_cast<UINT32>((double)n_samples * s.rate / IN_RATE);

    UINT32 buf_size = 0, padding = 0;
    if (FAILED(s.client->GetBufferSize(&buf_size)))    return false;
    if (FAILED(s.client->GetCurrentPadding(&padding))) return false;

    UINT32 avail = buf_size - padding;
    if (avail < out_frames) return false; // not enough room yet; keep frame queued

    BYTE* dst_data = nullptr;
    if (FAILED(s.render->GetBuffer(out_frames, &dst_data))) return false;

    for (UINT32 i = 0; i < out_frames; i++) {
        float mono = lerp_pcm(pcm, n_samples, IN_RATE, s.rate, i) * volume * GAIN;
        // note: volume is already (speaker_vol * global_gain); GAIN is only a mild trim boost
        float left  = mono * gain_l;
        float right = mono * gain_r;

        if (left  >  1.0f) left  =  1.0f;
        if (left  < -1.0f) left  = -1.0f;
        if (right >  1.0f) right =  1.0f;
        if (right < -1.0f) right = -1.0f;

        if (s.is_float) {
            float* d = reinterpret_cast<float*>(dst_data);
            if (s.channels >= 2) {
                d[i * s.channels + 0] = left;
                d[i * s.channels + 1] = right;
                for (WORD c = 2; c < s.channels; c++)
                    d[i * s.channels + c] = 0.5f * (left + right);
            } else {
                d[i * s.channels] = 0.5f * (left + right);
            }
        } else if (s.format.wBitsPerSample == 32) {
            int32_t* d = reinterpret_cast<int32_t*>(dst_data);
            int32_t  sl = static_cast<int32_t>(left  * 2147483647.0f);
            int32_t  sr = static_cast<int32_t>(right * 2147483647.0f);
            if (s.channels >= 2) {
                d[i * s.channels + 0] = sl;
                d[i * s.channels + 1] = sr;
                for (WORD c = 2; c < s.channels; c++)
                    d[i * s.channels + c] = static_cast<int32_t>((sl / 2) + (sr / 2));
            } else {
                d[i * s.channels] = static_cast<int32_t>((sl / 2) + (sr / 2));
            }
        } else {
            int16_t* d = reinterpret_cast<int16_t*>(dst_data);
            int16_t  sl = static_cast<int16_t>(left  * 32767.0f);
            int16_t  sr = static_cast<int16_t>(right * 32767.0f);
            if (s.channels >= 2) {
                d[i * s.channels + 0] = sl;
                d[i * s.channels + 1] = sr;
                for (WORD c = 2; c < s.channels; c++)
                    d[i * s.channels + c] = static_cast<int16_t>((sl / 2) + (sr / 2));
            } else {
                d[i * s.channels] = static_cast<int16_t>((sl / 2) + (sr / 2));
            }
        }
    }

    s.render->ReleaseBuffer(out_frames, 0);
    return true;
}

bool AudioPlayback::init_stream(int speaker_id) {
    ScopedCoInit co_init;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioRenderClient*  render     = nullptr;
    WAVEFORMATEX*        mix        = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                     CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return false;

    std::wstring render_device_id;
    {
        std::lock_guard<std::mutex> lk(device_mtx_);
        render_device_id = render_device_id_;
    }

    if (!render_device_id.empty()) {
        hr = enumerator->GetDevice(render_device_id.c_str(), &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (FAILED(hr)) { enumerator->Release(); return false; }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { device->Release(); enumerator->Release(); return false; }

    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) { client->Release(); device->Release(); enumerator->Release(); return false; }

    // Force stereo
    mix->nChannels       = 2;
    mix->nBlockAlign     = mix->nChannels * mix->wBitsPerSample / 8;
    mix->nAvgBytesPerSec = mix->nSamplesPerSec * mix->nBlockAlign;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
        ex->Format.nChannels      = 2;
        ex->Format.nBlockAlign    = ex->Format.nChannels * ex->Format.wBitsPerSample / 8;
        ex->Format.nAvgBytesPerSec= ex->Format.nSamplesPerSec * ex->Format.nBlockAlign;
        ex->dwChannelMask         = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        ex->Samples.wValidBitsPerSample = ex->Format.wBitsPerSample;
    }

    SampleKind kind = detect_kind(mix);
    bool  is_float  = (kind == SampleKind::Float32);
    WORD  channels  = mix->nChannels;
    DWORD rate      = mix->nSamplesPerSec;

    char buf[180];
    sprintf_s(buf, "[audio/rx] stream %d: rate=%lu ch=%u bits=%u kind=%d",
              speaker_id, rate, channels, mix->wBitsPerSample, (int)kind);
    dbglog(buf);

    // Balanced shared-mode render buffer. 60ms avoids most device underruns
    // while staying much more responsive than the old 200ms buffer.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 600000, 0, mix, nullptr);
    if (FAILED(hr)) { CoTaskMemFree(mix); client->Release(); device->Release(); enumerator->Release(); return false; }

    hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (FAILED(hr)) { CoTaskMemFree(mix); client->Release(); device->Release(); enumerator->Release(); return false; }

    hr = client->Start();
    if (FAILED(hr)) { render->Release(); CoTaskMemFree(mix); client->Release(); device->Release(); enumerator->Release(); return false; }

    // Measure render latency — used by AEC to set system delay
    {
        REFERENCE_TIME lat = 0;
        int ms = 20; // fallback
        if (SUCCEEDED(client->GetStreamLatency(&lat)))
            ms = static_cast<int>(lat / 10000);
        render_latency_ms_.store(ms);
        char lb[56]; sprintf_s(lb, "[audio/rx] render latency=%d ms", ms); dbglog(lb);
    }

    // Create Opus decoder (48kHz mono)
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    if (!dec) {
        char b[64]; sprintf_s(b, "[audio/rx] opus_decoder_create FAILED err=%d", err); dbglog(b);
        render->Release(); CoTaskMemFree(mix); client->Release(); device->Release(); enumerator->Release();
        return false;
    }

    std::shared_ptr<SpeakerStream> sp(new SpeakerStream(), &AudioPlayback::destroy_stream);
    sp->client     = client;
    sp->render     = render;
    sp->format     = *mix;
    sp->is_float   = is_float;
    sp->channels   = channels;
    sp->rate       = rate;
    sp->dec        = dec;
    sp->buffering  = true;
    sp->last_recv  = GetTickCount();
    streams_[speaker_id] = sp;

    CoTaskMemFree(mix);
    enumerator->Release();
    device->Release();
    return true;
}

void AudioPlayback::remove_speaker(int speaker_id) {
    std::shared_ptr<SpeakerStream> sp;
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        auto it = streams_.find(speaker_id);
        if (it == streams_.end()) return;
        sp = it->second;
        streams_.erase(it);
    }
}

void AudioPlayback::flush_all() {
    std::vector<std::shared_ptr<SpeakerStream>> active;
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        active.reserve(streams_.size());
        for (auto& kv : streams_) active.push_back(kv.second);
    }

    for (const auto& sp : active) {
        if (!sp) continue;
        std::lock_guard<std::mutex> lk(sp->jmtx);
        sp->jqueue.clear();
        sp->buffering = true;
        sp->last_recv = 0;
        sp->jitter_ms_ = 0.0f;
        sp->adaptive_target_ = 1; // 1 frame (20 ms) pre-buffer after channel switch
        sp->smooth_vol_ = 1.0f;
        sp->have_last_seq = false;
        sp->last_seq = 0;
        sp->lpf_z_ = 0.0f;
        sp->lpf_a_ = 1.0f;
        if (sp->dec) {
            opus_decoder_ctl(sp->dec, OPUS_RESET_STATE);
        }
    }
    rx_jitter_ms_stat_.store(0.0f, std::memory_order_relaxed);
}

void AudioPlayback::stop_all() {
    render_running_ = false;
    if (render_thread_.joinable()) render_thread_.join();

    std::lock_guard<std::mutex> lk(streams_mtx_);
    streams_.clear();
}
