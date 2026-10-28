#include "PluginEditor.h"

#include <BinaryData.h>

namespace
{
    // Near-black aubergine ground, panels one layer up, white type. Pale rose is what you
    // control (fader handles, the knob, selected options, the plug-in's name); magenta is the
    // clip being applied (clip meter, the transfer curve); levels are a quieter rose.
    namespace Theme
    {
        const juce::Colour background { 0xff130d14 }, panel { 0xff211722 }, text { 0xffffffff },
                           label { 0xffb4a6b8 }, accent { 0xffff4fb4 }, pale { 0xfff7d4ee },
                           field { 0xff43344a };

        // Meter tracks and fills are the label and pale colours at reduced alpha, not new greys.
        const juce::Colour track = label.withAlpha (0.18f), trackLit = field, level = pale.withAlpha (0.6f);

        juce::Font make (const juce::Typeface::Ptr& typeface, float height, float tracking)
        {
            return juce::Font (juce::FontOptions (typeface).withHeight (height)).withExtraKerningFactor (tracking);
        }

        /** Labels are upper-case and tracked out; pass upper-case text. */
        juce::Font labelFont (float height = 11.0f)  { return make (juce::SharedResourcePointer<Clipper1Fonts>()->inter, height, 0.09f); }
        juce::Font titleFont (float height)          { return make (juce::SharedResourcePointer<Clipper1Fonts>()->inter, height, 0.18f); }
        juce::Font bodyFont (float height = 12.0f)   { return make (juce::SharedResourcePointer<Clipper1Fonts>()->inter, height, 0.01f); }
        juce::Font valueFont (float height = 12.0f)  { return make (juce::SharedResourcePointer<Clipper1Fonts>()->mono, height, 0.0f); }

        int widthOf (const juce::Font& font, const juce::String& s)
        {
            return juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (font, s)));
        }
    }

    // Repeated gaps are tokens.
    constexpr int kMargin = 24, kHeaderHeight = 50;
    constexpr int kOptionGap = 10, kLabelGap = 18, kRowHeight = 18, kLaneGap = 24;
    constexpr int kGap = 12, kGroupGap = 36, kMeterGap = 4, kScaleWidth = 28;
    constexpr int kFaderBodyHeight = 170;

    const juce::Identifier helpId ("help"), helpTitleId ("helpTitle"), editorScaleId ("editorScale");

    // The product name; also CLIPPER_NAME in CMakeLists.txt.
    constexpr auto kPluginName = "CLIPPER1";
    constexpr auto kDefaultHelpTitle = kPluginName;
    constexpr auto kDefaultHelp = "Clipper1: peaks above the Threshold are clipped at the Out Ceiling, hard or with a soft knee. Pull Threshold below the "
                                  "input peaks, set the ceiling, add Softness for a gentler knee. Hover over a control to see what it does; "
                                  "double-click a knob for its default, or its value to type one.";
    constexpr auto kInputMeterHelp = "Input peaks after the input faders, left and right, with a 1.5 s hold. The line across them is the "
                                     "Threshold: peaks above it are clipped.";
    constexpr auto kOutputMeterHelp = "Output peaks, left and right, with a 1.5 s hold. The line across them is the Out Ceiling; nothing "
                                      "leaves the plug-in above it.";

    juce::StringArray oversamplingOptions()
    {
        const auto times = juce::String::charToString (0x00d7);
        return { "Off", "2" + times, "4" + times, "8" + times };
    }

    float toDb (float linear) noexcept { return 20.0f * std::log10 (std::max (linear, 1.0e-5f)); }

    void setHelp (juce::Component& c, const juce::String& title, const juce::String& help)
    {
        c.getProperties().set (helpTitleId, title.toUpperCase());
        c.getProperties().set (helpId, help);
    }

    juce::String dbText (float db) { return juce::String (db, 1); }
}

//==============================================================================
Clipper1Fonts::Clipper1Fonts()
    : inter (juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)),
      mono (juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize))
{
}

//==============================================================================
void Clipper1LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                           float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f;
    const float angle = startAngle + position * (endAngle - startAngle);
    const float dot = juce::jmax (3.0f, radius * 0.11f);

    g.setColour (slider.isMouseOverOrDragging() ? Theme::text : Theme::pale);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    g.setColour (Theme::background);
    g.fillEllipse (juce::Rectangle<float> (dot * 2.0f, dot * 2.0f).withCentre (centre.getPointOnCircumference (radius * 0.72f, angle)));
}

