#include "Theme.h"

#include <BinaryData.h>

namespace juststem::ui
{
juce::Colour Palette::forStem (int i)
{
    static const juce::Colour c[] { juce::Colour (0xffd98aa8), juce::Colour (0xffd9a07a), juce::Colour (0xffd8c47c),
                                    juce::Colour (0xff86c79a), juce::Colour (0xff86b6d9), juce::Colour (0xffaa9ad9) };
    return i >= 0 && i < 6 ? c[i] : Palette::dim;
}

// ---------------------------------------------------------------------------- fonts
static juce::Typeface::Ptr typeface (Weight w)
{
    static const juce::Typeface::Ptr faces[] {
        juce::Typeface::createSystemTypefaceFor (BinaryData::InterExtraLight_ttf, BinaryData::InterExtraLight_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::InterLight_ttf, BinaryData::InterLight_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize),
    };
    return faces[(int) w];
}

juce::Font font (Weight w, float size, float tracking)
{
    return juce::Font (juce::FontOptions (typeface (w)).withHeight (size).withKerningFactor (tracking));
}

// ---------------------------------------------------------------------------- painting helpers
void paintBackground (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto r = area.toFloat();
    g.setGradientFill (juce::ColourGradient (Palette::bgTop, r.getX(), r.getY(), Palette::bgBottom, r.getX(), r.getBottom(), false));
    g.fillRect (r);

    // A soft diagonal band of light, like a reflection on glass.
    juce::Path sheen;
    sheen.startNewSubPath (r.getX() + r.getWidth() * 0.58f, r.getY());
    sheen.lineTo (r.getX() + r.getWidth() * 0.84f, r.getY());
    sheen.lineTo (r.getX() + r.getWidth() * 0.52f, r.getBottom());
    sheen.lineTo (r.getX() + r.getWidth() * 0.26f, r.getBottom());
    sheen.closeSubPath();
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.045f), r.getX() + r.getWidth() * 0.7f, r.getY(),
                                             juce::Colours::white.withAlpha (0.0f), r.getX() + r.getWidth() * 0.4f, r.getBottom(), false));
    g.fillPath (sheen);
}

void paintPanel (juce::Graphics& g, juce::Rectangle<float> r, float radius)
{
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (Palette::panelBorder);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

void paintHeading (juce::Graphics& g, const juce::Path& icon, const juce::String& text, juce::Rectangle<int> area,
                   juce::Justification just)
{
    const auto f = capsFont (12.0f);
    const int textW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, text.toUpperCase())) + 4;
    const int iconW = icon.isEmpty() ? 0 : 22;
    auto content = juce::Rectangle<int> (0, 0, iconW + textW, area.getHeight());
    if (just.testFlags (juce::Justification::horizontallyCentred)) content.setCentre (area.getCentre());
    else if (just.testFlags (juce::Justification::right)) content.setPosition (area.getRight() - content.getWidth(), area.getY());
    else content.setPosition (area.getPosition());
    if (iconW > 0) drawIcon (g, icon, content.removeFromLeft (iconW).toFloat().withSizeKeepingCentre (14, 14), Palette::dim);
    g.setColour (Palette::dim);
    g.setFont (f);
    g.drawText (text.toUpperCase(), content, juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------------------- icons (0..1 box)
namespace icons
{
    juce::Path note()
    {
        juce::Path p;
        p.addEllipse (0.12f, 0.66f, 0.30f, 0.24f);
        p.startNewSubPath (0.42f, 0.78f);
        p.lineTo (0.42f, 0.08f);
        p.quadraticTo (0.70f, 0.18f, 0.86f, 0.36f);
        return p;
    }
    juce::Path waveform()
    {
        juce::Path p;
        const float h[] { 0.25f, 0.55f, 0.85f, 0.45f, 0.70f, 0.30f, 0.60f };
        for (int i = 0; i < 7; ++i)
        {
            const float x = 0.08f + (float) i * 0.14f;
            p.startNewSubPath (x, 0.5f - h[i] / 2);
            p.lineTo (x, 0.5f + h[i] / 2);
        }
        return p;
    }
    juce::Path gauge()
    {
        juce::Path p;
        p.addCentredArc (0.5f, 0.6f, 0.42f, 0.42f, 0.0f, -2.2f, 2.2f, true);
        p.startNewSubPath (0.5f, 0.6f);
        p.lineTo (0.72f, 0.32f);
        return p;
    }
    juce::Path key()
    {
        juce::Path p;
        p.addEllipse (0.06f, 0.30f, 0.40f, 0.40f);
        p.startNewSubPath (0.46f, 0.5f);
        p.lineTo (0.94f, 0.5f);
        p.lineTo (0.94f, 0.68f);
        p.startNewSubPath (0.78f, 0.5f);
        p.lineTo (0.78f, 0.64f);
        return p;
    }
    juce::Path layers()
    {
        juce::Path p;
        for (float y : { 0.20f, 0.50f, 0.80f })
        {
            p.startNewSubPath (0.06f, y);
            p.lineTo (0.94f, y);
        }
        return p;
    }
    juce::Path play()
    {
        juce::Path p;
        p.addTriangle (0.22f, 0.10f, 0.22f, 0.90f, 0.92f, 0.50f);
        return p;
    }
    juce::Path pause()
    {
        juce::Path p;
        p.addRoundedRectangle (0.18f, 0.10f, 0.22f, 0.80f, 0.04f);
        p.addRoundedRectangle (0.60f, 0.10f, 0.22f, 0.80f, 0.04f);
        return p;
    }
    juce::Path folder()
    {
        juce::Path p;
        p.startNewSubPath (0.06f, 0.24f);
        p.lineTo (0.38f, 0.24f);
        p.lineTo (0.48f, 0.34f);
        p.lineTo (0.94f, 0.34f);
        p.lineTo (0.94f, 0.84f);
        p.lineTo (0.06f, 0.84f);
        p.closeSubPath();
        return p;
    }
    juce::Path download()
    {
        juce::Path p;
        p.startNewSubPath (0.5f, 0.08f);
        p.lineTo (0.5f, 0.62f);
        p.startNewSubPath (0.26f, 0.40f);
        p.lineTo (0.5f, 0.64f);
        p.lineTo (0.74f, 0.40f);
        p.startNewSubPath (0.10f, 0.70f);
        p.lineTo (0.10f, 0.90f);
        p.lineTo (0.90f, 0.90f);
        p.lineTo (0.90f, 0.70f);
        return p;
    }
    juce::Path grip()
    {
        juce::Path p;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 2; ++c)
                p.addEllipse (0.30f + (float) c * 0.30f, 0.14f + (float) r * 0.30f, 0.14f, 0.14f);
        return p;
    }
    juce::Path link()
    {
        juce::Path p;
        p.addRoundedRectangle (0.04f, 0.30f, 0.52f, 0.40f, 0.20f);
        p.addRoundedRectangle (0.44f, 0.30f, 0.52f, 0.40f, 0.20f);
        return p;
    }
} // namespace icons

