#pragma once
// Spectral-class noise suppressor — time-domain, no FFT, no block artifacts.
//
// Improvements over a simple noise gate:
//  • Minimum-statistics noise floor: maintains a 1-second circular buffer of
//    per-frame RMS minimums.  The noise floor is the rolling minimum × 1.5,
//    which tracks slowly-changing backgrounds (fans, AC, keyboard) far better
//    than exponential smoothing.
//  • Pre-emphasis / de-emphasis: boosts high frequencies before gating so
//    consonants (S, T, P) are not clipped by the gate, then restores flat
//    response afterwards.
//  • Softer gate curve: lower floor (5% residual) + wider SNR knee gives
//    cleaner silences without choppiness on speech.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <cmath>
#include <cstdint>
#include <algorithm>

class NoiseSuppressor {
public:
    NoiseSuppressor() { reset(); }

    void reset() {
        for (float& v : rms_hist_) v = 0.001f;
        hist_idx_     = 0;
        hist_full_    = false;
        noise_floor_  = 0.001f;
        gain_         = 1.0f;
        pre_z_        = 0.f;   // pre-emphasis IIR state
        de_z_         = 0.f;   // de-emphasis IIR state
    }

    void set_enabled(bool v) { enabled_ = v; }
    bool is_enabled()  const { return enabled_; }

    // pcm: int16 mono samples, any frame size
    void process(int16_t* pcm, size_t n) {
        if (!enabled_ || n == 0) return;

        // ── 1. Pre-emphasis: 1-pole HPF to boost consonants ───────────────────
        //    y[n] = x[n] - PRE_COEFF * x[n-1]   (classic speech pre-emphasis)
        {
            float z = pre_z_;
            for (size_t i = 0; i < n; i++) {
                float x = pcm[i] * (1.f / 32768.f);
                float y = x - PRE_COEFF * z;
                z = x;
                // write back temporarily (we'll re-quantise after gate)
                pcm[i] = static_cast<int16_t>(std::max(-1.f, std::min(1.f, y)) * 32767.f);
            }
            pre_z_ = z;
        }

        // ── 2. Frame RMS (of pre-emphasised signal) ───────────────────────────
        float sum = 0.f;
        for (size_t i = 0; i < n; i++) {
            float s = pcm[i] * (1.f / 32768.f);
            sum += s * s;
        }
        float rms = std::sqrt(sum / static_cast<float>(n));

        // ── 3. Minimum-statistics noise floor ─────────────────────────────────
        rms_hist_[hist_idx_] = rms;
        hist_idx_ = (hist_idx_ + 1) % HIST_SIZE;
        if (hist_idx_ == 0) hist_full_ = true;

        int count   = hist_full_ ? HIST_SIZE : (hist_idx_ == 0 ? HIST_SIZE : hist_idx_);
        float min_r = rms_hist_[0];
        for (int i = 1; i < count; i++)
            if (rms_hist_[i] < min_r) min_r = rms_hist_[i];

        // Noise floor = rolling minimum × overhead factor
        noise_floor_ = min_r * OVERHEAD;

        // ── 4. Gate gain: smoothstep from closed (SNR=GATE_SNR) to open (SNR=OPEN_SNR)
        float snr    = rms / (noise_floor_ + 1e-9f);
        float target = smoothstep(GATE_SNR, OPEN_SNR, snr);

        float rate = (target > gain_) ? ATTACK : RELEASE;
        gain_ += rate * (target - gain_);
        float g = std::max(FLOOR_GAIN, gain_);

        // ── 5. Apply gate gain ─────────────────────────────────────────────────
        for (size_t i = 0; i < n; i++) {
            float v = pcm[i] * (1.f / 32768.f) * g;
            if (v >  1.f) v =  1.f;
            if (v < -1.f) v = -1.f;
            pcm[i] = static_cast<int16_t>(v * 32767.f);
        }

        // ── 6. De-emphasis: integrate back the pre-emphasis curve ─────────────
        //    y[n] = x[n] + PRE_COEFF * y[n-1]   (inverse of step 1)
        {
            float z = de_z_;
            for (size_t i = 0; i < n; i++) {
                float x = pcm[i] * (1.f / 32768.f);
                float y = x + PRE_COEFF * z;
                z = y;
                if (y >  1.f) y =  1.f;
                if (y < -1.f) y = -1.f;
                pcm[i] = static_cast<int16_t>(y * 32767.f);
            }
            de_z_ = z;
        }
    }

private:
    // ── Tuning ───────────────────────────────────────────────────────────────────
    static constexpr int   HIST_SIZE  = 50;    // 50 frames × 20 ms = 1-second window
    static constexpr float OVERHEAD   = 1.5f;  // noise floor = min_rms × 1.5
    static constexpr float GATE_SNR   = 1.8f;  // SNR below → gate closed
    static constexpr float OPEN_SNR   = 5.0f;  // SNR above → gate fully open
    static constexpr float FLOOR_GAIN = 0.05f; // 5% residual (quieter background vs 8% before)
    static constexpr float ATTACK     = 0.70f; // fast open (speech onset)
    static constexpr float RELEASE    = 0.03f; // slower close (smoother tail-off)
    static constexpr float PRE_COEFF  = 0.97f; // pre-emphasis coefficient

    // ── State ─────────────────────────────────────────────────────────────────────
    bool  enabled_   = false;
    float rms_hist_[HIST_SIZE] = {};
    int   hist_idx_  = 0;
    bool  hist_full_ = false;
    float noise_floor_= 0.001f;
    float gain_       = 1.0f;
    float pre_z_      = 0.f;
    float de_z_       = 0.f;

    static float smoothstep(float lo, float hi, float x) {
        if (x <= lo) return 0.f;
        if (x >= hi) return 1.f;
        float t = (x - lo) / (hi - lo);
        return t * t * (3.f - 2.f * t);
    }
};
