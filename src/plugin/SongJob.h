#pragma once

#include "SongResult.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

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
    };

    /** Called on the message thread when the job ends. `error` is "cancelled" after cancel(). */
    using Finished = std::function<void (SongResultPtr result, juce::String error)>;

    /** @param source  a YouTube link, an audio file path, or (Reload) a saved song folder. */
    SongJob (Kind, juce::String source, juce::File modelFile, Finished onFinished);
    ~SongJob() override;

    void start() { startThread (juce::Thread::Priority::normal); }
    void cancel() { cancelled = true; }
    Status status() const;

    /** Timestamped lines describing what the job has done so far ("[0:03] Reading audio"). */
    juce::StringArray log() const;

private:
    void run() override;
    void setStatus (const juce::String& step, double fraction);
    void addLog (const juce::String& line);
    SongResultPtr process();

    const Kind kind;
    const juce::String source;
    const juce::File modelFile;
    const Finished onFinished;
    std::atomic<bool> cancelled { false };
    mutable juce::CriticalSection statusLock; // guards current, logLines, lastStep, lastTenth
    Status current;
    juce::StringArray logLines;
    juce::String lastStep;
    int lastTenth = 0;
    const double startMs = juce::Time::getMillisecondCounterHiRes();
};
} // namespace juststem
