#include "PluginEditor.h"
#include "../io/AudioFiles.h"
#include "../io/YouTube.h"

using namespace juststem;
using namespace juststem::ui;

namespace
{
constexpr int windowW = 900, windowH = 660, margin = 28;

juce::String formatTime (double s)
{
    const int t = juce::jmax (0, (int) s);
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

juce::String displayName (Stem s)
{
    switch (s)
    {
        case Stem::Vocals: return "Vocals";
        case Stem::Drums: return "Drums";
        case Stem::Bass: return "Bass";
        case Stem::Guitar: return "Guitar";
        case Stem::Piano: return "Piano";
        case Stem::Other: return "Other";
    }
    return {};
}

juce::String bpmText (float bpm)
{
    const float r = std::round (bpm * 10.0f) / 10.0f;
    return std::fabs (r - std::round (r)) < 0.05f ? juce::String ((int) std::round (r)) : juce::String (r, 1);
}

/** A readout: small caption, big light value, one dim line under it. */
void paintReadout (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value,
                   const juce::String& detail, juce::Justification just, bool hasValue)
{
    r = r.withSizeKeepingCentre (r.getWidth(), 100);
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.5f));
    g.drawText (caption, r.removeFromTop (22), just, false);
    g.setColour (hasValue ? Palette::text : Palette::faint);
    g.setFont (font (Weight::Light, 38.0f));
    g.drawText (value, r.removeFromTop (50), just, false);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Regular, 13.0f));
    g.drawText (detail, r.removeFromTop (22), just, false);
}
} // namespace

namespace
{
/** Saves pitch-shifted stems on a background thread, with a progress window and Cancel. Deletes itself. */
class TransposedSave final : public juce::ThreadWithProgressWindow
{
public:
    TransposedSave (SongResultPtr s, int semis, juce::File folder)
        : juce::ThreadWithProgressWindow ("Saving stems transposed " + juce::String (semis > 0 ? "+" : "") + juce::String (semis), true, true),
          song (std::move (s)), semitones (semis), dir (std::move (folder))
    {
        setStatusMessage ("Pitch-shifting 6 stems and the instrumental...");
    }

    void run() override
    {
        try
        {
            done = saveTransposedStems (*song, semitones, dir, [this] (double f) {
                setProgress (f);
                return ! threadShouldExit();
            });
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
    }

    void threadComplete (bool) override
    {
        if (done) dir.revealToUser();
        else
        {
            dir.deleteRecursively(); // cancelled or failed: don't leave half a set behind
            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't save the stems", error);
        }
        delete this;
    }

private:
    SongResultPtr song;
    int semitones;
    juce::File dir;
    bool done = false;
    juce::String error;
};
} // namespace

void justStemEditor::saveTransposed (SongResultPtr song, int semitones, const juce::File& dest)
{
    const auto key = song->analysis.key.transposed (semitones);
    const auto dir = dest.getChildFile (juce::File::createLegalFileName (song->title + " stems (" + (semitones > 0 ? "+" : "")
                                                                         + juce::String (semitones) + ", " + juce::String (key.name()) + ")"))
                         .getNonexistentSibling();
    (new TransposedSave (std::move (song), semitones, dir))->launchThread();
}

// ============================================================================ setup
justStemEditor::justStemEditor (justStemProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);

    linkBox.setTextToShowWhenEmpty ("Paste a YouTube link, or drop a song anywhere", Palette::faint);
    linkBox.setFont (font (Weight::Regular, 14.0f));
    linkBox.setIndents (16, 0);
    linkBox.setJustification (juce::Justification::centredLeft);
    linkBox.onReturnKey = [this] { startFromInput(); };
    addAndMakeVisible (linkBox);

    splitButton.setColour (juce::TextButton::buttonColourId, Palette::text);
    splitButton.setColour (juce::TextButton::textColourOffId, Palette::bgBottom);
    splitButton.onClick = [this] { startFromInput(); };
    chooseButton.onClick = [this] { chooseFile(); };
    cancelButton.onClick = [this] { proc.cancelJob(); };
    addAndMakeVisible (splitButton);
    addAndMakeVisible (chooseButton);
    addChildComponent (cancelButton);

    addAndMakeVisible (gauge);