void Clipper1LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool, bool)
{
    // Two faint diagonals in the corner: enough to find, not a decoration.
    g.setColour (Theme::label.withAlpha (0.35f));
    for (float i = 0.0f; i < 2.0f; ++i)
        g.drawLine ((float) w * (0.45f + i * 0.25f), (float) h, (float) w, (float) h * (0.45f + i * 0.25f), 1.0f);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const char* paramId, const juce::String& nameToUse, const juce::String& help,
            int diameterToUse, float sweepDegrees)
    : name (nameToUse.toUpperCase()), diameter (diameterToUse), attachment (*state.getParameter (paramId), slider)
{
    const float halfSweep = juce::degreesToRadians (sweepDegrees * 0.5f), twoPi = juce::MathConstants<float>::twoPi;
    slider.setRotaryParameters (twoPi - halfSweep, twoPi + halfSweep, true);
    slider.setVelocityModeParameters (0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift: fine
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    editor.setJustification (juce::Justification::centred);
    editor.setFont (Theme::valueFont (12.0f));
    editor.setIndents (0, 0);
    editor.setColour (juce::TextEditor::backgroundColourId, Theme::field);
    editor.setColour (juce::TextEditor::textColourId, Theme::pale);
    editor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.5f));
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.onReturnKey = [this] { finishEditing (true); };
    editor.onEscapeKey = [this] { finishEditing (false); };
    editor.onFocusLost = [this] { finishEditing (true); };
    addChildComponent (editor);

    setHelp (*this, name, help);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (kLabelHeight + kGap - 1);
    slider.setBounds (area.removeFromTop (diameter + 2).withSizeKeepingCentre (diameter + 2, diameter + 2));
    editor.setBounds (getLocalBounds().removeFromBottom (kLabelHeight).withSizeKeepingCentre (64, kLabelHeight));
}

void Knob::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (Theme::label);
    g.setFont (Theme::labelFont());
    g.drawText (name, area.removeFromTop (kLabelHeight), juce::Justification::centred, false);

    g.setFont (Theme::valueFont (12.0f));
    g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromBottom (kLabelHeight), juce::Justification::centred, false);
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < getHeight() - kLabelHeight - 2)
        return;

    editor.setText (slider.getTextFromValue (slider.getValue()), false);
    editor.setVisible (true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void Knob::finishEditing (bool apply)
{
    if (! editor.isVisible())
        return;

    editor.setVisible (false);
    const auto text = editor.getText().trim().toLowerCase();

    // Only text that names a value: the parameters fall back to their defaults otherwise.
    const bool meaningful = text.containsAnyOf ("0123456789");
    if (apply && meaningful)
        slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
    repaint();
}

//==============================================================================
OptionRow::OptionRow (juce::RangedAudioParameter& param, const juce::String& labelToUse, juce::StringArray optionsToUse,
                      const juce::String& help)
    : label (labelToUse.toUpperCase()), options (std::move (optionsToUse)),
      attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); })
{
    labelColumn = labelTextWidth() + kLabelGap;
    attachment.sendInitialUpdate();
    setHelp (*this, label, help);
}

int OptionRow::labelTextWidth() const
{
    return Theme::widthOf (Theme::labelFont(), label);
}

std::vector<juce::Range<int>> OptionRow::optionRanges() const
{
    std::vector<juce::Range<int>> ranges;
    const auto font = Theme::labelFont();
    int x = labelColumn;

    for (const auto& option : options)
    {
        const int width = Theme::widthOf (font, option.toUpperCase());
        ranges.push_back ({ x, x + width });
        x += width + kOptionGap;
    }
    return ranges;
}

int OptionRow::preferredWidth() const
{
    const auto ranges = optionRanges();
    return ranges.empty() ? labelColumn : ranges.back().getEnd();
}

int OptionRow::optionAt (int x) const
{
    const auto ranges = optionRanges();
    for (int i = 0; i < (int) ranges.size(); ++i)
        if (ranges[(size_t) i].expanded (kOptionGap / 2).contains (x))
            return i;
    return -1;
}

