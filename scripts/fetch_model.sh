#!/usr/bin/env bash
# Downloads a model's source files from a pinned commit and checks their SHA-256, so every build (and
# every release) starts from the same files.
#   scripts/fetch_model.sh fast    htdemucs_6s_fp16weights.onnx    (see scripts/prepare_demucs_onnx.py)
#   scripts/fetch_model.sh hq      bs_roformer_sw_6stem_fp16.onnx  (see scripts/prepare_roformer_onnx.py)
#   scripts/fetch_model.sh midi    models/basic_pitch.onnx         (Basic Pitch, used as is)
#   scripts/fetch_model.sh piano   models/transkun_piano*          (Transkun, used as is)
set -euo pipefail

# fetch <url> <file> <sha256>
fetch() {
  curl -fL --retry 3 -o "$2.part" "$1"
  if command -v sha256sum > /dev/null; then got=$(sha256sum "$2.part" | cut -d' ' -f1)
  else got=$(shasum -a 256 "$2.part" | cut -d' ' -f1); fi
  if [ "$got" != "$3" ]; then
    echo "error: $2 has SHA-256 $got, expected $3" >&2
    rm -f "$2.part"
    exit 1
  fi
  mv "$2.part" "$2"
  echo "$2: SHA-256 OK"
}

case "${1:-}" in
  fast)
    fetch https://huggingface.co/StemSplitio/htdemucs-6s-onnx/resolve/49df9b6989cf2150840ea65b0bef77a2e471b678/htdemucs_6s_fp16weights.onnx \
      htdemucs_6s_fp16weights.onnx 7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869 ;;
  hq)
    fetch https://huggingface.co/elicwhite/bs-roformer-sw-6stem-onnx/resolve/a744f80957374e1735ad70fa122670b7961da8cc/bs_roformer_sw_6stem_fp16.onnx \
      bs_roformer_sw_6stem_fp16.onnx d3d2bac77a7023282cb5f35a5807179e34076b60589867b572275f1a8ec36444 ;;
  midi)
    # Spotify's Basic Pitch (Apache-2.0), the ONNX file shipped in its Python package.
    mkdir -p models
    fetch https://raw.githubusercontent.com/spotify/basic-pitch/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/saved_models/icassp_2022/nmp.onnx \
      models/basic_pitch.onnx 2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec ;;
  piano)
    # Transkun 2.0 (Yujia Yan, MIT) exported to ONNX by TuesdayCrowd/transkun-onnx. Its front end
    # (6 analysis windows, then the mel filters, raw float32) goes into one file.
    mkdir -p models
    base=https://huggingface.co/TuesdayCrowd/transkun-onnx/resolve/b44598166715a50d863973a8ecbbdbbb392f08d6
    fetch $base/transkun.onnx models/transkun_piano.onnx 6d6960ecbce5c3d51051ebc1ebdeec4d3018ea6cfd360fb1ae96359aa7a075a3
    fetch $base/transkun-heads.onnx models/transkun_piano_heads.onnx f609f53fbb4df0a2165f941aff1d5a1c1063ba40aaa699365448dca5d9f3544d
    fetch $base/windows.f32 windows.f32 b059ebf1e5550a8186b120ddf3f068dd9075e72338051bcd0349c1f436ddb14f
    fetch $base/freq2mels.f32 freq2mels.f32 449f2a5269cde41f6705cfde7157240ce7c8930f20b50909d57e633395bbf0f4
    cat windows.f32 freq2mels.f32 > models/transkun_piano_frontend.bin
    rm windows.f32 freq2mels.f32 ;;
  *)
    echo "usage: $0 fast|hq|midi|piano" >&2
    exit 2 ;;
esac
