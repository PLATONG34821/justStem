# CoverTools

A VST3 / AU plug-in (and standalone app) for people who make covers. Give it a song (an
audio file or a YouTube link) and it:

- splits it into **vocals, drums, bass, guitar, piano and other**, plus an instrumental,
  saved as 24-bit WAVs you can drag straight onto DAW tracks;
- finds the **key** (with relative key, Camelot code and tuning), **tempo** and **loudness** (LUFS);
- previews the stems in sync with your session (mute / solo / volume per stem).

Everything runs on your computer. The AI model (HT-Demucs 6-stem) ships inside the plug-in.

| | Formats | AI runs on | Requirements |
|---|---|---|---|
| **macOS** | VST3, AU, standalone | GPU / Neural Engine (CoreML), CPU fallback | macOS 12+, Apple Silicon |
| **Windows** | VST3, standalone | GPU (DirectML: any DirectX 12 GPU), CPU fallback | Windows 10 1903+, x64 or ARM64 |
| **Linux** | VST3, standalone | CPU | x64 or ARM64, glibc |

Intel Macs aren't supported: Microsoft ships ONNX Runtime for macOS on Apple Silicon only.

## Building

You need CMake ≥ 3.28, Ninja and a C++20 compiler: the Xcode command-line tools on macOS, Visual
Studio 2022 (run from a "Developer" command prompt) on Windows, GCC or Clang on Linux plus JUCE's
packages (see `.github/workflows/build.yml`). JUCE, ONNX Runtime and FAAD2 are downloaded by CMake
(pinned versions, checked against their SHA-256).

1. Build the model once (needs Python 3); see the top of `scripts/prepare_demucs_onnx.py`:
   ```sh
   python3 -m venv .venv && .venv/bin/pip install onnx onnxruntime numpy
   curl -L -o htdemucs_6s_fp16weights.onnx \
     https://huggingface.co/StemSplitio/htdemucs-6s-onnx/resolve/main/htdemucs_6s_fp16weights.onnx
   .venv/bin/python scripts/prepare_demucs_onnx.py htdemucs_6s_fp16weights.onnx models/htdemucs_6s_web2.onnx
   ```
2. Build and install:
   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```
   The plug-ins are in `build/CoverTools_artefacts/Release/`, and are also installed for you:
   - **macOS:** VST3 and AU in `~/Library/Audio/Plug-Ins/`. Builds are ad-hoc signed, so they run
     on the machine that built them; distributing them needs a Developer ID signature and notarization.
   - **Linux:** VST3 in `~/.vst3/`.
   - **Windows:** copy `CoverTools.vst3` to `C:\Program Files\Common Files\VST3\` (needs admin rights).

Every push is built and tested on macOS, Windows and Linux by GitHub Actions
(`.github/workflows/build.yml`); the built plug-ins can be downloaded from each run.

## Code layout

```
src/core/      Separation and analysis. Plain C++ + ONNX Runtime, no JUCE.
  Audio.h        shared types: StereoAudio, Stem, sample rate
  Separator.*    runs HT-Demucs in crossfaded chunks (CoreML / DirectML / CPU)
  Analysis.*     key, tempo, loudness (EBU R128)
  Resampler.*    sample-rate conversion to 44.1 kHz (polyphase, exact timing)
src/io/        Files and the outside world (JUCE).
  AudioFiles.*   decode audio files to 44.1 kHz stereo (gapless); write WAVs
  M4aDecoder.*   M4A / MP4 reader (classic and fragmented) + FAAD2 AAC decoding
  YouTube.*      download a video's audio with yt-dlp
  ToolInstaller.* download and verify yt-dlp and Deno
  AppPaths.h     where files are stored
src/plugin/    The plug-in.
  PluginProcessor.*  audio pass-through + stem preview, runs song jobs, saves state
  PluginEditor.*     the window
  SongJob.*          background work: import -> split -> analyse -> save
  SongResult.*       a split song; saves and reloads it
  StemPlayer.*       real-time stem preview (lock-free on the audio thread)
  ui/                dashboard theme (Inter font, colours, icons) and widgets (gauge, stem rows)