void OptionRow::paint (juce::Graphics& g)
{
    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText (label, 0, 0, labelColumn, getHeight(), juce::Justification::centredLeft, false);

    const auto ranges = optionRanges();
    for (int i = 0; i < options.size(); ++i)
    {
        const auto range = ranges[(size_t) i];
        g.setColour (i == selected ? Theme::pale : (i == hovered ? Theme::text : Theme::label.withAlpha (0.7f)));
        g.drawText (options[i].toUpperCase(), range.getStart(), 0, range.getLength() + 4, getHeight(),
                    juce::Justification::centredLeft, false);
    }
}

void OptionRow::mouseDown (const juce::MouseEvent& e)
{
    const int option = optionAt (e.x);
    if (option < 0)
        return;

    attachment.setValueAsCompleteGesture ((float) option);
}

void OptionRow::mouseMove (const juce::MouseEvent& e)   { hovered = optionAt (e.x); repaint(); }
void OptionRow::mouseExit (const juce::MouseEvent&)     { hovered = -1; repaint(); }

//==============================================================================
TextToggle::TextToggle (juce::RangedAudioParameter& param, const juce::String& textToUse, const juce::String& help)
    : text (textToUse.toUpperCase()),
      attachment (param, [this] (float v) { on = v > 0.5f; repaint(); })
{
    attachment.sendInitialUpdate();
    setHelp (*this, text, help);
}

void TextToggle::paint (juce::Graphics& g)
{
    g.setFont (Theme::labelFont());
    g.setColour (on ? Theme::pale : (isMouseOver() ? Theme::text : Theme::label.withAlpha (0.7f)));
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
}

void TextToggle::mouseDown (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
}

//==============================================================================
Fader::Fader (juce::AudioProcessorValueTreeState& state, const char* paramId, const juce::String& name, const juce::String& help)
    : param (*state.getParameter (paramId)),
      attachment (param, [this] (float v) { valueDb = v; repaint(); })
{
    attachment.sendInitialUpdate();
    setHelp (*this, name, help);
}

void Fader::linkTo (Fader* partnerToUse, std::function<bool()> isLinked)
{
    partner = partnerToUse;
    linked = std::move (isLinked);
}

void Fader::paint (juce::Graphics& g)
{
    const auto body = scale::bodyOf (getLocalBounds());
    const auto track = scale::trackOf (body);
    const float cx = track.getCentreX();
    const float y = scale::yForDb (valueDb, body);

    g.setColour (Theme::trackLit);
    g.fillRect (cx - 1.0f, track.getY(), 2.0f, track.getHeight());

    const auto handle = juce::Rectangle<float> (24.0f, 14.0f).withCentre ({ cx, y });
    g.setColour (dragging || isMouseOver() ? Theme::text : Theme::pale);
    g.fillRoundedRectangle (handle, 2.0f);
    g.setColour (Theme::background);
    g.fillRect (handle.getX() + 4.0f, y - 0.5f, handle.getWidth() - 8.0f, 1.0f);

    g.setColour (Theme::label);
    g.setFont (Theme::valueFont (10.0f));
    g.drawText (dbText (valueDb), getLocalBounds().removeFromBottom (scale::kValueRow - 6), juce::Justification::centred);
}

void Fader::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    dragging = true;
    dragStartDb = valueDb;
    attachment.beginGesture();

    partnerDragging = partnerLinked();
    if (partnerDragging)
    {
        partnerStartDb = partner->valueDb;
        partner->attachment.beginGesture();
    }
}

void Fader::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const auto track = scale::trackOf (scale::bodyOf (getLocalBounds()));
    const float pixelsPerDb = track.getHeight() / (scale::kMaxDb - scale::kMinDb);
    float delta = (float) (e.getMouseDownY() - e.y) / pixelsPerDb;
    if (e.mods.isShiftDown())
        delta *= 0.1f;
    delta = limitMove (delta, dragStartDb, partnerDragging, partnerStartDb);

    const auto& range = param.getNormalisableRange();
    attachment.setValueAsPartOfGesture (range.snapToLegalValue (dragStartDb + delta));
    if (partnerDragging)
        partner->attachment.setValueAsPartOfGesture (range.snapToLegalValue (partnerStartDb + delta));
}

