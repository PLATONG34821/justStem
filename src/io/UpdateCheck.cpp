#include "UpdateCheck.h"

#include "AppPaths.h"

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

#include <atomic>
#include <utility>

#ifndef JUSTSTEM_VERSION
 #error "JUSTSTEM_VERSION must be defined (CMakeLists.txt sets it from the project version)"
#endif

namespace juststem
{
void pinPluginBinary() noexcept
{
    static std::atomic<bool> pinned { false };
    if (pinned.exchange (true)) return;
#if defined (_WIN32)
    HMODULE self = nullptr;
    GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR> (&pinPluginBinary), &self);
#else
    Dl_info info {};
    if (dladdr (reinterpret_cast<const void*> (&pinPluginBinary), &info) && info.dli_fname != nullptr)
        dlopen (info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

struct UpdateCheck::State
{
    juce::CriticalSection lock;
    juce::String newer;
    std::atomic<bool> stop { false };
    std::atomic<bool> finished { false };
};

namespace
{
constexpr const char* repo = "PLATONG34821/justStem";
constexpr juce::int64 checkEveryMs = 24 * 60 * 60 * 1000;

juce::File cacheFile() { return paths::supportRoot().getChildFile ("update-check.json"); }

juce::File dismissedFile() { return paths::supportRoot().getChildFile ("update-dismissed.txt"); }

juce::String stripV (const juce::String& tag) { return tag.trim().trimCharactersAtStart ("vV"); }

juce::String fetchLatestTag (const std::atomic<bool>& stop)
{
    if (stop.load()) return {};
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5500.0;
    const juce::URL url ("https://api.github.com/repos/" + juce::String (repo) + "/releases/latest");
    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (5000)
                                             .withNumRedirectsToFollow (3)
                                             .withExtraHeaders ("Accept: application/vnd.github+json\r\n"
                                                                "User-Agent: justStem\r\n") // GitHub refuses requests without one
                                             .withStatusCode (&status));
    if (stream == nullptr || status != 200 || stop.load()) return {};

    // Bounded chunked read so a stalled stream or captive portal can neither hang forever nor fill RAM.
    juce::MemoryOutputStream body;
    char buf[4096];
    constexpr std::size_t maxBytes = 256 * 1024;
    for (;;)
    {
        if (stop.load() || juce::Time::getMillisecondCounterHiRes() > deadline) return {};
        const int n = stream->read (buf, (int) sizeof (buf));
        if (n <= 0) break;
        body.write (buf, (std::size_t) n);
        if (body.getDataSize() > maxBytes) return {};
    }
    if (stop.load()) return {};
    return juce::JSON::parse (body.toUTF8())["tag_name"].toString();
}
} // namespace

void UpdateCheck::runCheck (State& s)
{
    try
    {
        const auto cache = juce::JSON::parse (cacheFile());
        const auto now = juce::Time::currentTimeMillis();
        auto tag = cache["tag"].toString();
        const auto checked = (juce::int64) cache["checked"];

        if (tag.isEmpty() || now - checked > checkEveryMs || checked > now)
        {
            const auto fetched = fetchLatestTag (s.stop);
            if (s.stop.load()) { s.finished = true; return; }
            if (fetched.isNotEmpty())
            {
                tag = fetched;
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("tag", tag);
                obj->setProperty ("checked", now);
                cacheFile().replaceWithText (juce::JSON::toString (juce::var (obj)));
            }
        }

        if (! s.stop.load() && isNewer (tag, JUSTSTEM_VERSION))
        {
            const juce::ScopedLock sl (s.lock);
            s.newer = stripV (tag);
        }
    }
    catch (...) {}
    s.finished = true;
}

UpdateCheck::UpdateCheck() : state (std::make_shared<State>()) {}

void UpdateCheck::start()
{
    if (std::exchange (started, true)) return;
    const auto env = [] (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}).isNotEmpty(); };
    if (! env ("JUSTSTEM_DATA_DIR") && ! env ("JUSTSTEM_NO_UPDATE_CHECK"))
        worker = std::thread ([s = state] { runCheck (*s); });
}

UpdateCheck::~UpdateCheck()
{
    state->stop = true;
    if (worker.joinable())
    {
        if (state->finished.load())
            worker.join();
        else
        {
            // Don't block the DAW's main thread or force-kill a thread inside OS DNS / HTTP;
            // `state` stays alive via shared_ptr and exits right after the socket times out.
            pinPluginBinary();
            worker.detach();
        }
    }
}

juce::String UpdateCheck::newerVersion() const
{
    const juce::ScopedLock sl (state->lock);
    return state->newer;
}

juce::URL UpdateCheck::releasePage()
{
    return juce::URL ("https://github.com/" + juce::String (repo) + "/releases/latest");
}

void UpdateCheck::dismiss (const juce::String& version) { dismissedFile().replaceWithText (version); }

bool UpdateCheck::wasDismissed (const juce::String& version)
{
    return dismissedFile().loadFileAsString().trim() == version;
}

bool UpdateCheck::isNewer (const juce::String& latest, const juce::String& current)
{
    juce::StringArray a, b;
    a.addTokens (stripV (latest).upToFirstOccurrenceOf ("-", false, false), ".", {});
    b.addTokens (stripV (current).upToFirstOccurrenceOf ("-", false, false), ".", {});
    if (a.isEmpty() || ! a[0].containsOnly ("0123456789")) return false;
    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const int x = a[i].getIntValue(), y = b[i].getIntValue();
        if (x != y) return x > y;
    }
    return false;
}
} // namespace juststem