void drawIcon (juce::Graphics& g, const juce::Path& icon, juce::Rectangle<float> area, juce::Colour colour, bool filled,
               float strokeWidth)
{
    const auto t = juce::AffineTransform::scale (area.getWidth(), area.getHeight()).translated (area.getX(), area.getY());
    g.setColour (colour);
    if (filled) g.fillPath (icon, t);
    else
    {
        juce::Path scaled (icon);
        scaled.applyTransform (t);
        g.strokePath (scaled, juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

// ---------------------------------------------------------------------------- look and feel
DashboardLookAndFeel::DashboardLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::bgBottom);
    setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour (juce::TextButton::buttonOnColourId, Palette::text);
    setColour (juce::TextButton::textColourOffId, Palette::text);
    setColour (juce::TextButton::textColourOnId, Palette::bgBottom);
    setColour (juce::TextEditor::textColourId, Palette::text);
    setColour (juce::TextEditor::highlightColourId, Palette::accentFrom.withAlpha (0.6f));
    setColour (juce::CaretComponent::caretColourId, Palette::text);
    setColour (juce::Label::textColourId, Palette::text);
    setColour (juce::ToggleButton::textColourId, Palette::dim);
    setColour (juce::TooltipWindow::backgroundColourId, Palette::bgTop);
    setColour (juce::TooltipWindow::textColourId, Palette::text);
}

juce::Typeface::Ptr DashboardLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    // Fonts created without a typeface (TextEditor, tooltips, ...) use Inter too.
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return font (f.isBold() ? Weight::SemiBold : Weight::Regular, f.getHeight()).getTypefacePtr();
    return LookAndFeel_V4::getTypefaceForFont (f);
}

void DashboardLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool over, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() * 0.5f; // pill
    auto fill = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : base;
    if (! b.isEnabled()) fill = fill.withMultipliedAlpha (0.4f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    if (! b.getToggleState() && base.getAlpha() < 40)
    {
        g.setColour (Palette::panelBorder.withMultipliedAlpha (over ? 2.5f : 1.6f));
        g.drawRoundedRectangle (r, radius, 1.0f);
    }
    if (down)
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (r, radius);
    }
}

juce::Font DashboardLookAndFeel::getTextButtonFont (juce::TextButton&, int height)
{
    return capsFont (std::min (12.0f, (float) height * 0.42f));
}

void DashboardLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    auto c = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    g.setColour (b.isEnabled() ? c : c.withMultipliedAlpha (0.4f));
    g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds(), juce::Justification::centred, false);
}

void DashboardLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    // A small switch, then the label.
    auto r = b.getLocalBounds().toFloat();
    auto sw = r.removeFromLeft (30.0f).withSizeKeepingCentre (28.0f, 16.0f);
    const bool on = b.getToggleState();
    g.setColour (on ? Palette::accentTo.withAlpha (0.85f) : juce::Colours::white.withAlpha (0.10f));
    g.fillRoundedRectangle (sw, 8.0f);
    g.setColour (on ? Palette::bgBottom : Palette::dim);
    g.fillEllipse (on ? sw.getRight() - 14.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, 12.0f, 12.0f);
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.0f));
    g.drawText (b.getButtonText().toUpperCase(), r.withTrimmedLeft (8.0f), juce::Justification::centredLeft, false);
}

void DashboardLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                             juce::Slider::SliderStyle, juce::Slider& s)
{
    // A thin line with a small knob (the seek bar).
    const float cy = (float) y + (float) h * 0.5f;
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.fillRoundedRectangle ((float) x, cy - 1.0f, (float) w, 2.0f, 1.0f);
    if (! s.isEnabled()) return;
    g.setColour (Palette::text);
    g.fillRoundedRectangle ((float) x, cy - 1.0f, pos - (float) x, 2.0f, 1.0f);
    g.fillEllipse (pos - 5.0f, cy - 5.0f, 10.0f, 10.0f);
}

void DashboardLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (juce::Colours::black.withAlpha (0.22f));
    g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, (float) h * 0.5f);
}

void DashboardLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) ? Palette::accentTo.withAlpha (0.6f) : Palette::panelBorder);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, (float) h * 0.5f, 1.0f);
}
} // namespace juststem::ui
