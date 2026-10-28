#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace clipper;

namespace
{
    const juce::Identifier versionId ("version");
    constexpr int kStateVersion = 1;

    // Lock-free running maximum: keeps the largest value stored since the editor last exchanged it to zero.
    void storeMax (std::atomic<float>& target, float value) noexcept
    {
        auto current = target.load (std::memory_order_relaxed);
        while (value > current && ! target.compare_exchange_weak (current, value, std::memory_order_relaxed)) {}
    }

    juce::AudioParameterFloatAttributes dbAttributes()
    {
        return juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (
            [] (float v, int) { return juce::String (v, 1) + " dB"; });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout Clipper1Processor::createLayout()
{
    using Range = juce::NormalisableRange<float>;
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Bool = juce::AudioParameterBool;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const Range gainRange (kMinGainDb, 0.0f, 0.1f);

    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::inputL, 1 }, "Input L", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::inputR, 1 }, "Input R", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::threshold, 1 }, "Threshold", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::ceiling, 1 }, "Out Ceiling", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::softness, 1 }, "Softness", Range (0.0f, 100.0f, 0.1f), 0.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (
                                             [] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; })));
    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::oversampling, 1 }, "Oversampling", juce::StringArray { "Off", "2x", "4x", "8x" }, 2));
    layout.add (std::make_unique<Bool> (juce::ParameterID { ParamID::link, 1 }, "Link Threshold and Ceiling", true));
    layout.add (std::make_unique<Bool> (juce::ParameterID { ParamID::inputLink, 1 }, "Link Inputs", true));
    return layout;
}

Clipper1Processor::Clipper1Processor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Clipper1", createLayout())
{
    link.setRange (kMinGainDb, 0.0f);
    resetLink();
}

void Clipper1Processor::resetLink() noexcept
{
    link.reset (param (ParamID::threshold), param (ParamID::ceiling), param (ParamID::link) > 0.5f);
}

bool Clipper1Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

void Clipper1Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    clipperDsp.prepare (sampleRate, samplesPerBlock);
    resetLink();
    // The first processBlock applies the factor at once, so the host can already be told its latency.
    reportedLatency = clipperDsp.latencyForFactor (choice (ParamID::oversampling, 3));
    setLatencySamples (reportedLatency);
}

void Clipper1Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels(), 2);
    for (int ch = numChannels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (linkResetPending.exchange (false))
        resetLink();
    const auto linked = link.update (param (ParamID::threshold), param (ParamID::ceiling), param (ParamID::link) > 0.5f);
    if (linked.writeThreshold)
        pendingThreshold.store (linked.thresholdDb);
    if (linked.writeCeiling)
        pendingCeiling.store (linked.ceilingDb);
    if (linked.writeThreshold || linked.writeCeiling)
        triggerAsyncUpdate();
    effectiveThresholdDb.store (linked.thresholdDb);
    effectiveCeilingDb.store (linked.ceilingDb);

    Params p;
    p.inputGainDbL = param (ParamID::inputL);
    p.inputGainDbR = param (ParamID::inputR);
    p.thresholdDb = linked.thresholdDb;
    p.ceilingDb = linked.ceilingDb;
    p.softnessPercent = param (ParamID::softness);
    p.oversamplingLog2 = choice (ParamID::oversampling, 3);
    clipperDsp.setParams (p);

    clipperDsp.process (buffer.getArrayOfWritePointers(), numChannels, buffer.getNumSamples());

    // A factor change completes inside process(); tell the host the new latency from the message thread.
    if (clipperDsp.latencySamples() != reportedLatency)
    {
        reportedLatency = clipperDsp.latencySamples();
        triggerAsyncUpdate();
    }

    const auto& m = clipperDsp.meters();
    storeMax (inputPeakL, m.inputPeak[0]);
    storeMax (inputPeakR, numChannels > 1 ? m.inputPeak[1] : m.inputPeak[0]);
    storeMax (outputPeakL, m.outputPeak[0]);
    storeMax (outputPeakR, numChannels > 1 ? m.outputPeak[1] : m.outputPeak[0]);
    storeMax (clipDb, m.clipDb);
}

void Clipper1Processor::writeBack (const char* id, std::atomic<float>& pending)
{
    const float value = pending.exchange (NAN);
    if (auto* parameter = apvts.getParameter (id); parameter != nullptr && ! std::isnan (value))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void Clipper1Processor::handleAsyncUpdate()
{
    // The Link coupling moved a control: tell the host and the UI (message thread).
    writeBack (ParamID::threshold, pendingThreshold);
    writeBack (ParamID::ceiling, pendingCeiling);
    // The oversampling factor changed: the latency follows.
    if (clipperDsp.latencySamples() != getLatencySamples())
        setLatencySamples (clipperDsp.latencySamples());
}

void Clipper1Processor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (versionId, kStateVersion, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void Clipper1Processor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    linkResetPending.store (true);   // a restored pair is the new baseline for Link: nothing moves on load
}

juce::AudioProcessorEditor* Clipper1Processor::createEditor()
{
    return new Clipper1Editor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Clipper1Processor();
}