float Fader::limitMove (float deltaDb, float fromDb, bool withPartner, float partnerFromDb) const
{
    // Both faders stay in range and keep their offset: the pair stops at the first end stop.
    float low = scale::kMinDb - fromDb, high = scale::kMaxDb - fromDb;
    if (withPartner)
    {
        low = juce::jmax (low, scale::kMinDb - partnerFromDb);
        high = juce::jmin (high, scale::kMaxDb - partnerFromDb);
    }
    return juce::jlimit (low, high, deltaDb);
}

void Fader::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    attachment.endGesture();
    if (partnerDragging)
        partner->attachment.endGesture();
    partnerDragging = false;
    repaint();
}

void Fader::mouseDoubleClick (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
}

void Fader::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f || dragging)
        return;

    const bool withPartner = partnerLinked();
    float delta = (wheel.deltaY > 0.0f ? 0.5f : -0.5f) * (e.mods.isShiftDown() ? 0.2f : 1.0f);
    delta = limitMove (delta, valueDb, withPartner, withPartner ? partner->valueDb : 0.0f);

    const auto& range = param.getNormalisableRange();
    if (withPartner)
        partner->attachment.setValueAsCompleteGesture (range.snapToLegalValue (partner->valueDb + delta));
    attachment.setValueAsCompleteGesture (range.snapToLegalValue (valueDb + delta));
}

//==============================================================================
Meter::Meter (Kind kindToUse, std::function<float()> read, const juce::String& name, const juce::String& help)
    : kind (kindToUse), readDb (std::move (read))
{
    setInterceptsMouseClicks (true, false);   // hover only, for the help text
    setHelp (*this, name, help);
}

void Meter::setMarkerDb (float db)
{
    if (std::isnan (db) != std::isnan (markerDb) || std::abs (db - markerDb) > 0.01f)
    {
        markerDb = db;
        repaint();
    }
}

void Meter::update()
{
    // Instant rise; about 24 dB/s fall for levels and 18 dB/s for the reduction, at 30 Hz.
    const float db = readDb();
    const float fall = kind == Kind::peak ? 0.8f : 0.6f;
    const float floor = kind == Kind::peak ? -100.0f : 0.0f;
    const float next = juce::jmax (db, shownDb - fall, floor);

    float nextHold = holdDb;
    if (db >= holdDb)
    {
        nextHold = db;
        holdFrames = 45;   // 1.5 s
    }
    else if (holdFrames > 0)
        --holdFrames;
    else
        nextHold = juce::jmax (next, holdDb - fall);

    if (std::abs (next - shownDb) > 0.02f || std::abs (nextHold - holdDb) > 0.02f)
    {
        shownDb = next;
        holdDb = nextHold;
        repaint();
    }
}

void Meter::paint (juce::Graphics& g)
{
    const auto body = scale::bodyOf (getLocalBounds());
    const auto fullTrack = scale::trackOf (body);
    const auto track = fullTrack.withSizeKeepingCentre ((float) kWidth, fullTrack.getHeight());

    g.setColour (Theme::track);
    g.fillRect (track);

    if (kind == Kind::peak)
    {
        if (shownDb > scale::kMinDb)
        {
            const float y = scale::yForDb (shownDb, body);
            g.setColour (Theme::level);
            g.fillRect (track.withTop (y));
        }
        if (holdDb > scale::kMinDb)
        {
            g.setColour (Theme::text.withAlpha (0.85f));
            g.fillRect (track.getX(), scale::yForDb (holdDb, body) - 0.5f, track.getWidth(), 1.0f);
        }
    }
    else
    {
        // The clip grows downward from 0 dB at the top, like an attenuation meter.
        if (shownDb > 0.0f)
        {
            const float y = scale::yForDb (-shownDb, body);
            g.setColour (Theme::accent);
            g.fillRect (track.withBottom (y));
        }
        if (holdDb > 0.0f)
        {
            g.setColour (Theme::text.withAlpha (0.85f));
            g.fillRect (track.getX(), scale::yForDb (-holdDb, body) - 0.5f, track.getWidth(), 1.0f);
        }
    }

    if (! std::isnan (markerDb))
    {
        g.setColour (Theme::pale);
        g.fillRect (track.getX() - 2.0f, scale::yForDb (markerDb, body) - 0.5f, track.getWidth() + 4.0f, 1.0f);
    }

    const auto valueArea = getLocalBounds().removeFromBottom (scale::kValueRow - 6);
    g.setColour (Theme::label);
    if (showsValue)
    {
        g.setFont (Theme::valueFont (10.0f));
        g.drawText (dbText (kind == Kind::peak ? shownDb : -shownDb), valueArea, juce::Justification::centred);
    }
    else if (caption.isNotEmpty())
    {
        g.setFont (Theme::labelFont (9.0f));
        g.drawText (caption, valueArea, juce::Justification::centred);
    }
}

