#pragma once

#include "../SongResult.h"
#include "../StemPlayer.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** "m:ss" */
juce::String formatTime (double seconds);

/** The preview transport under the stems: play (flush left), seek bar, "position / duration", Follow DAW (flush right). */
class PlayerBar final : public juce::Component
{
public:
    explicit PlayerBar (StemPlayer&);

    /** The song on screen, or nullptr (everything disabled). */
    void setSong (SongResultPtr);
    /** Call regularly: follows the player's position, play state and Follow DAW. */
    void update();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    StemPlayer& player;
    SongResultPtr song;
    IconButton playButton;
    bool showsPause = false; // playButton's icon
    juce::Slider position;
    juce::ToggleButton followHost { "Follow DAW" };
    juce::Rectangle<int> timeArea;
};
} // namespace juststem::ui
