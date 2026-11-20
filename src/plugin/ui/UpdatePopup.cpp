#include "UpdatePopup.h"

namespace juststem::ui
{
namespace
{
constexpr int cardW = 400, cardH = 190, pad = 28, btnH = 36;
constexpr float radius = 18.0f;
} // namespace

UpdatePopup::UpdatePopup (const juce::String& v) : version (v)
{
    // Download is filled in the gauge's light blue, like the Update pill in the top bar.
    downloadButton.setColour (juce::TextButton::buttonColourId, Palette::accentTo);
    downloadButton.setColour (juce::TextButton::textColourOffId, Palette::bgBottom);
    laterButton.onClick = [this] { if (onLater) onLater(); };
    downloadButton.onClick = [this] { if (onDownload) onDownload(); };
    addAndMakeVisible (laterButton);
    addAndMakeVisible (downloadButton);
}

juce::Rectangle<int> UpdatePopup::card() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (cardW, getWidth() - 32), cardH);
}

void UpdatePopup::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.6f));
    const auto c = card();
    g.setColour (Palette::bgTop.darker (0.25f));
    g.fillRoundedRectangle (c.toFloat(), radius);
    g.setColour (Palette::panelBorder);
    g.drawRoundedRectangle (c.toFloat().reduced (0.5f), radius, 1.0f);

    auto r = c.reduced (pad, pad);
    g.setColour (Palette::text);
    g.setFont (font (Weight::Light, 24.0f));
    g.drawText ("justStem " + version + " is out", r.removeFromTop (32), juce::Justification::centredLeft, true);
    r.removeFromTop (6);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Regular, 13.0f));
    g.drawFittedText ("Download it from GitHub and replace the plug-in. Your songs are kept.",
                      r.removeFromTop (40), juce::Justification::topLeft, 2, 1.0f);
}

void UpdatePopup::resized()
{
    auto row = card().reduced (pad, pad).removeFromBottom (btnH);
    placeRight (row, downloadButton, downloadButton.idealWidth (btnH), btnH);
    row.removeFromRight (10);
    placeRight (row, laterButton, laterButton.idealWidth (btnH), btnH);
}

void UpdatePopup::mouseDown (const juce::MouseEvent& e)
{
    if (! card().contains (e.getPosition()) && onLater) onLater(); // click outside: Later
}
} // namespace juststem::ui