//==============================================================================
CurveDisplay::CurveDisplay (Clipper1Processor& p, const juce::String& help)
    : processor (p)
{
    setInterceptsMouseClicks (true, false);   // hover only, for the help text
    setHelp (*this, "Transfer curve", help);
}

void CurveDisplay::update (float peak)
{
    const float t = processor.effectiveThresholdDb.load(), c = processor.effectiveCeilingDb.load();
    const float s = processor.apvts.getRawParameterValue (ParamID::softness)->load();
    if (std::abs (t - thresholdDb) > 0.005f || std::abs (c - ceilingDb) > 0.005f || std::abs (s - softness) > 0.005f
        || std::abs (peak - peakDb) > 0.05f)
    {
        thresholdDb = t;
        ceilingDb = c;
        softness = s;
        peakDb = peak;
        repaint();
    }
}

void CurveDisplay::paint (juce::Graphics& g)
{
    g.setColour (Theme::panel);
    g.fillRect (getLocalBounds());

    // The plot is square on the shared scale: -30 dB to 0 dB on both axes, input along the bottom.
    const auto plot = getLocalBounds().toFloat().withTrimmedLeft (26.0f).withTrimmedBottom (18.0f).withTrimmedTop (18.0f).withTrimmedRight (10.0f);
    const auto xFor = [&plot] (float db) { return juce::jmap (db, scale::kMinDb, scale::kMaxDb, plot.getX(), plot.getRight()); };
    const auto yFor = [&plot] (float db) { return juce::jmap (db, scale::kMinDb, scale::kMaxDb, plot.getBottom(), plot.getY()); };

    g.setFont (Theme::valueFont (9.0f));
    for (float db = scale::kMinDb; db <= scale::kMaxDb + 0.01f; db += 6.0f)
    {
        const bool numbered = std::fmod (std::abs (db), 12.0f) < 0.01f || db > -0.01f;
        g.setColour (Theme::label.withAlpha (0.12f));
        g.fillRect (xFor (db) - 0.5f, plot.getY(), 1.0f, plot.getHeight());
        g.fillRect (plot.getX(), yFor (db) - 0.5f, plot.getWidth(), 1.0f);
        if (numbered)
        {
            g.setColour (Theme::label.withAlpha (0.8f));
            const auto text = juce::String ((int) db);
            g.drawText (text, juce::Rectangle<float> (0.0f, yFor (db) - 6.0f, plot.getX() - 4.0f, 12.0f), juce::Justification::centredRight);
            g.drawText (text, juce::Rectangle<float> (xFor (db) - 14.0f, plot.getBottom() + 3.0f, 28.0f, 12.0f), juce::Justification::centred);
        }
    }

    // Straight through, for reference.
    g.setColour (Theme::label.withAlpha (0.35f));
    g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);

    // The curve: Ceiling x shape (level / Threshold, knee), computed by the same function the processor runs.
    const float ceilingLinear = clipper::dbToGain (ceilingDb), knee = clipper::softnessToKnee (softness);
    const auto outDb = [&] (float inDb)
    {
        const float y = ceilingLinear * clipper::shape (clipper::dbToGain (inDb - thresholdDb), knee);
        return 20.0f * std::log10 (std::max (y, 1.0e-6f));
    };

    juce::Path path;
    const int steps = juce::roundToInt (plot.getWidth());
    for (int i = 0; i <= steps; ++i)
    {
        const float inDb = juce::jmap ((float) i, 0.0f, (float) steps, scale::kMinDb, scale::kMaxDb);
        const float p = juce::jlimit (plot.getY(), plot.getBottom(), yFor (outDb (inDb)));
        if (i == 0)
            path.startNewSubPath (plot.getX(), p);
        else
            path.lineTo (xFor (inDb), p);
    }
    g.setColour (Theme::accent);
    g.strokePath (path, juce::PathStrokeType (1.75f));

    if (peakDb > scale::kMinDb)
    {
        g.setColour (Theme::pale);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ xFor (peakDb), juce::jlimit (plot.getY(), plot.getBottom(), yFor (outDb (peakDb))) }));
    }

    g.setColour (Theme::label);
    g.setFont (Theme::valueFont (9.0f));
    g.drawText ("OUT dB", juce::Rectangle<float> (4.0f, 2.0f, 60.0f, 12.0f), juce::Justification::centredLeft);
    g.drawText ("IN", juce::Rectangle<float> (plot.getRight() - 30.0f, plot.getBottom() - 14.0f, 26.0f, 12.0f), juce::Justification::centredRight);
}

