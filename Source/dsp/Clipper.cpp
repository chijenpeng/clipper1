#include "Clipper.h"

namespace clipper
{
namespace
{
    constexpr double kSmoothingSeconds = 0.010;   // one-pole time constant of every parameter

    inline float sanitise (float x) noexcept { return (x - x) == 0.0f ? x : 0.0f; }   // NaN and inf become 0
}

std::unique_ptr<juce::dsp::Oversampling<float>> Clipper::makeOversampler (int log2)
{
    // Linear-phase equiripple half-band FIR at the higher quality setting, with the latency
    // rounded to whole samples. Linear phase keeps the clipped waveform's shape; the cost is
    // latency, which a mastering clipper can spend. The IIR option would have almost none but
    // rotates the phase near Nyquist.
    return std::make_unique<juce::dsp::Oversampling<float>> (
        2, (size_t) log2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
}

void Clipper::prepare (double sampleRate, int maxBlockSize)
{
    fs = sampleRate;
    maxBlock = std::max (1, maxBlockSize);
    smoothing = (float) (1.0 - std::exp (-1.0 / (kSmoothingSeconds * fs)));

    for (auto& v : work)
        v.assign ((size_t) maxBlock, 0.0f);
    kneeBuffer.assign ((size_t) maxBlock, 0.0f);
    ceilingBuffer.assign ((size_t) maxBlock, 0.0f);

    latencies[0] = 0;
    for (int log2 = 1; log2 <= kMaxOversamplingLog2; ++log2)
    {
        auto& o = oversamplers[(size_t) (log2 - 1)];
        o = makeOversampler (log2);
        o->initProcessing ((size_t) maxBlock);
        latencies[(size_t) log2] = (int) std::lround (o->getLatencyInSamples());
    }
    reset();
}

void Clipper::reset()
{
    for (auto& o : oversamplers)
        if (o != nullptr)
            o->reset();
    snapNext = true;
    fade = Fade::none;
    fadeGain = 1.0f;
    active = requested = 0;
    latency.store (0);
    meterValues = {};
}

void Clipper::setParams (const Params& p) noexcept
{
    params = p;
    requested = std::clamp (p.oversamplingLog2, 0, kMaxOversamplingLog2);

    const float tScale = dbToGain (-p.thresholdDb);
    inputL.target = dbToGain (p.inputGainDbL);
    inputR.target = dbToGain (p.inputGainDbR);
    thresholdScale.target = tScale;
    knee.target = softnessToKnee (p.softnessPercent);
    ceiling.target = dbToGain (p.ceilingDb);

    if (snapNext)
    {
        for (auto* s : { &inputL, &inputR, &thresholdScale, &knee, &ceiling })
            s->snap (s->target);
        active = requested;
        latency.store (latencies[(size_t) active]);
        snapNext = false;
    }
}

void Clipper::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    meterValues = {};
    numChannels = std::clamp (numChannels, 1, 2);

    for (int done = 0; done < numSamples; done += maxBlock)
    {
        const int n = std::min (maxBlock, numSamples - done);
        float* ptrs[2] = { channels[0] + done, numChannels > 1 ? channels[1] + done : nullptr };

        if (fade == Fade::none && requested != active)
            fade = Fade::out;

        processChunk (ptrs, numChannels, n);

        if (fade == Fade::out && fadeGain == 0.0f)
        {
            // Silent: swap to the requested factor and let the new filter's delay pass before fading in.
            active = requested;
            if (active > 0)
                oversamplers[(size_t) (active - 1)]->reset();
            latency.store (latencies[(size_t) active]);
            holdLeft = latencies[(size_t) active];
            fade = Fade::hold;
        }
    }
}

float Clipper::nextFadeGain() noexcept
{
    constexpr float step = 1.0f / (float) kFadeSamples;
    switch (fade)
    {
        case Fade::none: break;
        case Fade::out:
            fadeGain = std::max (0.0f, fadeGain - step);
            break;
        case Fade::hold:
            if (holdLeft > 0)
                --holdLeft;
            else
                fade = Fade::in;
            break;
        case Fade::in:
            fadeGain = std::min (1.0f, fadeGain + step);
            if (fadeGain >= 1.0f)
                fade = Fade::none;
            break;
    }
    return fadeGain;
}

void Clipper::shapeBlock (float* const* data, int numChannels, int numSamples, int factor) noexcept
{
    // Level relative to the Threshold in, level relative to the Ceiling out. The clip meter
    // is the largest ratio between the two in this block, taken at the rate the clip runs at.
    float worst = 1.0f;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* x = data[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            const float w = kneeBuffer[(size_t) (i / factor)];
            const float u = x[i];
            const float y = shape (u, w);
            const float a = std::abs (u);
            if (a > 1.0e-6f && std::abs (y) > 0.0f)
                worst = std::max (worst, a / std::abs (y));
            x[i] = y;
        }
    }
    chunkClipRatio = std::max (chunkClipRatio, worst);
}

void Clipper::processChunk (float* const* channels, int numChannels, int n) noexcept
{
    float* in[2] = { work[0].data(), work[1].data() };
    float peakIn[2] = { 0.0f, 0.0f };

    // 1. Smoothed gains: input gain, then 1/Threshold, so the clip sees u (1 = Threshold).
    for (int i = 0; i < n; ++i)
    {
        inputL.step (smoothing);
        inputR.step (smoothing);
        thresholdScale.step (smoothing);
        knee.step (smoothing);
        ceiling.step (smoothing);
        kneeBuffer[(size_t) i] = knee.value;
        ceilingBuffer[(size_t) i] = ceiling.value;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float g = (ch == 0 ? inputL : inputR).value;
            const float x = sanitise (channels[ch][i]) * g;
            peakIn[ch] = std::max (peakIn[ch], std::abs (x));
            in[ch][i] = x * thresholdScale.value;
        }
    }

    // 2. The clip, at the base rate or oversampled.
    chunkClipRatio = 1.0f;
    float* data[2] = { in[0], in[1] };
    if (active == 0)
    {
        shapeBlock (data, numChannels, n, 1);
    }
    else
    {
        auto& o = *oversamplers[(size_t) (active - 1)];
        juce::dsp::AudioBlock<float> block (data, (size_t) numChannels, (size_t) n);
        auto up = o.processSamplesUp (juce::dsp::AudioBlock<const float> (block));
        float* upData[2] = { up.getChannelPointer (0), numChannels > 1 ? up.getChannelPointer (1) : nullptr };
        shapeBlock (upData, numChannels, (int) up.getNumSamples(), 1 << active);
        o.processSamplesDown (block);
    }

    // 3. Back to the Ceiling, the final clip at the base rate, and the fade for a factor change.
    float worstOvershoot = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float c = ceilingBuffer[(size_t) i];
        const float gain = nextFadeGain();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float y = in[ch][i];
            worstOvershoot = std::max (worstOvershoot, std::abs (y));
            const float out = std::clamp (y * c, -c, c) * gain;
            channels[ch][i] = out;
            meterValues.outputPeak[ch] = std::max (meterValues.outputPeak[ch], std::abs (out));
        }
    }

    for (int ch = 0; ch < numChannels; ++ch)
        meterValues.inputPeak[ch] = std::max (meterValues.inputPeak[ch], peakIn[ch]);
    meterValues.clipDb = std::max (meterValues.clipDb, 20.0f * std::log10 (chunkClipRatio));
    meterValues.safetyClipDb = std::max (meterValues.safetyClipDb, 20.0f * std::log10 (worstOvershoot));
}
} // namespace clipper