    for (int i = 0; i < numStems; ++i)
        addChildComponent (rows.add (new StemRow (proc.player(), i, displayName (stemOrder[(size_t) i]))));
    addChildComponent (rows.add (new StemRow ("Instrumental")));
    revealButton.onClick = [this] { if (auto s = proc.getSong()) s->folder.revealToUser(); };
    saveAllButton.onClick = [this] { saveAll(); };
    addChildComponent (revealButton);
    addChildComponent (saveAllButton);
    storageButton.setTooltip ("See how much space justStem uses and free it");
    storageButton.onClick = [this] { showStorage(); };
    addAndMakeVisible (storageButton);

    stemsTab.onClick = [this] { showChords (false); };
    chordsTab.onClick = [this] { showChords (true); };
    chordView.onSeek = [this] (double t) { proc.player().seek (t); };
    transposeDown.onClick = [this] { proc.setTranspose (proc.getTranspose() - 1); };
    transposeUp.onClick = [this] { proc.setTranspose (proc.getTranspose() + 1); };
    transposeDown.setTooltip ("Transpose the chords and the preview down a semitone");
    transposeUp.setTooltip ("Transpose the chords and the preview up a semitone");
    copyButton.setTooltip ("Copy the chord chart as text");
    copyButton.onClick = [this] { copyChords(); };
    for (auto* c : std::initializer_list<juce::Component*> { &stemsTab, &chordsTab, &chordView, &transposeDown, &transposeUp, &copyButton })
        addChildComponent (c);

    playButton.setIcon (icons::play());
    playButton.onClick = [this] { proc.player().setPlaying (! proc.player().isPlaying()); };
    position.setSliderStyle (juce::Slider::LinearHorizontal);
    position.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    position.onValueChange = [this] { if (position.isMouseButtonDown()) proc.player().seek (position.getValue()); };
    followHost.onClick = [this] { proc.player().setFollowHost (followHost.getToggleState()); };
    followHost.setTooltip ("Play the stems along with your DAW's transport");
    addAndMakeVisible (playButton);
    addAndMakeVisible (position);
    addAndMakeVisible (followHost);

    setSize (windowW, windowH);
    proc.addChangeListener (this);
    refreshAll();
    startTimerHz (30);
}

justStemEditor::~justStemEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

// ============================================================================ layout
void justStemEditor::resized()
{
    auto r = getLocalBounds().reduced (margin, 22);

    auto input = r.removeFromTop (40);
    splitButton.setBounds (input.removeFromRight (96));
    input.removeFromRight (8);
    chooseButton.setBounds (input.removeFromRight (128));
    input.removeFromRight (10);
    linkBox.setBounds (input);
    statusLine = r.removeFromTop (26).withTrimmedLeft (16);

    auto readouts = r.removeFromTop (236);
    gauge.setBounds (readouts.withSizeKeepingCentre (236, 236));
    cancelButton.setBounds (gauge.getBounds().getCentreX() - 46, gauge.getBottom() - 34, 92, 26);
    tempoArea = readouts.removeFromLeft ((readouts.getWidth() - 236) / 2 - 20);
    loudArea = readouts.removeFromRight (tempoArea.getWidth());

    playerBar = r.removeFromBottom (40);
    r.removeFromBottom (10);
    r.removeFromTop (14);
    stemsHeader = r.removeFromTop (34);
    stemsArea = r;

    auto header = stemsHeader;
    stemsTab.setBounds (header.getX(), header.getY(), 64, header.getHeight());
    chordsTab.setBounds (header.getX() + 84, header.getY(), 74, header.getHeight());
    if (transposeUp.isVisible()) // a song is open
    {
        if (chordsShown) copyButton.setBounds (header.removeFromRight (128).withSizeKeepingCentre (128, 26));
        else
        {
            // Narrower than before so Storage + these two fit right of the centred Transpose.
            saveAllButton.setBounds (header.removeFromRight (104).withSizeKeepingCentre (104, 26));
            header.removeFromRight (8);
            revealButton.setBounds (header.removeFromRight (124).withSizeKeepingCentre (124, 26));
        }
        header.removeFromRight (8);

        // Transpose centred under the gauge.
        auto t = juce::Rectangle<int> (148, header.getHeight()).withCentre ({ gauge.getBounds().getCentreX(), header.getCentreY() });
        transposeDown.setBounds (t.removeFromLeft (30).withSizeKeepingCentre (30, 26));
        transposeUp.setBounds (t.removeFromRight (30).withSizeKeepingCentre (30, 26));
        transposeArea = t;
    }
    storageButton.setBounds (header.removeFromRight (88).withSizeKeepingCentre (88, 26));
    chordView.setBounds (stemsArea.withTrimmedTop (6));
    if (storagePanel != nullptr) storagePanel->setBounds (getLocalBounds());

    auto list = stemsArea;
    const int rowH = list.getHeight() / rows.size();
    for (auto* row : rows) row->setBounds (list.removeFromTop (rowH));

    auto bar = playerBar;
    playButton.setBounds (bar.removeFromLeft (40).withSizeKeepingCentre (36, 36));
    bar.removeFromLeft (14);
    followHost.setBounds (bar.removeFromRight (140));
    bar.removeFromRight (14);
    timeArea = bar.removeFromRight (96);
    bar.removeFromRight (10);
    position.setBounds (bar);
}

