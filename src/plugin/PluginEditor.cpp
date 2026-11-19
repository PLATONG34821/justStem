#include "PluginEditor.h"
#include "../io/AudioFiles.h"
#include "../io/YouTube.h"

using namespace juststem;
using namespace juststem::ui;

namespace
{
constexpr int windowW = 900, windowH = 620, margin = 28;

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

// The "justStem" wordmark (light "just", bold "Stem") and the song title share the top bar's baseline.
juce::Font wordmarkLight() { return font (Weight::Light, 23.0f, 0.01f); }
juce::Font wordmarkBold() { return font (Weight::SemiBold, 23.0f, 0.01f); }
juce::Font titleFont() { return font (Weight::Regular, 15.0f); }
int wordmarkWidth()
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (wordmarkLight(), "just")
                            + juce::GlyphArrangement::getStringWidth (wordmarkBold(), "Stem"));
}
int topBaseline (juce::Rectangle<int> row)
{
    const auto f = titleFont();
    return juce::roundToInt ((float) row.getCentreY() + (f.getAscent() - f.getDescent()) * 0.5f);
}

juce::String bpmText (float bpm)
{
    const float r = std::round (bpm * 10.0f) / 10.0f;
    return std::fabs (r - std::round (r)) < 0.05f ? juce::String ((int) std::round (r)) : juce::String (r, 1);
}

/** A readout: small caption, big light value, one dim line under it, each on the matching line of the
    gauge beside it (`lines` in editor coordinates). */
