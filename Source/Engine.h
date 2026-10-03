#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <vector>
#include "Params.h"

namespace shz
{
constexpr double kPi = 3.14159265358979323846;

inline float dB2g (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float lerpf (float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float expLerp (float a, float b, float t) noexcept { return a * std::pow (b / a, t); }   // a,b > 0

// ------------------------------------------------------------------------------------------
// RT-safe RBJ biquad (transposed direct form II). No allocation when coefficients change.
struct Bq
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }

    void raw (double B0, double B1, double B2, double A0, double A1, double A2) noexcept
    {
        b0 = (float) (B0 / A0); b1 = (float) (B1 / A0); b2 = (float) (B2 / A0);
        a1 = (float) (A1 / A0); a2 = (float) (A2 / A0);
    }

    static double cf (double f, double sr) noexcept { return juce::jlimit (8.0, sr * 0.45, f); }

    void lowpass (double sr, double f, double q = 0.7071) noexcept
    {
        const double w = 2 * kPi * cf (f, sr) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        raw ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass (double sr, double f, double q = 0.7071) noexcept
    {
        const double w = 2 * kPi * cf (f, sr) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        raw ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void bandpass (double sr, double f, double q = 0.7071) noexcept
    {
        const double w = 2 * kPi * cf (f, sr) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        raw (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }

    void peak (double sr, double f, double q, double dB) noexcept
    {
        const double A = std::pow (10.0, dB / 40.0), w = 2 * kPi * cf (f, sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2 * q);
        raw (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf (double sr, double f, double q, double dB) noexcept
    {
        const double A = std::pow (10.0, dB / 40.0), w = 2 * kPi * cf (f, sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2 * q), be = 2 * std::sqrt (A) * al;
        raw (A * ((A + 1) - (A - 1) * c + be), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - be),
             (A + 1) + (A - 1) * c + be, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - be);
    }
    void highShelf (double sr, double f, double q, double dB) noexcept
    {
        const double A = std::pow (10.0, dB / 40.0), w = 2 * kPi * cf (f, sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2 * q), be = 2 * std::sqrt (A) * al;
        raw (A * ((A + 1) + (A - 1) * c + be), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - be),
             (A + 1) - (A - 1) * c + be, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - be);
    }
};

// One-pole parameter smoother
struct Sm
{
    float v = 0, t = 0, k = 0.002f; bool init = false;
    void prepare (double sr, double ms = 12.0) { k = 1.0f - std::exp (-1.0f / (float) (ms * 0.001 * sr)); }
    void set (float x) { t = x; if (! init) { v = x; init = true; } }
    inline float next() noexcept { v += (t - v) * k; return v; }
};

// ------------------------------------------------------------------------------------------
// Noise gate (peak envelope, hysteresis, smoothed gain)
struct Gate
{
    float env = 0, g = 0, dec = 0.999f, atk = 0.01f, rel = 0.001f; bool open = false;
    void prepare (double sr)
    {
        dec = std::exp (-1.0f / (float) (0.030 * sr));
        atk = 1.0f - std::exp (-1.0f / (float) (0.002 * sr));
        rel = 1.0f - std::exp (-1.0f / (float) (0.060 * sr));
    }
    inline float process (float x, float thr, bool on) noexcept
    {
        if (! on) { g += (1.0f - g) * atk; open = true; return x * g; }
        const float a = std::abs (x);
        env = a > env ? a : env * dec;
        if (! open && env > thr) open = true;
        else if (open && env < thr * 0.5f) open = false;
        g += ((open ? 1.0f : 0.0f) - g) * (open ? atk : rel);
        return x * g;
    }
};

// ------------------------------------------------------------------------------------------
// Two-tap crossfaded delay-line pitch shifter (guitar-friendly, ~45 ms window)
struct PitchShifter
{
    std::vector<float> buf; int mask = 0, w = 0; float W = 1, phase = 0;

    void prepare (double sr, float ms = 45.0f)
    {
        W = (float) (ms * 0.001 * sr);
        int n = 1; while (n < (int) (W * 2 + 16)) n <<= 1;
        buf.assign ((size_t) n, 0.0f); mask = n - 1; w = 0; phase = 0;
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); phase = 0; w = 0; }

    inline float tap (float delay) const noexcept
    {
        const float pos = (float) w - delay;
        const int i = (int) std::floor (pos);
        const float f = pos - (float) i;
        const float xm1 = buf[(size_t) ((i - 1) & mask)], x0 = buf[(size_t) (i & mask)],
                    x1  = buf[(size_t) ((i + 1) & mask)], x2 = buf[(size_t) ((i + 2) & mask)];
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2,
                    c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    // ratio = 2^(semitones/12), wet = 0..1
    inline float process (float x, float ratio, float wet) noexcept
    {
        buf[(size_t) w] = x;
        float p2 = phase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
        const float w1 = 0.5f - 0.5f * std::cos (2.0f * (float) kPi * phase);
        const float wetSig = tap (3.0f + phase * W) * w1 + tap (3.0f + p2 * W) * (1.0f - w1);
        const float drySig = tap (3.0f + W * 0.5f);
        phase += (1.0f - ratio) / W;
        if (phase >= 1.0f) phase -= 1.0f; else if (phase < 0.0f) phase += 1.0f;
        w = (w + 1) & mask;
        return drySig + (wetSig - drySig) * wet;
    }
};

// ------------------------------------------------------------------------------------------
// Lightweight monophonic guitar pitch tracker. It deliberately favors stability over
// ultra-fast response, which keeps the synth playable on real guitar input.
struct PitchTracker
{
    Bq lp;
    double sr = 44100.0;
    float env = 0.0f, freq = 110.0f, period = 0.009f;
    float prev = 0.0f;
    int samplesSinceCross = 0;
    float lastCrossPeriod = 0.0f;
    bool armed = true;

    void prepare (double s)
    {
        sr = s;
        lp.reset();
        lp.lowpass (sr, 1600.0, 0.72);
        reset();
    }

    void reset()
    {
        lp.reset();
        env = 0.0f; freq = 110.0f; period = 0.009f;
        prev = 0.0f; samplesSinceCross = 0; lastCrossPeriod = 0.0f; armed = true;
    }

    inline float process (float x, float tracking) noexcept
    {
        const float y = lp.process (x);
        const float a = std::abs (y);
        env += (a - env) * (a > env ? 0.008f : 0.0012f);
        ++samplesSinceCross;

        // Schmitt-like zero crossing. The threshold scales with the current envelope
        // so quiet notes don't create a cloud of false periods.
        const float th = juce::jmax (0.004f, env * (0.10f + 0.12f * (1.0f - tracking)));
        if (armed && prev <= th && y > th && samplesSinceCross > (int) (sr / 1200.0))
        {
            const float p = (float) samplesSinceCross;
            if (p > sr / 1100.0 && p < sr / 55.0)
            {
                if (lastCrossPeriod <= 0.0f)
                    lastCrossPeriod = p;
                else
                    lastCrossPeriod += (p - lastCrossPeriod) * (0.18f + 0.55f * tracking);

                const float f = (float) sr / juce::jmax (1.0f, lastCrossPeriod);
                // Reject obvious octave jumps unless the new estimate persists.
                if (f > 55.0f && f < 1100.0f)
                    freq += (f - freq) * (0.10f + 0.45f * tracking);
            }
            samplesSinceCross = 0;
            armed = false;
        }
        if (!armed && y < -th)
            armed = true;
        prev = y;
        return freq;
    }

    inline float level() const noexcept { return env; }
};

// ------------------------------------------------------------------------------------------
// Guitar-to-synth voice. This is intentionally an original, compact architecture:
// three waves, sub oscillator, resonant filter, envelope, drive and glide.
struct GuitarSynth
{
    double sr = 44100.0;
    PitchTracker tracker;
    Bq filter, filterHP, filterBP;
    Sm cutoffSm;
    float phase1 = 0.0f, phase2 = 0.0f, phase3 = 0.0f, phaseSub = 0.0f, lfoPhase = 0.0f;
    float env = 0.0f, gate = 0.0f, lastFreq = 110.0f;
    uint32_t noiseState = 0x12345678u;
    bool active = false;

    void prepare (double s)
    {
        sr = s;
        tracker.prepare (s);
        filter.reset(); filterHP.reset(); filterBP.reset();
        cutoffSm.prepare (s, 8.0);
        cutoffSm.set (4200.0f);
        reset();
    }

    void reset()
    {
        tracker.reset();
        filter.reset(); filterHP.reset(); filterBP.reset();
        phase1 = phase2 = phase3 = phaseSub = lfoPhase = 0.0f;
        env = gate = 0.0f; lastFreq = 110.0f; active = false;
        noiseState = 0x12345678u;
    }

    static inline float tri (float p) noexcept
    {
        return 1.0f - 4.0f * std::abs (p - std::floor (p + 0.5f));
    }

    static inline float osc (float p, int wave, float pw = 0.5f) noexcept
    {
        switch (wave)
        {
            case 0: return 2.0f * p - 1.0f; // saw
            case 1: return p < pw ? 1.0f : -1.0f; // variable-width pulse
            case 2: return tri (p);
            case 3: return std::sin (2.0f * (float) kPi * p);
            default:
            {
                const float a = 2.0f * p - 1.0f;
                return 0.68f * a + 0.32f * std::sin (4.0f * (float) kPi * p);
            }
        }
    }

    inline float noise() noexcept
    {
        noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
        return (float) (noiseState * (1.0 / 4294967295.0)) * 2.0f - 1.0f;
    }

    inline float wrap (float p) noexcept
    {
        p -= std::floor (p);
        return p;
    }

    inline float process (float x, const float* P) noexcept
    {
        const float tracking = P[Id::syntrk];
        const float f0 = tracker.process (x, tracking);
        const float level = tracker.level();
        const int voice = juce::jlimit (0, 7, (int) std::lround (P[Id::synvoice]));
        const float attackMs = P[Id::synatt] * (voice == 3 ? 0.55f : 1.0f);
        const float decayMs = P[Id::syndec] * (voice == 3 ? 0.70f : 1.0f);
        const float att = 1.0f - std::exp (-1.0 / (0.001 * juce::jmax (0.5f, attackMs) * sr));
        const float rel = 1.0f - std::exp (-1.0 / (0.001 * P[Id::synrel] * sr));

        const float threshold = 0.008f + 0.040f * (1.0f - tracking);
        const bool wantGate = level > threshold;
        if (wantGate)
        {
            gate += (1.0f - gate) * att;
            active = true;
        }
        else
        {
            gate += (0.0f - gate) * rel;
            if (gate < 0.0005f) active = false;
        }

        // Dedicated glide control plus stability: high stability means less pitch wobble.
        const float glideTime = juce::jmax (0.0f, P[Id::synglide]);
        const float glideK = 1.0f - std::exp (-1.0 / (0.001 * (2.0 + glideTime * (0.35 + 0.65 * (1.0 - P[Id::synstab]))) * sr));
        const float target = juce::jlimit (25.0f, 2200.0f, f0 * std::pow (2.0f, P[Id::synoct]));
        lastFreq += (target - lastFreq) * glideK;
        const float f = juce::jlimit (25.0f, 2200.0f, lastFreq);

        const float detuneRatio = std::pow (2.0f, P[Id::syndetune] / 1200.0f);
        const float spread = P[Id::synspread];
        const float inc1 = f / (float) sr;
        const float inc2 = (f * detuneRatio * (1.0f + 0.006f * spread)) / (float) sr;
        const float inc3 = (f * std::pow (detuneRatio, -0.55f) * (1.0f - 0.004f * spread)) / (float) sr;
        const float incS = (0.5f * f) / (float) sr;

        phase1 = wrap (phase1 + inc1);
        phase2 = wrap (phase2 + inc2);
        phase3 = wrap (phase3 + inc3);
        phaseSub = wrap (phaseSub + incS);
        lfoPhase = wrap (lfoPhase + P[Id::synlforate] / (float) sr);

        if (! active && gate < 0.0005f)
            env = 0.0f;
        else
        {
            const float sustain = P[Id::synsus];
            if (gate > env)
                env += (gate - env) * att;
            else
                env += (sustain * gate - env) * (1.0f - std::exp (-1.0 / (0.001 * juce::jmax (5.0f, decayMs) * sr)));
        }

        const float lfo = std::sin (2.0f * (float) kPi * lfoPhase);
        const float mod = lfo * P[Id::synlfodepth];
        const float pw = juce::jlimit (0.05f, 0.95f, P[Id::synpw] + 0.22f * mod);
        const int wave = juce::jlimit (0, 4, (int) std::lround (P[Id::synwave]));
        const float o1 = osc (phase1, wave, pw);
        const float o2Wave = voice == 4 ? 3.0f : (voice == 5 ? 3.0f : (float) (wave == 1 ? 0 : wave));
        const float o2 = osc (phase2, (int) o2Wave, 0.5f);
        const float tri3 = osc (phase3, 2, 0.5f);
        const float sub = osc (phaseSub, 0, 0.5f);

        float y = 0.0f;
        switch (voice)
        {
            case 0: // Glass: clean, harmonic and articulate
                y = 0.48f * o1 + 0.30f * o2 + 0.16f * tri3;
                break;
            case 1: // Classic: dual saw/pulse
                y = 0.52f * o1 + 0.34f * o2 + 0.10f * sub;
                break;
            case 2: // Modern: tighter dual oscillator with sub
                y = 0.48f * o1 + 0.36f * o2 + 0.16f * sub;
                break;
            case 3: // Pluck: bright transient, shorter body
                y = 0.55f * o1 + 0.25f * tri3 + 0.15f * o2;
                break;
            case 4: // Glass/air
                y = 0.62f * std::sin (2.0f * (float) kPi * phase1) + 0.24f * tri3 + 0.08f * o2;
                break;
            case 5: // FM metal
            {
                const float fmIndex = 0.5f + 5.0f * P[Id::synmod];
                y = 0.78f * std::sin (2.0f * (float) kPi * phase1 + fmIndex * std::sin (2.0f * (float) kPi * phase2));
                y += 0.15f * sub;
                break;
            }
            case 6: // Pulse
                y = 0.70f * osc (phase1, 1, pw) + 0.20f * osc (phase2, 1, juce::jlimit (0.05f, 0.95f, 1.0f - pw));
                break;
            default: // Bass
                y = 0.48f * o1 + 0.22f * osc (phase2, 1, 0.52f) + 0.30f * sub;
                break;
        }

        y += P[Id::synnoise] * noise() * (0.10f + 0.30f * env);
        y *= 0.82f + 0.18f * P[Id::synosc2];

        // Envelope amount is bipolar and is deliberately capped to keep the filter musical.
        const float baseCut = P[Id::syncut];
        const float envOct = juce::jlimit (-1.0f, 1.0f, P[Id::synenvamt]);
        const float envCut = baseCut * std::pow (2.0f, envOct * (0.35f + 1.65f * env));
        const float modCut = envCut * std::pow (2.0f, mod * 0.85f);
        const float cutoff = juce::jlimit (70.0f, 18000.0f, modCut);
        cutoffSm.set (cutoff);
        const float fc = cutoffSm.next();
        const float q = 0.55f + 7.5f * P[Id::synres];
        const int ft = juce::jlimit (0, 2, (int) std::lround (P[Id::synfilter]));
        if (ft == 0)
        {
            filter.lowpass (sr, fc, q); y = filter.process (y);
        }
        else if (ft == 1)
        {
            filterHP.highpass (sr, fc, q); y = filterHP.process (y);
        }
        else
        {
            filterBP.bandpass (sr, fc, q); y = filterBP.process (y) * (1.0f + 0.25f * P[Id::synres]);
        }

        const float drive = 1.0f + P[Id::syndrive] * (voice == 5 ? 0.70f : 0.55f);
        y = std::tanh (y * drive) / std::tanh (drive);
        return y * env * P[Id::synmix] * 0.95f;
    }
};

// ------------------------------------------------------------------------------------------
// Sympathetic string (damped feedback comb, fractional delay)
struct SympString
{
    std::vector<float> buf; int mask = 0, w = 0; float delay = 100, lp = 0;

    void prepare (double sr, double freq)
    {
        delay = (float) (sr / freq) - 1.0f;
        int n = 1; while (n < (int) delay + 8) n <<= 1;
        buf.assign ((size_t) n, 0.0f); mask = n - 1; w = 0; lp = 0;
    }
    inline float process (float x, float fb, float damp) noexcept
    {
        const float pos = (float) w - delay;
        const int i = (int) std::floor (pos);
        const float f = pos - (float) i;
        const float y = buf[(size_t) (i & mask)] * (1.0f - f) + buf[(size_t) ((i + 1) & mask)] * f;
        lp += damp * (y - lp);
        buf[(size_t) w] = x * (1.0f - fb) + fb * lp;
        w = (w + 1) & mask;
        return y;
    }
};

struct SitarPedal
{
    static constexpr int N = 12;
    // A wider sympathetic bank: tonic, fifth, octave and color tones.
    static constexpr double freqs[N] =
        { 98.00, 130.81, 146.83, 164.81, 196.00, 220.00,
          261.63, 293.66, 329.63, 392.00, 440.00, 523.25 };

    std::array<SympString, N> strings;
    Bq hp, jawari, body, air;
    double sr = 44100;

    void prepare (double s)
    {
        sr = s;
        for (int i = 0; i < N; ++i)
            strings[(size_t) i].prepare (s, freqs[i]);
        hp.reset(); jawari.reset(); body.reset(); air.reset();
    }

    void update (float buzzK, float toneKnob)
    {
        const float buzz = buzzK * 0.1f;
        hp.highpass (sr, 85.0);
        // A resonant mid band supplies the woody bridge/body character.
        body.peak (sr, 850.0, 0.9, 1.5 + 3.5 * buzz);
        jawari.peak (sr, 2400.0, 0.75, 2.0 + 8.0 * buzz);
        air.highShelf (sr, 5200.0, 0.7, -1.5 + 7.0 * toneKnob * 0.1f);
    }

    inline float process (float x, float buzz, float res, float mix) noexcept
    {
        // Nonlinear jawari: asymmetric fold + short resonant edge, rather than
        // simply replacing the guitar waveform with a sine.
        const float b = buzz * 0.1f;
        float y = hp.process (x);
        const float folded = std::tanh ((1.0f + 3.8f * b) * y + 0.22f * y * y)
                           - 0.18f * std::tanh (2.0f * y);
        float jaw = jawari.process (folded);
        jaw = body.process (jaw);
        jaw = air.process (jaw);

        // Sympathetic strings are deliberately quieter individually so the bank
        // reads as resonance instead of an obvious fixed drone.
        const float fb = 0.86f + 0.118f * (res * 0.1f);
        float sym = 0.0f;
        for (auto& st : strings)
            sym += st.process (x, fb, 0.48f + 0.10f * (1.0f - res * 0.1f));
        sym *= 0.075f + 0.16f * res * 0.1f;

        const float jawMix = 0.18f + 0.72f * b;
        const float wet = mix * 0.1f;
        const float main = x * (1.0f - jawMix) + jaw * jawMix;
        return x + (main + sym - x) * wet;
    }
};

// ------------------------------------------------------------------------------------------
struct Boost
{
    Bq hp, lowCut, tilt; double sr = 44100;
    void prepare (double s) { sr = s; hp.reset(); lowCut.reset(); tilt.reset(); }
    void update (float tone)
    {
        hp.highpass (sr, 65.0);
        lowCut.highpass (sr, 105.0);
        tilt.highShelf (sr, 1800.0, 0.7, (tone - 5.0f) * 1.6f);
    }
    inline float process (float x, float gDb, float lvlLin) noexcept
    {
        float y = lowCut.process (hp.process (x));
        y *= dB2g (gDb);
        // Asymmetric clipping creates a tighter pick transient than a plain tanh.
        const float pos = std::tanh (1.10f * y);
        const float neg = std::tanh (0.86f * y);
        y = y >= 0.0f ? pos : neg;
        return tilt.process (y) * lvlLin;
    }
};

// Overdrive pedal, runs inside the oversampled block
struct DrivePedal
{
    Bq hp, lp, bite, body; double sr = 44100;
    void prepare (double s) { sr = s; hp.reset(); lp.reset(); bite.reset(); body.reset(); }
    void update (float tight, float tone)
    {
        hp.highpass (sr, expLerp (55.0f, 900.0f, tight * 0.1f));
        lp.lowpass (sr, expLerp (2600.0f, 10500.0f, tone * 0.1f));
        bite.peak (sr, 1850.0, 1.1, -2.0 + tone * 0.75f);
        body.peak (sr, 420.0, 0.9, -2.0 + (10.0f - tight) * 0.32f);
    }
    inline float process (float x, float gain, float lvl) noexcept
    {
        float y = hp.process (x) * gain;
        // Two-stage saturation with a small asymmetry keeps palm-mutes dense
        // while preserving pick attack.
        y = std::tanh (0.72f * y + 0.055f * y * y);
        y = std::tanh (1.18f * y) / std::tanh (1.18f);
        y = body.process (bite.process (lp.process (y)));
        return y * lvl;
    }
};

// ------------------------------------------------------------------------------------------
// Amp: 5 voicings (Clean, Crunch, Modern, Lead, Sitar), runs inside the oversampled block
struct Voice
{
    int n; float g0b, g0s, g1b, g1s, g2b, g2s; float bias, hard; float tMin, tMax;
    float lp0, lp1, lp2; float midF, midQ; float pdrive; float presF, lowF;
};
inline const Voice kVoices[5] =
{
    // n   g0         g1          g2         bias  hard  tMin tMax   interstage LPFs        mid        pwr  pres  low
    { 1,  4.8f, 1.9f,   0.0f, 0.0f,   0, 0,       0.00f, 0.00f,  30, 150,  10500, 10000, 9500,  520, 0.75f, 1.05f, 4700, 105 }, // Clean: wide and touch-sensitive
    { 2,  7.5f, 2.5f,   3.0f, 1.0f,   0, 0,       0.10f, 0.08f,  35, 230,   8200,  7200, 6800,  650, 0.90f, 1.55f, 4300, 100 }, // Crunch: flexible edge-of-breakup
    { 3,  9.5f, 2.8f,   4.0f, 1.2f,   0, 0.5f,    0.12f, 0.35f,  55, 460,   6900,  5700, 5000,  760, 0.82f, 2.35f, 3900,  88 }, // Modern: tight high-gain
    { 3,  8.8f, 2.5f,   5.0f, 1.25f,  2, 0.5f,    0.18f, 0.16f,  45, 300,   7600,  6100, 5400,  980, 0.72f, 2.00f, 3700,  92 }, // Lead: saturated singing mids
    { 2,  6.6f, 2.1f,   2.4f, 0.9f,   0, 0,       0.22f, 0.02f,  35, 180,  11500,  9800, 9300, 1350, 0.68f, 1.35f, 5100, 105 }, // Sitar: bright resonant front-end
};

struct Amp
{
    double sr = 44100; int mode = 2; int n = 3; float bias = 0, hard = 0, pdrive = 1.5f, pdNorm = 1;
    Bq tightHp, coup[3], lp[3], bassF, midF, trebF, presF, depthF;
    Sm g[3], master;

    void prepare (double osSr)
    {
        sr = osSr;
        for (auto& s : g) s.prepare (sr, 15.0);
        master.prepare (sr, 15.0);
        tightHp.reset(); bassF.reset(); midF.reset(); trebF.reset(); presF.reset(); depthF.reset();
        for (auto& b : coup) b.reset();
        for (auto& b : lp) b.reset();
    }

    void update (int m, float gainK, float tightK, float bassK, float midK, float trebK, float presK, float depthK, float masterK)
    {
        mode = juce::jlimit (0, 4, m);
        const Voice& v = kVoices[mode];
        n = v.n; bias = v.bias; hard = v.hard; pdrive = v.pdrive; pdNorm = 1.0f / std::tanh (pdrive);

        g[0].set (dB2g (v.g0b + v.g0s * gainK));
        g[1].set (dB2g (v.g1b + v.g1s * gainK));
        g[2].set (dB2g (v.g2b + v.g2s * gainK));

        const double tHz = expLerp (v.tMin, v.tMax, tightK * 0.1f);
        tightHp.highpass (sr, tHz);
        coup[0].highpass (sr, 25.0);
        coup[1].highpass (sr, juce::jmax (30.0, tHz * 0.5));
        coup[2].highpass (sr, juce::jmax (30.0, tHz * 0.5));
        lp[0].lowpass (sr, v.lp0); lp[1].lowpass (sr, v.lp1); lp[2].lowpass (sr, v.lp2);

        const double midShift = (midK - 5.0f) * (mode == 0 ? 55.0 : (mode == 3 ? 95.0 : 75.0));
        const double midFreq = juce::jlimit (250.0, 2500.0, v.midF + midShift);
        bassF.lowShelf  (sr, mode == 0 ? 125.0 : 105.0, 0.7, (bassK - 5.0f) * (mode == 2 ? 2.6 : 2.2));
        midF.peak       (sr, midFreq, v.midQ, (midK - 5.0f) * (mode == 3 ? 2.4 : 2.0));
        trebF.highShelf (sr, mode == 0 ? 3600.0 : 3000.0, 0.7, (trebK - 5.0f) * (mode == 2 ? 2.3 : 2.0));
        presF.highShelf (sr, v.presF, 0.7, (presK - 5.0f) * 1.8f + (mode == 3 ? 1.5f : 0.0f));
        depthF.lowShelf (sr, v.lowF, 0.8, (depthK - 5.0f) * 1.4f + (mode == 2 ? -0.5f : 0.0f));

        master.set (std::pow (masterK * 0.1f, 1.5f) * 1.0f);
    }

    inline float shape (float x) const noexcept
    {
        const float t = std::tanh (x + bias) - std::tanh (bias);
        return t + hard * (juce::jlimit (-1.0f, 1.0f, x) - t);
    }

    inline float process (float x) noexcept
    {
        x = tightHp.process (x);
        for (int s = 0; s < n; ++s)
        {
            x *= g[s].next();
            x = shape (x);
            x = coup[s].process (x);
            x = lp[s].process (x);
        }
        for (int s = n; s < 3; ++s) g[s].next();   // keep unused smoothers moving

        x = bassF.process (x);
        x = midF.process (x);
        x = trebF.process (x);
        x = std::tanh (x * pdrive) * pdNorm;      // power stage
        x = presF.process (x);
        x = depthF.process (x);
        return x * master.next();
    }
};

// ------------------------------------------------------------------------------------------
// Cabinet + microphone model (IIR). Replaced by the convolution engine when a WAV IR is loaded.
struct CabModel
{
    Bq hp, ls, pk1, pk2, nt, lp1, lp2, mic1, mic2, posHs, angHs, distLs, distHs, cone, box;
    double sr = 44100;

    void prepare (double s) { sr = s; for (Bq* b : { &hp, &ls, &pk1, &pk2, &nt, &lp1, &lp2, &mic1, &mic2, &posHs, &angHs, &distLs, &distHs, &cone, &box }) b->reset(); }

    void update (int cab, int mic, float dist, float pos, float angle)
    {
        double hpF = 85, lsF = 120, lsG = 1.5, p1F = 2400, p1G = 4.5, p1Q = 1.1, p2F = 800, p2G = -1.5, ntF = 3800, ntG = 0, lpF = 5200;
        switch (juce::jlimit (0, 5, cab))
        {
            case 0: // 1x12 open-back: woody, open upper mids
                break;
            case 1: // 2x12 open-back: wider low mids
                hpF = 75; lsF = 110; lsG = 3.0; p1F = 2000; p1G = 3.5; p2F = 700; p2G = -2.0; lpF = 5400;
                break;
            case 2: // 4x12 modern: tight V30-style contour
                hpF = 68; lsF = 100; lsG = 4.5; p1F = 1800; p1G = 3.0; p2F = 500; p2G = -2.0; ntG = -2.5; lpF = 5000;
                break;
            case 3: // 4x12 dark: lower resonant peak, smoother top
                hpF = 64; lsF = 95; lsG = 5.2; p1F = 1450; p1G = 2.2; p2F = 430; p2G = -1.0; ntF = 3300; ntG = -3.5; lpF = 4300;
                break;
            case 4: // 2x12 bright: articulate upper mids, slightly leaner lows
                hpF = 82; lsF = 125; lsG = 1.0; p1F = 2750; p1G = 5.0; p2F = 900; p2G = -2.5; ntF = 4200; ntG = 1.0; lpF = 6200;
                break;
            default: // 4x10: fast, dry and mid-forward
                hpF = 92; lsF = 145; lsG = -1.0; p1F = 1900; p1G = 4.2; p2F = 620; p2G = -1.0; ntF = 3500; ntG = 0.5; lpF = 5700;
                break;
        }

        double micLpScale = 1.0;
        switch (mic)
        {
            case 0: mic1.peak (sr, 5500.0, 1.4, 3.5);  mic2.peak (sr, 120.0, 0.7, 0.0); break;          // dynamic
            case 1: mic1.highShelf (sr, 7000.0, 0.7, 4.0); mic2.highpass (sr, 40.0); micLpScale = 1.35; break; // condenser
            default: mic1.highShelf (sr, 3500.0, 0.7, -4.5); mic2.lowShelf (sr, 200.0, 0.7, 3.0); break; // ribbon
        }

        const double d = dist, p = pos, a = angle;
        hp.highpass (sr, hpF, 0.8);
        ls.lowShelf (sr, lsF, 0.7, lsG);
        pk1.peak (sr, p1F, p1Q, p1G);
        pk2.peak (sr, p2F, 1.0, p2G);
        nt.peak (sr, ntF, 3.0, ntG);
        lp1.lowpass (sr, lpF * micLpScale, 0.5412);
        lp2.lowpass (sr, lpF * micLpScale, 1.3065);
        posHs.highShelf (sr, 3200.0, 0.7, 2.0 - 0.8 * p);
        angHs.highShelf (sr, 5000.0, 0.7, -0.6 * a);
        distLs.lowShelf (sr, 180.0, 0.7, 4.0 - 0.6 * d);
        distHs.highShelf (sr, 6000.0, 0.7, -0.35 * d);
        cone.peak (sr, 1450.0 + 180.0 * p, 1.4, 1.0 + 1.6 * (1.0 - p));
        box.peak (sr, 310.0, 1.1, -1.5 + 2.5 * (1.0 - d * 0.1));
    }

    inline float process (float x) noexcept
    {
        x = hp.process (x);   x = ls.process (x);  x = pk1.process (x); x = pk2.process (x); x = nt.process (x);
        x = lp1.process (x);  x = lp2.process (x);
        x = mic1.process (x); x = mic2.process (x);
        x = posHs.process (x); x = angHs.process (x); x = distLs.process (x); x = distHs.process (x);
        x = cone.process (x); x = box.process (x);
        return x;
    }
};

// ------------------------------------------------------------------------------------------
struct EQ
{
    Bq hp, lp, b[5]; double sr = 44100;
    void prepare (double s) { sr = s; hp.reset(); lp.reset(); for (auto& x : b) x.reset(); }
    void update (const float* P)
    {
        hp.highpass (sr, P[Id::hpf]);
        lp.lowpass  (sr, P[Id::lpf]);
        b[0].lowShelf  (sr, P[Id::f1], 0.7, P[Id::e1]);
        b[1].peak      (sr, P[Id::f2], 1.1, P[Id::e2]);
        b[2].peak      (sr, P[Id::f3], 1.1, P[Id::e3]);
        b[3].peak      (sr, P[Id::f4], 1.1, P[Id::e4]);
        b[4].highShelf (sr, P[Id::f5], 0.7, P[Id::e5]);
    }
    inline float process (float x) noexcept
    {
        x = hp.process (x);
        for (auto& f : b) x = f.process (x);
        return lp.process (x);
    }
};

// ------------------------------------------------------------------------------------------
class Engine
{
public:
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<bool> irLoaded { false };

    int prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate; maxN = juce::jmax (32, maxBlock);
        osSr = sr * 4.0;

        mono.setSize (1, maxN, false, true, false);
        cabBuf.setSize (1, maxN, false, true, false);
        mono.clear(); cabBuf.clear();

        os.initProcessing ((size_t) maxN);
        os.reset();

        inGain.prepare (sr); outGain.prepare (sr); cabMix.prepare (sr, 18.0);
        gate.prepare (sr);
        trans.prepare (sr); drop.prepare (sr);
        sitar.prepare (sr);
        synth.prepare (sr);
        boost.prepare (sr);
        drive.prepare (osSr);
        amp.prepare (osSr);
        driveGain.prepare (osSr, 15.0); driveLvl.prepare (osSr, 15.0);
        cabModel.prepare (sr);
        eq.prepare (sr);

        juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxN, 1 };
        comp.prepare (spec);
        comp.reset();
        conv.prepare (spec);
        conv.reset();

        reverb.setSampleRate (sr);
        reverb.reset();

        inGain.init = outGain.init = cabMix.init = false;
        return (int) std::lround (os.getLatencyInSamples());
    }

    void reset()
    {
        os.reset(); comp.reset(); conv.reset(); reverb.reset();
        trans.reset(); drop.reset(); synth.reset();
    }

    void loadIR (const juce::File& f)
    {
        conv.loadImpulseResponse (f, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::yes, 0,
                                  juce::dsp::Convolution::Normalise::yes);
        irLoaded.store (true);
    }
    void clearIR() { irLoaded.store (false); }

    void process (float* L, float* R, int total, int numIn, const float* P)
    {
        for (int off = 0; off < total; off += maxN)
        {
            const int n = juce::jmin (maxN, total - off);
            processChunk (L + off, R + off, n, numIn, P);
        }
    }

private:
    void processChunk (float* L, float* R, int n, int numIn, const float* P)
    {
        float* m = mono.getWritePointer (0);

        // ---- block-rate parameter setup ----
        inGain.set (dB2g (P[Id::ingain]));
        outGain.set (dB2g (P[Id::out]));

        const float gateThr = dB2g (P[Id::gate]);
        const bool  gateOn  = P[Id::gateon] > 0.5f;

        const bool  transOn = std::abs (P[Id::transpose]) > 0.01f;
        const float transRatio = std::pow (2.0f, std::round (P[Id::transpose]) / 12.0f);

        const bool  dropOn  = P[Id::tunon] > 0.5f;
        const float dropRatio = std::pow (2.0f, std::round (P[Id::t_pitch]) / 12.0f);
        const float dropWet = P[Id::t_blend] * 0.1f;

        const bool  sitarOn = P[Id::stron] > 0.5f;
        sitar.update (P[Id::s_buzz], P[Id::s_tone]);
        const float sBuzz = P[Id::s_buzz], sRes = P[Id::s_res], sMix = P[Id::s_mix];

        const bool  compOn = P[Id::cmpon] > 0.5f;
        {
            const float sus = P[Id::c_sus] * 0.1f, att = P[Id::c_att] * 0.1f;
            comp.setThreshold (lerpf (-8.0f, -38.0f, sus));
            comp.setRatio (lerpf (3.0f, 10.0f, sus));
            comp.setAttack (lerpf (1.0f, 40.0f, att));
            comp.setRelease (180.0f);
            compMakeup = dB2g (lerpf (1.0f, 12.0f, sus) + (P[Id::c_level] - 5.0f) * 2.4f);
        }

        const bool  boostOn = P[Id::bston] > 0.5f;
        boost.update (P[Id::b_tone]);
        const float bGainDb = P[Id::b_gain] * 2.4f;
        const float bLvl = dB2g ((P[Id::b_level] - 5.0f) * 2.4f);

        const bool  drvOn = P[Id::drvon] > 0.5f;
        drive.update (P[Id::d_tight], P[Id::d_tone]);
        driveGain.set (dB2g (P[Id::d_drive] * 3.8f));
        driveLvl.set (dB2g ((P[Id::d_level] - 5.0f) * 2.4f));

        amp.update ((int) std::lround (P[Id::mode]), P[Id::gain], P[Id::tight], P[Id::bass], P[Id::mid],
                    P[Id::treble], P[Id::pres], P[Id::depth], P[Id::master]);

        cabModel.update ((int) std::lround (P[Id::cab]), (int) std::lround (P[Id::mic]),
                         P[Id::dist], P[Id::pos], P[Id::angle]);
        eq.update (P);

        // ---- 1x-rate front end ----
        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            float x = numIn > 1 ? 0.5f * (L[i] + R[i]) : (numIn == 1 ? L[i] : 0.0f);
            pk = juce::jmax (pk, std::abs (x));
            x *= inGain.next();
            x = gate.process (x, gateThr, gateOn);
            if (transOn) x = trans.process (x, transRatio, 1.0f);
            if (dropOn)  x = drop.process (x, dropRatio, dropWet);
            if (sitarOn) x = sitar.process (x, sBuzz, sRes, sMix);
            if (P[Id::synon] > 0.5f)
                x += synth.process (x, P);
            if (compOn)  x = comp.processSample (0, x) * compMakeup;
            if (boostOn) x = boost.process (x, bGainDb, bLvl);
            m[i] = x;
        }
        inPeak.store (juce::jmax (pk, inPeak.load() * 0.85f));

        // ---- 4x oversampled: drive pedal + amp ----
        {
            juce::dsp::AudioBlock<float> blk (mono.getArrayOfWritePointers(), 1, (size_t) n);
            auto up = os.processSamplesUp (blk);
            float* u = up.getChannelPointer (0);
            const int un = (int) up.getNumSamples();
            for (int i = 0; i < un; ++i)
            {
                float x = u[i];
                const float dg = driveGain.next(), dl = driveLvl.next();
                if (drvOn) x = drive.process (x, dg, dl);
                u[i] = amp.process (x);
            }
            os.processSamplesDown (blk);
        }

        // ---- cabinet: built-in IIR model or user IR ----
        float* c = cabBuf.getWritePointer (0);
        if (irLoaded.load())
        {
            std::copy (m, m + n, c);
            juce::dsp::AudioBlock<float> cb (cabBuf.getArrayOfWritePointers(), 1, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> ctx (cb);
            conv.process (ctx);
        }
        else
        {
            for (int i = 0; i < n; ++i) c[i] = cabModel.process (m[i]);
        }

        cabMix.set (P[Id::cabmix] * 0.01f);
        for (int i = 0; i < n; ++i)
        {
            const float mix = cabMix.next();
            float y = m[i] + (c[i] - m[i]) * mix;
            y = eq.process (y);
            L[i] = R[i] = y;
        }

        // ---- room ----
        {
            juce::Reverb::Parameters rp;
            rp.roomSize = 0.30f + 0.05f * P[Id::room];
            rp.damping = 0.55f;
            rp.wetLevel = juce::jmin (0.5f, 0.025f * P[Id::room] + 0.012f * P[Id::dist]);
            rp.dryLevel = 0.5f;   // juce::Reverb doubles this -> unity dry
            rp.width = 1.0f;
            rp.freezeMode = 0.0f;
            reverb.setParameters (rp);
            reverb.processStereo (L, R, n);
        }

        // ---- output ----
        float op = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float og = outGain.next();
            float l = L[i] * og, r = R[i] * og;
            if (! std::isfinite (l)) l = 0.0f;
            if (! std::isfinite (r)) r = 0.0f;
            l = juce::jlimit (-8.0f, 8.0f, l); r = juce::jlimit (-8.0f, 8.0f, r);
            L[i] = l; R[i] = r;
            op = juce::jmax (op, std::abs (l), std::abs (r));
        }
        outPeak.store (juce::jmax (op, outPeak.load() * 0.85f));
    }

    double sr = 44100, osSr = 176400; int maxN = 512;
    juce::AudioBuffer<float> mono, cabBuf;
    juce::dsp::Oversampling<float> os { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };

    Sm inGain, outGain, cabMix, driveGain, driveLvl;
    Gate gate;
    PitchShifter trans, drop;
    SitarPedal sitar;
    GuitarSynth synth;
    juce::dsp::Compressor<float> comp; float compMakeup = 1.0f;
    Boost boost;
    DrivePedal drive;
    Amp amp;
    CabModel cabModel;
    juce::dsp::Convolution conv;
    EQ eq;
    juce::Reverb reverb;
};

} // namespace shz
