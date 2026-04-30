#pragma once
// Acoustic Echo Canceller — NLMS adaptive filter.
//
// Problem: when a player isn't using headphones, their mic picks up what the
// speakers are playing (other players' voices). Without AEC the system feeds
// that audio straight back out, creating echo + risk of positive feedback
// loops. Pro voice stacks (Discord/Zoom) rely on WebRTC AEC3 which is a full
// subband/delay-tracking/nonlinear processor. Building AEC3 from scratch is
// infeasible; instead we ship a correctly-tuned NLMS time-domain canceller
// that handles the common case (moderate volume, fixed ~0-20 ms system delay,
// mostly-linear speaker+mic path). This is enough to kill audible echo during
// normal play when headphones aren't worn, without dragging in WebRTC.
//
// Pipeline (both capture & playback run at 48 kHz mono):
//   1. playback writes rendered samples into a ring buffer (reference)
//   2. capture pulls `process(mic, n)`; for each sample y[n] we subtract the
//      filter's estimate  ĥᵀ·x[n-D..n-D-L]  to produce the echo-removed e[n]
//   3. filter coefficients are updated via NLMS:
//        ĥ ← ĥ + (μ / (xᵀx + ε)) · e[n] · x
//   4. a double-talk detector freezes the update when the near-end is
//      significantly louder than the estimated echo (so our own speech doesn't
//      corrupt the filter). DTX frames (silent reference) skip processing
//      entirely — no echo possible.
//
// x_buf_ uses a double-length circular buffer so the sliding tap window is
// always contiguous — no memmove per sample, O(1) slide.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

class EchoCanceller {
public:
    EchoCanceller() {
        h_.assign(FILTER_LEN, 0.0f);
        ref_ring_.assign(REF_RING_SIZE, 0.0f);
        // Double-length buffer: x_buf_[0..FILTER_LEN-1] and
        // x_buf_[FILTER_LEN..2*FILTER_LEN-1] are kept as mirror copies.
        // x_head_ is the index of the most-recent sample; the tap window is
        // always the contiguous slice x_buf_[x_head_..x_head_+FILTER_LEN-1].
        x_buf_.assign(FILTER_LEN * 2, 0.0f);
        ref_write_ = 0;
        x_head_    = 0;
    }

    void set_enabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool is_enabled() const  { return enabled_.load(std::memory_order_relaxed); }

    // Update the estimated render→capture round-trip delay.
    // Call once after WASAPI streams are initialized with the sum of render +
    // capture GetStreamLatency() values (in ms). Clamped to the ring buffer.
    void set_system_delay_ms(int ms) {
        if (ms < 0)   ms = 0;
        if (ms > 280) ms = 280;  // leave headroom inside the 300 ms ring
        std::lock_guard<std::mutex> lk(mtx_);
        system_delay_samples_ = static_cast<size_t>(ms) * 48000 / 1000;
    }
    int get_system_delay_ms() const {
        return static_cast<int>(system_delay_samples_ * 1000 / 48000);
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(ref_ring_.begin(), ref_ring_.end(), 0.0f);
        std::fill(x_buf_.begin(), x_buf_.end(), 0.0f);
        ref_write_ = 0;
        x_head_    = 0;
        erle_ewma_ = 0.0f;
    }

