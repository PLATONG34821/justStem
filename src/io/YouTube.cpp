#include "YouTube.h"
#include "ToolInstaller.h"

#include <stdexcept>

namespace juststem
{
bool isYouTubeLink (const juce::String& text)
{
    auto t = text.trim();
    if (! t.startsWithIgnoreCase ("http")) t = "https://" + t;
    const auto host = juce::URL (t).getDomain().toLowerCase().fromLastOccurrenceOf ("www.", false, false);
    return host == "youtube.com" || host == "youtu.be" || host == "m.youtube.com" || host == "music.youtube.com"
        || host.endsWith (".youtube.com");
}

DownloadedAudio downloadYouTubeAudio (const juce::String& link, const ProgressCallback& progress, const std::atomic<bool>& cancel)
{
    if (! isYouTubeLink (link)) throw std::runtime_error ("That doesn't look like a YouTube link");

    const auto ytdlp = ensureYtDlp (progress, cancel);
    const auto deno = ensureDeno (progress, cancel);

    // No shell is involved, so nothing in `link` can run as a command. yt-dlp prints the title
    // before downloading, progress lines while downloading, and the final file path at the end.
    const juce::StringArray args {
        ytdlp.getFullPathName(),
        "--ignore-config", "--no-playlist",
        "--js-runtimes", "deno:" + deno.getFullPathName(),
        "-f", "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]",
        "-o", paths::downloads().getChildFile ("%(id)s.%(ext)s").getFullPathName(),
        "--no-simulate", "--newline", "--progress",
        "--progress-template", "download:juststem-progress %(progress._percent_str)s",
        "--print", "before_dl:juststem-title %(title)s",
        "--print", "after_move:juststem-file %(filepath)s",
        "--", link.trim()
    };

    if (progress) progress ("Getting audio from YouTube", 0.0);
    juce::ChildProcess proc;
    if (! proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        throw std::runtime_error ("Couldn't start yt-dlp");

    DownloadedAudio result;
    juce::String pending, errors;
    char buf[4096];
    for (;;)
    {
        if (cancel.load())
        {
            proc.kill();
            throw std::runtime_error ("cancelled");
        }
        const int n = proc.readProcessOutput (buf, sizeof (buf));
        if (n <= 0)
        {
            if (! proc.isRunning()) break;
            juce::Thread::sleep (20);
            continue;
        }
        pending += juce::String::fromUTF8 (buf, n);
        while (pending.containsChar ('\n'))
        {
            const auto line = pending.upToFirstOccurrenceOf ("\n", false, false).trim();
            pending = pending.fromFirstOccurrenceOf ("\n", false, false);
            if (line.startsWith ("juststem-title ")) result.title = line.fromFirstOccurrenceOf (" ", false, false);
            else if (line.startsWith ("juststem-file ")) result.file = juce::File (line.fromFirstOccurrenceOf (" ", false, false));
            else if (line.startsWith ("juststem-progress "))
            {
                const auto pct = line.fromFirstOccurrenceOf (" ", false, false).retainCharacters ("0123456789.");
                if (progress && pct.isNotEmpty()) progress ("Getting audio from YouTube", pct.getDoubleValue() / 100.0);
            }
            else if (line.startsWith ("ERROR")) errors << line << "\n";
            else if (line.startsWith ("WARNING")) { DBG (line); }
        }
    }

    if (proc.getExitCode() != 0 || ! result.file.existsAsFile())
    {
        auto msg = errors.trim();
        if (msg.containsIgnoreCase ("Sign in to confirm") || msg.containsIgnoreCase ("bot"))
            msg = "YouTube asked to confirm you're not a bot. Try again later, or download the song and drop the file in.";
        else if (msg.containsIgnoreCase ("Requested format is not available"))
            msg = "This video has no AAC audio stream. Download the song yourself and drop the file in.";
        else if (msg.isEmpty())
            msg = "yt-dlp failed (exit code " + juce::String (proc.getExitCode()) + ")";
        throw std::runtime_error (msg.toStdString());
    }
    if (result.title.isEmpty()) result.title = result.file.getFileNameWithoutExtension();
    return result;
}
} // namespace juststem
