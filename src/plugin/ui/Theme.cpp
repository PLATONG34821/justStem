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

// ---------------------------------------------------------------------------- icons (0..1 box)
namespace icons
{
    /** A Phosphor icon (256 x 256 view box) mapped to the 0..1 box, cropped to its 16..240 drawing area.
        Each icon is parsed once and kept (they're drawn in paint calls and on timers). */
    static juce::Path phosphor (const char* svgPathData)
    {
        auto p = juce::Drawable::parseSVGPath (svgPathData);
        p.applyTransform (juce::AffineTransform::translation (-16.0f, -16.0f).scaled (1.0f / 224.0f));
        return p;
    }

    const juce::Path& note() { static const auto p = phosphor ("M209.72,58.25l-80-24A6,6,0,0,0,122,40V153.05A46,46,0,1,0,134,184V96.06l72.28,21.69A6,6,0,0,0,214,112V64A6,6,0,0,0,209.72,58.25ZM88,218a34,34,0,1,1,34-34A34,34,0,0,1,88,218ZM202,103.94l-68-20.4V48.06l68,20.4Z"); return p; } // music-note (light)
    const juce::Path& fileImport() { static const auto p = phosphor ("M212.24,83.76l-56-56A6,6,0,0,0,152,26H56A14,14,0,0,0,42,40V216a14,14,0,0,0,14,14H200a14,14,0,0,0,14-14V88A6,6,0,0,0,212.24,83.76ZM158,46.48,193.52,82H158ZM200,218H56a2,2,0,0,1-2-2V40a2,2,0,0,1,2-2h90V88a6,6,0,0,0,6,6h50V216A2,2,0,0,1,200,218Zm-43.76-62.24a6,6,0,0,1,0,8.48l-24,24a6,6,0,0,1-8.48,0l-24-24a6,6,0,0,1,8.48-8.48L122,169.51V120a6,6,0,0,1,12,0v49.51l13.76-13.75A6,6,0,0,1,156.24,155.76Z"); return p; } // file-arrow-down (light)
    const juce::Path& open() { static const auto p = phosphor ("M222,104a6,6,0,0,1-12,0V54.49l-69.75,69.75a6,6,0,0,1-8.48-8.48L201.51,46H152a6,6,0,0,1,0-12h64a6,6,0,0,1,6,6Zm-38,26a6,6,0,0,0-6,6v72a2,2,0,0,1-2,2H48a2,2,0,0,1-2-2V80a2,2,0,0,1,2-2h72a6,6,0,0,0,0-12H48A14,14,0,0,0,34,80V208a14,14,0,0,0,14,14H176a14,14,0,0,0,14-14V136A6,6,0,0,0,184,130Z"); return p; } // arrow-square-out (light)
    const juce::Path& trash() { static const auto p = phosphor ("M216,50H174V40a22,22,0,0,0-22-22H104A22,22,0,0,0,82,40V50H40a6,6,0,0,0,0,12H50V208a14,14,0,0,0,14,14H192a14,14,0,0,0,14-14V62h10a6,6,0,0,0,0-12ZM94,40a10,10,0,0,1,10-10h48a10,10,0,0,1,10,10V50H94ZM194,208a2,2,0,0,1-2,2H64a2,2,0,0,1-2-2V62H194ZM110,104v64a6,6,0,0,1-12,0V104a6,6,0,0,1,12,0Zm48,0v64a6,6,0,0,1-12,0V104a6,6,0,0,1,12,0Z"); return p; } // trash (light)
    const juce::Path& layers() { static const auto p = phosphor ("M229.18,173a6,6,0,0,1-2.16,8.2l-96,56a6,6,0,0,1-6,0l-96-56a6,6,0,0,1,6-10.36l93,54.23,93-54.23A6,6,0,0,1,229.18,173ZM221,122.82l-93,54.23L35,122.82a6,6,0,0,0-6,10.36l96,56a6,6,0,0,0,6,0l96-56a6,6,0,0,0-6-10.36ZM26,80a6,6,0,0,1,3-5.18l96-56a6,6,0,0,1,6,0l96,56a6,6,0,0,1,0,10.36l-96,56a6,6,0,0,1-6,0l-96-56A6,6,0,0,1,26,80Zm17.91,0L128,129.05,212.09,80,128,31Z"); return p; } // stack (light)
    const juce::Path& play() { static const auto p = phosphor ("M240,128a15.74,15.74,0,0,1-7.6,13.51L88.32,229.65a16,16,0,0,1-16.2.3A15.86,15.86,0,0,1,64,216.13V39.87a15.86,15.86,0,0,1,8.12-13.82,16,16,0,0,1,16.2.3L232.4,114.49A15.74,15.74,0,0,1,240,128Z"); return p; } // play (fill)
    const juce::Path& pause() { static const auto p = phosphor ("M216,48V208a16,16,0,0,1-16,16H160a16,16,0,0,1-16-16V48a16,16,0,0,1,16-16h40A16,16,0,0,1,216,48ZM96,32H56A16,16,0,0,0,40,48V208a16,16,0,0,0,16,16H96a16,16,0,0,0,16-16V48A16,16,0,0,0,96,32Z"); return p; } // pause (fill)
    const juce::Path& folder() { static const auto p = phosphor ("M216,74H130.49l-27.9-27.9a13.94,13.94,0,0,0-9.9-4.1H40A14,14,0,0,0,26,56V200.62A13.39,13.39,0,0,0,39.38,214H216.89A13.12,13.12,0,0,0,230,200.89V88A14,14,0,0,0,216,74ZM40,54H92.69a2,2,0,0,1,1.41.59L113.51,74H38V56A2,2,0,0,1,40,54ZM218,200.89a1.11,1.11,0,0,1-1.11,1.11H39.38A1.4,1.4,0,0,1,38,200.62V86H216a2,2,0,0,1,2,2Z"); return p; } // folder (light)
    const juce::Path& download() { static const auto p = phosphor ("M222,144v64a6,6,0,0,1-6,6H40a6,6,0,0,1-6-6V144a6,6,0,0,1,12,0v58H210V144a6,6,0,0,1,12,0Zm-98.24,4.24a6,6,0,0,0,8.48,0l40-40a6,6,0,0,0-8.48-8.48L134,129.51V32a6,6,0,0,0-12,0v97.51L92.24,99.76a6,6,0,0,0-8.48,8.48Z"); return p; } // download-simple (light)
    const juce::Path& grip() { static const auto p = phosphor ("M108,60A16,16,0,1,1,92,44,16,16,0,0,1,108,60Zm56,16a16,16,0,1,0-16-16A16,16,0,0,0,164,76ZM92,112a16,16,0,1,0,16,16A16,16,0,0,0,92,112Zm72,0a16,16,0,1,0,16,16A16,16,0,0,0,164,112ZM92,180a16,16,0,1,0,16,16A16,16,0,0,0,92,180Zm72,0a16,16,0,1,0,16,16A16,16,0,0,0,164,180Z"); return p; } // dots-six-vertical (bold)
    const juce::Path& plus() { static const auto p = phosphor ("M222,128a6,6,0,0,1-6,6H134v82a6,6,0,0,1-12,0V134H40a6,6,0,0,1,0-12h82V40a6,6,0,0,1,12,0v82h82A6,6,0,0,1,222,128Z"); return p; } // plus (light)
    const juce::Path& minus() { static const auto p = phosphor ("M222,128a6,6,0,0,1-6,6H40a6,6,0,0,1,0-12H216A6,6,0,0,1,222,128Z"); return p; } // minus (light)
    const juce::Path& close() { static const auto p = phosphor ("M204.24,195.76a6,6,0,1,1-8.48,8.48L128,136.49,60.24,204.24a6,6,0,0,1-8.48-8.48L119.51,128,51.76,60.24a6,6,0,0,1,8.48-8.48L128,119.51l67.76-67.75a6,6,0,0,1,8.48,8.48L136.49,128Z"); return p; } // x (light)
    const juce::Path& copy() { static const auto p = phosphor ("M216,34H88a6,6,0,0,0-6,6V82H40a6,6,0,0,0-6,6V216a6,6,0,0,0,6,6H168a6,6,0,0,0,6-6V174h42a6,6,0,0,0,6-6V40A6,6,0,0,0,216,34ZM162,210H46V94H162Zm48-48H174V88a6,6,0,0,0-6-6H94V46H210Z"); return p; } // copy (light)
    const juce::Path& drive() { static const auto p = phosphor ("M128,26C75.29,26,34,49.72,34,80v96c0,30.28,41.29,54,94,54s94-23.72,94-54V80C222,49.72,180.71,26,128,26Zm0,12c44.45,0,82,19.23,82,42s-37.55,42-82,42S46,102.77,46,80,83.55,38,128,38Zm82,138c0,22.77-37.55,42-82,42s-82-19.23-82-42V154.79C62,171.16,92.37,182,128,182s66-10.84,82-27.21Zm0-48c0,22.77-37.55,42-82,42s-82-19.23-82-42V106.79C62,123.16,92.37,134,128,134s66-10.84,82-27.21Z"); return p; } // database (light)
    const juce::Path& paste() { static const auto p = phosphor ("M166,152a6,6,0,0,1-6,6H96a6,6,0,0,1,0-12h64A6,6,0,0,1,166,152Zm-6-38H96a6,6,0,0,0,0,12h64a6,6,0,0,0,0-12Zm54-66V216a14,14,0,0,1-14,14H56a14,14,0,0,1-14-14V48A14,14,0,0,1,56,34H93.17a45.91,45.91,0,0,1,69.66,0H200A14,14,0,0,1,214,48ZM94,64v2h68V64a34,34,0,0,0-68,0ZM202,48a2,2,0,0,0-2-2H170.33A45.77,45.77,0,0,1,174,64v8a6,6,0,0,1-6,6H88a6,6,0,0,1-6-6V64a45.77,45.77,0,0,1,3.67-18H56a2,2,0,0,0-2,2V216a2,2,0,0,0,2,2H200a2,2,0,0,0,2-2Z"); return p; } // clipboard-text (light)
    const juce::Path& split()
    {
        static const auto icon = [] {
            // A prism: one beam in on the left face, three rays fanning out of the right face.
            juce::Path p;
            p.startNewSubPath (0.36f, 0.10f);
            p.lineTo (0.06f, 0.80f);
            p.lineTo (0.66f, 0.80f);
            p.closeSubPath();
            p.startNewSubPath (0.00f, 0.58f);
            p.lineTo (0.19f, 0.50f);
            const float rays[][3] = { { 0.49f, 0.40f, 0.20f }, { 0.53f, 0.49f, 0.48f }, { 0.57f, 0.58f, 0.76f } };
            for (const auto& r : rays)
            {
                p.startNewSubPath (r[0], r[1]);
                p.lineTo (1.0f, r[2]);
            }
            // Stroked here, as the Phosphor icons are filled outlines (Light weight: ~12/224 of the box).
            juce::Path outline;
            juce::PathStrokeType (0.055f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (outline, p);
            return outline;
        }();
        return icon;
    }
} // namespace icons

void drawIcon (juce::Graphics& g, const juce::Path& icon, juce::Rectangle<float> area, juce::Colour colour)
{
    g.setColour (colour);
    g.fillPath (icon, juce::AffineTransform::scale (area.getWidth(), area.getHeight()).translated (area.getX(), area.getY()));
}

// ---------------------------------------------------------------------------- pill button
// The text and the icon both scale with the button's height.
static constexpr float pillScale = 0.42f, pillIconGap = 8.0f;
static juce::Font pillFont (int height) { return capsFont (std::min (12.0f, (float) height * pillScale)); }
static float pillIconSize (int height) { return std::min (14.0f, std::round ((float) height * pillScale)); }

int PillButton::idealWidth (int height) const
{
    if (getButtonText().isEmpty()) return height;
    const auto f = pillFont (height);
    const float textW = juce::GlyphArrangement::getStringWidth (f, getButtonText().toUpperCase());
    const float iconW = icon.isEmpty() ? 0.0f : pillIconSize (height) + pillIconGap;
    return (int) std::ceil (textW + iconW) + 2 * juce::jmax (14, height / 2);
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
    setColour (juce::ScrollBar::thumbColourId, juce::Colours::white.withAlpha (0.16f));
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
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
    return pillFont (height);
}

void DashboardLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto f = getTextButtonFont (b, b.getHeight());
    g.setFont (f);
    auto c = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
    g.setColour (c);
    const auto text = b.getButtonText().toUpperCase();

