#include "AudioFiles.h"
#include "M4aDecoder.h"
#include "../core/Resampler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cctype>
#include <cstring>

namespace juststem
{
namespace
{
/** The same set of readers on every OS (no CoreAudio / Media Foundation). M4A is handled by decodeM4a. */
juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager fm;
    static const bool registered = [] {
        fm.registerFormat (new juce::WavAudioFormat(), true);
        fm.registerFormat (new juce::AiffAudioFormat(), false);
        fm.registerFormat (new juce::FlacAudioFormat(), false);
        fm.registerFormat (new juce::OggVorbisAudioFormat(), false);
        fm.registerFormat (new juce::MP3AudioFormat(), false);
        return true;
    }();
    juce::ignoreUnused (registered);
    return fm;
}

/** MP3 gapless info {encoder delay, padding} from the LAME-style tag that LAME and ffmpeg write
    after the Xing / Info header in the first frame. {-1, -1} if the file has none. */
std::pair<int, int> mp3EncoderDelay (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk()) return { -1, -1 };

    // Skip any leading ID3v2 tags first (album art often exceeds 64 KB).
    for (;;)
    {
        const auto pos = in.getPosition();
        unsigned char hdr[10] {};
        if (in.read (hdr, 10) != 10 || std::memcmp (hdr, "ID3", 3) != 0
            || ((hdr[6] | hdr[7] | hdr[8] | hdr[9]) & 0x80) != 0)
        {
            in.setPosition (pos);
            break;
        }
        const juce::int64 tagSize = ((juce::int64) hdr[6] << 21) | ((juce::int64) hdr[7] << 14)
                                  | ((juce::int64) hdr[8] << 7) | (juce::int64) hdr[9];
        const juce::int64 footer = (hdr[5] & 0x10) != 0 ? 10 : 0;
        if (! in.setPosition (pos + 10 + tagSize + footer)) return { -1, -1 };
    }

    juce::MemoryBlock head;
    in.readIntoMemoryBlock (head, 4096);
    const auto* d = static_cast<const unsigned char*> (head.getData());
    const std::size_t size = head.getSize();
    auto u32 = [&] (std::size_t at) { return (juce::uint32) d[at] << 24 | (juce::uint32) d[at + 1] << 16 | (juce::uint32) d[at + 2] << 8 | d[at + 3]; };

    // The Xing / Info header sits inside the first frame, right after the side information.
    for (std::size_t at = 0; at + 8 <= size && at < 512; ++at)
    {
        if (std::memcmp (d + at, "Xing", 4) != 0 && std::memcmp (d + at, "Info", 4) != 0) continue;
        const auto flags = u32 (at + 4);
        std::size_t tag = at + 8;
        if (flags & 0x1) tag += 4;   // frame count
        if (flags & 0x2) tag += 4;   // byte count
        if (flags & 0x4) tag += 100; // seek table
        if (flags & 0x8) tag += 4;   // quality
        // Then a 9-byte encoder name ("LAME3.100", "Lavc60.31", ...); delay and padding are
        // two 12-bit numbers at bytes 21-23 of the tag.
        if (tag + 24 > size || ! std::isalpha (d[tag])) return { -1, -1 };
        return { (d[tag + 21] << 4) | (d[tag + 22] >> 4), ((d[tag + 22] & 0x0f) << 8) | d[tag + 23] };
    }
    return { -1, -1 };
}

constexpr double minAudioSampleRate = 8000.0, maxAudioSampleRate = 384000.0;
constexpr juce::int64 maxDecodedSamples = 500'000'000;

