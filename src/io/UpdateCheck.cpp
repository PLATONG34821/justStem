#include "UpdateCheck.h"

#include "AppPaths.h"

#ifndef JUSTSTEM_VERSION
 #error "JUSTSTEM_VERSION must be defined (CMakeLists.txt sets it from the project version)"
#endif

namespace juststem
{
namespace
{
constexpr const char* repo = "PLATONG34821/justStem";
constexpr juce::int64 checkEveryMs = 24 * 60 * 60 * 1000;

juce::File cacheFile() { return paths::supportRoot().getChildFile ("update-check.json"); }

juce::String stripV (const juce::String& tag) { return tag.trim().trimCharactersAtStart ("vV"); }
} // namespace

UpdateCheck::UpdateCheck() : juce::Thread ("justStem update check")
{
    const auto env = [] (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}).isNotEmpty(); };
    if (! env ("JUSTSTEM_DATA_DIR") && ! env ("JUSTSTEM_NO_UPDATE_CHECK"))
        startThread (juce::Thread::Priority::low);
}

UpdateCheck::~UpdateCheck()
{
    stopThread (6000); // the request times out after 5 s
}

juce::String UpdateCheck::newerVersion() const
{
    const juce::ScopedLock sl (lock);
    return newer;
}

juce::URL UpdateCheck::releasePage()
{
    return juce::URL ("https://github.com/" + juce::String (repo) + "/releases/latest");
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

juce::String UpdateCheck::fetchLatestTag()
{
    const juce::URL url ("https://api.github.com/repos/" + juce::String (repo) + "/releases/latest");
    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (5000)
                                             .withNumRedirectsToFollow (3)
                                             .withExtraHeaders ("Accept: application/vnd.github+json\r\n"
                                                                "User-Agent: justStem\r\n") // GitHub refuses requests without one
                                             .withStatusCode (&status));
    if (stream == nullptr || status != 200 || threadShouldExit()) return {};
    return juce::JSON::parse (stream->readEntireStreamAsString())["tag_name"].toString();
}

void UpdateCheck::run()
{
    const auto cache = juce::JSON::parse (cacheFile());
    const auto now = juce::Time::currentTimeMillis();
    auto tag = cache["tag"].toString();
    const auto checked = (juce::int64) cache["checked"];

    if (tag.isEmpty() || now - checked > checkEveryMs || checked > now)
    {
        if (const auto fetched = fetchLatestTag(); fetched.isNotEmpty())
        {
            tag = fetched;
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("tag", tag);
            obj->setProperty ("checked", now);
            cacheFile().replaceWithText (juce::JSON::toString (juce::var (obj)));
        }
    }

    if (! threadShouldExit() && isNewer (tag, JUSTSTEM_VERSION))
    {
        const juce::ScopedLock sl (lock);
        newer = stripV (tag);
    }
}
} // namespace juststem
