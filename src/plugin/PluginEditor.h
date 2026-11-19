#pragma once

#include "PluginProcessor.h"
#include "ui/StoragePanel.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <juce_audio_processors/juce_audio_processors.h>

/** The plug-in window, top to bottom:
      input bar   YouTube link, Choose file, Split
      readouts    KEY (left), the tempo gauge (centre; shows progress while splitting), LOUDNESS (right)
      stems       one row per stem: mute, solo, volume, drag-to-DAW
      player bar  play, seek, time, Follow DAW */
class CoverToolsEditor final : public juce::AudioProcessorEditor,
                               public juce::DragAndDropContainer,
                               public juce::FileDragAndDropTarget,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    explicit CoverToolsEditor (CoverToolsProcessor&);
    ~CoverToolsEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHover = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshAll(); }
    void timerCallback() override;
    void refreshAll();
    void updateGauge();
    void updatePlayerBar();
    void startFromInput();
    void chooseFile();
    void saveAll();
    void showStorage();

    CoverToolsProcessor& proc;
    covertools::ui::DashboardLookAndFeel lnf;

    // layout regions (set in resized)
    juce::Rectangle<int> statusLine, keyArea, loudArea, stemsHeader, stemsArea, playerBar, timeArea;

    juce::TextEditor linkBox;
    juce::TextButton chooseButton { "Choose file" }, splitButton { "Split" }, cancelButton { "Cancel" };
    std::unique_ptr<juce::FileChooser> chooser;

    covertools::ui::Gauge gauge;
    juce::OwnedArray<covertools::ui::StemRow> rows;
#if JUCE_MAC
    juce::TextButton revealButton { "Show in Finder" };
#elif JUCE_WINDOWS
    juce::TextButton revealButton { "Show in Explorer" };
#else
    juce::TextButton revealButton { "Show in Folder" };
#endif
    juce::TextButton saveAllButton { "Save all" }, storageButton { "Storage" };
    std::unique_ptr<covertools::ui::StoragePanel> storagePanel;

    covertools::ui::IconButton playButton;
    juce::Slider position;
    juce::ToggleButton followHost { "Follow DAW" };

    covertools::SongResultPtr shownSong;
    bool dropHover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoverToolsEditor)
};
