#include "ChordView.h"

namespace covertools::ui
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
    if (seg != currentSegment || moved)
    {
        currentSegment = seg;
        repaint();
    }
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

    // Now / next.
    auto r = now;
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.0f));
    g.drawText ("NOW", r.removeFromLeft (44), juce::Justification::centredLeft, false);
    const auto nowName = currentSegment >= 0 ? name (currentSegment) : juce::String ("--");
    g.setColour (Palette::text);
    g.setFont (font (Weight::Light, 30.0f));
    g.drawText (nowName, r.removeFromLeft (170), juce::Justification::centredLeft, false);
    juce::StringArray next;
    for (int i = currentSegment + 1; i < (int) chords.segments.size() && next.size() < 3; ++i) next.add (name (i));
    if (! next.isEmpty())
    {
        g.setColour (Palette::dim);
        g.setFont (capsFont (11.0f));
        g.drawText ("NEXT", r.removeFromLeft (48), juce::Justification::centredLeft, false);
        g.setFont (font (Weight::Regular, 17.0f));
        g.drawText (next.joinIntoString (juce::String::fromUTF8 ("   \xe2\x86\x92   ")), r.removeFromLeft (360), juce::Justification::centredLeft, true);
    }
    if (transpose != 0)
    {
        g.setColour (Palette::dim);
        g.setFont (font (Weight::Regular, 13.0f));
        g.drawText ("in " + juce::String (song->analysis.key.transposed (transpose).name()), r, juce::Justification::centredRight, false);
    }
    g.setColour (Palette::hairline);
    g.drawHorizontalLine (now.getBottom() + 2, (float) now.getX(), (float) now.getRight());

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
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRoundedRectangle (cell.toFloat().reduced (2.0f, 3.0f), 6.0f);
                const double f = juce::jlimit (0.0, 1.0, (position - bar.start) / std::max (0.01, bar.end - bar.start));
                g.setColour (Palette::accentTo.withAlpha (0.8f));
                g.fillRect (juce::Rectangle<float> ((float) cell.getX() + 2.0f, (float) cell.getBottom() - 5.0f,
                                                    (float) ((cell.getWidth() - 4) * f), 2.0f));
            }
            g.setColour (Palette::hairline.withAlpha (0.25f));
            g.fillRect (cell.getX(), cell.getY() + 8, 1, cell.getHeight() - 16);

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
    scrollTo (firstRow + (w.deltaY < 0 ? 1 : -1));
}
} // namespace covertools::ui
