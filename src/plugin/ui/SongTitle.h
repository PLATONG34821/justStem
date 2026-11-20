#pragma once

#include "../SongResult.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** The open song's title in the top bar: a note icon, the title, and an "HQ" tag when the stems came from
    the HQ model. A title too long for the space holds, then slides left to show the rest, and loops. */
class SongTitle final : public juce::Component, private juce::Timer
{
public:
    SongTitle() { setInterceptsMouseClicks (false, false); }

    /** The title's font; the top bar puts the "justStem" wordmark on the same baseline. */
    static juce::Font textFont() { return font (Weight::Regular, 15.0f); }

    /** A new song starts its slide from the beginning. */
    void setSong (SongResultPtr);

    void paint (juce::Graphics&) override;
    void resized() override { updateTimer(); }
    void visibilityChanged() override { updateTimer(); }

private:
    struct Layout
    {
        juce::Rectangle<int> icon, text; // text: the room for the title (it slides inside this when longer)
        int textW = 0, tagW = 0;
        bool slides() const { return textW > text.getWidth(); }
    };
    Layout layout() const;
    void updateTimer();
    void timerCallback() override { repaint(); }

    SongResultPtr song;
    double shownMs = 0; // when the title appeared
};
} // namespace juststem::ui
