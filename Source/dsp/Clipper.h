// Stereo clipper: input gains, a gain of (Ceiling - Threshold) below the Threshold, a soft or hard
// clip at the Ceiling, optional oversampling around the clip, and a final clip at the Ceiling
// at the plug-in's own sample rate. Dual mono: each channel is clipped on its own. Parameter
// changes are smoothed (10 ms one-pole). See docs/dsp.md.
#pragma once

#include "ClipCurve.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace clipper
{
struct Params
{
    float inputGainDbL = 0.0f, inputGainDbR = 0.0f;   // -30..0 dB (mono uses L)
    float thresholdDb = 0.0f;                         // -30..0 dB
    float ceilingDb = 0.0f;                           // -30..0 dB
    float softnessPercent = 0.0f;                     // 0..100 %, 0 = hard clip
    int oversamplingLog2 = 0;                         // 0 = off, 1 = 2x, 2 = 4x, 3 = 8x
};

/** Block maxima published by every process() call. */
struct Meters
{
    float inputPeak[2] { 0.0f, 0.0f };    // after the input gains, linear
    float outputPeak[2] { 0.0f, 0.0f };   // final output, linear
    float clipDb = 0.0f;                  // deepest peak reduction the curve applied, >= 0
    float safetyClipDb = 0.0f;            // deepest trim of the final base-rate clip, >= 0 (oversampling overshoot)
};

class Clipper
{
public:
    static constexpr int kMaxOversamplingLog2 = 3;

    /** Allocates everything; call before process() and whenever the sample rate or block size changes. */
    void prepare (double sampleRate, int maxBlockSize);
    /** Clears all state; the next setParams() takes its values at once, with no smoothing. */
    void reset();

    void setParams (const Params&) noexcept;

    /** In place; numChannels is 1 or 2. Realtime-safe: no allocations or locks. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Latency of the oversampling stage that is in use now, in samples; safe to read from any thread. */
    int latencySamples() const noexcept { return latency.load(); }
    /** Latency of a factor (0..3), from the oversampler itself. */
    int latencyForFactor (int log2) const noexcept { return latencies[(size_t) log2]; }
    int activeOversamplingLog2() const noexcept { return active; }
    double sampleRate() const noexcept { return fs; }
    const Meters& meters() const noexcept { return meterValues; }

    /** The oversampler's filter, the same in every use (see Clipper.cpp). */
    static std::unique_ptr<juce::dsp::Oversampling<float>> makeOversampler (int log2);

private:
    /** One-pole smoother that lands exactly on its target. */
    struct Smoother
    {
        float value = 0.0f, target = 0.0f;
        void snap (float v) noexcept { value = target = v; }
        void step (float coefficient) noexcept
        {
            value += coefficient * (target - value);
            if (std::abs (target - value) <= 1.0e-6f * std::max (1.0f, std::abs (target)))
                value = target;
        }
    };

    void processChunk (float* const* channels, int numChannels, int numSamples) noexcept;
    float nextFadeGain() noexcept;
    void shapeBlock (float* const* data, int numChannels, int numSamples, int factor) noexcept;

    double fs = 48000.0;
    int maxBlock = 0;
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kMaxOversamplingLog2> oversamplers;   // factors 2, 4, 8
    std::array<int, kMaxOversamplingLog2 + 1> latencies { { 0, 0, 0, 0 } };
    std::atomic<int> latency { 0 };

    std::vector<float> work[2], kneeBuffer, ceilingBuffer;
    Smoother inputL, inputR, thresholdScale, knee, ceiling;
    float smoothing = 0.01f;
    // A change of oversampling factor: fade out, swap at silence, wait out the new filter's delay, fade in.
    enum class Fade { none, out, hold, in };
    static constexpr int kFadeSamples = 256;
    Fade fade = Fade::none;
    float fadeGain = 1.0f;
    int holdLeft = 0;
    bool snapNext = true;
    int active = 0, requested = 0;
    Params params;

    Meters meterValues;
    float chunkClipRatio = 1.0f;
};
} // namespace clipper
