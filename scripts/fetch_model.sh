#!/usr/bin/env bash
# Downloads a model's source ONNX file into the current folder, from a pinned Hugging Face commit,
# and checks its SHA-256, so every build (and every release) starts from the same file.
#   scripts/fetch_model.sh fast   htdemucs_6s_fp16weights.onnx    (see scripts/prepare_demucs_onnx.py)
#   scripts/fetch_model.sh hq     bs_roformer_sw_6stem_fp16.onnx  (see scripts/prepare_roformer_onnx.py)
set -euo pipefail

case "${1:-}" in
  fast)
    repo=StemSplitio/htdemucs-6s-onnx
    rev=49df9b6989cf2150840ea65b0bef77a2e471b678
    file=htdemucs_6s_fp16weights.onnx
    sha=7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869 ;;
  hq)
    repo=elicwhite/bs-roformer-sw-6stem-onnx
    rev=a744f80957374e1735ad70fa122670b7961da8cc
    file=bs_roformer_sw_6stem_fp16.onnx
    sha=d3d2bac77a7023282cb5f35a5807179e34076b60589867b572275f1a8ec36444 ;;
  *)
    echo "usage: $0 fast|hq" >&2
    exit 2 ;;
esac

curl -fL --retry 3 -o "$file.part" "https://huggingface.co/$repo/resolve/$rev/$file"
if command -v sha256sum > /dev/null; then got=$(sha256sum "$file.part" | cut -d' ' -f1)
else got=$(shasum -a 256 "$file.part" | cut -d' ' -f1); fi
if [ "$got" != "$sha" ]; then
  echo "error: $file has SHA-256 $got, expected $sha" >&2
  rm -f "$file.part"
  exit 1
fi
mv "$file.part" "$file"
echo "$file: SHA-256 OK"