//==============================================================================
Clipper1Surface::Clipper1Surface (Clipper1Processor& p)
    : processor (p),
      oversampling (*p.apvts.getParameter (ParamID::oversampling), "Oversampling", oversamplingOptions(),
                    "Runs the clip at 2, 4 or 8 times the sample rate so the harmonics it makes above Nyquist are filtered out "
                    "instead of folding back as aliasing. Adds latency (reported to the host). The filters can overshoot the ceiling "
                    "slightly, so a final clip at the plug-in's own rate keeps the ceiling exact."),
      inputL (p.apvts, ParamID::inputL, "Input L", "Left input level before the clip. With Link on, the two input faders move together and keep their offset."),
      inputR (p.apvts, ParamID::inputR, "Input R", "Right input level before the clip. With Link on, the two input faders move together and keep their offset."),
      threshold (p.apvts, ParamID::threshold, "Threshold", "Peaks above this are clipped. Everything below gets a constant gain of Ceiling minus Threshold, "
                                                           "so pulling it down below the input peaks raises the level and clips more."),
      ceiling (p.apvts, ParamID::ceiling, "Out Ceiling", "The highest level that leaves the plug-in. -0.3 to -0.5 dB is the usual choice for delivery."),
      inMeterL (Meter::Kind::peak, [&p] { return toDb (p.inputPeakL.exchange (0.0f)); }, "Input peaks", kInputMeterHelp),
      inMeterR (Meter::Kind::peak, [&p] { return toDb (p.inputPeakR.exchange (0.0f)); }, "Input peaks", kInputMeterHelp),
      clipMeter (Meter::Kind::clip, [&p] { return p.clipDb.exchange (0.0f); }, "Clip",
                 "How much the clip is turning peaks down right now: the largest ratio between the level going in and the level coming "
                 "out, measured at the rate the clip runs, with a 1.5 s hold on the deepest."),
      outMeterL (Meter::Kind::peak, [&p] { return toDb (p.outputPeakL.exchange (0.0f)); }, "Output peaks", kOutputMeterHelp),
      outMeterR (Meter::Kind::peak, [&p] { return toDb (p.outputPeakR.exchange (0.0f)); }, "Output peaks", kOutputMeterHelp),
      inputLink (*p.apvts.getParameter (ParamID::inputLink), "Link", "Moves Input L and Input R together, keeping their offset. The two values are what is stored."),
      link (*p.apvts.getParameter (ParamID::link), "Link", "Moves Threshold and Out Ceiling together by the same number of dB. Turning it on never changes the sound."),
      softness (p.apvts, ParamID::softness, "Softness", "Width of the knee. 0 % is a hard clip: the signal is straight up to the ceiling and then flat. "
                                                        "Higher values start bending below the Threshold and reach the ceiling later, so peaks are rounded "
                                                        "instead of cut off. 100 % bends from zero level.", 64),
      curve (p, "Output level against input level for the current Threshold, Out Ceiling and Softness. The faint diagonal is no "
                "change; the dot is the recent input peak. It shows the curve at the plug-in's own rate; Oversampling does not change it.")
{
    for (auto* c : std::initializer_list<juce::Component*> { &oversampling, &inputL, &inputR, &threshold, &ceiling,
                                                             &inMeterL, &inMeterR, &clipMeter, &outMeterL, &outMeterR,
                                                             &inputLink, &link, &softness, &curve })
        addAndMakeVisible (c);

    inputL.linkTo (&inputR, [this] { return inputLink.isOn(); });
    inputR.linkTo (&inputL, [this] { return inputLink.isOn(); });

    inMeterL.setCaption ("L");
    inMeterR.setCaption ("R");
    outMeterL.setCaption ("L");
    outMeterR.setCaption ("R");
    clipMeter.setShowsValue (true);

    helpTitle = kDefaultHelpTitle;
    helpText = kDefaultHelp;
    setSize (kWidth, kHeight);
    startTimerHz (30);
}

