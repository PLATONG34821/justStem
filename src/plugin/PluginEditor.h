#pragma once

#include "PluginProcessor.h"
#include "ui/ChordView.h"
#include "ui/PlayerBar.h"
#include "ui/SongTitle.h"
#include "ui/StoragePanel.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <juce_audio_processors/juce_audio_processors.h>

/** The plug-in window, top to bottom:
      top bar     YouTube link | "justStem" | Choose file, Split, Storage; with a song open: its title | "justStem" | New song, Storage
      readouts    TEMPO (left), the key gauge (centre; shows progress while splitting), LOUDNESS (right)
      stems       tabs: one row per stem (mute, solo, volume, drag-to-DAW), or the chords
      player bar  play, seek, time, Follow DAW (only with a song open) */
class justStemEditor final : public juce::AudioProcessorEditor,
                               public juce::DragAndDropContainer,
                               public juce::FileDragAndDropTarget,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    explicit justStemEditor (justStemProcessor&);
    ~justStemEditor() override;

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
    void showChords (bool);
    void copyChords();
    /** The link box (and Choose file / Split) are shown with no song open, while working, or after New song. */
    bool inputShown() const;
    void showInput (bool);
    void refreshPasteButton();
    /** The song on screen: the open one, or none while a new one is being split. */
    juststem::SongResultPtr visibleSong() const { return proc.isBusy() ? nullptr : proc.getSong(); }

    justStemProcessor& proc;
    juststem::ui::DashboardLookAndFeel lnf;

    // layout regions (set in resized)
    juce::Rectangle<int> topRow, wordmarkArea, tabsArea, tempoArea, loudArea, stemsHeader, stemsArea, transposeArea;

    juce::TextEditor linkBox;
    juststem::ui::PillButton chooseButton { {}, juststem::ui::icons::fileImport() };
    juststem::ui::PillButton splitButton { {}, juststem::ui::icons::split() };
    juststem::ui::PillButton cancelButton { {}, juststem::ui::icons::close() };
    juststem::ui::PillButton pasteButton { {}, juststem::ui::icons::paste() }; // DAWs often keep Cmd/Ctrl+V for themselves
    juststem::ui::PillButton newSongButton { {}, juststem::ui::icons::plus() };
    juststem::ui::PillButton closeInputButton { {}, juststem::ui::icons::close() };
    juststem::ui::PillButton hqToggle { "HQ" }; // on/off; shown only when the HQ model is bundled
    bool wantInput = false; // New song pressed while a song is open
    std::unique_ptr<juce::FileChooser> chooser;

    juststem::ui::Gauge gauge;
    juce::OwnedArray<juststem::ui::StemRow> rows;
    juststem::ui::PillButton revealButton { {}, juststem::ui::icons::folder() };
    juststem::ui::PillButton saveAllButton { {}, juststem::ui::icons::download() };
    juststem::ui::PillButton storageButton { {}, juststem::ui::icons::drive() };
    juststem::ui::PillButton updateButton { "Update", juststem::ui::icons::download() }; // only when a newer release is out
    std::unique_ptr<juststem::ui::StoragePanel> storagePanel;

    juststem::ui::TabButton stemsTab { "Stems" }, chordsTab { "Chords" };
    juststem::ui::ChordView chordView;
    juststem::ui::PillButton transposeDown { {}, juststem::ui::icons::minus() }, transposeUp { {}, juststem::ui::icons::plus() };
    juststem::ui::PillButton copyButton { {}, juststem::ui::icons::copy() };
    bool chordsShown = false;

    juststem::ui::SongTitle songTitle;
    juststem::ui::PlayerBar playerBar { proc.player() };

    juststem::SongResultPtr shownSong;
    int shownLogLines = -1; // log lines on screen while working
    bool dropHover = false;

    // Shows the buttons' tooltips. A child of the editor (not a desktop window), so it stays inside the
    // plug-in window in every host. Declared last so it's created after, and destroyed before, the rest.
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (justStemEditor)
};