DecodedAudio readWithJuce (const juce::File& file, double maxSeconds)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));
    if (reader == nullptr) throw std::runtime_error ("Couldn't read \"" + file.getFileName().toStdString() + "\" as audio");
    if (! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0 || reader->lengthInSamples <= 0)
        throw std::runtime_error ("The audio file is empty");
    if (reader->sampleRate < minAudioSampleRate || reader->sampleRate > maxAudioSampleRate)
        throw std::runtime_error ("Unsupported sample rate in \"" + file.getFileName().toStdString() + "\"");
    if ((double) reader->lengthInSamples / reader->sampleRate > maxSeconds || reader->lengthInSamples > maxDecodedSamples)
        throw songTooLong (maxSeconds);

    const int n = (int) reader->lengthInSamples;
    const int ch = (int) reader->numChannels;
    // Only the first two channels are kept, so only those are read (a long 5.1 file would fill GBs).
    juce::AudioBuffer<float> buf (std::clamp (ch, 1, 2), n);
    reader->read (&buf, 0, n, 0, true, ch > 1);

    // MP3 gapless playback: JUCE's decoder keeps the encoder delay and its own 529-sample decoder
    // delay. Drop them (and the end padding), as other decoders and DAWs do.
    int skip = 0, keep = n;
    if (reader->getFormatName().containsIgnoreCase ("MP3"))
    {
        constexpr int decoderDelay = 529;
        const auto [delay, padding] = mp3EncoderDelay (file);
        skip = std::min (n, decoderDelay + std::max (0, delay));
        keep = std::max (0, n - skip - std::max (0, padding - decoderDelay));
    }

    // Stereo keeps L/R; mono is copied to both; more channels: the first two.
    DecodedAudio out;
    out.sampleRate = reader->sampleRate;
    const float* l = buf.getReadPointer (0) + skip;
    const float* r = buf.getReadPointer (ch > 1 ? 1 : 0) + skip;
    out.left.assign (l, l + keep);
    out.right.assign (r, r + keep);
    return out;
}
} // namespace

std::runtime_error songTooLong (double maxSeconds)
{
    return std::runtime_error ("Songs longer than " + std::to_string ((int) (maxSeconds / 60)) + " minutes aren't supported");
}

juce::String supportedAudioWildcards() { return formats().getWildcardForAllFormats() + ";*.m4a;*.mp4;*.m4b"; }

juce::String describeAudioFile (const juce::File& file)
{
    if (isMp4File (file)) return "M4A (AAC)";
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));
    if (reader == nullptr) return file.getFileExtension().trimCharactersAtStart (".").toUpperCase();
    const double rate = reader->sampleRate;
    const auto rateText = juce::String (rate / 1000.0, std::fmod (rate, 1000.0) == 0 ? 0 : 1) + " kHz";
    const auto channels = reader->numChannels == 1 ? juce::String ("mono")
                        : reader->numChannels == 2 ? juce::String ("stereo")
                                                   : juce::String (reader->numChannels) + " channels";
    auto text = reader->getFormatName().upToFirstOccurrenceOf (" ", false, false) + ", " + rateText + ", " + channels;
    if (! reader->usesFloatingPointData && reader->bitsPerSample > 0 && ! file.hasFileExtension ("mp3;ogg"))
        text << ", " << (int) reader->bitsPerSample << "-bit";
    return text;
}

StereoAudio loadAudioFile (const juce::File& file, double maxSeconds)
{
    auto decoded = isMp4File (file) ? decodeM4a (file, maxSeconds) : readWithJuce (file, maxSeconds);
    if (! std::isfinite (decoded.sampleRate) || decoded.sampleRate <= 0 || decoded.left.empty())
        throw std::runtime_error ("The audio file is empty");
    if (decoded.sampleRate < minAudioSampleRate || decoded.sampleRate > maxAudioSampleRate)
        throw std::runtime_error ("Unsupported sample rate in \"" + file.getFileName().toStdString() + "\"");
    if ((double) decoded.left.size() / decoded.sampleRate > maxSeconds || decoded.left.size() > (std::size_t) maxDecodedSamples)
        throw songTooLong (maxSeconds);

    // 32-bit float bounces can carry NaN/Inf samples, which would poison resampling, FFTs and scaling.
    for (float& v : decoded.left) if (! std::isfinite (v)) v = 0.0f;
    for (float& v : decoded.right) if (! std::isfinite (v)) v = 0.0f;

    const int inRate = (int) std::lround (decoded.sampleRate);
    if (inRate != sampleRate)
        resampleStereo (decoded.left, decoded.right, inRate, sampleRate);
    return { std::move (decoded.left), std::move (decoded.right) };
}