// ============================================================================ painting
void justStemEditor::paint (juce::Graphics& g)
{
    paintBackground (g, getLocalBounds());
    const bool busy = proc.isBusy();
    const auto song = busy ? nullptr : proc.getSong();

    // Status (while busy) or error, under the input bar.
    if (busy || proc.lastError().isNotEmpty())
    {
        g.setColour (busy ? Palette::dim : Palette::bad);
        g.setFont (font (Weight::Regular, 12.5f));
        g.drawFittedText (busy ? proc.jobStatus().step : proc.lastError(), statusLine, juce::Justification::centredLeft, 1, 0.8f);
    }

    // Tempo and loudness, either side of the gauge (which shows the key).
    if (song != nullptr)
    {
        const auto& a = song->analysis;
        // The detected tempo octave is a guess, so offer half and double time too.
        paintReadout (g, tempoArea, "TEMPO", bpmText (a.tempo.bpm) + " BPM",
                      "half " + bpmText (a.tempo.bpm / 2) + juce::String::fromUTF8 ("  \xc2\xb7  ") + "double " + bpmText (a.tempo.bpm * 2),
                      juce::Justification::centredLeft, true);
        paintReadout (g, loudArea, "LOUDNESS", juce::String (a.loudness.integratedLufs, 1) + " LUFS",
                      "peak " + juce::String (a.loudness.samplePeakDb > 0 ? "+" : "") + juce::String (a.loudness.samplePeakDb, 1) + " dBFS",
                      juce::Justification::centredRight, true);
    }
    else
    {
        paintReadout (g, tempoArea, "TEMPO", "--", "", juce::Justification::centredLeft, false);
        paintReadout (g, loudArea, "LOUDNESS", "--", "", juce::Justification::centredRight, false);
    }

    // The lower area shows the stems, or the processing log while working (and after an error).
    const auto log = proc.jobLog();
    const bool showLog = busy || (song == nullptr && ! log.isEmpty());
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.5f));
    if (! stemsTab.isVisible()) g.drawText (showLog ? "LOG" : "STEMS", stemsHeader, juce::Justification::centredLeft, false);
    if (transposeUp.isVisible())
    {
        const int t = proc.getTranspose();
        g.setColour (t != 0 ? Palette::text : Palette::dim);
        g.setFont (font (Weight::Regular, 13.0f));
        g.drawText ("Transpose " + juce::String (t > 0 ? "+" : "") + juce::String (t), transposeArea, juce::Justification::centred, false);
    }
    g.setColour (Palette::hairline);
    g.drawHorizontalLine (stemsHeader.getBottom(), (float) stemsHeader.getX(), (float) stemsHeader.getRight());
    g.drawHorizontalLine (playerBar.getY() - 6, (float) playerBar.getX(), (float) playerBar.getRight());

    if (showLog)
    {
        // Newest line at the bottom; the current step brightest.
        constexpr int lineH = 19;
        auto area = stemsArea.reduced (0, 8);
        const int fit = area.getHeight() / lineH;
        const int first = std::max (0, log.size() - fit);
        g.setFont (font (Weight::Regular, 12.5f));
        auto y = area.getY();
        for (int i = first; i < log.size(); ++i, y += lineH)
        {
            const bool last = i == log.size() - 1;
            g.setColour (log[i].contains ("Error:") ? Palette::bad : last ? Palette::text : Palette::dim);
            g.drawText (log[i], area.getX(), y, area.getWidth(), lineH, juce::Justification::centredLeft, true);
        }
    }
    else if (song == nullptr)
    {
        g.setColour (Palette::faint);
        g.setFont (font (Weight::Light, 16.0f));
        g.drawText ("Drop a song anywhere in this window", stemsArea, juce::Justification::centred, false);
    }

    // Time in the player bar.
    const auto& pl = proc.player();
    g.setColour (song ? Palette::dim : Palette::faint);
    g.setFont (font (Weight::Regular, 13.0f));
    g.drawText (song ? formatTime (pl.positionSeconds()) + " / " + formatTime (pl.durationSeconds()) : "0:00 / 0:00",
                timeArea, juce::Justification::centredRight, false);
}

void justStemEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dropHover) return;
    const auto r = getLocalBounds().toFloat().reduced (10.0f);
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (r, 18.0f);
    g.setColour (Palette::accentTo.withAlpha (0.8f));
    g.drawRoundedRectangle (r, 18.0f, 1.5f);
    g.setColour (Palette::text);
    g.setFont (font (Weight::Light, 26.0f));
    g.drawText ("Drop to split this song", r, juce::Justification::centred, false);
}

// ============================================================================ state
void justStemEditor::refreshAll()
{
    const bool busy = proc.isBusy();
    const auto song = busy ? nullptr : proc.getSong();

    linkBox.setEnabled (! busy);
    splitButton.setEnabled (! busy);
    chooseButton.setEnabled (! busy);
    cancelButton.setVisible (busy);
    saveAllButton.setVisible (song != nullptr && ! chordsShown);
    revealButton.setVisible (song != nullptr && ! chordsShown);
    stemsTab.setVisible (song != nullptr);
    chordsTab.setVisible (song != nullptr);
    stemsTab.setToggleState (! chordsShown, juce::dontSendNotification);
    chordsTab.setToggleState (chordsShown, juce::dontSendNotification);
    for (auto* b : { &transposeDown, &transposeUp }) b->setVisible (song != nullptr);
    copyButton.setVisible (song != nullptr && chordsShown);
    const int t = proc.getTranspose();
    saveAllButton.setButtonText (t == 0 ? "Save all" : "Save all " + juce::String (t > 0 ? "+" : "") + juce::String (t));
    saveAllButton.setTooltip (t == 0 ? "Copy all the stems to a folder" : "Save all the stems pitch-shifted to the transposed key");
    chordView.setVisible (song != nullptr && chordsShown);
    chordView.setTranspose (proc.getTranspose());
    resized(); // Storage button position depends on the buttons above
    if (storagePanel != nullptr) storagePanel->refresh();
    for (auto* row : rows) row->setVisible (song != nullptr && ! chordsShown);
    followHost.setToggleState (proc.player().followsHost(), juce::dontSendNotification);

    if (song != nullptr && song != shownSong)
    {
        shownSong = song;
        for (int i = 0; i < numStems; ++i) rows[i]->setFile (song->stemFile (stemOrder[(size_t) i]));
        rows.getLast()->setFile (song->instrumentalFile());
        position.setRange (0.0, std::max (1.0, song->durationSeconds), 0.01);
        chordView.setSong (song);
    }
    for (auto* row : rows) row->refresh();
    updateGauge();
    updatePlayerBar();
    repaint();
}

void justStemEditor::updateGauge()
{
    Gauge::State s;
    if (proc.isBusy())
    {
        const auto st = proc.jobStatus();
        s.caption = st.step.upToFirstOccurrenceOf (" (", false, false);
        if (s.caption.isEmpty()) s.caption = "Starting";
        s.fraction = st.fraction;
        s.value = st.fraction >= 0 ? juce::String (juce::roundToInt (st.fraction * 100)) : juce::String::fromUTF8 ("\xe2\x80\xa6");
        s.unit = st.fraction >= 0 ? "%" : "";
    }
    else if (auto song = proc.getSong())
    {
        // The key follows Transpose. "C# minor" -> value "C#", unit "minor · 12A"; the arc is the Camelot number.
        const auto& detected = song->analysis.key;
        const auto key = detected.transposed (proc.getTranspose());
        const auto name = juce::String (key.name());
        const auto camelot = juce::String (key.camelot());
        const bool sure = detected.votes >= 2 && detected.confidence > 0.5f;
        s.caption = "Key";
        s.value = name.upToFirstOccurrenceOf (" ", false, false);
        s.unit = name.fromFirstOccurrenceOf (" ", false, false) + juce::String::fromUTF8 (" \xc2\xb7 ") + camelot
                 + (sure ? "" : " ?");
        s.fraction = camelot.getIntValue() / 12.0;
    }
    else
    {
        s.caption = "Key";
    }
    gauge.set (s);
}

