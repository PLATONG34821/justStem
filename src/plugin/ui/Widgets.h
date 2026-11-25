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
        bool operator== (const State& o) const
        {
            return juce::exactlyEqual (fraction, o.fraction) && slots == o.slots && activeSlot == o.activeSlot
                && value == o.value && unit == o.unit && caption == o.caption;
        }
    };
    /** Repaints only when something changed (it's set many times a second while working). */
    void set (State s)
    {
        if (s == state) return;
        state = std::move (s);
        setTitle ((state.caption + ": " + state.value + " " + state.unit).trim()); // what a screen reader says
        repaint();
    }
    void paint (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
    }

    /** Vertical centres of the caption, value and unit lines, in the gauge's own coordinates,
        so text beside the gauge can line up with them. */
    struct Lines { float caption, value, unit; };
    Lines lines() const;

private:
    /** Where everything goes, for paint() and lines(). */
    struct Layout
    {
        float size;
        juce::Rectangle<float> outer, disc, caption, value, unit;
    };
    Layout layout() const;

    State state;
};

/** Text the editor paints itself (tempo, loudness), for screen readers: an invisible component over it,
    which lets the mouse through. Its title is the text. */
class ReadoutText final : public juce::Component
{
public:
    ReadoutText() { setInterceptsMouseClicks (false, false); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
    }
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

/** Volume as a row of segments (0 .. 1.5; unity is marked). Drag or click to set, double-click resets.
    With the keyboard: arrows move a segment, Home resets; screen readers see a slider in percent. */
class SegmentBar final : public juce::Component
{
public:
    SegmentBar() { setWantsKeyboardFocus (true); }
    std::function<void (float)> onChange;
    void setValue (float v);
    float getValue() const { return value; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { setFromX (e.position.x); }
    void mouseDrag (const juce::MouseEvent& e) override { setFromX (e.position.x); }
    void mouseDoubleClick (const juce::MouseEvent&) override { change (1.0f); }
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    /** Sets the value and tells onChange, as the user changing it does. */
    void change (float v);

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
    void mouseDown (const juce::MouseEvent&) override { dragging = false; }
    void mouseDrag (const juce::MouseEvent&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        // Named by its row (see StemRow); dragging needs a mouse, and Save all is the keyboard way.
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
    }

private:
    juce::File file;
    bool dragging = false;
};

/** One stem: colour dot, name, mute, solo, volume, drag-to-DAW. */
class StemRow final : public juce::Component
{
public:
    StemRow (StemPlayer&, int index, juce::String name);
    /** Full mix / Instrumental row: no player controls, just drag. */
    StemRow (juce::String name, const juce::Path& icon);

    void setFile (const juce::File& f) { drag.setFile (f); }
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    StemPlayer* player = nullptr;
    int index = -1;
    juce::String name;
    const juce::Path* icon = nullptr; // rows without a player
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    SegmentBar volume;
    DragHandle drag;
};
} // namespace juststem::ui
