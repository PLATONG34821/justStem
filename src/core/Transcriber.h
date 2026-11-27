#pragma once

#include "Audio.h"

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace juststem
{
/** A note found in a stem. Times are seconds from the start of the song. */
struct MidiNote
{
    int pitch;          // MIDI note number
    double start, end;
    int velocity;       // 1..127
};

/** A pedal held down (piano): controller 64 = sustain, 67 = soft. */
struct PedalPress
{
    int controller;
    double start, end;
    int value;          // 0..127 while held
};

struct Transcription
{
    std::vector<MidiNote> notes;   // sorted by start
    std::vector<PedalPress> pedals;
};

namespace model
{
    /** Basic Pitch (Spotify, Apache-2.0): notes of any pitched instrument, 0.2 MB. Bundled with the
        fast model; fetched by scripts/fetch_model.sh midi. */
    inline constexpr std::string_view notesFile = "basic_pitch.onnx";
    /** Transkun (Yujia Yan, MIT): piano notes with velocity, and pedals. Downloaded the first time a
        song has piano; fetched by scripts/fetch_model.sh piano. Three files, kept side by side: the
        model, its velocity / timing heads, and its front end (6 analysis windows, then the mel filters). */
    inline constexpr std::string_view pianoFile = "transkun_piano.onnx";
    inline constexpr std::string_view pianoHeadsFile = "transkun_piano_heads.onnx";
    inline constexpr std::string_view pianoFrontEndFile = "transkun_piano_frontend.bin";
} // namespace model

/** The stems made into MIDI: piano by Transkun, the others by Basic Pitch in the stem's range. */
struct MidiStem
{
    Stem stem;
    float minHz, maxHz;
    int program; // General MIDI instrument (0-based) a DAW plays the file with
};
inline constexpr std::array<MidiStem, 4> midiStems { {
    { Stem::Vocals, 80.0f, 1100.0f, 53 },  // Voice Oohs
    { Stem::Bass, 30.0f, 400.0f, 33 },     // Electric Bass (finger)
    { Stem::Guitar, 80.0f, 1300.0f, 25 },  // Acoustic Guitar (steel)
    { Stem::Piano, 0.0f, 0.0f, 0 },        // Acoustic Grand Piano
} };

/** The notes of one pitched instrument (Basic Pitch), between minHz and maxHz. Runs on the CPU
    (CoreML can't compile this model). Blocking; throws std::runtime_error, "cancelled" when `cancel` is set. */
Transcription transcribeNotes (const std::string& modelPath, const StereoAudio& audio, float minHz, float maxHz,
                               const std::atomic<bool>& cancel);

/** Piano notes with velocity, and pedals (Transkun), in 16 s segments 8 s apart. `modelPath` is the
    model::pianoFile; the heads and front-end files must be next to it. Runs on the CPU (CoreML is ~90x
    slower on this model). `onProgress` gets 0..1. Blocking; throws like transcribeNotes. */
Transcription transcribePiano (const std::string& modelPath, const StereoAudio& audio,
                               const std::function<void (double)>& onProgress, const std::atomic<bool>& cancel);

/** The steps inside, exposed for the self-tests. */
namespace transcription
{
    /** A note from Basic Pitch's decoder: frames [start, end), key 0..87 (MIDI 21..108), mean activation. */
    struct FrameNote
    {
        int start, end, key;
        float amplitude;
    };

    /** Basic Pitch's output_to_notes_polyphonic: `frames` and `onsets` are [nFrames][88] activations
        (changed: keys outside the range are zeroed). Onsets inferred from frame jumps are added, and
        the melodia trick adds notes without a clear onset. Keys below minKey and from maxKey up are dropped. */
    std::vector<FrameNote> basicPitchNotes (std::vector<float>& frames, std::vector<float>& onsets, int nFrames,
                                            int minKey, int maxKey);

    /** Transkun's semi-CRF decode (viterbiBackward): `score` is [T][T][nTracks], score[e][b][k] being
        a note on track k from frame b to frame e. The best set of non-overlapping intervals on each
        track, starting the backtrace at forcedStart[k]. Intervals are (begin, end), in order. */
    std::vector<std::vector<std::pair<int, int>>> semiCrfDecode (const float* score, int T, int nTracks,
                                                                 const std::vector<int>& forcedStart);

    /** Transkun's log-mel features for audio already cut to a segment: [frames][229 mels][6 windows].
        `channels` are the same length; `frontEnd` is the model::pianoFrontEndFile's floats. */
    std::vector<float> pianoFeatures (const std::vector<const float*>& channels, int length,
                                      const std::vector<float>& frontEnd, int& nFrames);
} // namespace transcription
} // namespace juststem
