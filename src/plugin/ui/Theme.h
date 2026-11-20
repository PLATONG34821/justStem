#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** The look: a dark automotive-dashboard style. Charcoal gradient with a diagonal glass sheen,
    translucent panels with hairline borders, thin type (Inter, embedded so it's the same on every
    OS), thin line icons, and colour used only as small accents. */
namespace juststem::ui
{
struct Palette
{
    static inline const juce::Colour
        bgTop { 0xff2c2d30 }, bgBottom { 0xff141517 },
        panelBorder { 0x1affffff }, hairline { 0x14ffffff },
        text { 0xffeeeeef }, dim { 0xff9a9ca1 }, faint { 0xff5f6268 },
        accentFrom { 0xff2c5a99 }, accentTo { 0xffa8cbf2 }, // the blue gauge arc
        highlight { 0xffffd23f },                         // small yellow accents (solo)
        warn { 0xffe9a23b }, bad { 0xffe5534b };

    /** Muted per-stem colours (vocals, drums, bass, guitar, piano, other). */
    static juce::Colour forStem (int index);
};

enum class Weight { ExtraLight, Light, Regular, SemiBold };

/** Inter at a weight and size, with optional letter spacing (kerning factor, e.g. 0.12). */
juce::Font font (Weight, float size, float tracking = 0.0f);

/** Letter spacing of the caps style. Spaced text has this much extra space after its last letter, which
    right-aligned or centred caps text makes up for. */
constexpr float capsTracking = 0.14f;

/** Small uppercase label style used for headings ("TEMPO", "SOURCE"). */
inline juce::Font capsFont (float size = 11.5f) { return font (Weight::Regular, size, capsTracking); }

/** How far below a line's centre a single line of text in `f` sits on its baseline, so it looks centred. */
inline float baselineOffset (const juce::Font& f) { return (f.getAscent() - f.getDescent()) * 0.5f; }

/** The baseline that centres one line of `f` vertically in `row`. */
inline int baselineFor (const juce::Font& f, juce::Rectangle<int> row) { return juce::roundToInt ((float) row.getCentreY() + baselineOffset (f)); }

/** Takes `w` off the right of `r` and puts `c` there, `w` x `h`, centred vertically (the round icon buttons). */
inline void placeRight (juce::Rectangle<int>& r, juce::Component& c, int w, int h) { c.setBounds (r.removeFromRight (w).withSizeKeepingCentre (w, h)); }

/** Charcoal gradient + diagonal glass sheen. */
void paintBackground (juce::Graphics&, juce::Rectangle<int> area);

/** Icons as filled shapes in a 0..1 box (draw them with drawIcon): Phosphor Icons (MIT, phosphoricons.com),
    Light weight (Fill for play and pause, Bold for the grip dots), plus our own prism for Split. */
namespace icons
{
    juce::Path note();
    juce::Path fileImport(); // choose a file
    juce::Path split();      // a prism splitting light: start the split
    juce::Path trash();
    juce::Path open();       // a box with an arrow out of it: open a saved song
    juce::Path layers();
    juce::Path play();
    juce::Path pause();
    juce::Path folder();
    juce::Path download();
    juce::Path grip();  // dots
    juce::Path plus();
    juce::Path minus();
    juce::Path close();
    juce::Path copy();
    juce::Path drive();  // storage: a stack of disks
    juce::Path paste();  // clipboard
} // namespace icons

/** Stroke (or fill) an icon path scaled to fit `area`. */
void drawIcon (juce::Graphics&, const juce::Path& icon, juce::Rectangle<float> area, juce::Colour);

/** The usual pill button, with an optional line icon before its text (icon only when the text is empty). */
class PillButton final : public juce::TextButton
{
public:
    explicit PillButton (const juce::String& text = {}, juce::Path iconPath = {}) : juce::TextButton (text), icon (std::move (iconPath)) {}
    const juce::Path& getIcon() const { return icon; }
    /** Width that fits the icon and text at `height` (a circle when there's no text). */
    int idealWidth (int height) const;
    /** Draw the icon larger (or smaller) than usual, e.g. for the round buttons inside the link box. */
    float iconScale = 1.0f;

private:
    juce::Path icon;
};

class DashboardLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    DashboardLookAndFeel();
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int height) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float min, float max,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override { return 5; } // the seek knob; keeps the track close to its bounds
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize,
                        bool over, bool down) override;

    /** Pixel width of a ToggleButton's switch plus its label. */
    static int toggleWidth (const juce::ToggleButton&);
};
} // namespace juststem::ui
