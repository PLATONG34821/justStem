#pragma once

#include "SongJob.h"
#include "StemPlayer.h"
#include "../io/UpdateCheck.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

namespace juststem::ui { class StemExport; }

/** The plug-in: passes its input through, adds the stem preview, and runs song jobs.
    Broadcasts a change message whenever a job starts or finishes. */
class justStemProcessor final : public juce::AudioProcessor, public juce::ChangeBroadcaster
{
public:
    /** A bundled model (in Contents/Resources or next to the binary), or its path in paths::models()
        where it is downloaded on first use. */
    static juce::File defaultModelFile (std::string_view fileName);
    static juce::File bundledModelFile();

    justStemProcessor();
    explicit justStemProcessor (juce::File fastModelFile);
    ~justStemProcessor() override;

    // ---- song workflow (message thread)
    void startYouTube (const juce::String& link) { startJob (juststem::SongJob::Kind::YouTube, link); }
    void startFile (const juce::File& file) { startJob (juststem::SongJob::Kind::File, file.getFullPathName()); }
    /** Load a song split earlier (its folder under the songs folder) without splitting it again. */
    void openSaved (const juce::File& folder)
    {
        { const juce::ScopedLock sl (songLock); if (job == nullptr) pendingFolder = folder; }
        startJob (juststem::SongJob::Kind::Reload, folder.getFullPathName());
    }
    void cancelJob();
    bool isBusy() const { return job != nullptr; }
    juststem::SongJob::Status jobStatus() const;
    /** The running job's log, or the last job's once it has finished. */
    juce::StringArray jobLog() const { return job != nullptr ? job->log() : lastLog; }
    juce::String lastError() const { return error; }
    void showError (const juce::String& message) { error = message; sendChangeMessage(); }
    juststem::SongResultPtr getSong() const { return song; }
    juststem::StemPlayer& player() { return stemPlayer; }
    void exportStems (const juce::File& dest, juce::Component* editor = nullptr);
    bool isExporting() const { return stemExport != nullptr; }
    /** HQ mode splits with BS-RoFormer instead of HT-Demucs (saved with the DAW project). */
    bool hqAvailable() const { return allowModelDownload || hqModelFile.existsAsFile(); }
    bool getHQ() const { return hq && hqAvailable(); }
    void setHQ (bool on)
    {
        if (hq.exchange (on) != on) notifyHostStateChanged();
        sendChangeMessage();
    }
    /** Semitones to transpose the chords and the preview by (saved with the DAW project). */
    int getTranspose() const { return transpose; }
    void setTranspose (int semitones)
    {
        const int clamped = juce::jlimit (-11, 11, semitones);
        if (transpose.exchange (clamped) != clamped)
        {
            stemPlayer.setTranspose (clamped);
            notifyHostStateChanged();
        }
        sendChangeMessage();
    }

    // ---- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    using juce::AudioProcessor::processBlock; // keep the double-precision overload visible
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** The songs open in every justStem in this process (justStem on several tracks), and whether any
        of them is working: Storage mustn't delete from under another one. Message thread. */
    static juce::Array<juce::File> songsInUse();
    static bool anyBusy();

    /** Whether a newer release is out (checked once a day, shared by every instance). */
    juststem::UpdateCheck& updates() { return *updateCheck; }

private:
    void startJob (juststem::SongJob::Kind, const juce::String& source, bool fromRestore = false);
    /** Loads the song saved with a restored project, once any running job has ended. */
    void reloadRestored (const juce::File& folder);
    void notifyHostStateChanged();

    const juce::File fastModelFile, hqModelFile;
    const bool allowModelDownload = false;
    juststem::StemPlayer stemPlayer;
    std::unique_ptr<juststem::SongJob> job;
    std::unique_ptr<juststem::ui::StemExport> stemExport;
    juststem::SongResultPtr song; // set on the message thread, under songLock
    juce::File pendingFolder;     // target song folder while a restore/reload is in flight, under songLock
    // Hosts may save or restore the state on any thread, which reads `song` while a job replaces it.
    juce::CriticalSection songLock;
    juce::String error;
    juce::StringArray lastLog;
    juce::File restoredFolder; // a restored project's song, waiting for the running job (message thread)
    std::atomic<int> transpose { 0 };
    std::atomic<bool> hq { false };
    std::atomic<int> restoringState { 0 };
    juce::SharedResourcePointer<juststem::UpdateCheck> updateCheck;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true); // guards async job callbacks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (justStemProcessor)
};