void Clipper1Surface::showHelpFor (juce::Component* component)
{
    juce::String title = kDefaultHelpTitle, text = kDefaultHelp;

    // The help belongs to the nearest ancestor that has any (a knob's slider has none itself).
    for (auto* c = component; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c->getProperties().contains (helpId))
        {
            title = c->getProperties()[helpTitleId].toString();
            text = c->getProperties()[helpId].toString();
            break;
        }
    }

    if (title != helpTitle || text != helpText)
    {
        helpTitle = title;
        helpText = text;
        repaint (helpArea);
    }
}

void Clipper1Surface::timerCallback()
{
    const float thresholdDb = processor.effectiveThresholdDb.load();
    const float ceilingDb = processor.effectiveCeilingDb.load();
    inMeterL.setMarkerDb (thresholdDb);
    inMeterR.setMarkerDb (thresholdDb);
    outMeterL.setMarkerDb (ceilingDb);
    outMeterR.setMarkerDb (ceilingDb);

    for (auto* m : { &inMeterL, &inMeterR, &clipMeter, &outMeterL, &outMeterR })
        m->update();
    curve.update (juce::jmax (inMeterL.shownLevelDb(), inMeterR.shownLevelDb()));

    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    showHelpFor (under != nullptr && isParentOf (under) ? under : nullptr);
}

void Clipper1Surface::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    g.setColour (Theme::pale);
    g.setFont (Theme::titleFont (15.0f));
    g.drawSingleLineText (kPluginName, kMargin, 33);

    // Section titles over the fader groups.
    g.setColour (Theme::label);
    g.setFont (Theme::labelFont());
    g.drawText ("INPUT", inputTitle, juce::Justification::centred);
    g.drawText ("THRESHOLD", thresholdTitle, juce::Justification::centred);
    g.drawText ("OUT CEILING", ceilingTitle, juce::Justification::centred);

    // The shared dB scale, on both sides of the fader section.
    g.setFont (Theme::valueFont (9.0f));
    for (float db = scale::kMaxDb; db >= scale::kMinDb - 0.01f; db -= 6.0f)
    {
        const float y = scale::yForDb (db, scaleBody);
        const auto text = juce::String ((int) db);
        g.setColour (Theme::label.withAlpha (0.35f));
        g.fillRect ((float) leftScaleX + kScaleWidth - 4.0f, y - 0.5f, 4.0f, 1.0f);
        g.fillRect ((float) rightScaleX, y - 0.5f, 4.0f, 1.0f);
        g.setColour (Theme::label.withAlpha (0.8f));
        g.drawText (text, juce::Rectangle<float> ((float) leftScaleX, y - 6.0f, kScaleWidth - 7.0f, 12.0f), juce::Justification::centredRight);
        g.drawText (text, juce::Rectangle<float> ((float) rightScaleX + 7.0f, y - 6.0f, kScaleWidth - 7.0f, 12.0f), juce::Justification::centredLeft);
    }

    // Help, bottom: what the control under the mouse does.
    auto help = helpArea;
    g.setColour (Theme::pale);
    g.setFont (Theme::labelFont());
    g.drawText (helpTitle, help.removeFromTop (16), juce::Justification::topLeft, false);
    g.setColour (Theme::label);
    g.setFont (Theme::bodyFont());
    g.drawFittedText (helpText, help, juce::Justification::topLeft, 3, 1.0f);
}

