#pragma once

#include "SongJob.h"
#include "StemPlayer.h"

#include <juce_audio_processors/juce_audio_processors.h>

/** The plug-in: passes its input through, adds the stem preview, and runs song jobs.
    Broadcasts a change message whenever a job starts or finishes. */
class CoverToolsProcessor final : public juce::AudioProcessor, public juce::ChangeBroadcaster
{
public:
    /** The model shipped with the plug-in (in its bundle's Contents/Resources, or next to the binary). */
    static juce::File bundledModelFile();

    explicit CoverToolsProcessor (juce::File modelFile = bundledModelFile());
    ~CoverToolsProcessor() override;

    // ---- song workflow (message thread)
    void startYouTube (const juce::String& link) { startJob (covertools::SongJob::Kind::YouTube, link); }
    void startFile (const juce::File& file) { startJob (covertools::SongJob::Kind::File, file.getFullPathName()); }
    void cancelJob();
    bool isBusy() const { return job != nullptr; }
    covertools::SongJob::Status jobStatus() const;
    /** The running job's log, or the last job's once it has finished. */
    juce::StringArray jobLog() const { return job != nullptr ? job->log() : lastLog; }
    juce::String lastError() const { return error; }
    void showError (const juce::String& message) { error = message; sendChangeMessage(); }
    covertools::SongResultPtr getSong() const { return song; }
    covertools::StemPlayer& player() { return stemPlayer; }
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
    void startJob (covertools::SongJob::Kind, const juce::String& source);

    const juce::File modelFile;
    covertools::StemPlayer stemPlayer;
    std::unique_ptr<covertools::SongJob> job;
    covertools::SongResultPtr song;
    juce::String error;
    juce::StringArray lastLog;
    int transpose = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true); // guards async job callbacks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoverToolsProcessor)
};
