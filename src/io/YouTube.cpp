#include "YouTube.h"
#include "AudioFiles.h"
#include "ToolInstaller.h"

#include <stdexcept>
#include <thread>
#include <utility>

namespace juststem
{
DownloadedAudio& DownloadedAudio::operator= (DownloadedAudio&& other) noexcept
{
    if (this != &other)
    {
        deleteFolder();
        file = std::move (other.file);
        title = std::move (other.title);
        folder = std::exchange (other.folder, juce::File());
    }
    return *this;
}

void DownloadedAudio::deleteFolder()
{
    if (folder.isAChildOf (paths::downloads())) folder.deleteRecursively();
}

bool isYouTubeLink (const juce::String& text)
{
    auto t = text.trim();
    if (! t.contains ("://")) t = "https://" + t;
    if (! t.startsWithIgnoreCase ("https://") && ! t.startsWithIgnoreCase ("http://")) return false;
    const auto afterScheme = t.fromFirstOccurrenceOf ("://", false, false);
    const int end = afterScheme.indexOfAnyOf ("/?#");
    const auto authority = end >= 0 ? afterScheme.substring (0, end) : afterScheme;
    if (authority.isEmpty() || authority.containsAnyOf ("@ \t\r\n")) return false;
    if (authority.containsChar (':'))
    {
        const auto port = authority.fromFirstOccurrenceOf (":", false, false);
        if (port.isEmpty() || ! port.containsOnly ("0123456789")) return false;
    }
    auto host = authority.upToFirstOccurrenceOf (":", false, false).toLowerCase();
    if (host.startsWith ("www.")) host = host.substring (4);
    // Plain DNS names only: a backslash or other odd character can be parsed as a different host downstream.
    if (! host.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789.-") || host.startsWith (".") || host.contains ("..")) return false;
    return host == "youtube.com" || host == "youtu.be" || host.endsWith (".youtube.com");
}

void readUtf8Lines (std::string& pending, const char* data, int bytes, bool eof,
                    const std::function<void (const juce::String&)>& onLine)
{
    if (bytes > 0) pending.append (data, (std::size_t) bytes);
    std::size_t start = 0;
    for (;;)
    {
        const auto nl = pending.find ('\n', start);
        if (nl == std::string::npos) break;
        onLine (juce::String::fromUTF8 (pending.data() + start, (int) (nl - start)).trim());
        start = nl + 1;
    }
    pending.erase (0, start);
    if (eof && ! pending.empty())
    {
        onLine (juce::String::fromUTF8 (pending.data(), (int) pending.size()).trim());
        pending.clear();
    }
}

DownloadedAudio downloadYouTubeAudio (const juce::String& link, const ProgressCallback& progress, const std::atomic<bool>& cancel)
{
    if (! isYouTubeLink (link)) throw std::runtime_error ("That doesn't look like a YouTube link");

    const auto ytdlp = ensureYtDlp (progress, cancel);
    const auto deno = ensureDeno (progress, cancel);

    // A folder of its own, so plug-ins downloading at the same time don't meet, and a cancelled or
    // failed download (yt-dlp's .part files) goes with `result`.
    const auto folder = paths::downloads().getChildFile (juce::Uuid().toString());
    folder.createDirectory();
    DownloadedAudio result (folder);

    // No shell is involved, so nothing in `link` can run as a command. yt-dlp prints the title
    // before downloading, progress lines while downloading, and the final file path at the end.
    const juce::StringArray args {
        ytdlp.getFullPathName(),
        "--ignore-config", "--no-playlist",
        "--encoding", "utf-8", // the title and paths it prints (on Windows it would use the ANSI code page)
        // Too long (or live, with no length): refused before anything is downloaded; yt-dlp exits with 101.
        "--break-match-filters", "duration <= " + juce::String ((int) maxSongSeconds),
        "--js-runtimes", "deno:" + deno.getFullPathName(),
        "-f", "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]",
        "-o", folder.getChildFile ("%(id)s.%(ext)s").getFullPathName(),
        "--no-simulate", "--newline", "--progress",
        "--progress-template", "download:juststem-progress %(progress._percent_str)s",
        "--print", "before_dl:juststem-title %(title)s",
        "--print", "after_move:juststem-file %(filepath)s",
        "--", link.trim()
    };

    if (progress) progress ("Getting audio", 0.0);
    juce::ChildProcess proc;
    if (! proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        throw std::runtime_error ("Couldn't start yt-dlp");

    // readProcessOutput waits until its buffer is full or yt-dlp exits, which can be a long time while
    // yt-dlp prints nothing; this watcher ends yt-dlp on cancel so the read returns.
    struct Watcher
    {
        std::atomic<bool> done { false };
        std::thread thread;
        ~Watcher() { done = true; if (thread.joinable()) thread.join(); }
    } watcher;
    watcher.thread = std::thread ([&] {
        while (! watcher.done.load())
        {
            if (cancel.load()) { proc.kill(); return; }
            juce::Thread::sleep (50);
        }
    });

    std::string pending;
    juce::String errors;
    auto handleLine = [&] (const juce::String& line) {
        if (line.startsWith ("juststem-title ")) result.title = line.fromFirstOccurrenceOf (" ", false, false);
        else if (line.startsWith ("juststem-file ")) result.file = juce::File (line.fromFirstOccurrenceOf (" ", false, false));
        else if (line.startsWith ("juststem-progress "))
        {
            const auto pct = line.fromFirstOccurrenceOf (" ", false, false).retainCharacters ("0123456789.");
            if (progress && pct.isNotEmpty()) progress ("Getting audio", pct.getDoubleValue() / 100.0);
        }
        else if (line.startsWith ("ERROR")) errors << line << "\n";
        else if (line.startsWith ("WARNING")) { DBG (line); }
    };
    char buf[256]; // small, so progress lines arrive as they're printed
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
            if (! proc.isRunning())
            {
                readUtf8Lines (pending, nullptr, 0, true, handleLine);
                break;
            }
            juce::Thread::sleep (20);
            continue;
        }
        readUtf8Lines (pending, buf, n, false, handleLine);
    }

    if (proc.getExitCode() == 101)
        throw std::runtime_error (std::string (songTooLong().what()) + ", and neither are live streams");
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
