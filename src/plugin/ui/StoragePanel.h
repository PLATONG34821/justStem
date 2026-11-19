#pragma once

#include "../../io/Storage.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace covertools::ui
{
/** Overlay listing what CoverTools keeps on disk, with buttons to free space:
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
    void refresh();

    void paint (juce::Graphics&) override;
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

    juce::TextButton closeButton { "Close" }, deleteAllButton { "Delete all" }, showFolderButton { "Show folder" };
    juce::TextButton clearCacheButton { "Clear" }, removeToolsButton { "Remove" }, clearDownloadsButton { "Clear" };
    juce::Viewport viewport;
    juce::Component listContent;
    juce::OwnedArray<SongRow> rows;
    juce::Rectangle<int> songsHeader, otherArea;
};
} // namespace covertools::ui