void justStemEditor::updatePlayerBar()
{
    auto& pl = proc.player();
    const bool hasSong = ! proc.isBusy() && proc.getSong() != nullptr;
    const bool manual = hasSong && ! pl.followsHost();
    playButton.setEnabled (manual);
    playButton.setIcon (manual && pl.isPlaying() ? icons::pause() : icons::play());
    position.setEnabled (hasSong);
    if (hasSong && ! position.isMouseButtonDown()) position.setValue (pl.positionSeconds(), juce::dontSendNotification);
    repaint (timeArea);
}

void justStemEditor::timerCallback()
{
    if (proc.isBusy())
    {
        updateGauge();
        repaint (statusLine);
        repaint (stemsArea); // the log
    }
    else
    {
        updatePlayerBar();
        if (chordView.isVisible()) chordView.setPosition (proc.player().positionSeconds());
    }
}

// ============================================================================ actions
void justStemEditor::startFromInput()
{
    const auto text = linkBox.getText().trim();
    if (text.isEmpty()) return chooseFile();
    if (isYouTubeLink (text)) return proc.startYouTube (text);
    if (juce::File::isAbsolutePath (text) && juce::File (text).existsAsFile()) return proc.startFile (juce::File (text));
    proc.showError ("That doesn't look like a YouTube link or an audio file.");
}

void justStemEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a song", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   supportedAudioWildcards());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) {
                              if (fc.getResult().existsAsFile()) proc.startFile (fc.getResult());
                          });
}

void justStemEditor::saveAll()
{
    auto song = proc.getSong();
    if (song == nullptr) return;
    chooser = std::make_unique<juce::FileChooser> ("Save all stems to...", juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [song, semitones = proc.getTranspose()] (const juce::FileChooser& fc) {
                              const auto dest = fc.getResult();
                              if (! dest.isDirectory()) return;
                              if (semitones != 0) return saveTransposed (song, semitones, dest);
                              auto dir = dest.getChildFile (juce::File::createLegalFileName (song->title) + " stems");
                              dir.createDirectory();
                              for (auto stem : stemOrder) song->stemFile (stem).copyFileTo (dir.getChildFile (song->stemFile (stem).getFileName()));
                              song->instrumentalFile().copyFileTo (dir.getChildFile (song->instrumentalFile().getFileName()));
                              dir.revealToUser();
                          });
}

bool justStemEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return ! proc.isBusy() && files.size() == 1;
}

void justStemEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dropHover = false;
    repaint();
    if (files.size() == 1 && ! proc.isBusy()) proc.startFile (juce::File (files[0]));
}

// ============================================================================ storage
void justStemEditor::showStorage()
{
    storagePanel = std::make_unique<StoragePanel> (
        [this] { auto s = proc.getSong(); return s != nullptr ? s->folder : juce::File(); },
        [this] { return proc.isBusy(); });
    storagePanel->onClose = [this] {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<justStemEditor> (this)] {
            if (safe != nullptr) safe->storagePanel.reset();
        });
    };
    addAndMakeVisible (*storagePanel);
    storagePanel->setBounds (getLocalBounds());
}

// ============================================================================ chords
void justStemEditor::showChords (bool show)
{
    chordsShown = show;
    refreshAll();
}

void justStemEditor::copyChords()
{
    auto song = proc.getSong();
    if (song == nullptr) return;
    juce::SystemClipboard::copyTextToClipboard (ChordView::chartText (*song, proc.getTranspose()));
    copyButton.setButtonText ("Copied");
    juce::Timer::callAfterDelay (1500, [safe = juce::Component::SafePointer<justStemEditor> (this)] {
        if (safe != nullptr) safe->copyButton.setButtonText ("Copy chords");
    });
}
