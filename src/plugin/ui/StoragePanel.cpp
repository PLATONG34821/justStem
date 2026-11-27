#include "StoragePanel.h"
#include "../../core/Separator.h"
#include "../../core/Transcriber.h"
#include "../Text.h"

#include <algorithm>

namespace juststem::ui
{
namespace
{
juce::String size (juce::int64 bytes) { return juce::File::descriptionOfSizeInBytes (bytes); }
/** The downloaded HQ and piano MIDI models, plus any interrupted model downloads (ensureModel's
    "<name>-<uuid>.part"), but not a shared fast model installed there by the macOS .pkg ("Install for me only"). */
juce::Array<juce::File> downloadedModelFiles()
{
    auto files = paths::models().findChildFiles (juce::File::findFiles, false, "*.part");
    for (const auto name : { model::hq.fileName, model::pianoFile, model::pianoHeadsFile, model::pianoFrontEndFile })
        if (const auto f = paths::models().getChildFile (text::from (name)); f.existsAsFile()) files.add (f);
    return files;
}

constexpr int btn = 32;        // round icon buttons throughout the panel
constexpr int headerH = 80;    // the darker band with the title and Close
constexpr int songRowH = 46;   // a saved song in the list
constexpr int otherRowH = 44;  // the cache / tools / downloads rows in the footer
constexpr int scrollGap = 12;  // between the list and its scrollbar, out in the card's margin
constexpr float radius = 18.0f;

/** The one-time CoreML compile, for each model: "35 s, 2.5 min in HQ mode". */
juce::String compileTimes()
{
    return text::roughDuration (model::fast.coremlCompileSeconds) + ", " + text::roughDuration (model::hq.coremlCompileSeconds) + " in HQ mode";
}

juce::Colour cardColour() { return Palette::bgTop.darker (0.25f); }

/** The title row, centred in the header band. */
juce::Rectangle<int> titleRow (juce::Rectangle<int> card) { return card.withHeight (headerH).reduced (28, (headerH - 44) / 2); }

/** A darker band across the top or bottom of the card, rounded only on the card's side. */
void fillBand (juce::Graphics& g, juce::Rectangle<float> r, bool top)
{
    juce::Path shape;
    shape.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius, top, top, ! top, ! top);
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.fillPath (shape);
}
} // namespace

/** One saved song: title, size and date, Open (off for the song open here) and Delete (off for
    songs open in any justStem). */
struct StoragePanel::SongRow final : public juce::Component
{
    SongRow (storage::SavedSong s, bool inUse, bool here) : song (std::move (s)), used (inUse), openHere (here)
    {
        openButton.setTooltip ("Open this song (no new split)");
        deleteButton.setTooltip ("Delete this song");
        openButton.setTitle ("Open " + song.title); // icon-only: screen readers say these
        deleteButton.setTitle ("Delete " + song.title);
        addAndMakeVisible (deleteButton);
        addAndMakeVisible (openButton);
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().withTrimmedRight (2 * btn + 16);
        g.setColour (Palette::text);
        g.setFont (font (Weight::Regular, 13.5f));
        g.drawText (song.folder.getFileName(), r.removeFromTop (getHeight() / 2 + 2).withTrimmedTop (4), juce::Justification::bottomLeft, true);
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Regular, 11.5f));
        g.drawText (size (song.bytes) + "   " + song.created.formatted ("%d %b %Y") + (used ? "   in use" : ""), r,
                    juce::Justification::topLeft, true);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        placeRight (r, deleteButton, btn, btn);
        r.removeFromRight (8);
        placeRight (r, openButton, btn, btn);
    }

    storage::SavedSong song;
    bool used, openHere;
    PillButton deleteButton { {}, icons::trash() }, openButton { {}, icons::open() };
};

