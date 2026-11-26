#pragma once

#include "../../io/Storage.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** Overlay listing what justStem keeps on disk, with buttons to free space:
    saved songs (moved to the Trash, after confirming), the CoreML cache, the YouTube tools and
    temporary downloads. */
class StoragePanel final : public juce::Component, private juce::Timer
{
public:
    /** @param songOpenHere  folder of the song open in this plug-in (can't be opened again)
        @param busyHere      true while this plug-in is processing a song (nothing can be opened)
        @param songsInUse    folders of the songs open in any justStem (can't be deleted)
        @param anyBusy       true while any justStem is processing a song (nothing can be deleted)
        Other plug-ins don't tell the panel when they change, so it checks twice a second. */
    StoragePanel (std::function<juce::File()> songOpenHere, std::function<bool()> busyHere,
                  std::function<juce::Array<juce::File>()> songsInUse, std::function<bool()> anyBusy);
    ~StoragePanel() override;

    /** Which justStem this is, shown in the header (e.g. "justStem 0.0.5  ·  VST3"), for bug reports. */
    juce::String about;
    std::function<void()> onClose;
    /** Open a saved song (its folder) in the plug-in. */
    std::function<void (const juce::File&)> onOpen;
    /** Reads the disk again (sizes and the song list). */
    void refresh();
    /** Call when the plug-in's state changes: refreshes only if being busy or the songs in use changed. */
    void stateChanged();

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    struct SongRow;
    void timerCallback() override { stateChanged(); }
    juce::Rectangle<int> card() const;
    void confirm (const juce::String& title, const juce::String& message, const juce::String& action, std::function<void()> then);

    std::function<juce::File()> songOpenHere;
    std::function<bool()> busyHere;
    std::function<juce::Array<juce::File>()> songsInUse;
    std::function<bool()> isBusy;

    bool shownBusy = false, shownBusyHere = false;
    juce::File shownOpenHere;
    juce::Array<juce::File> shownInUse;
    juce::int64 songsBytes = 0, modelsBytes = 0, cacheBytes = 0, toolsBytes = 0, downloadsBytes = 0;

    PillButton closeButton { {}, icons::close() }, deleteAllButton { {}, icons::trash() }, showFolderButton { {}, icons::folder() };
    PillButton clearModelsButton { {}, icons::trash() }, clearCacheButton { {}, icons::trash() }, removeToolsButton { {}, icons::trash() },
        clearDownloadsButton { {}, icons::trash() };
    /** Tells the panel when the list scrolls, so the edge fades follow. */
    struct ListViewport final : juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    };
    ListViewport viewport;
    juce::Component listContent;
    juce::OwnedArray<SongRow> rows;
    juce::Rectangle<int> songsHeader, otherArea;
};
} // namespace juststem::ui
