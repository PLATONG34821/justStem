#include "StoragePanel.h"

namespace juststem::ui
{
namespace
{
juce::String size (juce::int64 bytes) { return juce::File::descriptionOfSizeInBytes (bytes); }
} // namespace

/** One saved song: title, size and date, Delete (or "in use"). */
struct StoragePanel::SongRow final : public juce::Component
{
    SongRow (storage::SavedSong s, bool inUse) : song (std::move (s)), used (inUse)
    {
        deleteButton.setEnabled (! used);
        addAndMakeVisible (deleteButton);
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (Palette::hairline);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        auto r = getLocalBounds().withTrimmedRight (100);
        g.setColour (Palette::text);
        g.setFont (font (Weight::Regular, 13.5f));
        g.drawText (song.folder.getFileName(), r.removeFromTop (getHeight() / 2 + 2).withTrimmedTop (4), juce::Justification::bottomLeft, true);
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Regular, 11.5f));
        g.drawText (size (song.bytes) + "   " + song.created.formatted ("%d %b %Y") + (used ? "   in use" : ""), r,
                    juce::Justification::topLeft, true);
    }
    void resized() override { deleteButton.setBounds (getLocalBounds().removeFromRight (90).withSizeKeepingCentre (90, 26)); }

    storage::SavedSong song;
    bool used;
    juce::TextButton deleteButton { "Delete" };
};

