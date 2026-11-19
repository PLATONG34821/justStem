#pragma once

#include "SongJob.h"
#include "StemPlayer.h"

#include <juce_audio_processors/juce_audio_processors.h>

/** The plug-in: passes its input through, adds the stem preview, and runs song jobs.
    Broadcasts a change message whenever a job starts or finishes. */
class justStemProcessor final : public juce::AudioProcessor, public juce::ChangeBroadcaster
{
public:
    /** The model shipped with the plug-in (in its bundle's Contents/Resources, or next to the binary). */
    static juce::File bundledModelFile();

    explicit justStemProcessor (juce::File modelFile = bundledModelFile());
    ~justStemProcessor() override;

    // ---- song workflow (message thread)
    void startYouTube (const juce::String& link) { startJob (juststem::SongJob::Kind::YouTube, link); }
    void startFile (const juce::File& file) { startJob (juststem::SongJob::Kind::File, file.getFullPathName()); }
    void cancelJob();
    bool isBusy() const { return job != nullptr; }
    juststem::SongJob::Status jobStatus() const;
    /** The running job's log, or the last job's once it has finished. */
    juce::StringArray jobLog() const { return job != nullptr ? job->log() : lastLog; }
    juce::String lastError() const { return error; }
    void showError (const juce::String& message) { error = message; sendChangeMessage(); }
    juststem::SongResultPtr getSong() const { return song; }
    juststem::StemPlayer& player() { return stemPlayer; }
    /** Semitones to transpose the chords and the preview by (saved with the DAW project). */
    int getTranspose() const { return transpose; }
    void setTranspose (int semitones)
    {
        transpose = juce::jlimit (-11, 11, semitones);
        stemPlayer.setTranspose (transpose);
        sendChangeMessage();
    }

    // ---- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
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

private:
    void startJob (juststem::SongJob::Kind, const juce::String& source);

    const juce::File modelFile;
    juststem::StemPlayer stemPlayer;
    std::unique_ptr<juststem::SongJob> job;
    juststem::SongResultPtr song;
    juce::String error;
    juce::StringArray lastLog;
    int transpose = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true); // guards async job callbacks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (justStemProcessor)
};
