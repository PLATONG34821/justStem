#pragma once

#include <juce_core/juce_core.h>

namespace juststem
{
/** Asks GitHub, on a background thread, whether a newer justStem release is out.

    It downloads only the latest release's tag (nothing about the user is sent) and asks at most once a
    day: the answer is cached in supportRoot()/update-check.json. Offline or any error: it stays quiet.
    Skipped when JUSTSTEM_DATA_DIR is set (tests, screenshots) or JUSTSTEM_NO_UPDATE_CHECK is set.

    Share one between plug-in instances with juce::SharedResourcePointer<UpdateCheck>. */
class UpdateCheck final : private juce::Thread
{
public:
    UpdateCheck();
    ~UpdateCheck() override;

    /** Starts the check, the first time it's called: when a plug-in window opens. Not on creating the
        plug-in, as DAWs' plug-in scanners and validators (Logic's auval) create and delete it many
        times, and each delete would wait for the request. Message thread. */
    void start();

    /** The newer release's version (e.g. "0.0.3"), or empty while checking / when up to date. Thread-safe. */
    juce::String newerVersion() const;

    /** The GitHub page with the latest release, to open in a browser. */
    static juce::URL releasePage();

    /** The user pressed Later or Download for `version`: don't announce it again (the Update pill stays). */
    static void dismiss (const juce::String& version);
    static bool wasDismissed (const juce::String& version);

    /** True if `latest` (e.g. "v0.1.0") is a higher version than `current` (e.g. "0.0.2"). */
    static bool isNewer (const juce::String& latest, const juce::String& current);

private:
    void run() override;
    juce::String fetchLatestTag();

    mutable juce::CriticalSection lock;
    juce::String newer;
    bool started = false; // message thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateCheck)
};
} // namespace juststem
