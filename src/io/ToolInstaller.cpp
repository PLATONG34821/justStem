#include "ToolInstaller.h"
#include "../core/Separator.h"

#include <juce_cryptography/juce_cryptography.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef JUSTSTEM_VERSION
 #error "JUSTSTEM_VERSION must be defined (CMakeLists.txt sets it from the project version)"
#endif

namespace juststem
{
namespace
{
struct HttpError : std::runtime_error
{
    int status;
    HttpError (int code, const std::string& msg) : std::runtime_error (msg), status (code) {}
};

void download (const juce::URL& url, const juce::File& dest, const juce::String& step, const ProgressCallback& progress,
               const std::atomic<bool>& cancel)
{
    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (20000)
                                             .withNumRedirectsToFollow (10)
                                             .withStatusCode (&status));
    if (stream == nullptr || status >= 400)
        throw HttpError (status, "Download failed (HTTP " + std::to_string (status) + "): " + url.toString (false).toStdString());

    dest.deleteFile();
    juce::FileOutputStream out (dest);
    if (! out.openedOk()) throw std::runtime_error ("Can't write " + dest.getFullPathName().toStdString());

    const auto total = stream->getTotalLength();
    juce::HeapBlock<char> buf (1 << 16);
    juce::int64 done = 0;
    for (int n; (n = stream->read (buf.get(), 1 << 16)) > 0;)
    {
        if (cancel.load()) throw std::runtime_error ("cancelled");
        if (! out.write (buf.get(), (size_t) n))
            throw std::runtime_error ("Write failed (disk full?): " + dest.getFullPathName().toStdString());
        done += n;
        if (progress) progress (step, total > 0 ? (double) done / (double) total : -1.0);
    }
    out.flush();
    if (out.getStatus().failed())
        throw std::runtime_error ("Write failed: " + out.getStatus().getErrorMessage().toStdString());
    if (done == 0 || (total > 0 && done != total))
        throw std::runtime_error ("Download interrupted (" + juce::File::descriptionOfSizeInBytes (done).toStdString()
                                  + (total > 0 ? " of " + juce::File::descriptionOfSizeInBytes (total).toStdString() : "") + ")");
}

/** Throws (and deletes `file`) unless its SHA-256 matches the entry for `name` in the checksum file at `sumsUrl`. */
void verifyChecksum (const juce::File& file, const juce::URL& sumsUrl, const juce::String& name)
{
    int status = 0;
    auto stream = sumsUrl.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs (20000)
                                                 .withNumRedirectsToFollow (10)
                                                 .withStatusCode (&status));
    const auto text = stream != nullptr && status < 400 ? stream->readEntireStreamAsString() : juce::String();
    const auto expected = findSha256InSums (text, name);
    if (expected.isEmpty())
    {
        file.deleteFile();
        throw std::runtime_error ("Couldn't fetch checksums for " + name.toStdString());
    }

    juce::FileInputStream in (file);
    if (! juce::SHA256 (in).toHexString().equalsIgnoreCase (expected))
    {
        file.deleteFile();
        throw std::runtime_error (name.toStdString() + " checksum mismatch, download discarded");
    }
}

/** The official release files for this OS and CPU. */
struct Platform
{
    const char* ytDlpAsset; // in https://github.com/yt-dlp/yt-dlp/releases
    const char* denoTriple; // deno-<triple>.zip in https://github.com/denoland/deno/releases
    const char* exeSuffix;
};

constexpr Platform platform =
#if JUCE_MAC
    { "yt-dlp_macos", "aarch64-apple-darwin", "" };
#elif JUCE_WINDOWS && JUCE_ARM
    { "yt-dlp_arm64.exe", "aarch64-pc-windows-msvc", ".exe" };
#elif JUCE_WINDOWS
    { "yt-dlp.exe", "x86_64-pc-windows-msvc", ".exe" };
#elif JUCE_ARM
    { "yt-dlp_linux_aarch64", "aarch64-unknown-linux-gnu", "" };
#else
    { "yt-dlp_linux", "x86_64-unknown-linux-gnu", "" };
#endif

/** A small file holding the time a tool was last installed or updated. */
struct Stamp
{
    juce::File file;
    bool olderThanDays (int days) const
    {
        return juce::Time::currentTimeMillis() - file.loadFileAsString().getLargeIntValue() > (juce::int64) days * 24 * 3600 * 1000;
    }
    void touch() const { file.replaceWithText (juce::String (juce::Time::currentTimeMillis())); }
};

/** A temporary file or folder next to `exe`, of its own: plug-ins installing at the same time don't
    share (and delete) each other's downloads. */
juce::File scratchFor (const juce::File& exe, const char* suffix)
{
    return exe.getSiblingFile (exe.getFileNameWithoutExtension() + "-" + juce::Uuid().toString() + suffix);
}

/** Rename `from` onto `to` without unlinking `to` first (unlike File::moveFileTo), so an existing
    working tool survives if replacement fails. */
bool replaceFileAtomically (const juce::File& from, const juce::File& to)
{
    auto path = [] (const juce::File& f) {
        const auto s = f.getFullPathName().toStdString();
        return std::filesystem::path (std::u8string (s.begin(), s.end()));
    };
    std::error_code ec;
    std::filesystem::rename (path (from), path (to), ec);
    return ! ec;
}
} // namespace

