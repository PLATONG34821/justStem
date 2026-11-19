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
        panel { 0x0dffffff }, panelBorder { 0x1affffff }, hairline { 0x14ffffff },
        text { 0xffeeeeef }, dim { 0xff9a9ca1 }, faint { 0xff5f6268 },
        accentFrom { 0xff2c5a99 }, accentTo { 0xffa8cbf2 }, // the blue gauge arc
        highlight { 0xffffd23f },                         // small yellow accents (solo)
        good { 0xff2fbf71 }, warn { 0xffe9a23b }, bad { 0xffe5534b };

    /** Muted per-stem colours (vocals, drums, bass, guitar, piano, other). */
    static juce::Colour forStem (int index);
};

enum class Weight { ExtraLight, Light, Regular, SemiBold };

/** Inter at a weight and size, with optional letter spacing (kerning factor, e.g. 0.12). */
juce::Font font (Weight, float size, float tracking = 0.0f);

/** Small uppercase label style used for headings ("TEMPO", "SOURCE"). */
inline juce::Font capsFont (float size = 11.5f) { return font (Weight::Regular, size, 0.14f); }

/** Charcoal gradient + diagonal glass sheen. */
void paintBackground (juce::Graphics&, juce::Rectangle<int> area);

/** Translucent card with a hairline border. */
void paintPanel (juce::Graphics&, juce::Rectangle<float> area, float radius = 14.0f);

/** A heading: thin icon + spaced uppercase text. */
void paintHeading (juce::Graphics&, const juce::Path& icon, const juce::String& text, juce::Rectangle<int> area,
                   juce::Justification = juce::Justification::centredLeft);

/** Thin line icons, drawn in a 0..1 box (stroke them with drawIcon). */
namespace icons
{
    juce::Path note();
    juce::Path waveform();
    juce::Path gauge();
    juce::Path key();
    juce::Path layers();
    juce::Path play();  // filled
    juce::Path pause(); // filled
    juce::Path folder();
    juce::Path download();
    juce::Path grip();  // filled dots
    juce::Path link();
} // namespace icons

/** Stroke (or fill) an icon path scaled to fit `area`. */
void drawIcon (juce::Graphics&, const juce::Path& icon, juce::Rectangle<float> area, juce::Colour, bool filled = false,
               float strokeWidth = 1.4f);

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
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
};
} // namespace juststem::ui
