#include "Widgets.h"

#if JUCE_MAC
 #include <CoreGraphics/CoreGraphics.h>
#endif

namespace juststem::ui
{
namespace
{
constexpr float pi = juce::MathConstants<float>::pi;

/** Angles for a 270° gauge: from 225° (bottom-left) clockwise to 135° (bottom-right). */
constexpr float gaugeStart = -0.75f * pi, gaugeEnd = 0.75f * pi;

juce::ColourGradient accentGradient (juce::Rectangle<float> r)
{
    return juce::ColourGradient (Palette::accentFrom, r.getX(), r.getBottom(), Palette::accentTo, r.getRight(), r.getY(), false);
}
} // namespace

// ============================================================================ gauge
Gauge::Layout Gauge::layout() const
{
    const auto bounds = getLocalBounds().toFloat();
    Layout l;
    l.size = std::min (bounds.getWidth(), bounds.getHeight());
    l.outer = bounds.withSizeKeepingCentre (l.size, l.size).reduced (4.0f);
    l.disc = l.outer.reduced (l.size * 0.14f);
    const auto& d = l.disc;
    l.caption = { d.getX(), d.getY() + d.getHeight() * 0.18f, d.getWidth(), l.size * 0.06f };
    l.value = d.withSizeKeepingCentre (d.getWidth() * 0.50f, l.size * 0.25f);
    l.unit = { d.getX(), l.outer.getCentreY() + l.size * 0.13f, d.getWidth(), l.size * 0.07f };
    return l;
}

Gauge::Lines Gauge::lines() const
{
    const auto l = layout();
    return { l.caption.getCentreY(), l.value.getCentreY(), l.unit.getCentreY() };
}

void Gauge::paint (juce::Graphics& g)
{
    const auto l = layout();
    const float size = l.size;
    const auto& outer = l.outer;
    const auto& disc = l.disc;
    const auto c = outer.getCentre();

    // Outer hairline ring and a darker inner disc with a soft rim.
    g.setColour (Palette::hairline);
    g.drawEllipse (outer, 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d1e21), c.x, c.y - disc.getHeight() * 0.5f,
                                             juce::Colour (0xff121315), c.x, c.y + disc.getHeight() * 0.5f, false));
    g.fillEllipse (disc);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawEllipse (disc, 1.0f);

    // Arc track + value.
    const float arcR = (outer.getWidth() * 0.5f) - size * 0.07f;
    const float thickness = size * 0.045f;
    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, gaugeStart, gaugeEnd, true);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    const auto pointAt = [&] (float angle, float radius) {
        return juce::Point<float> (c.x + radius * std::sin (angle), c.y - radius * std::cos (angle));
    };
    if (state.slots > 0)
    {
        // A faint dot per position, and the active position as a lit segment.
        const float step = (gaugeEnd - gaugeStart) / (float) state.slots;
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        for (int i = 0; i < state.slots; ++i)
        {
            const auto p = pointAt (gaugeStart + step * ((float) i + 0.5f), arcR);
            g.fillEllipse (juce::Rectangle<float> (thickness * 0.3f, thickness * 0.3f).withCentre (p));
        }
        if (state.activeSlot >= 0 && state.activeSlot < state.slots)
        {
            const float a0 = gaugeStart + step * (float) state.activeSlot + 0.02f, a1 = a0 + step - 0.04f;
            juce::Path seg;
            seg.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, a0, a1, true);
            g.setColour (Palette::accentTo.withAlpha (0.18f)); // soft glow
            g.strokePath (seg, juce::PathStrokeType (thickness * 2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setGradientFill (accentGradient (outer));
            g.strokePath (seg, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
    else if (state.fraction >= 0)
    {
        juce::Path arc;
        const float end = gaugeStart + (gaugeEnd - gaugeStart) * (float) juce::jlimit (0.0, 1.0, state.fraction);
        arc.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, gaugeStart, std::max (end, gaugeStart + 0.01f), true);
        g.setGradientFill (accentGradient (outer));
        g.strokePath (arc, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }
    // Caption, big value, unit.
    g.setColour (Palette::dim);
    // Long captions (e.g. a translated one) shrink to stay inside the disc at that height.
    {
        const auto caption = state.caption.toUpperCase();
        auto f = capsFont (size * 0.04f);
        const float maxW = disc.getWidth() * 0.80f;
        const float w = juce::GlyphArrangement::getStringWidth (f, caption);
        if (w > maxW) f = capsFont (size * 0.04f * std::max (0.7f, maxW / w));
        g.setFont (f);
        g.drawText (caption, l.caption, juce::Justification::centred, true);
    }
    g.setColour (Palette::text);
    g.setFont (font (Weight::ExtraLight, size * 0.22f));
    g.drawFittedText (state.value, l.value.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Light, size * 0.055f));
    g.drawText (state.unit, l.unit, juce::Justification::centred, false);
}

// ============================================================================ tab button
void TabButton::paintButton (juce::Graphics& g, bool over, bool)
{
    // One half of a segmented control (the editor draws the shared pill behind both halves):
    // the selected segment is a filled inner pill.
    const bool on = getToggleState();
    const auto r = getLocalBounds().toFloat().reduced (3.0f);
    if (on)
    {
        g.setColour (juce::Colours::white.withAlpha (0.13f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    }
    else if (over)
    {
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    }
    g.setFont (capsFont (11.0f));
    g.setColour (on ? Palette::text : over ? Palette::dim.brighter (0.3f) : Palette::dim);
    g.drawText (getButtonText().toUpperCase(), getLocalBounds(), juce::Justification::centred, false);
}

// ============================================================================ icon button
void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (juce::Colours::white.withAlpha (! isEnabled() ? 0.04f : down ? 0.20f : over ? 0.14f : 0.09f));
    g.fillEllipse (r);
    g.setColour (Palette::panelBorder);
    g.drawEllipse (r, 1.0f);
    drawIcon (g, icon, r.withSizeKeepingCentre (r.getWidth() * 0.34f, r.getHeight() * 0.34f),
              Palette::text.withAlpha (isEnabled() ? 1.0f : 0.3f));
}

// ============================================================================ segment bar
void SegmentBar::setValue (float v)
{
    const float clamped = juce::jlimit (0.0f, maxValue, v);
    if (juce::exactlyEqual (clamped, value)) return;
    value = clamped;
    repaint();
    if (auto* h = getAccessibilityHandler()) h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void SegmentBar::change (float v)
{
    setValue (v);
    if (onChange) onChange (value);
}

void SegmentBar::setFromX (float x)
{
    const float f = juce::jlimit (0.0f, 1.0f, x / (float) getWidth());
    change (std::round (f * segments) / segments * maxValue);
}

bool SegmentBar::keyPressed (const juce::KeyPress& key)
{
    constexpr float step = maxValue / segments;
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey) change (value + step);
    else if (code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey) change (value - step);
    else if (code == juce::KeyPress::homeKey) change (1.0f);
    else return false;
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> SegmentBar::createAccessibilityHandler()
{
    // Read and set in percent of unity gain (100% = as split, up to 150%).
    struct Value final : juce::AccessibilityValueInterface
    {
        explicit Value (SegmentBar& b) : bar (b) {}
        bool isReadOnly() const override { return false; }
        double getCurrentValue() const override { return bar.getValue(); }
        void setValue (double v) override { bar.change ((float) v); }
        juce::String getCurrentValueAsString() const override { return juce::String (juce::roundToInt (bar.getValue() * 100.0f)) + "%"; }
        void setValueAsString (const juce::String& s) override { bar.change (s.retainCharacters ("0123456789.").getFloatValue() / 100.0f); }
        AccessibleValueRange getRange() const override { return { { 0.0, (double) maxValue }, (double) (maxValue / segments) }; }
        SegmentBar& bar;
    };
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions {},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

void SegmentBar::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre ((float) getWidth(), 4.0f);
    const float gap = 3.0f, w = (r.getWidth() - gap * (segments - 1)) / segments;
    const int lit = (int) std::round (value / maxValue * segments);
    const int unity = (int) std::round (1.0f / maxValue * segments);
    for (int i = 0; i < segments; ++i)
    {
        const auto seg = juce::Rectangle<float> (r.getX() + (float) i * (w + gap), r.getY(), w, r.getHeight());
        g.setColour (i < lit ? Palette::text.withAlpha (i < unity ? 0.85f : 0.55f) : juce::Colours::white.withAlpha (0.10f));
        g.fillRoundedRectangle (seg, 1.5f);
    }
    // Unity (0 dB) marker.
    g.setColour (Palette::faint);
    const float ux = r.getX() + (float) unity * (w + gap) - gap * 0.5f;
    g.drawLine (ux, r.getY() - 5.0f, ux, r.getBottom() + 5.0f, 1.0f);
    if (hasKeyboardFocus (false)) // where the arrow keys go
    {
        g.setColour (Palette::accentTo.withAlpha (0.8f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }
}

// ============================================================================ drag handle
namespace
{
bool mouseButtonReallyDown()
{
   #if JUCE_MAC
    // JUCE's "realtime" modifiers refresh only the keys on macOS, not the mouse buttons.
    return CGEventSourceButtonState (kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
   #else
    return juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown();
   #endif
}
} // namespace

void DragHandle::paint (juce::Graphics& g)
{
    const bool enabled = file.existsAsFile();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colours::white.withAlpha (enabled ? 0.07f : 0.03f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (Palette::panelBorder);
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
    drawIcon (g, icons::grip(), r.withSizeKeepingCentre (12.0f, 14.0f), enabled ? Palette::text : Palette::faint);
}

void DragHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! file.existsAsFile() || e.getDistanceFromDragStart() < 4) return;
    // One drag per press, and only while the button is really down: in a plug-in, JUCE gets mouse
    // ups late (macOS), so a stray drag event after the drop would start a second drag, which
    // drops straight away and gives the DAW the file twice (seen in Waveform).
    if (! mouseButtonReallyDown()) return;
    dragging = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
}

// ============================================================================ stem row
StemRow::StemRow (StemPlayer& p, int i, juce::String n) : player (&p), index (i), name (std::move (n))
{
    for (auto* b : { &muteButton, &soloButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    muteButton.setColour (juce::TextButton::buttonOnColourId, Palette::text);
    soloButton.setColour (juce::TextButton::buttonOnColourId, Palette::highlight);
    muteButton.setTooltip ("Mute");
    soloButton.setTooltip ("Solo");
    // Screen readers would say "M" and "S"; say what and which.
    muteButton.setTitle ("Mute " + name);
    soloButton.setTitle ("Solo " + name);
    volume.setTitle (name + " volume");
    drag.setTitle ("Drag " + name + " onto a track");
    muteButton.onClick = [this] { player->setMute (index, muteButton.getToggleState()); repaint(); };
    soloButton.onClick = [this] { player->setSolo (index, soloButton.getToggleState()); };
    volume.onChange = [this] (float v) { player->setVolume (index, v); };
    addAndMakeVisible (volume);
    addAndMakeVisible (drag);
    refresh();
}

StemRow::StemRow (juce::String n, const juce::Path& i) : name (std::move (n)), icon (&i)
{
    drag.setTitle ("Drag " + name + " onto a track");
    addAndMakeVisible (drag);
}

void StemRow::refresh()
{
    if (player == nullptr) return;
    muteButton.setToggleState (player->getMute (index), juce::dontSendNotification);
    soloButton.setToggleState (player->getSolo (index), juce::dontSendNotification);
    volume.setValue (player->getVolume (index));
    repaint();
}

void StemRow::paint (juce::Graphics& g)
{
    const bool muted = player != nullptr && player->getMute (index);
    const float cy = (float) getHeight() * 0.5f;
    if (index >= 0)
    {
        g.setColour (Palette::forStem (index).withAlpha (muted ? 0.3f : 1.0f));
        g.fillEllipse (4.0f, cy - 4.0f, 8.0f, 8.0f);
    }
    else if (icon != nullptr)
        drawIcon (g, *icon, { 2.0f, cy - 6.0f, 12.0f, 12.0f }, Palette::dim);

    g.setFont (font (Weight::Regular, 14.0f));
    g.setColour (muted ? Palette::faint : Palette::text);
    g.drawText (name, juce::Rectangle<int> (26, 0, 200, getHeight()), juce::Justification::centredLeft, false);
}

void StemRow::resized()
{
    const int h = std::min (22, getHeight() - 10); // room between the rows
    auto r = getLocalBounds().withSizeKeepingCentre (getWidth(), h);
    drag.setBounds (r.removeFromRight (40));
    if (player == nullptr) return;
    r.removeFromLeft (150); // name
    r.removeFromRight (24);
    volume.setBounds (r.removeFromRight (std::max (160, r.getWidth() - 110)));
    r.removeFromRight (20);
    soloButton.setBounds (r.removeFromRight (34));
    r.removeFromRight (6);
    muteButton.setBounds (r.removeFromRight (34));
}
} // namespace juststem::ui
