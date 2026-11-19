#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "../core/Separator.h"

using namespace covertools;

juce::File CoverToolsProcessor::bundledModelFile()
{
    // For a plug-in, currentExecutableFile is the plug-in binary itself, not the host.
    const auto binaryDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const juce::String name { std::string (model::fileName) };
    const juce::File candidates[] {
        // Bundles keep resources in Contents/Resources next to the binary's folder: macOS bundles
        // (Contents/MacOS) and VST3 bundles on every OS (Contents/x86_64-win, Contents/x86_64-linux).
        binaryDir.getSiblingFile ("Resources").getChildFile (name),
        // A plain executable or library (e.g. the Windows / Linux standalone app): next to it.
        binaryDir.getChildFile (name),
    };
    for (const auto& f : candidates)
        if (f.existsAsFile()) return f;
    return candidates[0]; // missing: SongJob reports it
}

CoverToolsProcessor::CoverToolsProcessor (juce::File model)
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      modelFile (std::move (model))
{
}

CoverToolsProcessor::~CoverToolsProcessor()
{
    *alive = false;
    job.reset(); // stops the thread
}

void CoverToolsProcessor::prepareToPlay (double hostRate, int samplesPerBlock)
{
    stemPlayer.prepare (hostRate, samplesPerBlock);
}

bool CoverToolsProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
        && layouts.getMainInputChannelSet() == out;
}

void CoverToolsProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // The input passes through; the stem preview is added on top.
    double hostSeconds = -1;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition(); pos && pos->getIsPlaying())
            if (auto t = pos->getTimeInSeconds()) hostSeconds = *t;
    stemPlayer.process (buffer, hostSeconds);
}

void CoverToolsProcessor::startJob (SongJob::Kind kind, const juce::String& source)
{
    if (job != nullptr) return;
    error.clear();
    std::weak_ptr<bool> token = alive;
    job = std::make_unique<SongJob> (kind, source, modelFile, [this, token] (SongResultPtr result, juce::String err) {
        if (token.expired()) return;
        lastLog = job->log();
        job.reset();
        if (result != nullptr)
        {
            song = result;
            stemPlayer.setSong (song);
        }
        else if (err != "cancelled")
            error = err;
        sendChangeMessage();
    });
    job->start();
    sendChangeMessage();
}

void CoverToolsProcessor::cancelJob()
{
    if (job != nullptr) job->cancel();
}

SongJob::Status CoverToolsProcessor::jobStatus() const
{
    return job != nullptr ? job->status() : SongJob::Status {};
}

void CoverToolsProcessor::getStateInformation (juce::MemoryBlock& data)
{
    juce::ValueTree state ("CoverTools");
    if (song != nullptr) state.setProperty ("folder", song->folder.getFullPathName(), nullptr);
    state.setProperty ("followHost", stemPlayer.followsHost(), nullptr);
    state.setProperty ("transpose", transpose, nullptr);
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

void CoverToolsProcessor::setStateInformation (const void* data, int size)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t) size);
    if (! state.hasType ("CoverTools")) return;
    stemPlayer.setFollowHost (state["followHost"]);
    transpose = juce::jlimit (-11, 11, (int) state["transpose"]);
    stemPlayer.setTranspose (transpose);
    for (int i = 0; i < numStems && i < state.getNumChildren(); ++i)
    {
        const auto s = state.getChild (i);
        stemPlayer.setVolume (i, (float) s["volume"]);
        stemPlayer.setMute (i, s["mute"]);
        stemPlayer.setSolo (i, s["solo"]);
    }
    const juce::File folder (state["folder"].toString());
    if (folder.isDirectory() && (song == nullptr || song->folder != folder))
        juce::MessageManager::callAsync ([this, token = std::weak_ptr<bool> (alive), folder] {
            if (! token.expired()) startJob (SongJob::Kind::Reload, folder.getFullPathName());
        });
}

juce::AudioProcessorEditor* CoverToolsProcessor::createEditor()
{
    return new CoverToolsEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CoverToolsProcessor();
}
