#pragma once

#include "dsp/Clipper.h"
#include "dsp/LinkedControls.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

/** Parameter IDs. Stable: never reorder or rename, sessions depend on them. */
namespace ParamID
{
    inline constexpr auto inputL = "input_l", inputR = "input_r", threshold = "threshold", ceiling = "ceiling",
                          softness = "softness", oversampling = "oversampling", link = "link", inputLink = "input_link";
}

class Clipper1Processor final : public juce::AudioProcessor,
                                  private juce::AsyncUpdater
{
public:
    Clipper1Processor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // For the UI
    juce::AudioProcessorValueTreeState apvts;

    /** Maxima since the editor last read them (it exchanges them to zero), linear (0..1) and dB of clip; raw, no ballistics. */
    std::atomic<float> inputPeakL { 0.0f }, inputPeakR { 0.0f }, outputPeakL { 0.0f }, outputPeakR { 0.0f }, clipDb { 0.0f };
    /** What the clipper is actually using after the Link coupling (equal to the parameters
        except for the block or two before a coupled write-back lands). */
    std::atomic<float> effectiveThresholdDb { 0.0f }, effectiveCeilingDb { 0.0f };

    const clipper::Clipper& getClipper() const noexcept { return clipperDsp; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleAsyncUpdate() override;
    float param (const char* id) const noexcept { return apvts.getRawParameterValue (id)->load(); }
    int choice (const char* id, int lastIndex) const noexcept { return juce::jlimit (0, lastIndex, juce::roundToInt (param (id))); }
    /** The current Threshold / Ceiling pair becomes Link's baseline: nothing moves. */
    void resetLink() noexcept;
    void writeBack (const char* id, std::atomic<float>& pending);

    clipper::Clipper clipperDsp;
    clipper::LinkedControls link;
    std::atomic<float> pendingThreshold { NAN }, pendingCeiling { NAN };
    int reportedLatency = 0;   // audio thread: the latency last sent to the host
    std::atomic<bool> linkResetPending { false };   // a restored state re-baselines Link, on the audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Clipper1Processor)
};
