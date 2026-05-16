#pragma once
// Automatic Gain Control — normalizes microphone input to a target RMS level.
// Uses fast-attack / slow-release envelope following:
//   - Fast attack: gain drops quickly if signal is too loud (prevents clipping)
//   - Slow release: gain rises slowly when signal is quiet (prevents pumping)
// A soft clipper protects against sudden peaks that bypass the envelope.

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

class AutoGainControl {
public:
    AutoGainControl() { reset(); }

    void reset() {
        envelope_ = TARGET_RMS;
        gain_      = 1.0f;
    }

    void set_enabled(bool v) { enabled_ = v; }
    bool is_enabled()  const { return enabled_; }

    // process: int16 mono samples, any frame size.
    void process(int16_t* pcm, size_t n) {
        if (!enabled_ || n == 0) return;

        // 1. Measure frame RMS
        float sum = 0.f;
        for (size_t i = 0; i < n; i++) {
            float s = pcm[i] * (1.f / 32768.f);
            sum += s * s;
        }
        float rms = std::sqrt(sum / static_cast<float>(n));

        // 2. Envelope follower — fast attack, slow release
        float alpha = (rms > envelope_) ? ATTACK : RELEASE;
        envelope_ = (1.f - alpha) * envelope_ + alpha * rms;

        // 3. Compute target gain: how much we need to scale to reach TARGET_RMS
        float target_gain = (envelope_ > 1e-7f) ? (TARGET_RMS / envelope_) : 1.0f;
        target_gain = std::min(target_gain, MAX_GAIN);

        // 4. Smooth gain: lower gain fast (prevents clipping), raise slowly (prevents pumping)
        float g_alpha = (target_gain < gain_) ? GAIN_ATTACK : GAIN_RELEASE;
        gain_ += g_alpha * (target_gain - gain_);

        // 5. Apply gain with a soft clipper (tanh-like saturation, no hard distortion)
        for (size_t i = 0; i < n; i++) {
            float v = pcm[i] * (1.f / 32768.f) * gain_;
            // Soft saturation: x / (1 + |x|/K) — smooth knee at K = 0.8
            constexpr float K = 0.8f;
            v = v / (1.f + std::abs(v) / K);
            // Hard-limit just in case
            if (v >  1.f) v =  1.f;
            if (v < -1.f) v = -1.f;
            pcm[i] = static_cast<int16_t>(v * 32767.f);
        }
    }

private:
    // ── Tuning ──────────────────────────────────────────────────────────────────
    static constexpr float TARGET_RMS    = 0.10f;  // -20 dBFS target level
    static constexpr float MAX_GAIN      = 4.0f;   // max 12 dB boost
    static constexpr float ATTACK        = 0.90f;  // envelope attack per frame (~fast, ~2ms at 20ms frames)
    static constexpr float RELEASE       = 0.003f; // envelope release per frame (~slow, ~7s decay)
    static constexpr float GAIN_ATTACK   = 0.50f;  // gain drop speed (fast — prevents clipping)
    static constexpr float GAIN_RELEASE  = 0.015f; // gain rise speed (slow — prevents pumping, ~1.3s ramp)

    // ── State ────────────────────────────────────────────────────────────────────
    bool  enabled_  = false;
    float envelope_ = TARGET_RMS;
    float gain_     = 1.0f;
};
