#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/SaveStems.h"
#include "../core/Separator.h"
#include "Text.h"

#include <algorithm>
#include <utility>

using namespace juststem;

namespace
{
/** Every live processor (see songsInUse); hosts may create and delete them on any thread. */
juce::CriticalSection instancesLock;
juce::Array<justStemProcessor*> instances;
} // namespace

juce::File justStemProcessor::defaultModelFile (std::string_view fileName)
{
    // For a plug-in, currentExecutableFile is the plug-in binary itself, not the host.
    const auto binaryDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto name = text::from (fileName);
    const juce::File candidates[] {
        // Bundles keep resources in Contents/Resources next to the binary's folder: macOS bundles
        // (Contents/MacOS) and VST3 bundles on every OS (Contents/x86_64-win, Contents/x86_64-linux).
        binaryDir.getSiblingFile ("Resources").getChildFile (name),
        // A plain executable or library (e.g. the Windows / Linux standalone app): next to it.
        binaryDir.getChildFile (name),
       #if JUCE_MAC
        // Shared system folder installed by the macOS .pkg so VST3, AU and Standalone share one copy.
        juce::File ("/Library/Application Support/justStem/models").getChildFile (name),
       #endif
    };
    for (const auto& f : candidates)
        if (f.existsAsFile()) return f;
    return paths::models().getChildFile (name);
}

juce::File justStemProcessor::bundledModelFile()
{
    return defaultModelFile (model::fast.fileName);
}

justStemProcessor::justStemProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      fastModelFile (defaultModelFile (model::fast.fileName)),
      hqModelFile (defaultModelFile (model::hq.fileName)),
      allowModelDownload (true)
{
    const juce::ScopedLock sl (instancesLock);
    instances.add (this);
    stemPlayer.onSettingChanged = [this] { notifyHostStateChanged(); };
}

justStemProcessor::justStemProcessor (juce::File fastModel)
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      fastModelFile (std::move (fastModel)),
      hqModelFile (fastModelFile.getSiblingFile (text::from (model::hq.fileName))),
      allowModelDownload (false)
{
    const juce::ScopedLock sl (instancesLock);
    instances.add (this);
    stemPlayer.onSettingChanged = [this] { notifyHostStateChanged(); };
}

justStemProcessor::~justStemProcessor()
{
    stemExport.reset();
    {
        const juce::ScopedLock sl (instancesLock);
        instances.removeFirstMatchingValue (this);
    }
    SongJob::abandon (std::move (job)); // don't wait for it: see abandon
}

void justStemProcessor::prepareToPlay (double hostRate, int samplesPerBlock)
{
    stemPlayer.prepare (hostRate, samplesPerBlock);
}

bool justStemProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
        && layouts.getMainInputChannelSet() == out;
}

void justStemProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // The input passes through; the stem preview is added on top.
    double hostSeconds = -1;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition(); pos && pos->getIsPlaying())
            if (auto t = pos->getTimeInSeconds()) hostSeconds = *t;
    stemPlayer.process (buffer, hostSeconds);
}

