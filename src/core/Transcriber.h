#pragma once

#include "Audio.h"
#include "Separator.h" // Backend

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
    Backend backend = Backend::CPU; // where the model ran
    std::string fallbackReason;     // why the GPU wasn't used, when it failed
};

namespace model
{
    /** Transkun (Yujia Yan, MIT): notes with velocity, and pedals; trained on piano, used for every
        pitched stem. Downloaded the first time MIDI is made; fetched by scripts/fetch_model.sh midi.
        Three files, kept side by side: the model, its velocity / timing heads, and its front end
        (6 analysis windows, then the mel filters). */
    inline constexpr std::string_view midiModelFile = "transkun_piano.onnx";
    inline constexpr std::string_view midiHeadsFile = "transkun_piano_heads.onnx";
    inline constexpr std::string_view midiFrontEndFile = "transkun_piano_frontend.bin";
    /** macOS: the same network for Core ML (scripts/prepare_transkun_coreml.py), a folder unpacked from
        midiCoreMLDownload next to the others. Optional: without it MIDI runs on the CPU. */
    inline constexpr std::string_view midiCoreMLFile = "transkun_piano.mlpackage";
    inline constexpr std::string_view midiCoreMLDownload = "transkun_piano.mlpackage.zip";
} // namespace model

/** The stems made into MIDI (when MIDI is on), all by Transkun. */
struct MidiStem
{
    Stem stem;
    int program; // General MIDI instrument (0-based) a DAW plays the file with
};
inline constexpr std::array<MidiStem, 5> midiStems { {
    { Stem::Vocals, 53 },  // Voice Oohs
    { Stem::Bass, 33 },    // Electric Bass (finger)
    { Stem::Guitar, 25 },  // Acoustic Guitar (steel)
    { Stem::Piano, 0 },    // Acoustic Grand Piano
    { Stem::Other, 0 },    // Acoustic Grand Piano: synths, strings and pads, heard as piano
} };

/** Notes with velocity, and pedals (Transkun), in 16 s segments 8 s apart. `modelPath` is the
    model::midiModelFile; the heads and front-end files must be next to it. Runs on the GPU, falling back
    to the CPU: DirectML on Windows; Core ML on macOS when model::midiCoreMLFile is beside the model and
    `coremlCache` (where Core ML keeps compiled models) is given; the CPU on Linux. `onProgress` gets
    0..1. Blocking; throws std::runtime_error, "cancelled" when `cancel` is set. */
Transcription transcribe (const std::string& modelPath, const StereoAudio& audio,
                          const std::function<void (double)>& onProgress, const std::atomic<bool>& cancel,
                          const std::string& coremlCache = {});

/** The steps inside, exposed for the self-tests. */
namespace transcription
{
    /** Transkun's semi-CRF decode (viterbiBackward): `score` is [T][T][nTracks], score[e][b][k] being
        a note on track k from frame b to frame e. The best set of non-overlapping intervals on each
        track, starting the backtrace at forcedStart[k]. Intervals are (begin, end), in order. */
    std::vector<std::vector<std::pair<int, int>>> semiCrfDecode (const float* score, int T, int nTracks,
                                                                 const std::vector<int>& forcedStart);

    /** Transkun's log-mel features for audio already cut to a segment: [frames][229 mels][6 windows].
        `channels` are the same length; `frontEnd` is the model::midiFrontEndFile's floats. */
    std::vector<float> transkunFeatures (const std::vector<const float*>& channels, int length,
                                      const std::vector<float>& frontEnd, int& nFrames);
} // namespace transcription
} // namespace juststem
