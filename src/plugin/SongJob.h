#pragma once

#include "SongResult.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>

namespace juststem
{
/** Runs one song on a background thread: get audio -> split -> analyse -> save,
    or reload a song saved earlier. */
class SongJob final : private juce::Thread
{
public:
    enum class Kind { YouTube, File, Reload };

    struct Status
    {
        juce::String step;     // human-readable phase
        double fraction = -1;  // 0..1, or -1 when unknown
        /** During a one-time step with a known typical length (compiling the model): seconds left, 0 once
            it runs over, -1 otherwise. status() works this and `fraction` out from the clock. */
        double secondsLeft = -1;
    };

    /** Called on the message thread when the job ends. `error` is "cancelled" after cancel(). */
    using Finished = std::function<void (SongResultPtr result, juce::String error)>;

    /** Where the MIDI models are: model::notesFile, and model::pianoFile (its other two files beside it). */
    struct MidiModels
    {
        juce::File notes, piano;
    };

    /** @param source  a YouTube link, an audio file path, or (Reload) a saved song folder.
        @param allowModelDownload  download the HQ or piano model when it's missing (the plug-in), or not (tests). */
    SongJob (Kind, juce::String source, juce::File modelFile, MidiModels, Finished onFinished, bool allowModelDownload = false);
    ~SongJob() override;

    void start() { startThread (juce::Thread::Priority::normal); }
    void cancel() { cancelled = true; }

    /** For an owner that goes away mid-job (the plug-in is closed while splitting): cancels the job and
        lets it end on its own instead of waiting for it. Some steps can't stop part-way (loading or
        compiling the model, updating yt-dlp), and waiting for them would freeze the DAW. The job is
        deleted on the message thread once its thread has ended; `onFinished` isn't called.
        Message thread. */
    static void abandon (std::unique_ptr<SongJob>);
    /** How many abandoned jobs haven't ended yet (for tests). Message thread. */
    static int numAbandoned();
    Status status() const;

    /** Timestamped lines describing what the job has done so far ("[0:03] Reading audio"). */
    juce::StringArray log() const;

private:
    void run() override;
    void setStatus (const juce::String& step, double fraction);
    /** Like setStatus, but the gauge counts down from `expectedSeconds` until the next setStatus. */
    void setTimedStatus (const juce::String& step, double expectedSeconds);
    void addLog (const juce::String& line);
    SongResultPtr process();
    /** MIDI of the song's pitched stems (model::midiStems) it doesn't have yet, into its folder. */
    void makeMidi (const SongResult&);

    const Kind kind;
    const juce::String source;
    const juce::File modelFile;
    const MidiModels midiModels;
    const Finished onFinished;
    const bool allowModelDownload;
    std::atomic<bool> cancelled { false };
    bool abandoned = false; // message thread
    mutable juce::CriticalSection statusLock; // guards current, logLines, lastStep, lastTenth
    Status current;
    juce::StringArray logLines;
    juce::String lastStep;
    int lastTenth = 0;
    double timedStartMs = 0, timedSeconds = 0; // setTimedStatus; timedSeconds 0 when not counting down
    const double startMs = juce::Time::getMillisecondCounterHiRes();
};
} // namespace juststem