juce::String findSha256InSums (const juce::String& sumsText, const juce::String& fileName)
{
    // Checksum files are "<hash>  <name>" (or "<hash> *<name>", "<hash>\t<name>"), or "<hash>" alone for a single file.
    const auto lines = juce::StringArray::fromLines (sumsText.trim());
    juce::String expected;
    for (const auto& raw : lines)
    {
        const auto line = raw.trim();
        const auto token = line.upToFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf ("\t", false, false);
        if (token.length() != 64 || ! token.containsOnly ("0123456789abcdefABCDEF")) continue;
        const auto rest = line.substring (token.length()).trimCharactersAtStart (" \t*");
        const auto name = rest.startsWith ("./") ? rest.substring (2) : rest;
        if ((lines.size() == 1 && rest.isEmpty()) || name == fileName)
            expected = token;
    }
    return expected;
}

juce::File ensureYtDlp (const ProgressCallback& progress, const std::atomic<bool>& cancel)
{
    const juce::String release = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";
    const juce::String binary = platform.ytDlpAsset;
    const auto exe = paths::tools().getChildFile (juce::String ("yt-dlp") + platform.exeSuffix);
    const Stamp stamp { paths::tools().getChildFile ("yt-dlp.lastupdate") };

    if (! exe.existsAsFile())
    {
        const auto tmp = scratchFor (exe, ".part");
        try
        {
            download (juce::URL (release + binary), tmp, "Downloading yt-dlp", progress, cancel);
            verifyChecksum (tmp, juce::URL (release + "SHA2-256SUMS"), binary);
            tmp.setExecutePermission (true);
            // Another plug-in may have installed it meanwhile (and, on Windows, be running it).
            if (! replaceFileAtomically (tmp, exe) && ! exe.existsAsFile()) throw std::runtime_error ("Can't install yt-dlp");
        }
        catch (...)
        {
            tmp.deleteFile();
            throw;
        }
        tmp.deleteFile();
        stamp.touch();
    }
    else if (stamp.olderThanDays (1))
    {
        // YouTube changes often. yt-dlp's own updater verifies its signatures.
        if (progress) progress ("Updating yt-dlp", -1.0);
        juce::ChildProcess p;
        bool finished = false;
        if (p.start (juce::StringArray { exe.getFullPathName(), "--ignore-config", "-U" }))
            for (const auto end = juce::Time::getMillisecondCounterHiRes() + 120000.0; ! (finished = p.waitForProcessToFinish (50));)
            {
                if (cancel.load()) { p.kill(); throw std::runtime_error ("cancelled"); } // the update is tried again next time
                if (juce::Time::getMillisecondCounterHiRes() > end) { p.kill(); break; }
            }
        if (finished) stamp.touch();
    }
    return exe;
}

juce::File ensureDeno (const ProgressCallback& progress, const std::atomic<bool>& cancel)
{
    const juce::String release = "https://github.com/denoland/deno/releases/latest/download/";
    const juce::String zipName = juce::String ("deno-") + platform.denoTriple + ".zip";
    const juce::String exeName = juce::String ("deno") + platform.exeSuffix;
    const auto exe = paths::tools().getChildFile (exeName);
    const Stamp stamp { paths::tools().getChildFile ("deno.lastupdate") };
    if (exe.existsAsFile() && ! stamp.olderThanDays (30)) return exe;

    const auto zip = scratchFor (exe, ".zip.part");
    const auto unpackDir = scratchFor (exe, ".unpack");
    try
    {
        download (juce::URL (release + zipName), zip, "Downloading Deno", progress, cancel);
        verifyChecksum (zip, juce::URL (release + zipName + ".sha256sum"), zipName);

        juce::ZipFile archive (zip);
        const int entry = archive.getIndexOfFileName (exeName);
        unpackDir.deleteRecursively();
        if (entry < 0 || archive.uncompressEntry (entry, unpackDir).failed())
            throw std::runtime_error ("Couldn't unpack Deno");
        const auto unpacked = unpackDir.getChildFile (exeName);
        unpacked.setExecutePermission (true);
        if (! replaceFileAtomically (unpacked, exe)) throw std::runtime_error ("Can't install Deno");
        stamp.touch();
    }
    catch (...)
    {
        zip.deleteFile();
        unpackDir.deleteRecursively();
        if (exe.existsAsFile()) return exe; // an update failed, but the installed copy still works
        throw;
    }
    zip.deleteFile();
    unpackDir.deleteRecursively();
    return exe;
}

juce::File ensureModel (const juce::File& dest, const ProgressCallback& progress, const std::atomic<bool>& cancel)
{
    if (dest.existsAsFile()) return dest;
    dest.getParentDirectory().createDirectory();

    const juce::String fileName = dest.getFileName();
    const juce::String step = fileName == juce::String::fromUTF8 (model::hq.fileName.data(), (int) model::hq.fileName.size())
                                  ? "Downloading HQ AI model"
                                  : "Downloading AI model";
    const juce::String repoBase = "https://github.com/PLATONG34821/justStem/releases/";
    const juce::String bases[] { repoBase + "download/models-v1/",
                                 repoBase + "download/v" + juce::String (JUSTSTEM_VERSION) + "/",
                                 repoBase + "latest/download/" };

    const auto tmp = scratchFor (dest, ".part");
    try
    {
        for (size_t i = 0; i < std::size (bases); ++i)
        {
            try
            {
                download (juce::URL (bases[i] + fileName), tmp, step, progress, cancel);
                verifyChecksum (tmp, juce::URL (bases[i] + "SHA256SUMS"), fileName);
                break;
            }
            catch (const HttpError& e)
            {
                if (i + 1 < std::size (bases) && e.status == 404) continue;
                throw;
            }
        }
        // Another plug-in may have finished installing the same model meanwhile.
        if (! replaceFileAtomically (tmp, dest) && ! dest.existsAsFile())
            throw std::runtime_error ("Can't install " + fileName.toStdString());
    }
    catch (...)
    {
        tmp.deleteFile();
        throw;
    }
    tmp.deleteFile();
    return dest;
}
} // namespace juststem
