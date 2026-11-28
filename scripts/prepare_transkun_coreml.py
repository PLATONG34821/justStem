"""
Build transkun_piano.mlpackage.zip, the MIDI model for the Mac's Neural Engine and GPU, from
Transkun 2.0.1's PyTorch weights (pip transkun, Yujia Yan, MIT).

It is the same network as transkun_piano.onnx (featuresBatch -> S, ctx), converted with coremltools
instead of run through ONNX Runtime's CoreML provider, which runs it barely faster than the CPU. Fixed
at T = 691 frames, the size of every segment justStem cuts (Transcriber.cpp, tk::segmentSize).

Mixed precision. All-fp16 (the Neural Engine) is fastest but wrong: S overflows fp16 (it reaches
~90k) and the context drifts ~4%, enough to add notes that aren't there. So these stay fp32:
  - the interval scorer (from the linear making q, k and the diagonal to S),
  - GELU, the convolutions and the RMS norms, where most of the drift came from.
Per 16 s segment on an M4: ~250 ms, against ~460 ms all-fp32 on the GPU and ~780 ms on the CPU
(ONNX Runtime). On a whole song the notes match the CPU's: 99-100% per stem within 10 ms.

Usage (Python 3.12):
  pip install transkun==2.0.1 torch==2.15.0 coremltools==9.0 "setuptools<70"
  python scripts/prepare_transkun_coreml.py release/transkun_piano.mlpackage.zip
"""

import os
import shutil
import sys
import tempfile

import numpy as np
import torch
import coremltools as ct
import moduleconf
import pkg_resources
import transkun.LayersTransformer as layers

T = 691


class _TorchForConversion:
    """torch, with the two calls coremltools can't convert replaced by equivalents."""

    def __getattr__(self, name):
        return getattr(torch, name)

    @staticmethod
    def meshgrid(coords, indexing="ij"):  # one coordinate vector: meshgrid is the identity
        return tuple(coords) if len(coords) == 1 else torch.meshgrid(coords, indexing=indexing)

    @staticmethod
    def diag_embed(x):
        return x.unsqueeze(-1) * torch.eye(x.shape[-1], dtype=x.dtype)


layers.torch = _TorchForConversion()


class Network(torch.nn.Module):
    """The part transkun_piano.onnx holds: features -> pairwise scores S [T][T][90], context ctx [90][T][256]."""

    def __init__(self, model):
        super().__init__()
        self.model = model
        self.pitches = torch.tensor(model.targetMIDIPitch)

    def forward(self, features):
        ctx = self.model.backbone(features, outputIndices=self.pitches)
        S, _ = self.model.scorer(ctx)
        return S.flatten(-2, -1), ctx[0]


FP32_OPS = {"gelu", "conv", "conv_transpose", "reduce_mean", "pow", "rsqrt"}


def fp16_op(op, _scorer=[]):
    """Whether an op of the converted program runs in fp16 (see the top of this file)."""
    if not _scorer:  # the scorer: the last linear before the last matmul (q . k), and everything after
        ops = list(op.enclosing_block.operations)
        last_matmul = max(i for i, o in enumerate(ops) if o.op_type == "matmul")
        first = max(i for i, o in enumerate(ops[:last_matmul]) if o.op_type == "linear")
        _scorer.append({id(o) for o in ops[first:]})
    return op.op_type not in FP32_OPS and id(op) not in _scorer[0]


def main(out_zip):
    conf = moduleconf.parseFromFile(pkg_resources.resource_filename("transkun", "pretrained/2.0.conf"))["Model"]
    model = conf.module.TransKun(conf=conf.config)
    checkpoint = torch.load(pkg_resources.resource_filename("transkun", "pretrained/2.0.pt"), map_location="cpu")
    model.load_state_dict(checkpoint.get("best_state_dict", checkpoint.get("state_dict")), strict=False)
    model.eval()
    model.backbone.useGradientCheckpoint = False

    net = Network(model).eval()
    x = torch.from_numpy(np.random.default_rng(0).random((1, T, 229, 6)).astype(np.float32))
    traced = torch.jit.trace(net, x)
    mlmodel = ct.convert(
        traced,
        inputs=[ct.TensorType(name="featuresBatch", shape=x.shape, dtype=np.float32)],
        outputs=[ct.TensorType(name="S", dtype=np.float32), ct.TensorType(name="ctx", dtype=np.float32)],
        compute_precision=ct.transform.FP16ComputePrecision(op_selector=fp16_op),
        minimum_deployment_target=ct.target.macOS13,
        convert_to="mlprogram",
    )
    mlmodel.short_description = "Transkun 2.0 (Yujia Yan, MIT) for justStem: features -> S, ctx"

    with tempfile.TemporaryDirectory() as tmp:
        mlmodel.save(os.path.join(tmp, "transkun_piano.mlpackage"))
        base = out_zip[:-4] if out_zip.endswith(".zip") else out_zip
        shutil.make_archive(base, "zip", tmp, "transkun_piano.mlpackage")
    print("wrote", out_zip, os.path.getsize(out_zip), "bytes")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "transkun_piano.mlpackage.zip")
