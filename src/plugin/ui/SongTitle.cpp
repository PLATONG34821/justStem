#include "SongTitle.h"

namespace juststem::ui
{
namespace
{
juce::Font tagFont() { return capsFont (10.0f); }
constexpr int tagGap = 10; // between the title and the HQ tag
} // namespace

void SongTitle::setSong (SongResultPtr s)
{
    if (s == song) return;
    song = std::move (s);
    shownMs = juce::Time::getMillisecondCounterHiRes();
    updateTimer();
    repaint();
}

void SongTitle::updateTimer()
{
    if (isVisible() && song != nullptr && layout().slides()) startTimerHz (30);
    else stopTimer();
}

SongTitle::Layout SongTitle::layout() const
{
    Layout l;
    auto t = getLocalBounds();
    l.icon = t.removeFromLeft (16);
    t.removeFromLeft (10);
    if (song == nullptr) return l;
    l.tagW = song->isHQ() ? juce::roundToInt (juce::GlyphArrangement::getStringWidth (tagFont(), "HQ")) + 14 : 0;
    l.textW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (textFont(), song->title)) + 1;
    // drawText can't take a baseline, so place the box so its centred text lands on the top bar's baseline.
    const auto f = textFont();
    const int y = baselineFor (f, getLocalBounds()) - juce::roundToInt (baselineOffset (f)) - t.getHeight() / 2;
    l.text = t.withY (y).withWidth (juce::jmax (0, t.getWidth() - (l.tagW > 0 ? l.tagW + tagGap : 0)));
    return l;
}

void SongTitle::paint (juce::Graphics& g)
{
    if (song == nullptr) return;
    const auto l = layout();
    drawIcon (g, icons::note(), l.icon.toFloat().withSizeKeepingCentre (14, 14), Palette::dim);

    g.setColour (Palette::text);
    g.setFont (textFont());
    if (! l.slides()) g.drawText (song->title, l.text, juce::Justification::centredLeft, false);
    else
    {
        // Too long: hold, then slide left to show the rest, and loop (a second copy follows the first).
        const int gap = 48;
        const double holdMs = 2000, pxPerMs = 0.03;
        const double cycle = holdMs + (l.textW + gap) / pxPerMs;
        const double phase = std::fmod (juce::Time::getMillisecondCounterHiRes() - shownMs, cycle);
        const int offset = phase < holdMs ? 0 : juce::roundToInt ((phase - holdMs) * pxPerMs);
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (l.text.withY (0).withHeight (getHeight()));
        for (int x : { l.text.getX() - offset, l.text.getX() - offset + l.textW + gap })
            g.drawText (song->title, l.text.withX (x).withWidth (l.textW), juce::Justification::centredLeft, false);
    }

    if (l.tagW > 0)
    {
        const auto tag = juce::Rectangle<float> ((float) (l.text.getX() + juce::jmin (l.textW, l.text.getWidth()) + tagGap),
                                                 (float) (getHeight() / 2) - 9.0f, (float) l.tagW, 18.0f);
        const auto f = tagFont();
        g.setColour (Palette::accentTo.withAlpha (0.85f));
        g.drawRoundedRectangle (tag.reduced (0.5f), 9.0f, 1.0f);
        g.setFont (f);
        g.drawText ("HQ", tag.translated (f.getHeight() * capsTracking * 0.5f, 0.0f), juce::Justification::centred, false); // centred without the spacing after the Q
    }
}
} // namespace juststem::ui
