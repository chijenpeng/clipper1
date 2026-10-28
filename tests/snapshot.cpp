// Renders the plug-in editor to a PNG without a host or a screen, after running a few seconds
// of synthetic programme through the processor so the meters show something.
//
//   clipper1_snapshot out.png [editorScale=1] [pixelScale=2]
//
// editorScale sizes the window (0.5 .. 2, as a user would); pixelScale is the render
// resolution on top of that (2 for a Retina-like PNG of the design, 1 to see what a user at
// that window size sees pixel for pixel).

#include "../Source/PluginEditor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "editor.png");
    const double editorScale = juce::jlimit (0.5, 2.0, argc > 2 ? juce::String (argv[2]).getDoubleValue() : 1.0);
    const float pixelScale = juce::jlimit (0.25f, 4.0f, argc > 3 ? juce::String (argv[3]).getFloatValue() : 2.0f);

    constexpr double fs = 48000.0;
    constexpr int block = 512;

    std::unique_ptr<juce::AudioProcessor> processor (createPluginFilter());
    auto& clipperProcessor = dynamic_cast<Clipper1Processor&> (*processor);
    processor->setPlayConfigDetails (2, 2, fs, block);
    processor->prepareToPlay (fs, block);

    const auto set = [&] (const char* id, float value)
    {
        auto* p = clipperProcessor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    };
    set (ParamID::threshold, -8.0f);
    set (ParamID::ceiling, -0.3f);
    set (ParamID::softness, 35.0f);
    set (ParamID::oversampling, 2.0f);   // 4x
    set (ParamID::inputR, -1.5f);

    // A 100 Hz tone at -14 dB with bursts 12 dB louder; the programme ends inside a burst so
    // the last block (what the meters read) shows clipping.
    const int length = (int) (fs * 3.0);
    juce::AudioBuffer<float> audio (2, length);
    for (int n = 0; n < length; ++n)
    {
        const double t = n / fs;
        const bool burst = std::fmod (t, 0.5) > 0.3;
        const float amp = juce::Decibels::decibelsToGain (burst ? -2.0f : -14.0f);
        const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * 100.0 * t);
        audio.setSample (0, n, v);
        audio.setSample (1, n, v * 0.9f);
    }

    juce::MidiBuffer midi;
    Clipper1Editor editor (clipperProcessor);
    editor.setSize (juce::roundToInt (Clipper1Surface::kWidth * editorScale), juce::roundToInt (Clipper1Surface::kHeight * editorScale));

    // The meters hold the maximum since the last editor read, so clear them before each block:
    // afterwards they hold exactly the last block, which is what the snapshot shows.
    const auto clearMeters = [&clipperProcessor]
    {
        clipperProcessor.inputPeakL.store (0.0f);  clipperProcessor.inputPeakR.store (0.0f);
        clipperProcessor.outputPeakL.store (0.0f); clipperProcessor.outputPeakR.store (0.0f);
        clipperProcessor.clipDb.store (0.0f);
    };
    for (int pos = 0; pos + block <= length; pos += block)
    {
        clearMeters();
        juce::AudioBuffer<float> chunk (audio.getArrayOfWritePointers(), 2, pos, block);
        processor->processBlock (chunk, midi);
    }

    const float shownInL = clipperProcessor.inputPeakL.load(), shownInR = clipperProcessor.inputPeakR.load();
    const float shownOutL = clipperProcessor.outputPeakL.load(), shownOutR = clipperProcessor.outputPeakR.load();
    const float shownClip = clipperProcessor.clipDb.load();
    // The editor's timer exchanges the meters to 0 on each read, so re-store the last block
    // before every short dispatch slice (ticks are 33 ms apart, slices 10 ms): each read sees it.
    for (int slice = 0; slice < 15; ++slice)
    {
        clipperProcessor.inputPeakL.store (shownInL);   clipperProcessor.inputPeakR.store (shownInR);
        clipperProcessor.outputPeakL.store (shownOutL); clipperProcessor.outputPeakR.store (shownOutR);
        clipperProcessor.clipDb.store (shownClip);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, pixelScale);
    output.deleteFile();
    juce::FileOutputStream out (output);
    return juce::PNGImageFormat().writeImageToStream (image, out) ? 0 : 1;
}
