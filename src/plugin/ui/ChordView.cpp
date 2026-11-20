#include "ChordView.h"

namespace juststem::ui
{
void ChordView::setSong (SongResultPtr s)
{
    song = std::move (s);
    bars = song != nullptr ? song->analysis.chords.bars (song->durationSeconds) : std::vector<ChordResult::Bar> {};
    currentBar = currentSegment = -1;
    firstRow = 0;
    setPosition (position);
    repaint();
}

void ChordView::setTranspose (int t)
{
    if (t == transpose) return;
    transpose = t;
    repaint();
}

void ChordView::setPosition (double seconds)
{
    if (song == nullptr) return;
    const auto it = std::upper_bound (bars.begin(), bars.end(), seconds, [] (double v, const ChordResult::Bar& b) { return v < b.start; });
    const int bar = std::max (0, (int) (it - bars.begin()) - 1);
    const int seg = song->analysis.chords.segmentAt (seconds);
    const bool moved = std::abs (seconds - position) > 0.02;
    position = seconds;
    if (bar != currentBar)
    {
        currentBar = bar;
        const int row = bar / barsPerRow;
        if (row < firstRow || row >= firstRow + visibleRows()) scrollTo (row - 1); // keep one row of context above
    }
    if (seg != currentSegment)
    {
        // Slide only when moving on to the next chord in normal playback; a seek just jumps.
        animateFrom = (currentSegment >= 0 && seg == currentSegment + 1) ? currentSegment : -1;
        changeMs = juce::Time::getMillisecondCounterHiRes();
        currentSegment = seg;
        repaint();
    }
    else if (moved || (animateFrom >= 0 && juce::Time::getMillisecondCounterHiRes() - changeMs < slideMs + 50))
        repaint();
}

void ChordView::scrollTo (int row)
{
    firstRow = juce::jlimit (0, juce::jmax (0, rowCount() - visibleRows()), row);
    repaint();
}

juce::String ChordView::name (int segment) const
{
    return chordName (song->analysis.chords.segments[(size_t) segment].chord, song->analysis.key, transpose);
}

juce::String ChordView::chartText (const SongResult& s, int transpose)
{
    const auto& a = s.analysis;
    auto text = s.title + "\n" + "Key: " + juce::String (a.key.transposed (transpose).name())
              + (transpose != 0 ? " (transposed " + juce::String (transpose > 0 ? "+" : "") + juce::String (transpose) + " from "
                                      + juce::String (a.key.name()) + ")"
                                : juce::String())
              + "\nTempo: " + juce::String (juce::roundToInt (a.tempo.bpm)) + " BPM\n\n";
    return text + juce::String (chordChart (a.chords, s.durationSeconds, a.key, transpose));
}

void ChordView::resized()
{
    auto r = getLocalBounds();
    now = r.removeFromTop (52);
    r.removeFromTop (6);
    grid = r;
    scrollTo (firstRow);
}

juce::Rectangle<int> ChordView::barBounds (int bar) const
{
    const int row = bar / barsPerRow - firstRow, col = bar % barsPerRow;
    const int w = (grid.getWidth() - numberW) / barsPerRow;
    return { grid.getX() + numberW + col * w, grid.getY() + row * rowH, w, rowH };
}

void ChordView::paint (juce::Graphics& g)
{
    if (song == nullptr) return;
    const auto& chords = song->analysis.chords;
    if (chords.segments.empty())
    {
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Light, 16.0f));
        g.drawText ("No chords found", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // The progression as one centred line, "A#m  ->  B  ->  F#  ->  G#m  ->  A#m": the chord playing now is
    // big and bright in the middle, the one before fades out on the left, the next three dim to the right.
    // When the chord changes the line slides left, and the new chord grows into place.
    {
        constexpr float bigSize = 30.0f, smallSize = 17.0f;
        const double sinceChange = juce::Time::getMillisecondCounterHiRes() - changeMs;
        const float t = animateFrom >= 0 ? (float) juce::jlimit (0.0, 1.0, sinceChange / slideMs) : 1.0f;
        const float e = 1.0f - std::pow (1.0f - t, 3.0f); // ease out

        struct Item { int segment; float size; juce::Colour colour; };
        std::vector<Item> items;
        const int count = (int) chords.segments.size();
        for (int k = -1; k <= 3; ++k)
        {
            const int s = currentSegment + k;
            if (currentSegment < 0 || s < 0 || s >= count) continue;
            // Size and brightness by distance from "now"; the old and new chord trade them while sliding.
            float size = smallSize, alpha = k < 0 ? 0.35f : k == 1 ? 0.75f : k == 2 ? 0.55f : k == 3 ? 0.38f : 1.0f;
            if (k == 0) size = smallSize + (bigSize - smallSize) * e;
            if (k == -1) { size = bigSize + (smallSize - bigSize) * e; alpha = 1.0f + (0.35f - 1.0f) * e; }
            items.push_back ({ s, size, (k == 0 ? Palette::text : Palette::dim).withAlpha (alpha) });
        }

        const auto arrow = juce::String::fromUTF8 ("\xe2\x86\x92");
        const auto arrowFont = font (Weight::Regular, 15.0f);
        const float arrowW = juce::GlyphArrangement::getStringWidth (arrowFont, arrow), gap = 18.0f;
        auto widthOf = [&] (const Item& it) {
            return juce::GlyphArrangement::getStringWidth (font (it.size >= bigSize - 0.5f ? Weight::Light : Weight::Regular, it.size), name (it.segment));
        };

        // x of each item's left edge, with "now" centred.
        std::vector<float> xs (items.size());
        float x = 0;
        int nowIndex = -1;
        for (size_t n = 0; n < items.size(); ++n)
        {
            if (items[n].segment == currentSegment) nowIndex = (int) n;
            xs[n] = x;
            x += widthOf (items[n]) + gap + arrowW + gap;
        }
        if (nowIndex >= 0)
        {
            const float cx = (float) now.getCentreX();
            float shift = cx - (xs[(size_t) nowIndex] + widthOf (items[(size_t) nowIndex]) * 0.5f);
            // While sliding, start where the previous chord was centred.
            if (nowIndex > 0 && e < 1.0f)
            {
                const float step = (xs[(size_t) nowIndex] + widthOf (items[(size_t) nowIndex]) * 0.5f)
                                 - (xs[(size_t) nowIndex - 1] + widthOf (items[(size_t) nowIndex - 1]) * 0.5f);
                shift += step * (1.0f - e);
            }
            const float baseline = (float) now.getCentreY() + 10.0f;
            for (size_t n = 0; n < items.size(); ++n)
            {
                const auto& it = items[n];
                const float ix = xs[n] + shift;
                const auto f = font (it.size >= bigSize - 0.5f ? Weight::Light : Weight::Regular, it.size);
                g.setColour (chords.segments[(size_t) it.segment].chord.isNone() ? Palette::faint : it.colour);
                g.setFont (f);
                g.drawSingleLineText (name (it.segment), juce::roundToInt (ix), juce::roundToInt (baseline));
                if (n + 1 < items.size())
                {
                    g.setColour (Palette::faint.withAlpha (n < (size_t) nowIndex ? 0.5f : 0.9f));
                    g.setFont (arrowFont);
                    g.drawSingleLineText (arrow, juce::roundToInt (ix + widthOf (it) + gap), juce::roundToInt (baseline - 2.0f));
                }
            }
        }
    }

    // Grid of bars.
    g.saveState();
    g.reduceClipRegion (grid);
    const int lastRow = std::min (rowCount(), firstRow + visibleRows()); // whole rows only
    for (int row = firstRow; row < lastRow; ++row)
    {
        const auto first = barBounds (row * barsPerRow);
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Regular, 11.0f));
        g.drawText (juce::String (row * barsPerRow + 1), first.withX (grid.getX()).withWidth (numberW - 8), juce::Justification::centredLeft, false);