void Clipper1Surface::resized()
{
    auto area = getLocalBounds().reduced (kMargin, 0);
    area.removeFromTop (kHeaderHeight);

    // Help block at the bottom left: 54 px, 10 px to the window edge.
    area.removeFromBottom (10);
    helpArea = area.removeFromBottom (54);
    area.removeFromBottom (14);

    // Fader section: title row, the bodies with their value row, and the link row.
    auto section = area;
    const auto titleRow = section.removeFromTop (16);
    section.removeFromTop (8);
    const auto bodies = section.removeFromTop (kFaderBodyHeight + scale::kValueRow);
    section.removeFromTop (8);
    const auto linkRow = section.removeFromTop (kRowHeight);
    scaleBody = scale::bodyOf (bodies);

    int x = area.getX();
    leftScaleX = x;
    x += kScaleWidth;

    const auto place = [&] (juce::Component& c, int width) { c.setBounds (x, bodies.getY(), width, bodies.getHeight()); x += width; };

    const int inputStart = x;
    place (inputL, Fader::kWidth);
    place (inputR, Fader::kWidth);
    x += kGap;
    place (inMeterL, Meter::kWidth);
    x += kMeterGap;
    place (inMeterR, Meter::kWidth);
    inputTitle = titleRow.withLeft (inputStart).withRight (x);
    x += kGroupGap;

    const int thresholdStart = x;
    place (threshold, Fader::kWidth);
    x += kGap;
    place (clipMeter, Fader::kWidth);   // wide enough for its value; the bar is centred
    thresholdTitle = titleRow.withLeft (thresholdStart).withRight (x);
    x += kGroupGap;

    const int ceilingStart = x;
    place (ceiling, Fader::kWidth);
    x += kGap;
    place (outMeterL, Meter::kWidth);
    x += kMeterGap;
    place (outMeterR, Meter::kWidth);
    ceilingTitle = titleRow.withLeft (ceilingStart).withRight (x);
    x += kMeterGap;
    rightScaleX = x;
    x += kScaleWidth;

    inputLink.setBounds (linkRow.withLeft (inputTitle.getX()).withRight (inputTitle.getRight()).withSizeKeepingCentre (60, kRowHeight));
    link.setBounds (linkRow.withLeft (thresholdTitle.getRight()).withRight (ceilingStart).withSizeKeepingCentre (60, kRowHeight));

    // Right group: the Oversampling row on top, then Softness beside the transfer curve. The curve is
    // square and ends on the link row's bottom edge; the knob is centred on it.
    const auto column = area.withLeft (x + kGroupGap).withBottom (linkRow.getBottom());
    oversampling.setBounds (column.getX(), column.getY(), oversampling.preferredWidth(), kRowHeight);

    const int curveSize = 212;
    const int curveX = column.getX() + Knob::kCellWidth + kLaneGap;
    curve.setBounds (curveX, column.getBottom() - curveSize, curveSize, curveSize);
    softness.setBounds (column.getX(), curve.getBounds().getCentreY() - softness.preferredHeight() / 2,
                        Knob::kCellWidth, softness.preferredHeight());
}

//==============================================================================
Clipper1Editor::Clipper1Editor (Clipper1Processor& p)
    : AudioProcessorEditor (p), owner (p), surface (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (surface);

    // Scales as a whole, 0.75x to 2x, keeping the design's aspect ratio.
    constexpr int w = Clipper1Surface::kWidth, h = Clipper1Surface::kHeight;
    setResizable (true, true);
    setResizeLimits (w * 3 / 4, h * 3 / 4, w * 2, h * 2);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    const double scale = juce::jlimit (0.75, 2.0, (double) p.apvts.state.getProperty (editorScaleId, 1.0));
    setSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
}

Clipper1Editor::~Clipper1Editor()
{
    setLookAndFeel (nullptr);
}

void Clipper1Editor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void Clipper1Editor::resized()
{
    const float scale = (float) getWidth() / (float) Clipper1Surface::kWidth;
    surface.setTransform (juce::AffineTransform::scale (scale));
    surface.setBounds (0, 0, Clipper1Surface::kWidth, Clipper1Surface::kHeight);

    // Remember a size the user chose (a window on screen), not the ones a host passes through.
    if (isShowing() && getPeer() != nullptr)
        owner.apvts.state.setProperty (editorScaleId, (double) scale, nullptr);
}