tools/         Developer tools, not shipped (see Testing).
scripts/       Builds the model file.
resources/     Embedded fonts.
cmake/         ONNX Runtime download (per OS), packaging.
.github/       CI: build + test on macOS, Windows and Linux.
```

## Audio formats

WAV, AIFF, FLAC, Ogg Vorbis, MP3 and M4A (AAC-LC and HE-AAC, including YouTube's fragmented
files). The same decoders are used on every OS: JUCE's readers, plus our own MP4 reader with FAAD2
for AAC. Decoding is gapless: the encoder delay and padding recorded in the file (iTunes
`iTunSMPB`, MP4 edit lists, LAME / ffmpeg MP3 tags) are removed, so the stems line up sample-exactly
with the original file in a DAW.

## Where files go

| What | Where |
|---|---|
| Stems for each song (7 WAVs + `info.json`) | `<your Music folder>/CoverTools/<song title>/` |
| yt-dlp and Deno (downloaded on first YouTube link) | `<app data>/CoverTools/bin/` |
| CoreML's compiled model (macOS; makes later loads fast) | `<app data>/CoverTools/coreml-cache/` |
| YouTube audio while it's being processed | `<app data>/CoverTools/downloads/` |

`<app data>` is `~/Library/Application Support` on macOS, `%APPDATA%` on Windows and `~/.config` on Linux.

A DAW project only stores the song folder's path and the mute / solo / volume settings.

Uninstalling the plug-in doesn't remove any of this. To free space, use the **Storage** button
(above the stems). It shows how much each part uses and lets you:

- delete saved songs (moved to the Trash after you confirm; the song open in the plug-in is kept),
- clear the CoreML cache (rebuilt automatically, about 25 s the next time you split a song),
- remove the YouTube tools (downloaded again the next time you paste a link),
- clear temporary downloads.

Everything is disabled while a song is being processed.

## Testing

```sh
build/covertools_cli_artefacts/Release/covertools_cli analyze-selftest
build/covertools_cli_artefacts/Release/covertools_cli selftest models/htdemucs_6s_web2.onnx
build/covertools_uitest_artefacts/Release/covertools_uitest models/htdemucs_6s_web2.onnx <song> <screenshot folder>
```

- `analyze-selftest`: the resampler against exact sine waves and an aliasing test, loudness against
  the EBU reference signals, key detection in all 24 keys, tempo from 72 to 174 BPM.
- `selftest`: separates a synthetic track and scores each stem against the original parts.
- `covertools_uitest`: runs the real plug-in code without a DAW. It renders the window to PNGs,
  splits a song, checks the preview audio and saves / reloads the plug-in state.

`covertools_cli` can also `separate`, `analyze` or `import` real songs; see the top of
`tools/cli.cpp`.

## Licences

CoverTools is licensed under the GNU AGPL-3.0 (see `LICENSE`), as required by JUCE's free licence.

- [JUCE](https://juce.com): AGPL-3.0 / commercial
- [ONNX Runtime](https://github.com/microsoft/onnxruntime): MIT
- [Inter](https://rsms.me/inter/) font (UI typeface, embedded): SIL Open Font License 1.1, see `resources/fonts/OFL-LICENSE.txt`
- [FAAD2](https://github.com/knik0/faad2) (AAC decoder): GPL-2.0-or-later, used under GPL-3.0 with the AGPL
- [DirectML](https://github.com/microsoft/DirectML) (Windows builds only): Microsoft's proprietary,
  redistributable `DirectML.dll` (Microsoft Software License Terms). Check that bundling it suits
  how you distribute CoverTools.
- [HT-Demucs](https://github.com/facebookresearch/demucs) (Meta): MIT. ONNX export by
  [StemSplitio](https://huggingface.co/StemSplitio/htdemucs-6s-onnx) (MIT)
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) (Unlicense) and [Deno](https://deno.com) (MIT),
  downloaded at runtime, not bundled

Only process music you have the rights to use. Downloading from YouTube may break YouTube's
terms of service.