void justStemProcessor::notifyHostStateChanged()
{
    if (restoringState.load() == 0)
        updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void justStemProcessor::startJob (SongJob::Kind kind, const juce::String& source, bool fromRestore)
{
    if (job != nullptr) return;
    error.clear();
    std::weak_ptr<bool> token = alive;
    job = std::make_unique<SongJob> (kind, source, getHQ() ? hqModelFile : fastModelFile,
        [this, token, fromRestore] (SongResultPtr result, juce::String err) {
        if (token.expired()) return;
        lastLog = job->log();
        job.reset();
        if (result != nullptr)
        {
            {
                const juce::ScopedLock sl (songLock);
                song = result;
            }
            stemPlayer.setSong (song);
            if (! fromRestore) notifyHostStateChanged();
        }
        else if (err != "cancelled")
            error = err;
        sendChangeMessage();
        if (restoredFolder != juce::File()) reloadRestored (std::exchange (restoredFolder, {}));
    }, allowModelDownload);
    job->start();
    sendChangeMessage();
}

void justStemProcessor::exportStems (const juce::File& dest, juce::Component* editor)
{
    SongResultPtr s;
    {
        const juce::ScopedLock sl (songLock);
        s = song;
    }
    if (s == nullptr || stemExport != nullptr) return;
    std::weak_ptr<bool> token = alive;
    stemExport = ui::startSaveStems (std::move (s), getTranspose(), dest, editor, [this, token] {
        if (! token.expired()) stemExport.reset();
    });
}

void justStemProcessor::reloadRestored (const juce::File& folder)
{
    // The project's song wins, as when nothing is running; a song split meanwhile is saved, in Storage.
    if (job != nullptr) restoredFolder = folder;
    else if (folder.isDirectory() && (song == nullptr || song->folder != folder)) startJob (SongJob::Kind::Reload, folder.getFullPathName(), true);
}

juce::Array<juce::File> justStemProcessor::songsInUse()
{
    juce::Array<juce::File> folders;
    const juce::ScopedLock sl (instancesLock);
    for (auto* p : instances)
    {
        if (p->restoredFolder != juce::File()) folders.addIfNotAlreadyThere (p->restoredFolder); // about to load
        const juce::ScopedLock songSl (p->songLock);
        if (p->song != nullptr) folders.addIfNotAlreadyThere (p->song->folder);
    }
    return folders;
}

bool justStemProcessor::anyBusy()
{
    // A closed plug-in's job may still be ending (see SongJob::abandon), and using the tools.
    const juce::ScopedLock sl (instancesLock);
    return SongJob::numAbandoned() > 0 || std::any_of (instances.begin(), instances.end(), [] (const auto* p) { return p->isBusy(); });
}

void justStemProcessor::cancelJob()
{
    if (job != nullptr) job->cancel();
}

SongJob::Status justStemProcessor::jobStatus() const
{
    return job != nullptr ? job->status() : SongJob::Status {};
}

void justStemProcessor::getStateInformation (juce::MemoryBlock& data)
{
    juce::ValueTree state ("justStem");
    {
        const juce::ScopedLock sl (songLock);
        if (song != nullptr) state.setProperty ("folder", song->folder.getFullPathName(), nullptr);
    }
    state.setProperty ("followHost", stemPlayer.followsHost(), nullptr);
    state.setProperty ("transpose", transpose.load(), nullptr);
    state.setProperty ("hq", hq.load(), nullptr);
    for (int i = 0; i < numStems; ++i)
    {
        juce::ValueTree s ("Stem");
        s.setProperty ("volume", stemPlayer.getVolume (i), nullptr);
        s.setProperty ("mute", stemPlayer.getMute (i), nullptr);
        s.setProperty ("solo", stemPlayer.getSolo (i), nullptr);
        state.appendChild (s, nullptr);
    }
    juce::MemoryOutputStream out (data, false);
    state.writeToStream (out);
}

void justStemProcessor::setStateInformation (const void* data, int size)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t) size);
    if (! state.hasType ("justStem")) return;
    struct Restoring
    {
        std::atomic<int>& count;
        explicit Restoring (std::atomic<int>& c) : count (c) { ++count; }
        ~Restoring() { --count; }
    } restoring (restoringState);
    stemPlayer.setFollowHost (wrapperType != wrapperType_Standalone && (bool) state["followHost"]);
    transpose = juce::jlimit (-11, 11, (int) state["transpose"]);
    stemPlayer.setTranspose (transpose);
    hq = (bool) state["hq"];
    for (int i = 0; i < numStems && i < state.getNumChildren(); ++i)
    {
        const auto s = state.getChild (i);
        stemPlayer.setVolume (i, (float) s["volume"]);
        stemPlayer.setMute (i, s["mute"]);
        stemPlayer.setSolo (i, s["solo"]);
    }
    const auto path = state["folder"].toString();
    const juce::File folder = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    bool sameSong = false;
    {
        const juce::ScopedLock sl (songLock);
        sameSong = song != nullptr && song->folder == folder;
    }
    if (folder != juce::File() && ! sameSong)
        juce::MessageManager::callAsync ([this, token = std::weak_ptr<bool> (alive), folder] {
            if (token.expired()) return;
            // Deleted, or the project came from another computer: say so rather than opening empty.
            if (folder.isDirectory()) reloadRestored (folder);
            else showError ("The stems saved with this project weren't found (" + folder.getFullPathName() + "). Split the song again to get them back.");
        });
    sendChangeMessage(); // an open window shows the restored settings
}

juce::AudioProcessorEditor* justStemProcessor::createEditor()
{
    return new justStemEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new justStemProcessor();
}