StoragePanel::StoragePanel (std::function<juce::File()> inUse, std::function<bool()> busy)
    : songInUse (std::move (inUse)), isBusy (std::move (busy))
{
    closeButton.onClick = [this] { if (onClose) onClose(); };
    showFolderButton.onClick = [] { paths::stems().revealToUser(); };
    deleteAllButton.onClick = [this] {
        confirm ("Delete all songs?",
                 "The stems of " + juce::String ((int) rows.size()) + " song(s) will be moved to the Trash. "
                     "The song open in this plug-in is kept.",
                 "Move to Trash", [this] {
                     for (auto* row : rows)
                         if (! row->used) storage::moveSongToTrash (row->song);
                     refresh();
                 });
    };
    clearCacheButton.onClick = [this] {
        confirm ("Clear the AI model cache?", "It's rebuilt automatically the next time you split a song (about 25 seconds, once).",
                 "Clear", [this] { storage::clearFolder (paths::coremlCache()); refresh(); });
    };
    removeToolsButton.onClick = [this] {
        confirm ("Remove the YouTube tools?", "yt-dlp and Deno are downloaded again the next time you paste a YouTube link.",
                 "Remove", [this] { storage::clearFolder (paths::tools()); refresh(); });
    };
    clearDownloadsButton.onClick = [this] { storage::clearFolder (paths::downloads()); refresh(); };

    for (auto* b : { &closeButton, &deleteAllButton, &showFolderButton, &clearCacheButton, &removeToolsButton, &clearDownloadsButton })
        addAndMakeVisible (*b);
    viewport.setViewedComponent (&listContent, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (6);
    viewport.setSingleStepSizes (46, 46); // a row per step
    viewport.onScroll = [this] { repaint (viewport.getBounds()); };
    addAndMakeVisible (viewport);
    refresh();
}

StoragePanel::~StoragePanel() = default;

void StoragePanel::refresh()
{
    const bool busy = isBusy();
    const auto inUse = songInUse();
    songs = storage::savedSongs();
    songsBytes = 0;
    rows.clear();
    for (const auto& s : songs)
    {
        songsBytes += s.bytes;
        auto* row = rows.add (new SongRow (s, s.folder == inUse));
        row->deleteButton.setEnabled (! busy && ! row->used);
        row->deleteButton.onClick = [this, row] {
            confirm ("Delete this song?", "\"" + row->song.title + "\" (" + size (row->song.bytes) + ") will be moved to the Trash.",
                     "Move to Trash", [this, song = row->song] { storage::moveSongToTrash (song); refresh(); });
        };
        listContent.addAndMakeVisible (row);
    }
    cacheBytes = storage::folderSize (paths::coremlCache());
    toolsBytes = storage::folderSize (paths::tools());
    downloadsBytes = storage::folderSize (paths::downloads());

    const bool anyDeletable = std::any_of (rows.begin(), rows.end(), [] (SongRow* r) { return ! r->used; });
    deleteAllButton.setEnabled (! busy && anyDeletable);
    clearCacheButton.setEnabled (! busy && cacheBytes > 0);
    removeToolsButton.setEnabled (! busy && toolsBytes > 0);
    clearDownloadsButton.setEnabled (! busy && downloadsBytes > 0);
    resized();
    repaint();
}

juce::Rectangle<int> StoragePanel::card() const { return getLocalBounds().withSizeKeepingCentre (600, 484); } // list fits 4 whole rows

void StoragePanel::resized()
{
    auto r = card().reduced (28, 24);
    auto top = r.removeFromTop (44);
    closeButton.setBounds (top.removeFromRight (90).withSizeKeepingCentre (90, 28));
    r.removeFromTop (14);

    songsHeader = r.removeFromTop (30);
    auto h = songsHeader;
    deleteAllButton.setBounds (h.removeFromRight (104).withSizeKeepingCentre (104, 26));
    h.removeFromRight (8);
    showFolderButton.setBounds (h.removeFromRight (120).withSizeKeepingCentre (120, 26));

    otherArea = r.removeFromBottom (3 * 44);
    r.removeFromBottom (28); // room for the busy warning above the footer
    // The scrollbar sits out in the card's margin, so the rows keep the full width and their
    // Delete buttons line up with the Clear / Remove buttons below.
    // Whole rows only, so the list doesn't stop halfway through a song (the rest goes to the gap above the footer).
    constexpr int rowH = 46, scrollGap = 12;
    auto list = r.withTrimmedTop (4);
    list.setHeight (std::max (rowH, list.getHeight() / rowH * rowH));
    viewport.setBounds (list.withTrimmedRight (-(scrollGap + viewport.getScrollBarThickness())));
    listContent.setSize (r.getWidth(), rows.size() * rowH);
    for (int i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, i * rowH, listContent.getWidth(), rowH);

    auto other = otherArea;
    for (auto* b : { &clearCacheButton, &removeToolsButton, &clearDownloadsButton })
        b->setBounds (other.removeFromTop (44).removeFromRight (90).withSizeKeepingCentre (90, 26));
}

void StoragePanel::paintOverChildren (juce::Graphics& g)
{
    // Fade the list out at an edge that has more songs past it.
    const auto v = viewport.getBounds().withTrimmedRight (viewport.getScrollBarThickness() + 12).toFloat();
    const int pos = viewport.getViewPositionY(), hidden = listContent.getHeight() - viewport.getViewHeight();
    const auto bg = Palette::bgTop.darker (0.25f); // the card colour
    constexpr float fade = 18.0f;
    if (pos > 0)
    {
        g.setGradientFill (juce::ColourGradient (bg, 0, v.getY(), bg.withAlpha (0.0f), 0, v.getY() + fade, false));
        g.fillRect (v.withHeight (fade));
    }
    if (pos < hidden)
    {
        g.setGradientFill (juce::ColourGradient (bg.withAlpha (0.0f), 0, v.getBottom() - fade, bg, 0, v.getBottom(), false));
        g.fillRect (v.withTrimmedTop (v.getHeight() - fade));
    }
}

void StoragePanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.6f));
    const auto c = card();
    g.setColour (Palette::bgTop.darker (0.25f));
    g.fillRoundedRectangle (c.toFloat(), 18.0f);
    g.setColour (Palette::panelBorder);
    g.drawRoundedRectangle (c.toFloat().reduced (0.5f), 18.0f, 1.0f);

    auto r = c.reduced (28, 24);
    auto top = r.removeFromTop (44);
    g.setColour (Palette::text);
    g.setFont (font (Weight::Light, 26.0f));
    g.drawText ("Storage", top.removeFromLeft (130), juce::Justification::centredLeft, false);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Regular, 13.0f));
    g.drawText (size (songsBytes + cacheBytes + toolsBytes + downloadsBytes) + " used", top, juce::Justification::centredLeft, false);

    g.setColour (Palette::dim);
    g.setFont (capsFont (11.5f));
    g.drawText ("SONGS   " + juce::String ((int) songs.size()) + "   " + size (songsBytes), songsHeader, juce::Justification::centredLeft, false);
    if (songs.empty())
    {
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Light, 15.0f));
        g.drawText ("No saved songs", viewport.getBounds(), juce::Justification::centred, false);
    }
    if (isBusy())
    {
        g.setColour (Palette::warn);
        g.setFont (font (Weight::Regular, 12.0f));
        g.drawText ("Wait until the current song is finished to delete anything.", otherArea.withY (otherArea.getY() - 26).withHeight (16),
                    juce::Justification::centredLeft, false);
    }

    // Cache / tools / downloads rows.
    struct Item { const char* name; const char* note; juce::int64 bytes; };
    const Item items[] { { "AI model cache", "Rebuilt automatically (about 25 s, once)", cacheBytes },
                         { "YouTube tools", "yt-dlp and Deno, downloaded again when needed", toolsBytes },
                         { "Temporary downloads", "YouTube audio waiting to be split", downloadsBytes } };
    // They sit in a darker footer, set apart from the songs.
    {
        const auto cardF = c.toFloat();
        const auto footer = cardF.withTop ((float) otherArea.getY() - 8.0f);
        juce::Path shape;
        shape.addRoundedRectangle (footer.getX(), footer.getY(), footer.getWidth(), footer.getHeight(), 18.0f, 18.0f,
                                   false, false, true, true); // square top, rounded bottom like the card
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillPath (shape);
        g.setColour (Palette::hairline);
        g.drawHorizontalLine ((int) footer.getY(), cardF.getX() + 1.0f, cardF.getRight() - 1.0f);
    }
    auto other = otherArea;
    for (const auto& item : items)
    {
        auto row = other.removeFromTop (44).withTrimmedRight (100);
        g.setColour (Palette::hairline);
        if (row.getY() > otherArea.getY()) g.drawHorizontalLine (row.getY(), (float) otherArea.getX(), (float) otherArea.getRight());
        g.setColour (Palette::text);
        g.setFont (font (Weight::Regular, 13.5f));
        g.drawText (juce::String (item.name) + "   " + size (item.bytes), row.removeFromTop (24).withTrimmedTop (4),
                    juce::Justification::bottomLeft, false);
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Regular, 11.5f));
        g.drawText (item.note, row, juce::Justification::topLeft, false);
    }
}

void StoragePanel::mouseDown (const juce::MouseEvent& e)
{
    if (! card().contains (e.getPosition()) && onClose) onClose(); // click outside closes
}

void StoragePanel::confirm (const juce::String& title, const juce::String& message, const juce::String& action, std::function<void()> then)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton (action)
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [safe = juce::Component::SafePointer<StoragePanel> (this), then] (int result) {
                                      if (safe != nullptr && result == 1) then();
                                  });
}
} // namespace juststem::ui