void paintReadout (juce::Graphics& g, juce::Rectangle<int> r, Gauge::Lines lines, const juce::String& caption,
                   const juce::String& value, const juce::String& detail, juce::Justification just, bool hasValue)
{
    const auto line = [&r] (float centreY, int h) { return r.withHeight (h).withCentre ({ r.getCentreX(), juce::roundToInt (centreY) }); };
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.5f));
    g.drawText (caption, line (lines.caption, 22), just, false);
    g.setColour (hasValue ? Palette::text : Palette::faint);
    g.setFont (font (Weight::Light, 38.0f));
    g.drawText (value, line (lines.value, 50), just, false);
    g.setColour (Palette::dim);
    g.setFont (font (Weight::Regular, 13.0f));
    g.drawText (detail, line (lines.unit, 22), just, false);
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

    linkBox.setTextToShowWhenEmpty ("Paste a YouTube link", Palette::faint); // dropping a song is hinted below
    linkBox.setFont (font (Weight::Regular, 14.0f));
    linkBox.setIndents (16, 0);
    linkBox.setJustification (juce::Justification::centredLeft);
    linkBox.onReturnKey = [this] { startFromInput(); };
    linkBox.onEscapeKey = [this] { if (wantInput) showInput (false); };
    linkBox.onTextChange = [this] { refreshPasteButton(); }; // Paste only while the box is empty, so it never covers a link
    addAndMakeVisible (linkBox);
    // Many hosts take Cmd/Ctrl+V before the plug-in sees it, so pasting needs a button.
    pasteButton.setName ("Paste");
    pasteButton.setTooltip ("Paste a link from the clipboard");
    pasteButton.onClick = [this] {
        linkBox.setText (juce::SystemClipboard::getTextFromClipboard().trim(), false);
        refreshPasteButton();
        linkBox.grabKeyboardFocus();
        linkBox.moveCaretToEnd();
    };
    addAndMakeVisible (pasteButton); // after linkBox, so it sits on top of the box's right end
    newSongButton.setTooltip ("Split another song (this one stays saved)");
    newSongButton.onClick = [this] { showInput (true); };
    closeInputButton.setTooltip ("Back to the open song");
    closeInputButton.onClick = [this] { showInput (false); };
    addChildComponent (newSongButton);
    addChildComponent (closeInputButton);

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
    // Icon-only buttons get names (tooltips for people, names for tools/uitest).
    storageButton.setName ("Storage");
    transposeDown.setName ("Transpose down");
    transposeUp.setName ("Transpose up");
    newSongButton.setName ("New song");
    closeInputButton.setName ("Back to song");
    storageButton.setTooltip ("Storage: see how much space justStem uses and free it");
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
    auto r = getLocalBounds().reduced (margin, 0).withTrimmedTop (12).withTrimmedBottom (22); // less air at the top

    // Top bar: song title (or the link box) on the left, "justStem" centred over the gauge, buttons on the right.
    auto top = r.removeFromTop (40);
    topRow = top;
    wordmarkArea = juce::Rectangle<int> (wordmarkWidth() + 4, top.getHeight()).withCentre ({ getWidth() / 2, top.getCentreY() });
    storageButton.setBounds (top.removeFromRight (40));
    top.removeFromRight (10);
    if (inputShown())
    {
        if (closeInputButton.isVisible())
        {
            closeInputButton.setBounds (top.removeFromRight (40));
            top.removeFromRight (8);
        }
        splitButton.setBounds (top.removeFromRight (84));
        top.removeFromRight (8);
        chooseButton.setBounds (top.removeFromRight (chooseButton.idealWidth (40)));
        top.removeFromRight (16);
    }
    else
    {
        newSongButton.setBounds (top.removeFromRight (newSongButton.idealWidth (40)));
        top.removeFromRight (16);
    }
    // The name needs room on both sides; when the buttons reach it (e.g. a narrow layout) it's left out.
    if (top.getRight() < wordmarkArea.getRight() + 16) wordmarkArea = {};
    const auto left = wordmarkArea.isEmpty() ? top : top.withRight (wordmarkArea.getX() - 16);
    if (inputShown())
    {
        linkBox.setBounds (left);
        const int pw = pasteButton.idealWidth (28);
        pasteButton.setBounds (left.withLeft (left.getRight() - pw - 6).withTrimmedRight (6).withSizeKeepingCentre (pw, 28));
        titleArea = {};
    }
    else titleArea = left;
    // The gauge tucks up under the top bar: its ring only comes near the bar in the middle, below "justStem".
    r.setTop (r.getY() - 6);

    auto readouts = r.removeFromTop (236);
    gauge.setBounds (readouts.withSizeKeepingCentre (236, 236));
    tempoArea = readouts.removeFromLeft ((readouts.getWidth() - 236) / 2 - 20);
    loudArea = readouts.removeFromRight (tempoArea.getWidth());

    // The player only shows with a song open; otherwise the log / drop hint gets its space.
    const bool busy = proc.isBusy();
    const bool player = ! busy && proc.getSong() != nullptr;
    playerBar = player ? r.removeFromBottom (40) : juce::Rectangle<int>();
    if (player) r.removeFromBottom (10);
    r.removeFromTop (14);
    stemsHeader = r.removeFromTop (34);
    stemsArea = r;

    auto header = stemsHeader;
    if (busy) // Cancel on the right of the LOG heading
    {
        const int w = cancelButton.idealWidth (26);
        cancelButton.setBounds (header.removeFromRight (w).withSizeKeepingCentre (w, 26));
    }
    // Stems | Chords as a segmented control, the same height as the other buttons.
    tabsArea = header.removeFromLeft (184).withSizeKeepingCentre (184, 26);
    {
        auto t = tabsArea;
        stemsTab.setBounds (t.removeFromLeft (t.getWidth() / 2));
        chordsTab.setBounds (t);
    }
    const auto placeRight = [&header] (PillButton& b) {
        const int w = b.idealWidth (26);
        b.setBounds (header.removeFromRight (w).withSizeKeepingCentre (w, 26));
    };
    if (transposeUp.isVisible()) // a song is open
    {
        if (chordsShown) placeRight (copyButton);
        else
        {
            placeRight (saveAllButton);
            header.removeFromRight (8);
            placeRight (revealButton);
        }

        // Transpose centred under the gauge.
        auto t = juce::Rectangle<int> (148, header.getHeight()).withCentre ({ gauge.getBounds().getCentreX(), header.getCentreY() });
        transposeDown.setBounds (t.removeFromLeft (30).withSizeKeepingCentre (30, 26));
        transposeUp.setBounds (t.removeFromRight (30).withSizeKeepingCentre (30, 26));
        transposeArea = t;
    }
    chordView.setBounds (stemsArea.withTrimmedTop (6));
    if (storagePanel != nullptr) storagePanel->setBounds (getLocalBounds());

    auto list = stemsArea;
    const int rowH = list.getHeight() / rows.size();
    for (auto* row : rows) row->setBounds (list.removeFromTop (rowH));

    // Play flush left, Follow DAW flush right, the time sized to its text, and the same gap either side of the seek bar.
    auto bar = playerBar;
    playButton.setBounds (bar.removeFromLeft (36).withSizeKeepingCentre (36, 36));
    bar.removeFromLeft (16);
    followHost.setBounds (bar.removeFromRight (DashboardLookAndFeel::toggleWidth (followHost)));
    bar.removeFromRight (24);
    const auto song = proc.getSong();
    const auto longest = formatTime (song != nullptr ? song->durationSeconds : 0.0);
    timeArea = bar.removeFromRight ((int) std::ceil (juce::GlyphArrangement::getStringWidth (font (Weight::Regular, 13.0f),
                                                                                            longest + " / " + longest)) + 2);
    bar.removeFromRight (16);
    position.setBounds (bar);
}

