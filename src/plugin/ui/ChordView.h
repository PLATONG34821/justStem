#pragma once

#include "../SongResult.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** The chord progression: one centred line (previous -> now -> next three) that slides as the chords change, then the song as a grid of
    bars (4 per row) that follows playback. Click a bar to jump there. */
class ChordView final : public juce::Component
{
public:
    std::function<void (double seconds)> onSeek;

    void setSong (SongResultPtr);
    void setTranspose (int semitones);
    /** Call regularly with the player position; repaints only when something visible changes. */
    void setPosition (double seconds);

    /** Title, key, tempo and the chord chart as text (for the clipboard). */
    static juce::String chartText (const SongResult&, int transpose);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    /** The first row of bars on screen. */
    int firstVisibleRow() const { return firstRow; }

private:
    static constexpr int barsPerRow = 4, rowH = 34, numberW = 34;
    juce::Rectangle<int> barBounds (int bar) const;
    int visibleRows() const { return juce::jmax (1, grid.getHeight() / rowH); }
    int rowCount() const { return ((int) bars.size() + barsPerRow - 1) / barsPerRow; }
    void scrollTo (int row);
    juce::String name (int segment) const;

    SongResultPtr song;
    std::vector<ChordResult::Bar> bars;
    int transpose = 0;
    double position = 0;
    int currentBar = -1, currentSegment = -1, firstRow = 0;
    float wheelTravel = 0;       // trackpad scrolling not yet turned into rows
    int animateFrom = -1;        // the chord we're sliding away from, or -1 (no slide)
    double changeMs = 0;         // when the chord last changed
    static constexpr double slideMs = 280;
    juce::Rectangle<int> now, grid;
};
} // namespace juststem::ui
