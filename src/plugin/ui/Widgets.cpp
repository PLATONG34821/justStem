#include "Widgets.h"

namespace covertools::ui
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
void Gauge::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float size = std::min (bounds.getWidth(), bounds.getHeight());
    const auto outer = bounds.withSizeKeepingCentre (size, size).reduced (4.0f);
    const auto c = outer.getCentre();

    // Outer hairline ring and a darker inner disc with a soft rim.
    g.setColour (Palette::hairline);
    g.drawEllipse (outer, 1.0f);
    const auto disc = outer.reduced (size * 0.14f);
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
    if (state.fraction >= 0)
    {
        juce::Path arc;
        const float end = gaugeStart + (gaugeEnd - gaugeStart) * (float) juce::jlimit (0.0, 1.0, state.fraction);
        arc.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, gaugeStart, std::max (end, gaugeStart + 0.01f), true);
        g.setGradientFill (accentGradient (outer));
        g.strokePath (arc, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }
    // Caption, big value, unit.
    g.setColour (Palette::dim);
    g.setFont (capsFont (size * 0.04f));
    g.drawText (state.caption.toUpperCase(), juce::Rectangle<float> (disc.getX(), disc.getY() + disc.getHeight() * 0.18f, disc.getWidth(), size * 0.06f),
                juce::Justification::centred, true);
    g.setColour (Palette::text);
    g.setFont (font (Weight::ExtraLight, size * 0.22f));
    g.drawFittedText (state.value, disc.withSizeKeepingCentre (disc.getWidth() * 0.50f, size * 0.25f).toNearestInt(),
                      juce::Justification::centred, 1, 0.6f);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Light, size * 0.055f));
    g.drawText (state.unit, juce::Rectangle<float> (disc.getX(), c.y + size * 0.13f, disc.getWidth(), size * 0.07f), juce::Justification::centred, false);
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
              Palette::text.withAlpha (isEnabled() ? 1.0f : 0.3f), true);
}

// ============================================================================ segment bar
void SegmentBar::setFromX (float x)
{
    const float f = juce::jlimit (0.0f, 1.0f, x / (float) getWidth());
    setValue (std::round (f * segments) / segments * maxValue);
    if (onChange) onChange (value);
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
}

// ============================================================================ drag handle
void DragHandle::paint (juce::Graphics& g)
{
    const bool enabled = file.existsAsFile();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colours::white.withAlpha (enabled ? 0.07f : 0.03f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (Palette::panelBorder);
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
    drawIcon (g, icons::grip(), r.withSizeKeepingCentre (12.0f, 14.0f), enabled ? Palette::text : Palette::faint, true);
}

void DragHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! file.existsAsFile() || e.getDistanceFromDragStart() < 4) return;
    dragging = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [this] { dragging = false; });
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
    muteButton.onClick = [this] { player->setMute (index, muteButton.getToggleState()); repaint(); };
    soloButton.onClick = [this] { player->setSolo (index, soloButton.getToggleState()); };
    volume.onChange = [this] (float v) { player->setVolume (index, v); };
    addAndMakeVisible (volume);
    addAndMakeVisible (drag);
    refresh();
}

StemRow::StemRow (juce::String n) : name (std::move (n)) { addAndMakeVisible (drag); }

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
    g.setColour (Palette::hairline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    const bool muted = player != nullptr && player->getMute (index);
    const float cy = (float) getHeight() * 0.5f;
    if (index >= 0)
    {
        g.setColour (Palette::forStem (index).withAlpha (muted ? 0.3f : 1.0f));
        g.fillEllipse (4.0f, cy - 4.0f, 8.0f, 8.0f);
    }
    else
        drawIcon (g, icons::layers(), { 2.0f, cy - 6.0f, 12.0f, 12.0f }, Palette::dim);

    g.setFont (font (Weight::Regular, 14.0f));
    g.setColour (muted ? Palette::faint : Palette::text);
    g.drawText (name, juce::Rectangle<int> (26, 0, 200, getHeight()), juce::Justification::centredLeft, false);
}

void StemRow::resized()
{
    const int h = std::min (26, getHeight() - 6);
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
} // namespace covertools::ui