// ============================================================================ painting
void justStemEditor::paint (juce::Graphics& g)
{
    paintBackground (g, getLocalBounds());
    const bool busy = proc.isBusy();
    const auto song = busy ? nullptr : proc.getSong();

    // "justStem" and the song title, on one baseline in the top bar.
    const int baseline = topBaseline (topRow);
    if (! wordmarkArea.isEmpty())
    {
        const int x = wordmarkArea.getX() + 2;
        g.setColour (Palette::text.withAlpha (0.85f));
        g.setFont (wordmarkLight());
        g.drawSingleLineText ("just", x, baseline);
        g.setFont (wordmarkBold());
        g.drawSingleLineText ("Stem", x + juce::roundToInt (juce::GlyphArrangement::getStringWidth (wordmarkLight(), "just")), baseline);
    }
    if (song != nullptr && ! titleArea.isEmpty())
    {
        auto t = titleArea;
        drawIcon (g, icons::note(), t.removeFromLeft (16).toFloat().withSizeKeepingCentre (14, 14), Palette::dim);
        t.removeFromLeft (10);
        g.setColour (Palette::text);
        g.setFont (titleFont());
        // drawText can't take a baseline, so place the box so its centred text lands on it.
        const auto f = titleFont();
        const int y = baseline - juce::roundToInt ((f.getAscent() - f.getDescent()) * 0.5f) - t.getHeight() / 2;
        g.drawText (song->title, t.withY (y), juce::Justification::centredLeft, true);
    }

    // An error, in red in the free space above TEMPO (while working, progress is on the gauge and in the log).
    if (! busy && proc.lastError().isNotEmpty())
    {
        auto area = tempoArea.withBottom (gauge.getY() + juce::roundToInt (gauge.lines().caption) - 18);
        g.setColour (Palette::bad);
        g.setFont (font (Weight::Regular, 12.5f));
        g.drawFittedText (proc.lastError(), area, juce::Justification::topLeft, 3, 0.9f);
    }

    // Tempo and loudness, either side of the gauge (which shows the key), on the gauge's text lines.
    auto lines = gauge.lines();
    for (auto* y : { &lines.caption, &lines.value, &lines.unit }) *y += (float) gauge.getY();
    if (song != nullptr)
    {
        const auto& a = song->analysis;
        // The detected tempo octave is a guess, so offer half and double time too.
        paintReadout (g, tempoArea, lines, "TEMPO", bpmText (a.tempo.bpm) + " BPM",
                      "half " + bpmText (a.tempo.bpm / 2) + juce::String::fromUTF8 ("  \xc2\xb7  ") + "double " + bpmText (a.tempo.bpm * 2),
                      juce::Justification::centredLeft, true);
        paintReadout (g, loudArea, lines, "LOUDNESS", juce::String (a.loudness.integratedLufs, 1) + " LUFS",
                      "peak " + juce::String (a.loudness.samplePeakDb > 0 ? "+" : "") + juce::String (a.loudness.samplePeakDb, 1) + " dBFS",
                      juce::Justification::centredRight, true);
    }
    else
    {
        paintReadout (g, tempoArea, lines, "TEMPO", "--", "", juce::Justification::centredLeft, false);
        paintReadout (g, loudArea, lines, "LOUDNESS", "--", "", juce::Justification::centredRight, false);
    }

    // The lower area shows the stems, or the processing log while working (and after an error).
    const auto log = proc.jobLog();
    const bool showLog = busy || (song == nullptr && ! log.isEmpty());
    g.setColour (Palette::dim);
    g.setFont (capsFont (11.5f));
    if (! stemsTab.isVisible()) g.drawText (showLog ? "LOG" : "STEMS", stemsHeader, juce::Justification::centredLeft, false);
    else
    {
        const auto pill = tabsArea.toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (Palette::panelBorder.withMultipliedAlpha (1.6f));
        g.drawRoundedRectangle (pill, pill.getHeight() * 0.5f, 1.0f);
    }
    if (transposeUp.isVisible())
    {
        const int t = proc.getTranspose();
        g.setColour (t != 0 ? Palette::text : Palette::dim);
        g.setFont (font (Weight::Regular, 13.0f));
        g.drawText ("Transpose " + juce::String (t > 0 ? "+" : "") + juce::String (t), transposeArea, juce::Justification::centred, false);
    }

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
    if (song == nullptr) return;
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
    if (busy) wantInput = false; // a new song is on its way; show it when it's done

    const bool input = inputShown();
    for (auto* c : std::initializer_list<juce::Component*> { &linkBox, &chooseButton, &splitButton }) c->setVisible (input);
    refreshPasteButton();
    newSongButton.setVisible (! input);
    closeInputButton.setVisible (input && song != nullptr);
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
    resized(); // the top bar and the stems header depend on what's visible
    if (storagePanel != nullptr) storagePanel->refresh();
    for (auto* row : rows) row->setVisible (song != nullptr && ! chordsShown);
    followHost.setToggleState (proc.player().followsHost(), juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &position, &followHost }) c->setVisible (song != nullptr);

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
        s.slots = 12; // a place on the Camelot wheel, not an amount
        s.activeSlot = camelot.getIntValue() - 1;
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

void justStemEditor::refreshPasteButton()
{
    pasteButton.setVisible (inputShown() && ! proc.isBusy() && linkBox.isEmpty());
}

// ============================================================================ new song
bool justStemEditor::inputShown() const
{
    return proc.isBusy() || proc.getSong() == nullptr || wantInput;
}

void justStemEditor::showInput (bool show)
{
    wantInput = show;
    if (! show) proc.showError ({}); // drop a stale "not a link" message with the box
    refreshAll();
    if (show) linkBox.grabKeyboardFocus();
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
