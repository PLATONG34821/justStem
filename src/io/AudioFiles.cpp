#include "AudioFiles.h"
#include "M4aDecoder.h"
#include "../core/Resampler.h"

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
    juce::MemoryBlock head;
    in.readIntoMemoryBlock (head, 1 << 16);
    const auto* d = static_cast<const unsigned char*> (head.getData());
    const std::size_t size = head.getSize();
    auto u32 = [&] (std::size_t at) { return (juce::uint32) d[at] << 24 | (juce::uint32) d[at + 1] << 16 | (juce::uint32) d[at + 2] << 8 | d[at + 3]; };

    std::size_t start = 0;
    if (size > 10 && std::memcmp (d, "ID3", 3) == 0) // skip an ID3v2 tag (its size is 7 bits per byte)
        start = 10 + ((std::size_t) (d[6] & 0x7f) << 21 | (std::size_t) (d[7] & 0x7f) << 14 | (std::size_t) (d[8] & 0x7f) << 7 | (d[9] & 0x7f));

    // The Xing / Info header sits inside the first frame, right after the side information.
    for (std::size_t at = start; at + 8 <= size && at < start + 512; ++at)
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

DecodedAudio readWithJuce (const juce::File& file, double maxSeconds)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));
    if (reader == nullptr) throw std::runtime_error ("Couldn't read \"" + file.getFileName().toStdString() + "\" as audio");
    if (reader->sampleRate <= 0 || reader->lengthInSamples <= 0) throw std::runtime_error ("The audio file is empty");
    if ((double) reader->lengthInSamples / reader->sampleRate > maxSeconds) throw songTooLong (maxSeconds);

    const int n = (int) reader->lengthInSamples;
    const int ch = (int) reader->numChannels;
    juce::AudioBuffer<float> buf (std::max (1, ch), n);
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
    if (decoded.sampleRate <= 0 || decoded.left.empty()) throw std::runtime_error ("The audio file is empty");
    if ((double) decoded.left.size() / decoded.sampleRate > maxSeconds) throw songTooLong (maxSeconds);

    StereoAudio out;
    if ((int) decoded.sampleRate == sampleRate)
    {
        out.left = std::move (decoded.left);
        out.right = std::move (decoded.right);
    }
    else
    {
        const int rate = (int) std::lround (decoded.sampleRate);
        out.left = resample (decoded.left, rate, sampleRate);
        out.right = resample (decoded.right, rate, sampleRate);
    }
    return out;
}

namespace
{
void writeWith (juce::AudioFormat& format, const juce::File& file, const StereoAudio& audio, int bitsPerSample)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) throw std::runtime_error ("Can't write " + file.getFullPathName().toStdString());
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                      .withSampleRate (sampleRate)
                                                      .withNumChannels (2)
                                                      .withBitsPerSample (bitsPerSample));
    if (writer == nullptr) throw std::runtime_error ("Can't create a " + format.getFormatName().toStdString() + " writer");
    const float* channels[] { audio.left.data(), audio.right.data() };
    if (! writer->writeFromFloatArrays (channels, 2, (int) audio.size()))
        throw std::runtime_error ("Error while writing " + file.getFullPathName().toStdString());
}
} // namespace

void writeWav (const juce::File& file, const StereoAudio& audio, int bitsPerSample)
{
    juce::WavAudioFormat wav;
    writeWith (wav, file, audio, bitsPerSample);
}

void writeFlac (const juce::File& file, const StereoAudio& audio, int bitsPerSample)
{
    juce::FlacAudioFormat flac;
    writeWith (flac, file, audio, bitsPerSample);
}
} // namespace juststem