namespace
{
bool writeWith (juce::AudioFormat& format, const juce::File& file, const StereoAudio& audio, int bitsPerSample,
                const std::function<bool()>& shouldContinue = {})
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) throw std::runtime_error ("Can't write " + file.getFullPathName().toStdString());
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                      .withSampleRate (sampleRate)
                                                      .withNumChannels (2)
                                                      .withBitsPerSample (bitsPerSample));
    if (writer == nullptr) throw std::runtime_error ("Can't create a " + format.getFormatName().toStdString() + " writer");
    constexpr std::size_t chunk = 65536;
    const std::size_t total = std::min (audio.left.size(), audio.right.size());
    for (std::size_t done = 0; done < total; done += chunk)
    {
        if (shouldContinue && ! shouldContinue())
        {
            writer.reset();
            file.deleteFile();
            return false;
        }
        const int n = (int) std::min (chunk, total - done);
        const float* channels[] { audio.left.data() + done, audio.right.data() + done };
        if (! writer->writeFromFloatArrays (channels, 2, n))
            throw std::runtime_error ("Error while writing " + file.getFullPathName().toStdString());
    }
    return true;
}
} // namespace

bool writeWav (const juce::File& file, const StereoAudio& audio, int bitsPerSample, const std::function<bool()>& shouldContinue)
{
    juce::WavAudioFormat wav;
    return writeWith (wav, file, audio, bitsPerSample, shouldContinue);
}

void writeFlac (const juce::File& file, const StereoAudio& audio, int bitsPerSample)
{
    juce::FlacAudioFormat flac;
    writeWith (flac, file, audio, bitsPerSample);
}
} // namespace juststem

namespace juststem
{
namespace
{
void writeMidiFile (const juce::File& file, const juce::MidiFile& midi)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk() || ! midi.writeTo (out) || (out.flush(), out.getStatus().failed()))
        throw std::runtime_error ("Can't write " + file.getFullPathName().toStdString());
}
} // namespace

void writeMidi (const juce::File& file, const Transcription& notes, double bpm, const juce::String& trackName, int program)
{
    constexpr int ticksPerQuarter = 960;
    if (! (bpm > 0)) bpm = 120;
    const double ticksPerSecond = bpm / 60.0 * ticksPerQuarter;
    auto ticks = [&] (double seconds) { return std::round (seconds * ticksPerSecond); };

    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::textMetaEvent (3, trackName)); // track name
    track.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60'000'000.0 / bpm)));
    track.addEvent (juce::MidiMessage::programChange (1, program));
    for (const auto& n : notes.notes)
    {
        // At least a tick long, or a DAW may drop it.
        const double on = ticks (n.start), off = std::max (on + 1, ticks (n.end));
        track.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) n.velocity), on);
        track.addEvent (juce::MidiMessage::noteOff (1, n.pitch), off);
    }
    for (const auto& p : notes.pedals)
    {
        const double on = ticks (p.start), off = std::max (on + 1, ticks (p.end));
        track.addEvent (juce::MidiMessage::controllerEvent (1, p.controller, p.value), on);
        track.addEvent (juce::MidiMessage::controllerEvent (1, p.controller, 0), off);
    }
    track.updateMatchedPairs();
    track.sort(); // stable: a note ending where the next on its key starts stays before it
    track.addEvent (juce::MidiMessage::endOfTrack(), track.getEndTime());

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerQuarter);
    midi.addTrack (track);
    writeMidiFile (file, midi);
}

void writeTransposedMidi (const juce::File& from, const juce::File& to, int semitones)
{
    juce::FileInputStream in (from);
    juce::MidiFile midi;
    if (! in.openedOk() || ! midi.readFrom (in)) throw std::runtime_error ("Can't read " + from.getFullPathName().toStdString());
    juce::MidiFile shifted;
    shifted.setTicksPerQuarterNote (midi.getTimeFormat());
    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence track;
        for (const auto* e : *midi.getTrack (t))
        {
            auto m = e->message;
            if (m.isNoteOnOrOff())
            {
                const int note = m.getNoteNumber() + semitones;
                if (note < 0 || note > 127) continue;
                m.setNoteNumber (note);
            }
            track.addEvent (m);
        }
        shifted.addTrack (track);
    }
    writeMidiFile (to, shifted);
}
} // namespace juststem