        for (int b = row * barsPerRow; b < std::min ((int) bars.size(), (row + 1) * barsPerRow); ++b)
        {
            const auto cell = barBounds (b);
            const auto& bar = bars[(size_t) b];
            const bool current = b == currentBar;
            if (current)
            {
                // Just a progress line under the current bar (the playing chord is also coloured).
                const double f = juce::jlimit (0.0, 1.0, (position - bar.start) / std::max (0.01, bar.end - bar.start));
                g.setColour (Palette::accentTo.withAlpha (0.8f));
                g.fillRect (juce::Rectangle<float> ((float) cell.getX() + 2.0f, (float) cell.getBottom() - 5.0f,
                                                    (float) ((cell.getWidth() - 4) * f), 2.0f));
            }
            // A short, faint bar-line tick, so it doesn't read like a letter next to the chords.
            g.setColour (Palette::hairline.withAlpha (0.18f));
            g.fillRect (cell.getX(), cell.getCentreY() - 5, 1, 10);

            // Chord names where they start in the bar (a held chord is repeated at the bar line).
            const int inner = cell.getWidth() - 12;
            int nextFreeX = cell.getX();
            for (int s : bar.segments)
            {
                const auto& seg = chords.segments[(size_t) s];
                const double inBar = std::min (seg.end, bar.end) - std::max (seg.start, bar.start);
                if (bar.segments.size() > 1 && inBar < 0.2 * (bar.end - bar.start)) continue; // slivers
                const double f = juce::jlimit (0.0, 1.0, (std::max (seg.start, bar.start) - bar.start) / std::max (0.01, bar.end - bar.start));
                const int x = std::max (nextFreeX, cell.getX() + 10 + (int) (inner * f));
                const auto label = name (s);
                const bool playing = current && s == currentSegment;
                g.setColour (seg.chord.isNone() ? Palette::faint : playing ? Palette::accentTo : Palette::text);
                g.setFont (font (playing ? Weight::SemiBold : Weight::Regular, 15.0f));
                const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label) + 10;
                g.drawText (label, x, cell.getY(), std::min (w, cell.getRight() - x), cell.getHeight(), juce::Justification::centredLeft, true);
                nextFreeX = x + w;
            }
        }
    }
    g.restoreState();

    if (rowCount() > visibleRows()) // scroll position
    {
        const float h = (float) grid.getHeight(), frac = (float) visibleRows() / (float) rowCount();
        const float y = (float) grid.getY() + h * (float) firstRow / (float) rowCount();
        g.setColour (Palette::faint.withAlpha (0.5f));
        g.fillRoundedRectangle ((float) grid.getRight() - 3.0f, y, 3.0f, h * frac, 1.5f);
    }
}

void ChordView::mouseUp (const juce::MouseEvent& e)
{
    if (! grid.contains (e.getPosition()) || e.mouseWasDraggedSinceMouseDown()) return;
    for (int b = firstRow * barsPerRow; b < (int) bars.size(); ++b)
        if (barBounds (b).contains (e.getPosition()))
        {
            if (onSeek) onSeek (bars[(size_t) b].start);
            return;
        }
}

void ChordView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (std::abs (w.deltaY) <= std::abs (w.deltaX)) return; // a sideways swipe isn't for this view
    if (! w.isSmooth) // a mouse wheel: a row per click
    {
        scrollTo (firstRow + (w.deltaY < 0 ? 1 : -1));
        return;
    }
    // A trackpad sends many small steps (JUCE reports its travel in points x 0.5 / 256): a row once
    // they add up to a row's height, like dragging the grid.
    constexpr float perRow = (float) rowH * 0.5f / 256.0f;
    wheelTravel += w.deltaY;
    for (; wheelTravel <= -perRow; wheelTravel += perRow) scrollTo (firstRow + 1);
    for (; wheelTravel >= perRow; wheelTravel -= perRow) scrollTo (firstRow - 1);
}
} // namespace juststem::ui