    // Called from the playback render thread with the MIX of all decoded
    // far-end voices that are about to be written to the speakers. We push
    // them into the reference ring so capture can align + subtract.
    // Samples are mono float in [-1, 1]; if the playback mixer only has int16
    // available, convert on the way in.
    void push_reference(const int16_t* pcm, size_t n) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < n; i++) {
            ref_ring_[ref_write_] = pcm[i] * (1.0f / 32768.0f);
            ref_write_ = (ref_write_ + 1) % REF_RING_SIZE;
        }
    }

    // Called by capture on every frame (typically 20 ms / 960 samples at
    // 48 kHz). Mutates pcm in place — pcm = near_end - estimated_echo.
    void process(int16_t* pcm, size_t n) {
        if (!enabled_.load(std::memory_order_relaxed) || n == 0) return;

        std::lock_guard<std::mutex> lk(mtx_);

        // Quick test: if the reference ring is essentially silent (nobody is
        // talking), bail out so we don't spend CPU or risk filter drift on
        // pure-noise reference.
        float ref_pow = 0.0f;
        for (float s : ref_ring_) ref_pow += s * s;
        ref_pow /= static_cast<float>(REF_RING_SIZE);
        if (ref_pow < 1e-7f) return;

        // Frozen read pointer for this whole frame: most recent sample aligned
        // by SYSTEM_DELAY_SAMPLES (captures the typical round-trip lag on the
        // Windows WASAPI stack — mic callback runs a few ms behind speaker
        // render).
        size_t read_base = (ref_write_ + REF_RING_SIZE - system_delay_samples_) % REF_RING_SIZE;

        // Load the tap window into both halves of the double-length buffer so
        // that x_buf_[x_head_..x_head_+FILTER_LEN-1] is always contiguous.
        for (size_t i = 0; i < FILTER_LEN; i++) {
            size_t idx = (read_base + REF_RING_SIZE - i) % REF_RING_SIZE;
            x_buf_[i]              = ref_ring_[idx];
            x_buf_[i + FILTER_LEN] = ref_ring_[idx];
        }
        x_head_ = 0;

        float xpow = 0.0f;
        for (size_t k = 0; k < FILTER_LEN; k++) xpow += x_buf_[k] * x_buf_[k];

        for (size_t n_i = 0; n_i < n; n_i++) {
            float y = pcm[n_i] * (1.0f / 32768.0f);

            // Contiguous tap window — x_head_ keeps the newest sample at [0]
            // relative to xp, oldest at [FILTER_LEN-1]. No modulo in loops.
            const float* xp = x_buf_.data() + x_head_;

            // ĥᵀ x — echo estimate
            float y_est = 0.0f;
            for (size_t k = 0; k < FILTER_LEN; k++)
                y_est += h_[k] * xp[k];

            // Error / echo-removed sample
            float e = y - y_est;

            // ── Double-talk detection ────────────────────────────────
            // Compare near-end power to reference power. If the near-end is
            // much stronger than the reference, we're almost certainly
            // in double-talk → don't adapt.
            float near_pow = y * y;
            float ratio    = near_pow / (xpow / FILTER_LEN + 1e-9f);
            bool  doubletalk = (ratio > DOUBLETALK_RATIO);

            if (!doubletalk && xpow > 1e-6f) {
                float mu = NLMS_STEP / (xpow + 1e-6f);
                for (size_t k = 0; k < FILTER_LEN; k++)
                    h_[k] += mu * e * xp[k];
            }

            // Track ERLE (Echo Return Loss Enhancement) just for diagnostics.
            float e2 = e * e;
            erle_ewma_ = 0.995f * erle_ewma_
                       + 0.005f * (near_pow / (e2 + 1e-9f));

            // Write back echo-removed sample
            float eo = e;
            if (eo >  1.f) eo =  1.f;
            if (eo < -1.f) eo = -1.f;
            pcm[n_i] = static_cast<int16_t>(eo * 32767.f);

            // Slide tap window by one sample — O(1), no memmove.
            // Grab the oldest sample (last in current window) before moving head.
            float dropped = xp[FILTER_LEN - 1];
            read_base = (read_base + 1) % REF_RING_SIZE;
            float new_x = ref_ring_[read_base];
            // Decrement head (wraps at FILTER_LEN); write new sample in both halves.
            if (x_head_ == 0) x_head_ = FILTER_LEN;
            x_head_--;
            x_buf_[x_head_]              = new_x;
            x_buf_[x_head_ + FILTER_LEN] = new_x;
            xpow += new_x * new_x - dropped * dropped;
            if (xpow < 0.0f) xpow = 0.0f;   // defend against rounding drift
        }
    }

    float erle_db() const {
        // erle_ewma_ is written by process() under mtx_; take a snapshot under
        // the lock so we don't read a torn float on the overlay/UI thread.
        std::lock_guard<std::mutex> lk(mtx_);
        const float e = erle_ewma_;
        if (e <= 1.0f) return 0.0f;
        return 10.0f * std::log10(e);
    }

private:
    // ── Tuning ────────────────────────────────────────────────────────────
    // 1024 taps at 48 kHz = 21.3 ms filter length. Covers the typical
    // speaker→mic echo path including WASAPI buffer delay (10–20 ms) and
    // early room reflections. The old 128-tap (2.67 ms) filter was too
    // short to cancel most real-world echo paths.
    static constexpr size_t FILTER_LEN        = 1024;

    // Reference ring: 300 ms buffer — extra headroom for systems with longer
    // WASAPI latency (e.g. USB audio, some Bluetooth devices).
    static constexpr size_t REF_RING_SIZE     = 48000 * 3 / 10;  // 300 ms

    // Default one-way playback-to-capture system delay — overridden at
    // runtime via set_system_delay_ms() once WASAPI reports real latencies.

    // NLMS step size. Slightly larger than before for faster convergence
    // with the longer filter; still conservative enough to stay stable.
    static constexpr float  NLMS_STEP         = 0.20f;

    // Double-talk threshold (near-power / average reference power).
    // Above this ratio we freeze adaptation.
    static constexpr float  DOUBLETALK_RATIO  = 4.0f;

    // ── State ─────────────────────────────────────────────────────────────
    std::atomic<bool>   enabled_{ false };
    mutable std::mutex  mtx_;
    std::vector<float>  h_;           // adaptive filter coefficients
    std::vector<float>  ref_ring_;    // far-end reference ring buffer
    // Double-length circular tap window (2 × FILTER_LEN).
    // x_head_ = index of most-recent sample; read window is always
    // x_buf_[x_head_..x_head_+FILTER_LEN-1] — contiguous, no modulo.
    std::vector<float>  x_buf_;
    size_t              x_head_              = 0;
    size_t              ref_write_           = 0;
    float               erle_ewma_           = 0.0f;
    size_t              system_delay_samples_= 1440; // default 30 ms; updated from WASAPI
};
