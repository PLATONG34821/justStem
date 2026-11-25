#include "PlayerBar.h"
#include "../Text.h"

namespace juststem::ui
{
namespace
{
juce::Font timeFont() { return font (Weight::Regular, 13.0f); }
} // namespace

PlayerBar::PlayerBar (StemPlayer& p, bool canFollowHost) : player (p)
{
    playButton.setIcon (icons::play());
    playButton.setTitle ("Play");
    position.setTitle ("Position");
    playButton.onClick = [this] { player.setPlaying (! player.isPlaying()); };
    position.setSliderStyle (juce::Slider::LinearHorizontal);
    position.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    position.onValueChange = [this] { if (position.isMouseButtonDown()) player.seek (position.getValue()); };
    followHost.onClick = [this] { player.setFollowHost (followHost.getToggleState()); };
    followHost.setTooltip ("Play the stems along with your DAW's transport");
    addAndMakeVisible (playButton);
    addAndMakeVisible (position);
    if (canFollowHost) addAndMakeVisible (followHost);
    else addChildComponent (followHost);
}

void PlayerBar::setSong (SongResultPtr s)
{
    if (s == song) return;
    song = std::move (s);
    if (song != nullptr) position.setRange (0.0, std::max (1.0, song->durationSeconds), 0.01);
    resized(); // the time's width depends on the duration
    update();
}

void PlayerBar::update()
{
    const bool hasSong = song != nullptr;
    const bool follow = followHost.isVisible() && player.followsHost();
    const bool manual = hasSong && ! follow;
    playButton.setEnabled (manual);
    if (const bool pause = manual && player.isPlaying(); pause != showsPause) // update() runs 30 times a second
    {
        showsPause = pause;
        playButton.setIcon (pause ? icons::pause() : icons::play());
        playButton.setTitle (pause ? "Pause" : "Play");
    }
    position.setEnabled (hasSong);
    followHost.setToggleState (follow, juce::dontSendNotification);
    // Following the DAW: the bar shows the DAW's position but can't move it (plug-ins can't seek the host).
    position.setInterceptsMouseClicks (! follow, false);
    position.setTooltip (follow ? "Following your DAW: move the DAW's playhead to jump" : juce::String());
    if (hasSong && ! position.isMouseButtonDown()) position.setValue (player.positionSeconds(), juce::dontSendNotification);
    repaint (timeArea);
}

void PlayerBar::resized()
{
    // Play flush left, Follow DAW flush right, the time sized to its text, and the same gap either side of the seek bar.
    auto bar = getLocalBounds();
    playButton.setBounds (bar.removeFromLeft (36).withSizeKeepingCentre (36, 36));
    bar.removeFromLeft (16);
    if (followHost.isVisible())
    {
        followHost.setBounds (bar.removeFromRight (DashboardLookAndFeel::toggleWidth (followHost)));
        bar.removeFromRight (24);
    }
    // Sized for the duration's digit count with every digit as a wide "0" (Inter's digits differ in width).
    auto pattern = text::minutesSeconds (song != nullptr ? song->durationSeconds : 0.0);
    for (const char* d = "123456789"; *d != 0; ++d) pattern = pattern.replaceCharacter ((juce::juce_wchar) *d, '0');
    timeArea = bar.removeFromRight ((int) std::ceil (juce::GlyphArrangement::getStringWidth (timeFont(), pattern + " / " + pattern)) + 4);
    bar.removeFromRight (16);
    position.setBounds (bar);
}

void PlayerBar::paint (juce::Graphics& g)
{
    if (song == nullptr) return;
    g.setColour (Palette::dim);
    g.setFont (timeFont());
    g.drawText (text::minutesSeconds (player.positionSeconds()) + " / " + text::minutesSeconds (player.durationSeconds()), timeArea,
                juce::Justification::centredRight, false);
}
} // namespace juststem::ui
