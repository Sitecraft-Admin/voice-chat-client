#pragma once
// VoiceActivityDetector — lightweight, no-dependency VAD in the spirit of
// WebRTC's GMM-based detector but built from features we can compute with a
// single pass over int16 samples:
//   • frame energy               (robust against low-level steady noise)
//   • zero-crossing rate (ZCR)   (high for sibilants + fricatives, low for voiced)
//   • spectral flatness measure  (SFM — cheap FFT-free approximation via the
//                                 ratio of geometric to arithmetic mean of the
//                                 per-sub-band energies)
//   • hangover window            (keeps the gate open for ~120 ms after last
//                                 detected speech so word-final consonants and
//                                 short pauses don't chop)
//
// The combination catches both voiced speech (high energy, low ZCR, low SFM)
// and unvoiced consonants (moderate energy, high ZCR) while rejecting broadband
// stationary noise (high SFM, no hangover flip).
//
// Designed to be called frame-by-frame on 20 ms @ 48 kHz mono (FRAME = 960).
// Cheap enough to run before Opus encode on every packet.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cmath>
#include <cstdint>
#include <algorithm>

class VoiceActivityDetector {
public:
    VoiceActivityDetector() { reset(); }

    void reset() {
        noise_energy_   = 1e-4f;
        hangover_ticks_ = 0;
        speech_ewma_    = 0.0f;
    }

    void set_enabled(bool v) { enabled_ = v; }
    bool is_enabled() const  { return enabled_; }

    // Returns true when the current frame is considered speech (or inside the
    // hangover window). When disabled, always returns true so the simple RMS
    // gate upstream remains the sole arbiter.
    bool is_speech(const int16_t* pcm, size_t n) {
        if (!enabled_ || n == 0) return true;

        // ── 1. Frame energy (mean-square, linear) ────────────────────────
        double sumsq = 0.0;
        for (size_t i = 0; i < n; i++) {
            double s = pcm[i] * (1.0 / 32768.0);
            sumsq += s * s;
        }
        float energy = static_cast<float>(sumsq / static_cast<double>(n));

        // ── 2. Zero-crossing rate ─────────────────────────────────────────
        // Tight centered around 0; voiced speech sits ~0.02-0.08, fricatives
        // up to ~0.3, broadband noise ~0.5.
        int zc = 0;
        int16_t prev = pcm[0];
        for (size_t i = 1; i < n; i++) {
            if ((pcm[i] ^ prev) < 0) zc++;   // sign flipped
            prev = pcm[i];
        }
        float zcr = static_cast<float>(zc) / static_cast<float>(n - 1);

        // ── 3. Spectral flatness approximation (no FFT) ───────────────────
        // Split the frame into N contiguous sub-bands, compute per-band RMS,
        // then SFM = geometric_mean(rms) / arithmetic_mean(rms). Voiced
        // speech concentrates energy in a few bands → low SFM; white noise
        // spreads evenly → SFM close to 1.
        constexpr int BANDS = 8;
        float band_rms[BANDS] = {};
        size_t band_len = n / BANDS;
        if (band_len == 0) band_len = 1;
        for (int b = 0; b < BANDS; b++) {
            size_t start = b * band_len;
            size_t end   = (b == BANDS - 1) ? n : start + band_len;
            double bs    = 0.0;
            for (size_t i = start; i < end; i++) {
                double s = pcm[i] * (1.0 / 32768.0);
                bs += s * s;
            }
            band_rms[b] = static_cast<float>(std::sqrt(bs / static_cast<double>(end - start)) + 1e-6);
        }
        double amean = 0.0, glog = 0.0;
        for (int b = 0; b < BANDS; b++) {
            amean += band_rms[b];
            glog  += std::log(static_cast<double>(band_rms[b]));
        }
        amean /= BANDS;
        double gmean = std::exp(glog / BANDS);
        float sfm = static_cast<float>(gmean / (amean + 1e-9));

        // ── 4. Adaptive noise energy estimate ─────────────────────────────
        // When NOT speech, track the min with slow attack; when speech, hold.
        // This converges to the quiet-room energy so the decision threshold
        // scales with the user's environment.
        bool likely_speech_now = (energy > noise_energy_ * ENERGY_OVER_NOISE);
        if (!likely_speech_now) {
            noise_energy_ = 0.98f * noise_energy_ + 0.02f * energy;
        }

        // ── 5. Voting decision ────────────────────────────────────────────
        // Start with the energy vote; then use ZCR + SFM to discriminate.
        int votes = 0;

        if (energy > noise_energy_ * ENERGY_OVER_NOISE) votes++;
        if (energy > ABS_ENERGY_MIN)                    votes++;
        if (zcr    < ZCR_NOISE_MAX)                     votes++;  // not broadband noise
        if (sfm    < SFM_SPEECH_MAX)                    votes++;  // tonal structure

        // EWMA of votes for stability — don't oscillate on near-threshold frames
        speech_ewma_ = 0.6f * speech_ewma_ + 0.4f * static_cast<float>(votes);

        bool speech = (speech_ewma_ >= VOTES_FOR_SPEECH);

        // ── 6. Hangover ───────────────────────────────────────────────────
        // Once we flip to speech, keep returning true for HANGOVER_FRAMES
        // (≈120 ms) so brief pauses and trailing consonants aren't cut.
        if (speech) {
            hangover_ticks_ = HANGOVER_FRAMES;
            return true;
        }
        if (hangover_ticks_ > 0) {
            hangover_ticks_--;
            return true;
        }
        return false;
    }

private:
    // ── Tuning ───────────────────────────────────────────────────────────
    static constexpr float ENERGY_OVER_NOISE = 3.5f;   // must beat noise floor by 5.4 dB
    static constexpr float ABS_ENERGY_MIN    = 1e-5f;  // absolute silence gate
    static constexpr float ZCR_NOISE_MAX     = 0.35f;  // above → broadband noise
    static constexpr float SFM_SPEECH_MAX    = 0.55f;  // above → noise-like spectrum
    static constexpr float VOTES_FOR_SPEECH  = 2.5f;   // out of 4 features, EWMA
    static constexpr int   HANGOVER_FRAMES   = 6;      // 6 × 20 ms = 120 ms

    // ── State ────────────────────────────────────────────────────────────
    // OFF by default — the legacy RMS gate + Opus DTX already handle silence;
    // turning VAD on can drop quiet speech if thresholds don't match the user's
    // mic/room, so make it opt-in from the Settings UI.
    bool  enabled_        = false;
    float noise_energy_   = 1e-4f;
    int   hangover_ticks_ = 0;
    float speech_ewma_    = 0.0f;
};
