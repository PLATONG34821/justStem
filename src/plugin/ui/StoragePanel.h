#pragma once

#include "../../io/Storage.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** Overlay listing what justStem keeps on disk, with buttons to free space:
    saved songs (moved to the Trash, after confirming), the CoreML cache, the YouTube tools and
    temporary downloads. */
class StoragePanel final : public juce::Component
{
public:
    /** @param songInUse  folder of the song open in the plug-in (can't be deleted)
        @param isBusy     true while a song is being processed (everything is disabled) */
    StoragePanel (std::function<juce::File()> songInUse, std::function<bool()> isBusy);
    ~StoragePanel() override;

    std::function<void()> onClose;
    /** Open a saved song (its folder) in the plug-in. */
    std::function<void (const juce::File&)> onOpen;
    void refresh();

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct SongRow;
    juce::Rectangle<int> card() const;
    void confirm (const juce::String& title, const juce::String& message, const juce::String& action, std::function<void()> then);

    std::function<juce::File()> songInUse;
    std::function<bool()> isBusy;

    std::vector<storage::SavedSong> songs;
    juce::int64 songsBytes = 0, cacheBytes = 0, toolsBytes = 0, downloadsBytes = 0;

    PillButton closeButton { {}, icons::close() }, deleteAllButton { {}, icons::trash() }, showFolderButton { {}, icons::folder() };
    PillButton clearCacheButton { {}, icons::trash() }, removeToolsButton { {}, icons::trash() }, clearDownloadsButton { {}, icons::trash() };
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
