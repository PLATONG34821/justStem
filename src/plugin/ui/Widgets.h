#pragma once

#include "../StemPlayer.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** The centre dial: a 270° arc (blue gradient) around a big thin number, with a caption above
    and a unit below. With `slots` set, the arc is split into that many positions and only the
    active one is lit (for values that aren't amounts, like the key's place on the Camelot wheel). */
class Gauge final : public juce::Component
{
public:
    struct State
    {
        double fraction = -1;          // arc fill 0..1; < 0: no arc (idle)
        int slots = 0, activeSlot = -1; // slots > 0: light one position instead of filling
        juce::String value = "--", unit, caption;
    };
    void set (State s) { state = std::move (s); repaint(); }
    void paint (juce::Graphics&) override;

    /** Vertical centres of the caption, value and unit lines, in the gauge's own coordinates,
        so text beside the gauge can line up with them. */
    struct Lines { float caption, value, unit; };
    Lines lines() const;

private:
    State state;
};

/** One segment of a segmented control: spaced uppercase text, a filled pill when selected (toggle state). */
class TabButton final : public juce::Button
{
public:
    explicit TabButton (const juce::String& name) : juce::Button (name) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

/** Round button with a filled icon (play / pause). */
class IconButton final : public juce::Button
{
public:
    IconButton() : juce::Button ({}) {}
    void setIcon (juce::Path p) { icon = std::move (p); repaint(); }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    juce::Path icon;
};

/** Volume as a row of segments (0 .. 1.5; unity is marked). Drag or click to set, double-click resets. */
class SegmentBar final : public juce::Component
{
public:
    std::function<void (float)> onChange;
    void setValue (float v) { value = juce::jlimit (0.0f, maxValue, v); repaint(); }
    float getValue() const { return value; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { setFromX (e.position.x); }
    void mouseDrag (const juce::MouseEvent& e) override { setFromX (e.position.x); }
    void mouseDoubleClick (const juce::MouseEvent&) override { setValue (1.0f); if (onChange) onChange (value); }

    static constexpr int segments = 15;
    static constexpr float maxValue = 1.5f;

private:
    void setFromX (float x);
    float value = 1.0f;
};

/** Press and drag to drop a file onto a DAW track (or into a folder). */
class DragHandle final : public juce::Component, public juce::SettableTooltipClient
{
public:
    DragHandle() { setMouseCursor (juce::MouseCursor::DraggingHandCursor); setTooltip ("Drag onto a track in your DAW"); }
    void setFile (juce::File f) { file = std::move (f); repaint(); }
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

private:
    juce::File file;
    bool dragging = false;
};

/** One stem: colour dot, name, mute, solo, volume, drag-to-DAW. */
class StemRow final : public juce::Component
{
public:
    StemRow (StemPlayer&, int index, juce::String name);
    /** Instrumental row: no player controls, just drag. */
    explicit StemRow (juce::String name);

    void setFile (const juce::File& f) { drag.setFile (f); }
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    StemPlayer* player = nullptr;
    int index = -1;
    juce::String name;
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    SegmentBar volume;
    DragHandle drag;
};
} // namespace juststem::ui