    auto* pill = dynamic_cast<PillButton*> (&b);
    if (pill == nullptr || pill->getIcon().isEmpty())
    {
        g.drawText (text, b.getLocalBounds(), juce::Justification::centred, false);
        return;
    }
    // Icon then text, centred together.
    const float h = (float) b.getHeight(), iconS = std::round (pillIconSize (b.getHeight()) * pill->iconScale);
    const float textW = text.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (f, text);
    const float gap = text.isEmpty() ? 0.0f : pillIconGap;
    const float x = std::round (((float) b.getWidth() - (iconS + gap + textW)) * 0.5f);
    drawIcon (g, pill->getIcon(), { x, std::round ((h - iconS) * 0.5f), iconS, iconS }, c);
    if (textW > 0)
    {
        g.setColour (c);
        g.drawText (text, juce::Rectangle<float> (x + iconS + gap, 0.0f, textW + 4.0f, h), juce::Justification::centredLeft, false);
    }
}

static juce::Font toggleFont() { return capsFont (11.0f); }

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
    g.setColour (on ? Palette::text : Palette::dim);
    const auto f = toggleFont();
    g.setFont (f);
    // Right-aligned, nudged by the letter spacing after the last letter, so the label ends flush with the
    // button's right edge (which the editor puts on the window margin).
    g.drawText (b.getButtonText().toUpperCase(), r.withTrimmedLeft (8.0f).translated (f.getHeight() * capsTracking, 0.0f),
                juce::Justification::centredRight, false);
}

int DashboardLookAndFeel::toggleWidth (const juce::ToggleButton& b)
{
    return 38 + (int) std::ceil (juce::GlyphArrangement::getStringWidth (toggleFont(), b.getButtonText().toUpperCase()));
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

void DashboardLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int w, int h, bool vertical,
                                          int thumbStart, int thumbSize, bool over, bool down)
{
    // Just a thin rounded thumb, brighter while hovered or dragged.
    if (thumbSize <= 0) return;
    const auto thumb = vertical ? juce::Rectangle<int> (x, thumbStart, w, thumbSize) : juce::Rectangle<int> (thumbStart, y, thumbSize, h);
    auto r = thumb.toFloat().reduced (vertical ? 1.0f : 2.0f, vertical ? 2.0f : 1.0f);
    const auto c = bar.findColour (juce::ScrollBar::thumbColourId);
    g.setColour (over || down ? c.withMultipliedAlpha (1.8f) : c);
    g.fillRoundedRectangle (r, std::min (r.getWidth(), r.getHeight()) * 0.5f);
}
} // namespace juststem::ui
