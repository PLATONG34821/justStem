#include "PluginEditor.h"
#include "../io/AudioFiles.h"
#include "../io/YouTube.h"

using namespace covertools;
using namespace covertools::ui;

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

// ============================================================================ setup
CoverToolsEditor::CoverToolsEditor (CoverToolsProcessor& p) : AudioProcessorEditor (&p), proc (p)
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
    storageButton.setTooltip ("See how much space CoverTools uses and free it");
    storageButton.onClick = [this] { showStorage(); };
    addAndMakeVisible (storageButton);

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

CoverToolsEditor::~CoverToolsEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

// ============================================================================ layout
void CoverToolsEditor::resized()
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
    keyArea = readouts.removeFromLeft ((readouts.getWidth() - 236) / 2 - 20);
    loudArea = readouts.removeFromRight (keyArea.getWidth());

    playerBar = r.removeFromBottom (40);
    r.removeFromBottom (10);
    r.removeFromTop (14);
    stemsHeader = r.removeFromTop (34);
    stemsArea = r;

    auto header = stemsHeader;
    saveAllButton.setBounds (header.removeFromRight (96).withSizeKeepingCentre (96, 26));
    header.removeFromRight (8);
    revealButton.setBounds (header.removeFromRight (150).withSizeKeepingCentre (150, 26));
    if (! revealButton.isVisible()) header = stemsHeader; // Storage moves to the right edge when there's no song
    else header.removeFromRight (8);
    storageButton.setBounds (header.removeFromRight (104).withSizeKeepingCentre (104, 26));
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
void CoverToolsEditor::paint (juce::Graphics& g)
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

    // Key and loudness, either side of the gauge.
    if (song != nullptr)
    {
        const auto& a = song->analysis;
        const bool sure = a.key.votes >= 2 && a.key.confidence > 0.5f;
        paintReadout (g, keyArea, "KEY", a.key.name(),
                      "Camelot " + juce::String (a.key.camelot()) + (sure ? "" : "   (uncertain)"), juce::Justification::centredLeft, true);
        paintReadout (g, loudArea, "LOUDNESS", juce::String (a.loudness.integratedLufs, 1) + " LUFS",
                      "peak " + juce::String (a.loudness.samplePeakDb > 0 ? "+" : "") + juce::String (a.loudness.samplePeakDb, 1) + " dBFS",
                      juce::Justification::centredRight, true);
    }
    else
    {
        paintReadout (g, keyArea, "KEY", "--", "", juce::Justification::centredLeft, false);
        paintReadout (g, loudArea, "LOUDNESS", "--", "", juce::Justification::centredRight, false);
    }

    // The lower area shows the stems, or the processing log while working (and after an error).
    const auto log = proc.jobLog();
    const bool showLog = busy || (song == nullptr && ! log.isEmpty());
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.5f));
    g.drawText (showLog ? "LOG" : "STEMS", stemsHeader, juce::Justification::centredLeft, false);
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

void CoverToolsEditor::paintOverChildren (juce::Graphics& g)
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
void CoverToolsEditor::refreshAll()
{
    const bool busy = proc.isBusy();
    const auto song = busy ? nullptr : proc.getSong();

    linkBox.setEnabled (! busy);
    splitButton.setEnabled (! busy);
    chooseButton.setEnabled (! busy);
    cancelButton.setVisible (busy);
    revealButton.setVisible (song != nullptr);
    saveAllButton.setVisible (song != nullptr);
    resized(); // Storage button position depends on the buttons above
    if (storagePanel != nullptr) storagePanel->refresh();
    for (auto* row : rows) row->setVisible (song != nullptr);
    followHost.setToggleState (proc.player().followsHost(), juce::dontSendNotification);

    if (song != nullptr && song != shownSong)
    {
        shownSong = song;
        for (int i = 0; i < numStems; ++i) rows[i]->setFile (song->stemFile (stemOrder[(size_t) i]));
        rows.getLast()->setFile (song->instrumentalFile());
        position.setRange (0.0, std::max (1.0, song->durationSeconds), 0.01);
    }
    for (auto* row : rows) row->refresh();
    updateGauge();
    updatePlayerBar();
    repaint();
}

void CoverToolsEditor::updateGauge()
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
        const float bpm = song->analysis.tempo.bpm;
        s.caption = "Tempo";
        s.value = bpmText (bpm);
        s.unit = "BPM";
        s.fraction = juce::jlimit (0.0, 1.0, (bpm - 60.0) / 140.0); // 60..200 BPM across the arc
    }
    else
    {
        s.caption = "Tempo";
        s.unit = "BPM";
    }
    gauge.set (s);
}

void CoverToolsEditor::updatePlayerBar()
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

void CoverToolsEditor::timerCallback()
{
    if (proc.isBusy())
    {
        updateGauge();
        repaint (statusLine);
        repaint (stemsArea); // the log
    }
    else
        updatePlayerBar();
}

// ============================================================================ actions
void CoverToolsEditor::startFromInput()
{
    const auto text = linkBox.getText().trim();
    if (text.isEmpty()) return chooseFile();
    if (isYouTubeLink (text)) return proc.startYouTube (text);
    if (juce::File::isAbsolutePath (text) && juce::File (text).existsAsFile()) return proc.startFile (juce::File (text));
    proc.showError ("That doesn't look like a YouTube link or an audio file.");
}

void CoverToolsEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a song", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   supportedAudioWildcards());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) {
                              if (fc.getResult().existsAsFile()) proc.startFile (fc.getResult());
                          });
}

void CoverToolsEditor::saveAll()
{
    auto song = proc.getSong();
    if (song == nullptr) return;
    chooser = std::make_unique<juce::FileChooser> ("Save all stems to...", juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [song] (const juce::FileChooser& fc) {
                              const auto dest = fc.getResult();
                              if (! dest.isDirectory()) return;
                              auto dir = dest.getChildFile (juce::File::createLegalFileName (song->title) + " stems");
                              dir.createDirectory();
                              for (auto stem : stemOrder) song->stemFile (stem).copyFileTo (dir.getChildFile (song->stemFile (stem).getFileName()));
                              song->instrumentalFile().copyFileTo (dir.getChildFile (song->instrumentalFile().getFileName()));
                              dir.revealToUser();
                          });
}

bool CoverToolsEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return ! proc.isBusy() && files.size() == 1;
}

void CoverToolsEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dropHover = false;
    repaint();
    if (files.size() == 1 && ! proc.isBusy()) proc.startFile (juce::File (files[0]));
}

// ============================================================================ storage
void CoverToolsEditor::showStorage()
{
    storagePanel = std::make_unique<StoragePanel> (
        [this] { auto s = proc.getSong(); return s != nullptr ? s->folder : juce::File(); },
        [this] { return proc.isBusy(); });
    storagePanel->onClose = [this] {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<CoverToolsEditor> (this)] {
            if (safe != nullptr) safe->storagePanel.reset();
        });
    };
    addAndMakeVisible (*storagePanel);
    storagePanel->setBounds (getLocalBounds());
}
