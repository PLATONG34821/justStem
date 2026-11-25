# Developing justStem

## Building

You need CMake ≥ 3.28, Ninja and a C++20 compiler: the Xcode command-line tools on macOS, Visual
Studio 2022 (run from a "Developer" command prompt) on Windows, GCC or Clang on Linux plus JUCE's
packages and libcurl for HTTPS (see `.github/workflows/build.yml`). JUCE, ONNX Runtime and FAAD2 are downloaded by CMake
(pinned versions, checked against their SHA-256).

1. Build the model once (needs Python 3.12, as onnxruntime has no wheels for newer versions yet);
   see the top of `scripts/prepare_demucs_onnx.py`. The model isn't in git (`models/` is ignored),
   so every clone needs this step. `fetch_model.sh` downloads it from a pinned commit and checks its
   SHA-256, and `requirements.txt` pins the packages, so every build makes the same model:
   ```sh
   python3.12 -m venv .venv && .venv/bin/pip install -r scripts/requirements.txt
   scripts/fetch_model.sh fast
   .venv/bin/python scripts/prepare_demucs_onnx.py htdemucs_6s_fp16weights.onnx models/htdemucs_6s_opt.onnx
   ```
   Optional, for HQ mode (BS-RoFormer 6-stem): build `models/bs_roformer_sw_6stem_opt.onnx` and the
   plug-in bundles it and shows the HQ switch; without it HQ mode is hidden. See the top of
   `scripts/prepare_roformer_onnx.py`. The weights have no stated licence (justStem treats them as CC-BY-NC-SA-4.0);
   see [Third-party licences](#third-party-licences) before distributing a build that includes them.
   ```sh
   scripts/fetch_model.sh hq
   .venv/bin/python scripts/prepare_roformer_onnx.py bs_roformer_sw_6stem_fp16.onnx models/bs_roformer_sw_6stem_opt.onnx
   ```
2. Build:
   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```
   The plug-ins are in `build/justStem_artefacts/Release/`, and are also installed for you:
   - **macOS:** VST3 and AU in `~/Library/Audio/Plug-Ins/`. Local builds are ad-hoc signed, so
     they run on the machine that built them.
   - **Linux:** VST3 in `~/.vst3/`.
   - **Windows:** copy `justStem.vst3` to `C:\Program Files\Common Files\VST3\` (needs admin rights).

Every push is built and tested on macOS, Windows and Linux by GitHub Actions
(`.github/workflows/build.yml`); the built plug-ins can be downloaded from each run.

## Testing

```sh
build/juststem_cli_artefacts/Release/juststem_cli analyze-selftest
build/juststem_cli_artefacts/Release/juststem_cli selftest models/htdemucs_6s_opt.onnx
build/juststem_uitest_artefacts/Release/juststem_uitest models/htdemucs_6s_opt.onnx <song> <screenshot folder>   # add --hq to split with the HQ model
```

- `analyze-selftest`: the resampler against exact sine waves and an aliasing test, loudness
  against the EBU reference signals, key detection in all 24 keys, tempo from 72 to 174 BPM,
  beats, bar lines and chords (7ths, sus, dim, slash chords) in a synthetic band recording in 4
  keys, the tempo of that band with its drums muted, and pitch shifting (pitch within 5 cents, no
  timing offset, same level).
- `selftest`: separates a synthetic track and scores each stem against the original parts.
- `juststem_uitest`: runs the real plug-in code without a DAW. It renders every screen to PNGs
  (the README screenshots come from it), splits a song, and checks:
  - the preview audio, including a host block larger than prepared and preparing again while
    transposed, and the transposed preview (in time with the original);
  - the UI: New song and Paste, the Storage panel, the chord view and its scrolling, and a stem
    dropped back on the window (not split again);
  - saving: Save all (copies), transposed stems (and Cancel), saving / reloading the plug-in state
    and refreshing an open window when it's restored;
  - the awkward cases: an error naming a file in Thai, closing the plug-in mid-job, a stem edited
    outside justStem, a damaged `info.json`.

Set `JUSTSTEM_DATA_DIR` to an absolute folder to keep a run's songs and caches there instead of
in your Music folder and app data (useful for tests and screenshots).

`juststem_cli` can also `separate`, `analyze` or `import` real songs, or print the `chords` of a
song folder the plug-in saved; see the top of `tools/cli.cpp` (the self-tests are in
`tools/selftest.cpp`). Paths can be relative and contain
any characters (they're read as UTF-8).

## Code layout

```
src/core/      Separation and analysis. Plain C++ + ONNX Runtime, no JUCE.
  Audio.h        shared types: StereoAudio, Stem (and its file name), sample rate
  Pitch.h        note names and pitch classes
  Separator.*    runs the model (HT-Demucs, or BS-RoFormer in HQ mode) in crossfaded chunks
                 (CoreML / DirectML / CPU); each model's settings are in its ModelSpec
  Analysis.*     key, tempo, loudness (EBU R128), and analyzeSong: the whole song from its stems
  Chords.*       beat tracking, bar lines and chord recognition
  Dsp.*          shared spectral peaks and onset envelope
  PitchShift.*   real-time and offline pitch shifting (Signalsmith Stretch)
  Resampler.*    sample-rate conversion to 44.1 kHz (polyphase, exact timing)
src/io/        Files and the outside world (JUCE).
  AudioFiles.*   decode audio files to 44.1 kHz stereo (gapless); write WAV and FLAC
  M4aDecoder.*   M4A / MP4 reader (classic and fragmented) + FAAD2 AAC decoding
  YouTube.*      download a video's audio with yt-dlp
  ToolInstaller.* download and verify yt-dlp and Deno
  AppPaths.h     where files are stored
src/plugin/    The plug-in.
  PluginProcessor.*  audio pass-through + stem preview, runs song jobs, saves state
  PluginEditor.*     the window
  SongJob.*          background work: import -> split -> analyse -> save, and its log
  SongResult.*       a split song; saves and reloads it
  StemPlayer.*       real-time stem preview (lock-free on the audio thread)
  Text.h             text helpers: BPM, m:ss, stem names
  ui/                theme (Inter font, colours, icons, buttons), widgets (key gauge, stem rows,
                     segmented tabs), the song title, the player bar, the chord view, the Storage
                     panel and Save all (SaveStems)
tools/         Developer tools, not shipped (see Testing).
scripts/       Builds the model file.
resources/     Embedded fonts, and the font and icon licences.
docs/          This file and the README screenshots.
cmake/         ONNX Runtime download (per OS), packaging.
.github/       CI: build + test on macOS, Windows and Linux.
```

## Where files go

| What | Where |
|---|---|
| Stems for each song (7 FLACs + `info.json`; older songs have WAVs) | `<your Music folder>/justStem/<song title>/` |
| yt-dlp and Deno (downloaded on the first YouTube link) | `<app data>/justStem/bin/` |
| CoreML's compiled model (macOS; makes later loads fast) | `<app data>/justStem/coreml-cache/` |
| YouTube audio while it's being decoded (a folder per download, deleted after) | `<app data>/justStem/downloads/` |

`<app data>` is `~/Library/Application Support` on macOS, `%APPDATA%` on Windows and `~/.config`
on Linux. A DAW project only stores the song folder's path and the mute / solo / volume /
transpose settings. Uninstalling the plug-in doesn't remove any of this; the Storage panel does.

## Audio formats

WAV, AIFF, FLAC, Ogg Vorbis, MP3 and M4A (AAC-LC and HE-AAC, including YouTube's fragmented
files). The same decoders are used on every OS: JUCE's readers, plus our own MP4 reader with FAAD2
for AAC. Decoding is gapless: the encoder delay and padding recorded in the file (iTunes
`iTunSMPB`, MP4 edit lists, LAME / ffmpeg MP3 tags) are removed, so the stems line up
sample-exactly with the original file in a DAW.

Songs can be up to 15 minutes (`maxSongSeconds`). Longer files are refused before they're decoded,
and longer YouTube videos (and live streams) before they're downloaded.

## How chords are found

The chords come from the separated stems: guitar + piano + other for the harmony, the bass stem
for the bass note, and the drums for the beats. Vocals are left out (they carry the melody). In a
song without drums (a near-silent drums stem), the tempo and the beats come from the harmony and
the bass instead.

1. **Beats**: a dynamic-programming beat tracker follows the detected tempo through the drums.
2. **Per beat**: the strength of each of the 12 notes (chroma) in the chord instruments, and the
   lowest strong note in the bass.
3. **Matching**: major, minor, 7, maj7, m7, sus2, sus4 and dim on every root, in root position or
   with another chord tone in the bass (slash chords only when the bass clearly says so).
4. **Smoothing**: Viterbi decoding makes chords hold rather than flicker and leans towards chords
   in the song's key. A first pass finds which beat of four the chords change on (the bar line),
   and the second pass prefers changes there.

4/4 is assumed. Expect most chords right on pop and rock; dense or jazzy harmony is harder.

## Transpose

The preview is pitch-shifted live; the shifter delays its output a little, so playback reads that
far ahead and stays in time with the DAW. With Transpose on, Save all writes a pitch-shifted set of
stems + instrumental + full mix to a new folder, three stems at a time (about 4 s for a 5-minute song on an M4).

## Third-party licences

- [JUCE](https://juce.com): AGPL-3.0 / commercial
- [ONNX Runtime](https://github.com/microsoft/onnxruntime): MIT
- [Inter](https://rsms.me/inter/) font (UI typeface, embedded): SIL Open Font License 1.1, see `resources/fonts/OFL-LICENSE.txt`
- [Phosphor Icons](https://phosphoricons.com) (UI icons, path data in `src/plugin/ui/Theme.cpp`): MIT, see `resources/icons/PHOSPHOR-LICENSE.txt`
- [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) and
  [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) (pitch shifting): MIT
- [FAAD2](https://github.com/knik0/faad2) (AAC decoder): GPL-2.0-or-later, used under GPL-3.0 with the AGPL
- [DirectML](https://github.com/microsoft/DirectML) (Windows builds only): Microsoft's proprietary,
  redistributable `DirectML.dll` (Microsoft Software License Terms). Check that bundling it suits
  how you distribute justStem.
- [HT-Demucs](https://github.com/facebookresearch/demucs) (Meta): MIT. ONNX export by
  [StemSplitio](https://huggingface.co/StemSplitio/htdemucs-6s-onnx) (MIT)
- BS-RoFormer SW 6-stem (HQ mode; bundled only when you build it): ONNX export by
  [elicwhite](https://huggingface.co/elicwhite/bs-roformer-sw-6stem-onnx) (MIT code) of weights
  that were rehosted as `jarredou/BS-ROFO-SW-Fixed`. **No licence has ever been stated for the
  weights.** That repo (created 16 Aug 2025, marked `license: unknown`) was deleted between
  Feb and Jul 2026; its archived
  [model card](https://web.archive.org/web/20250902051524/https://huggingface.co/jarredou/BS-ROFO-SW-Fixed)
  and [discussion](https://web.archive.org/web/20260125081636/https://huggingface.co/jarredou/BS-ROFO-SW-Fixed/discussions/1)
  say it was an edited checkpoint first shared on Discord, with no information on who trained it
  or on what. Mirrors such as `enerjazzer/BS-ROFO-SW-Fixed` are also `license: unknown`.
  justStem treats the weights as CC-BY-NC-SA-4.0 (non-commercial, share-alike) as its own
  cautious choice, not an inherited licence. Releases from 0.0.2 bundle them, so those builds
  are for non-commercial use.
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) (Unlicense) and [Deno](https://deno.com) (MIT),
  downloaded at runtime, not bundled
