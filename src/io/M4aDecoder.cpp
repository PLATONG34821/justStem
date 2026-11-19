#include "M4aDecoder.h"

#include <neaacdec.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <stdexcept>

namespace covertools
{
namespace
{
[[noreturn]] void fail (const std::string& why) { throw std::runtime_error ("Couldn't read this M4A file: " + why); }

/** Big-endian reads over the whole file, with bounds checks. */
struct Bytes
{
    const juce::uint8* data;
    std::size_t size;

    juce::uint8 u8 (std::size_t at) const { need (at, 1); return data[at]; }
    juce::uint16 u16 (std::size_t at) const { need (at, 2); return (juce::uint16) ((data[at] << 8) | data[at + 1]); }
    juce::uint32 u32 (std::size_t at) const
    {
        need (at, 4);
        return ((juce::uint32) data[at] << 24) | ((juce::uint32) data[at + 1] << 16) | ((juce::uint32) data[at + 2] << 8) | data[at + 3];
    }
    juce::uint64 u64 (std::size_t at) const { return ((juce::uint64) u32 (at) << 32) | u32 (at + 4); }
    bool is (std::size_t at, const char* fourcc) const { return at + 4 <= size && std::memcmp (data + at, fourcc, 4) == 0; }
    void need (std::size_t at, std::size_t n) const { if (at > size || n > size - at) fail ("it is truncated or damaged"); }
};

/** An MP4 box: its type and where its payload is. */
struct Box
{
    char type[5] {};
    std::size_t start = 0, payload = 0, end = 0; // payload = first byte after the header
};

/** The boxes directly inside [from, to). */
std::vector<Box> children (const Bytes& b, std::size_t from, std::size_t to)
{
    std::vector<Box> out;
    for (std::size_t pos = from; pos + 8 <= to;)
    {
        Box box;
        juce::uint64 size = b.u32 (pos);
        std::memcpy (box.type, b.data + pos + 4, 4);
        std::size_t header = 8;
        if (size == 1) { size = b.u64 (pos + 8); header = 16; }
        else if (size == 0) size = to - pos;
        if (size < header || size > to - pos) break; // trailing garbage
        box.start = pos;
        box.payload = pos + header;
        box.end = pos + (std::size_t) size;
        out.push_back (box);
        pos = box.end;
    }
    return out;
}

std::optional<Box> child (const Bytes& b, const Box& parent, const char* type, std::size_t skip = 0)
{
    for (auto& c : children (b, parent.payload + skip, parent.end))
        if (std::strcmp (c.type, type) == 0) return c;
    return std::nullopt;
}

/** MPEG-4 descriptor length: up to 4 bytes, 7 bits each. */
std::size_t descriptorLength (const Bytes& b, std::size_t& pos)
{
    std::size_t len = 0;
    for (int i = 0; i < 4; ++i)
    {
        const auto byte = b.u8 (pos++);
        len = (len << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0) break;
    }
    return len;
}

/** The AudioSpecificConfig (decoder setup bytes) from an 'esds' box. */
std::vector<juce::uint8> audioSpecificConfig (const Bytes& b, const Box& esds)
{
    std::size_t pos = esds.payload + 4; // version + flags
    if (b.u8 (pos++) != 0x03) fail ("no audio decoder description");
    descriptorLength (b, pos);
    pos += 2; // ES_ID
    const auto flags = b.u8 (pos++);
    if (flags & 0x80) pos += 2;                  // dependsOn_ES_ID
    if (flags & 0x40) pos += 1 + b.u8 (pos);     // URL
    if (flags & 0x20) pos += 2;                  // OCR_ES_ID
    if (b.u8 (pos++) != 0x04) fail ("no audio decoder configuration");
    descriptorLength (b, pos);
    const auto objectType = b.u8 (pos);
    if (objectType != 0x40 && (objectType < 0x66 || objectType > 0x68)) fail ("the audio isn't AAC"); // MPEG-4 / MPEG-2 AAC
    pos += 13; // objectType, streamType, bufferSize, max/avg bitrate
    if (b.u8 (pos++) != 0x05) fail ("no AAC decoder settings");
    const auto len = descriptorLength (b, pos);
    b.need (pos, len);
    return { b.data + pos, b.data + pos + len };
}

struct Frame
{
    std::size_t offset, size;
};

struct Track
{
    juce::uint32 id = 0, timescale = 0;
    std::vector<juce::uint8> config;
    std::vector<Frame> frames;            // classic layout (empty for fragmented files)
    juce::int64 editListSkip = -1;        // from 'elst', in the track timescale (-1: none)
};

/** The first AAC audio track in 'moov', with its classic sample table if it has one. */
Track findAudioTrack (const Bytes& b, const Box& moov)
{
    for (auto& trak : children (b, moov.payload, moov.end))
    {
        if (std::strcmp (trak.type, "trak") != 0) continue;
        auto mdia = child (b, trak, "mdia");
        auto hdlr = mdia ? child (b, *mdia, "hdlr") : std::nullopt;
        if (! hdlr || ! b.is (hdlr->payload + 8, "soun")) continue; // not audio

        Track t;
        if (auto tkhd = child (b, trak, "tkhd"))
            t.id = b.u32 (tkhd->payload + (b.u8 (tkhd->payload) == 1 ? 20 : 12));
        if (auto mdhd = child (b, *mdia, "mdhd"))
            t.timescale = b.u32 (mdhd->payload + (b.u8 (mdhd->payload) == 1 ? 20 : 12));

        auto minf = child (b, *mdia, "minf");
        auto stbl = minf ? child (b, *minf, "stbl") : std::nullopt;
        auto stsd = stbl ? child (b, *stbl, "stsd") : std::nullopt;
        if (! stsd) fail ("the audio track has no sample description");
        auto entries = children (b, stsd->payload + 8, stsd->end);
        if (entries.empty() || std::strcmp (entries[0].type, "mp4a") != 0) fail ("the audio isn't AAC");
        // AudioSampleEntry: 28 bytes of fields, then child boxes ('esds', or 'wave' > 'esds' in QuickTime files).
        const auto& mp4a = entries[0];
        auto esds = child (b, mp4a, "esds", 28);
        if (! esds)
            if (auto wave = child (b, mp4a, "wave", 28)) esds = child (b, *wave, "esds");
        if (! esds) // QuickTime v1/v2 entries have longer headers: look for the box directly
            for (std::size_t at = mp4a.payload; at + 8 <= mp4a.end && ! esds; ++at)
                if (b.is (at + 4, "esds"))
                    if (auto found = children (b, at, mp4a.end); ! found.empty() && std::strcmp (found[0].type, "esds") == 0)
                        esds = found[0];
        if (! esds) fail ("no AAC decoder settings");
        t.config = audioSpecificConfig (b, *esds);

        // Edit list: the first non-empty edit says how many samples of encoder delay to skip.
        if (auto edts = child (b, trak, "edts"))
            if (auto elst = child (b, *edts, "elst"))
            {
                const bool v1 = b.u8 (elst->payload) == 1;
                const auto count = b.u32 (elst->payload + 4);
                for (juce::uint32 i = 0, at = (juce::uint32) elst->payload + 8; i < count; ++i, at += v1 ? 20 : 12)
                {
                    const juce::int64 mediaTime = v1 ? (juce::int64) b.u64 (at + 8) : (juce::int32) b.u32 (at + 4);
                    if (mediaTime >= 0) { t.editListSkip = mediaTime; break; }
                }
            }

        // Classic sample table: sizes (stsz), chunk offsets (stco / co64), samples per chunk (stsc).
        auto stsz = child (b, *stbl, "stsz");
        auto stco = child (b, *stbl, "stco");
        auto co64 = child (b, *stbl, "co64");
        auto stsc = child (b, *stbl, "stsc");
        if (stsz && stsc && (stco || co64))
        {
            const auto fixedSize = b.u32 (stsz->payload + 4);
            const auto count = b.u32 (stsz->payload + 8);
            const auto chunks = b.u32 ((stco ? stco : co64)->payload + 4);
            auto chunkOffset = [&] (juce::uint32 i) -> std::size_t {
                return stco ? b.u32 (stco->payload + 8 + 4 * (std::size_t) i) : (std::size_t) b.u64 (co64->payload + 8 + 8 * (std::size_t) i);
            };
            const auto runs = b.u32 (stsc->payload + 4);
            juce::uint32 sample = 0;
            for (juce::uint32 r = 0; r < runs && sample < count; ++r)
            {
                const auto entry = stsc->payload + 8 + 12 * (std::size_t) r;
                const auto first = b.u32 (entry) - 1, perChunk = b.u32 (entry + 4);
                const auto last = r + 1 < runs ? b.u32 (entry + 12) - 1 : chunks;
                for (auto chunk = first; chunk < last && sample < count; ++chunk)
                {
                    std::size_t offset = chunkOffset (chunk);
                    for (juce::uint32 k = 0; k < perChunk && sample < count; ++k, ++sample)
                    {
                        const std::size_t size = fixedSize != 0 ? fixedSize : b.u32 (stsz->payload + 12 + 4 * (std::size_t) sample);
                        t.frames.push_back ({ offset, size });
                        offset += size;
                    }
                }
            }
        }
        return t;
    }
    fail ("it has no audio track");
}

/** Fragmented files (e.g. YouTube): each 'moof' lists the frames of the following data. */
void addFragments (const Bytes& b, const std::vector<Box>& top, const Box& moov, Track& t)
{
    juce::uint32 defaultSize = 0;
    if (auto mvex = child (b, moov, "mvex"))
        for (auto& trex : children (b, mvex->payload, mvex->end))
            if (std::strcmp (trex.type, "trex") == 0 && b.u32 (trex.payload + 4) == t.id)
                defaultSize = b.u32 (trex.payload + 16);

    for (auto& moof : top)
    {
        if (std::strcmp (moof.type, "moof") != 0) continue;
        for (auto& traf : children (b, moof.payload, moof.end))
        {
            if (std::strcmp (traf.type, "traf") != 0) continue;
            auto tfhd = child (b, traf, "tfhd");
            if (! tfhd || b.u32 (tfhd->payload + 4) != t.id) continue;

            const auto flags = b.u32 (tfhd->payload) & 0xffffff;
            std::size_t at = tfhd->payload + 8;
            std::size_t base = moof.start; // default-base-is-moof, and the common default
            if (flags & 0x01) { base = (std::size_t) b.u64 (at); at += 8; }
            if (flags & 0x02) at += 4; // sample description index
            if (flags & 0x08) at += 4; // default duration
            juce::uint32 trackDefaultSize = defaultSize;
            if (flags & 0x10) trackDefaultSize = b.u32 (at);

            std::size_t next = base;
            for (auto& trun : children (b, traf.payload, traf.end))
            {
                if (std::strcmp (trun.type, "trun") != 0) continue;
                const auto tf = b.u32 (trun.payload) & 0xffffff;
                const auto count = b.u32 (trun.payload + 4);
                std::size_t p = trun.payload + 8;
                if (tf & 0x01) { next = base + (std::size_t) (juce::int32) b.u32 (p); p += 4; }
                if (tf & 0x04) p += 4; // first sample flags
                for (juce::uint32 i = 0; i < count; ++i)
                {
                    if (tf & 0x100) p += 4; // duration
                    juce::uint32 size = trackDefaultSize;
                    if (tf & 0x200) { size = b.u32 (p); p += 4; }
                    if (tf & 0x400) p += 4; // flags
                    if (tf & 0x800) p += 4; // composition offset
                    t.frames.push_back ({ next, size });
                    next += size;
                }
            }
        }
    }
}
/** iTunes gapless info ("iTunSMPB"): encoder priming, padding and the original length, in samples
    at the AAC core rate. Written by Apple's and most other AAC encoders. */
struct GaplessInfo
{
    juce::int64 priming = -1, originalLength = -1;
};

GaplessInfo iTunesGapless (const Bytes& b, const Box& moov)
{
    GaplessInfo g;
    const std::string_view moovBytes (reinterpret_cast<const char*> (b.data + moov.payload), moov.end - moov.payload);
    const auto name = moovBytes.find ("iTunSMPB");
    if (name == std::string_view::npos) return g;
    const auto data = moovBytes.find ("data", name);
    if (data == std::string_view::npos || data + 12 > moovBytes.size()) return g;
    // 'data' box payload: 4 bytes type + 4 bytes locale, then hex fields separated by spaces.
    const auto text = juce::String (moovBytes.data() + data + 12, std::min<std::size_t> (200, moovBytes.size() - data - 12));
    auto fields = juce::StringArray::fromTokens (text.retainCharacters ("0123456789abcdefABCDEF "), " ", "");
    fields.removeEmptyStrings();
    if (fields.size() >= 4)
    {
        g.priming = fields[1].getHexValue64();
        g.originalLength = fields[3].getHexValue64();
    }
    return g;
}
} // namespace

bool isMp4File (const juce::File& file)
{
    juce::FileInputStream in (file);
    char header[8] {};
    return in.openedOk() && in.read (header, 8) == 8 && std::memcmp (header + 4, "ftyp", 4) == 0;
}

DecodedAudio decodeM4a (const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data)) fail ("it can't be opened");
    const Bytes b { static_cast<const juce::uint8*> (data.getData()), data.getSize() };

    const auto top = children (b, 0, b.size);
    const auto moov = std::find_if (top.begin(), top.end(), [] (const Box& x) { return std::strcmp (x.type, "moov") == 0; });
    if (moov == top.end()) fail ("it has no movie header");
    auto track = findAudioTrack (b, *moov);
    if (track.frames.empty()) addFragments (b, top, *moov, track);
    if (track.frames.empty()) fail ("it contains no audio");

    // Decode every AAC frame with FAAD2 (as float; surround is downmixed to stereo).
    struct Decoder { NeAACDecHandle h = NeAACDecOpen(); ~Decoder() { NeAACDecClose (h); } } dec;
    auto* cfg = NeAACDecGetCurrentConfiguration (dec.h);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    cfg->downMatrix = 1;
    NeAACDecSetConfiguration (dec.h, cfg);
    unsigned long rate = 0;
    unsigned char channels = 0;
    if (NeAACDecInit2 (dec.h, track.config.data(), (unsigned long) track.config.size(), &rate, &channels) < 0)
        fail ("unsupported AAC settings");

    DecodedAudio out;
    out.sampleRate = (double) rate;
    out.left.reserve (track.frames.size() * 1024);
    out.right.reserve (track.frames.size() * 1024);
    std::size_t frameLength = 0; // output samples per frame: 1024, or 2048 with SBR (HE-AAC)
    bool sbr = false;
    for (const auto& f : track.frames)
    {
        b.need (f.offset, f.size);
        NeAACDecFrameInfo info {};
        auto* pcm = static_cast<const float*> (NeAACDecDecode (dec.h, &info, const_cast<juce::uint8*> (b.data + f.offset), (unsigned long) f.size));
        if (info.error != 0 || pcm == nullptr || info.channels == 0) continue; // FAAD2 outputs nothing for the first frame
        out.sampleRate = (double) info.samplerate; // SBR doubles the rate
        frameLength = info.samples / info.channels;
        sbr = info.sbr != NO_SBR;
        const auto ch = (std::size_t) info.channels;
        for (std::size_t i = 0; i + ch <= info.samples; i += ch)
        {
            out.left.push_back (pcm[i]);
            out.right.push_back (ch > 1 ? pcm[i + 1] : pcm[i]);
        }
    }
    if (out.left.empty()) fail ("no audio could be decoded");

    // Gapless playback: drop the encoder delay at the start and the padding at the end, the way
    // Apple's decoder (and DAWs) do, so the stems line up with the original file.
    // Delays are in AAC core samples; SBR doubles them and adds its own 481-sample delay.
    const std::size_t upsample = frameLength > 1024 ? 2 : 1;
    const auto alreadyDropped = (juce::int64) (track.frames.size() * frameLength) - (juce::int64) out.left.size();
    const auto gapless = iTunesGapless (b, *moov);
    juce::int64 priming = 2112; // what Apple assumes when a file doesn't say (e.g. YouTube)
    if (track.editListSkip >= 0 && track.timescale > 0)
        priming = (juce::int64) std::llround ((double) track.editListSkip * (out.sampleRate / (double) upsample) / track.timescale);
    else if (gapless.priming >= 0)
        priming = gapless.priming;
    const auto skip = std::clamp<juce::int64> ((priming + (sbr ? 481 : 0)) * (juce::int64) upsample - alreadyDropped, 0, (juce::int64) out.left.size());
    out.left.erase (out.left.begin(), out.left.begin() + (std::ptrdiff_t) skip);
    out.right.erase (out.right.begin(), out.right.begin() + (std::ptrdiff_t) skip);
    if (gapless.originalLength > 0)
    {
        const auto length = (std::size_t) std::min<juce::int64> (gapless.originalLength * (juce::int64) upsample, (juce::int64) out.left.size());
        out.left.resize (length);
        out.right.resize (length);
    }
    return out;
}
} // namespace covertools