StoragePanel::StoragePanel (std::function<juce::File()> here, std::function<bool()> workingHere,
                            std::function<juce::Array<juce::File>()> inUse, std::function<bool()> busy)
    : songOpenHere (std::move (here)), busyHere (std::move (workingHere)), songsInUse (std::move (inUse)), isBusy (std::move (busy))
{
    setWantsKeyboardFocus (true);
    closeButton.onClick = [this] { if (onClose) onClose(); };
    closeButton.setTooltip ("Close");
    showFolderButton.setTooltip ("Show the songs folder");
    // Icon-only buttons: titles for screen readers (the tooltips are read as help).
    closeButton.setTitle ("Close");
    showFolderButton.setTitle ("Show songs folder");
    deleteAllButton.setTitle ("Delete all songs");
    clearModelsButton.setTitle ("Remove downloaded AI models");
    clearCacheButton.setTitle ("Clear model cache");
    removeToolsButton.setTitle ("Remove YouTube tools");
    clearDownloadsButton.setTitle ("Clear temporary downloads");
    deleteAllButton.setTooltip ("Delete all songs (except the ones open in justStem)");
    clearModelsButton.setTooltip ("Remove the downloaded HQ and piano MIDI models");
    clearCacheButton.setTooltip ("Clear the AI model cache");
    removeToolsButton.setTooltip ("Remove the YouTube tools");
    clearDownloadsButton.setTooltip ("Clear temporary downloads");
    showFolderButton.onClick = [] { paths::stems().revealToUser(); };
    deleteAllButton.onClick = [this] {
        // Songs open in a justStem aren't deleted, so they aren't counted either.
        const auto deletable = (int) std::count_if (rows.begin(), rows.end(), [] (const SongRow* r) { return ! r->used; });
        confirm ("Delete all songs?",
                 "The stems of " + juce::String (deletable) + (deletable == 1 ? " song" : " songs") + " will be moved to the Trash."
                     + (deletable < rows.size() ? " Songs open in justStem are kept." : ""),
                 "Move to Trash", [this] {
                     // Checked again: another justStem may have started or opened one while this asked.
                     if (! isBusy())
                     {
                         const auto openNow = songsInUse();
                         for (auto* row : rows)
                             if (! openNow.contains (row->song.folder)) storage::moveSongToTrash (row->song);
                     }
                     refresh();
                 });
    };
    clearModelsButton.onClick = [this] {
        confirm ("Remove the downloaded AI models?", "They're downloaded again the next time you use HQ mode or split a song with piano.",
                 "Remove", [this] { if (! isBusy()) for (const auto& f : downloadedModelFiles()) f.deleteFile(); refresh(); });
    };
    clearCacheButton.onClick = [this] {
        confirm ("Clear the AI model cache?", "It's rebuilt automatically the next time you split a song (once; about " + compileTimes() + ").",
                 "Clear", [this] { if (! isBusy()) storage::clearFolder (paths::coremlCache()); refresh(); });
    };
    removeToolsButton.onClick = [this] {
        confirm ("Remove the YouTube tools?", "yt-dlp and Deno are downloaded again the next time you paste a YouTube link.",
                 "Remove", [this] { if (! isBusy()) storage::clearFolder (paths::tools()); refresh(); });
    };
    clearDownloadsButton.onClick = [this] { if (! isBusy()) storage::clearFolder (paths::downloads()); refresh(); };

    for (auto* b : { &closeButton, &deleteAllButton, &showFolderButton, &clearModelsButton, &clearCacheButton, &removeToolsButton,
                     &clearDownloadsButton })
        addAndMakeVisible (*b);
    viewport.setViewedComponent (&listContent, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (6);
    viewport.setSingleStepSizes (songRowH, songRowH); // a row per step
    viewport.onScroll = [this] { repaint (viewport.getBounds()); };
    addAndMakeVisible (viewport);
    refresh();
    startTimerHz (2);
}

StoragePanel::~StoragePanel() = default;

void StoragePanel::stateChanged()
{
    if (isBusy() != shownBusy || busyHere() != shownBusyHere || songsInUse() != shownInUse || songOpenHere() != shownOpenHere)
        refresh();
}

void StoragePanel::refresh()
{
    const bool busy = shownBusy = isBusy();
    const bool working = shownBusyHere = busyHere();
    const auto inUse = shownInUse = songsInUse();
    const auto here = shownOpenHere = songOpenHere();
    songsBytes = 0;
    rows.clear();
    for (const auto& s : storage::savedSongs())
    {
        songsBytes += s.bytes;
        auto* row = rows.add (new SongRow (s, inUse.contains (s.folder), s.folder == here));
        row->deleteButton.setEnabled (! busy && ! row->used);
        row->openButton.setEnabled (! working && ! row->openHere);
        row->openButton.onClick = [this, folder = row->song.folder] { if (onOpen) onOpen (folder); };
        row->deleteButton.onClick = [this, row] {
            confirm ("Delete this song?", "\"" + row->song.title + "\" (" + size (row->song.bytes) + ") will be moved to the Trash.",
                     "Move to Trash", [this, song = row->song] {
                         if (! isBusy() && ! songsInUse().contains (song.folder)) storage::moveSongToTrash (song);
                         refresh();
                     });
        };
        listContent.addAndMakeVisible (row);
    }
    modelsBytes = 0;
    for (const auto& f : downloadedModelFiles()) modelsBytes += f.getSize();
    cacheBytes = storage::folderSize (paths::coremlCache());
    toolsBytes = storage::folderSize (paths::tools());
    downloadsBytes = storage::folderSize (paths::downloads());

    const bool anyDeletable = std::any_of (rows.begin(), rows.end(), [] (SongRow* r) { return ! r->used; });
    deleteAllButton.setEnabled (! busy && anyDeletable);
    clearModelsButton.setEnabled (! busy && modelsBytes > 0);
    clearCacheButton.setEnabled (! busy && cacheBytes > 0);
    removeToolsButton.setEnabled (! busy && toolsBytes > 0);
    clearDownloadsButton.setEnabled (! busy && downloadsBytes > 0);
    resized();
    repaint();
}

juce::Rectangle<int> StoragePanel::card() const { return getLocalBounds().withSizeKeepingCentre (600, 490 + otherRowH); } // list fits 4 whole rows

void StoragePanel::resized()
{
    auto r = card().reduced (28, 24).withTop (card().getY() + headerH + 8);
    auto top = titleRow (card());
    placeRight (top, closeButton, btn, btn);

    songsHeader = r.removeFromTop (30);
    auto h = songsHeader;
    placeRight (h, deleteAllButton, btn, btn);
    h.removeFromRight (8);
    placeRight (h, showFolderButton, btn, btn);

    otherArea = r.removeFromBottom (4 * otherRowH);
    r.removeFromBottom (28); // room for the busy warning above the footer
    // The scrollbar sits out in the card's margin, so the rows keep the full width and their
    // Delete buttons line up with the Clear / Remove buttons below.
    // Whole rows only, so the list doesn't stop halfway through a song (the rest goes to the gap above the footer).
    auto list = r.withTrimmedTop (4);
    list.setHeight (std::max (songRowH, list.getHeight() / songRowH * songRowH));
    viewport.setBounds (list.withTrimmedRight (-(scrollGap + viewport.getScrollBarThickness())));
    listContent.setSize (r.getWidth(), rows.size() * songRowH);
    for (int i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, i * songRowH, listContent.getWidth(), songRowH);

    auto other = otherArea;
    for (auto* b : { &clearModelsButton, &clearCacheButton, &removeToolsButton, &clearDownloadsButton })
    {
        auto row = other.removeFromTop (otherRowH);
        placeRight (row, *b, btn, btn);
    }
}

void StoragePanel::paintOverChildren (juce::Graphics& g)
{
    // Fade the list out at an edge that has more songs past it.
    const auto v = viewport.getBounds().withTrimmedRight (viewport.getScrollBarThickness() + scrollGap).toFloat();
    const int pos = viewport.getViewPositionY(), hidden = listContent.getHeight() - viewport.getViewHeight();
    const auto bg = cardColour();
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
    g.setColour (cardColour());
    g.fillRoundedRectangle (c.toFloat(), radius);
    g.setColour (Palette::panelBorder);
    g.drawRoundedRectangle (c.toFloat().reduced (0.5f), radius, 1.0f);

    // The title sits in a darker header band, like the footer below.
    fillBand (g, c.toFloat().withHeight ((float) headerH), true);
    auto top = titleRow (c);
    // Title and total on one baseline.
    const auto titleFont = font (Weight::Light, 26.0f);
    const int baseline = baselineFor (titleFont, top);
    g.setColour (Palette::text);
    g.setFont (titleFont);
    g.drawSingleLineText ("Storage", top.getX(), baseline);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Regular, 13.0f));
    g.drawSingleLineText (size (songsBytes + modelsBytes + cacheBytes + toolsBytes + downloadsBytes) + " used", top.getX() + 130, baseline);
    g.setColour (Palette::faint);
    g.setFont (font (Weight::Regular, 12.0f));
    g.drawSingleLineText (about, top.getRight() - btn - 16, baseline, juce::Justification::right);

    g.setColour (Palette::dim);
    g.setFont (capsFont());
    g.drawText ("SONGS   " + juce::String (rows.size()) + "   " + size (songsBytes), songsHeader, juce::Justification::centredLeft, false);
    if (rows.isEmpty())
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

    // Models / cache / tools / downloads rows.
    struct Item { const char* name; juce::String note; juce::int64 bytes; };
    const Item items[] { { "Downloaded AI models", "HQ mode and piano MIDI, downloaded again when needed", modelsBytes },
                         { "AI model cache", "Rebuilt automatically on the next split (" + compileTimes() + ")", cacheBytes },
                         { "YouTube tools", "yt-dlp and Deno, downloaded again when needed", toolsBytes },
                         { "Temporary downloads", "YouTube audio waiting to be split", downloadsBytes } };
    // They sit in a darker footer, set apart from the songs.
    fillBand (g, c.toFloat().withTop ((float) otherArea.getY() - 8.0f), false);
    auto other = otherArea;
    for (const auto& item : items)
    {
        auto row = other.removeFromTop (otherRowH).withTrimmedRight (100);
        g.setColour (Palette::text);
        g.setFont (font (Weight::Regular, 13.5f));
        g.drawText (juce::String (item.name) + "   " + size (item.bytes), row.removeFromTop (22).withTrimmedTop (2),
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

bool StoragePanel::keyPressed (const juce::KeyPress& key)
{
    if (key != juce::KeyPress::escapeKey) return false;
    if (onClose) onClose();
    return true;
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
