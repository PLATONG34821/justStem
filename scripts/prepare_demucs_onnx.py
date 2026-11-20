"""
Build models/htdemucs_6s_opt.onnx, the model bundled in the plug-in, from the public
htdemucs_6s export (StemSplitio/htdemucs-6s-onnx).

Steps:
  1. Constant-fold with ONNX Runtime (BASIC level: hardware-independent rewrites only). The
     export has ~30k shape-computation nodes, some on DOUBLE tensors; with the input shape
     fixed at [1, 2, 343980] they all fold away (~1.4k nodes remain).
  2. Rewrite the iSTFT. The export implements it as two ConvTranspose ops with a dense
     [2049 x 4096] inverse-DFT kernel (~137 GFLOP per chunk); as one MatMul plus a 4-way
     overlap-add it is the same math and much faster on GPUs.
  3. Store Conv/MatMul/Gemm weights as fp16 with a Cast back to fp32 (halves the file; the
     maths stays fp32). Technique from elicwhite/bs-roformer-web/python/fp16_weights.py (MIT).
  4. Check the result against the original model on CPU.

Converting the model to fp16 *compute* doesn't work: it isn't faster and the stems come out
as noise.

Usage:
  pip install onnx onnxruntime numpy
  curl -L -o htdemucs_6s_fp16weights.onnx \
    https://huggingface.co/StemSplitio/htdemucs-6s-onnx/resolve/main/htdemucs_6s_fp16weights.onnx
  python scripts/prepare_demucs_onnx.py htdemucs_6s_fp16weights.onnx models/htdemucs_6s_opt.onnx
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper

WEIGHT_OP_INPUTS = {"Conv": (1, 2), "ConvTranspose": (1, 2), "MatMul": (0, 1), "Gemm": (0, 1, 2)}
N_SAMPLES = 343980


def fold(src: Path, dst: Path) -> None:
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    so.optimized_model_filepath = str(dst)
    ort.InferenceSession(str(src), so, providers=["CPUExecutionProvider"])


def fp16_weights(model: onnx.ModelProto) -> int:
    weights = {
        node.input[i]
        for node in model.graph.node
        for i in WEIGHT_OP_INPUTS.get(node.op_type, ())
        if i < len(node.input) and node.input[i]
    }
    produced = {o for n in model.graph.node for o in n.output}
    graph_inputs = {i.name for i in model.graph.input}
    new_inits, casts = [], []
    for init in model.graph.initializer:
        if (
            init.name in weights
            and init.data_type == TensorProto.FLOAT
            and init.name not in produced
            and init.name not in graph_inputs
        ):
            arr = numpy_helper.to_array(init).astype(np.float16)
            new_inits.append(numpy_helper.from_array(arr, name=init.name + "_fp16"))
            casts.append(
                helper.make_node(
                    "Cast", [init.name + "_fp16"], [init.name], to=TensorProto.FLOAT,
                    name=init.name + "_cast_to_fp32",
                )
            )
        else:
            new_inits.append(init)
    model.graph.ClearField("initializer")
    model.graph.initializer.extend(new_inits)
    nodes = list(model.graph.node)
    model.graph.ClearField("node")
    model.graph.node.extend(casts + nodes)
    return len(casts)


def istft_to_matmul(model: onnx.ModelProto) -> None:
    """Replace real_istft's ConvTranspose pair with MatMul + overlap-add (same math)."""
    g = model.graph
    by_name = {n.name: n for n in g.node}
    inits = {i.name: i for i in g.initializer}
    prod = {o: n for n in g.node for o in n.output}
    add = by_name["/real_istft/Add"]
    convs = [prod[i] for i in add.input]
    assert all(c.op_type == "ConvTranspose" for c in convs)
    gathers = [prod[c.input[0]] for c in convs]
    spec = gathers[0].input[0]  # [B, 2, F, T]: the two Gathers pick re / im on axis 1
    assert all(gt.input[0] == spec and helper.get_attribute_value(gt.attribute[0]) == 1 for gt in gathers)
    idx = [int(numpy_helper.to_array(inits[gt.input[1]])) for gt in gathers]
    kern = [numpy_helper.to_array(inits[c.input[1]])[:, 0, :] for c in convs]  # [F, K]
    stride = next(helper.get_attribute_value(a) for a in convs[0].attribute if a.name == "strides")[0]
    F, K = kern[0].shape
    hops = K // stride
    assert K % stride == 0

    w = np.zeros((2 * F, K), np.float32)
    for i, k in zip(idx, kern):
        w[i * F:(i + 1) * F] = k

    # Shapes are fixed by the trace: spec [B, 2, F, T], output [B, 1, (T - 1) * stride + K].
    inferred = onnx.shape_inference.infer_shapes(model)
    dims = {v.name: [d.dim_value for d in v.type.tensor_type.shape.dim] for v in inferred.graph.value_info}
    B, _, F2, T = dims[spec]
    assert F2 == F
    p = "/istft_mm/"

    def c(name: str, value: np.ndarray) -> str:
        g.initializer.append(numpy_helper.from_array(np.asarray(value), p + name))
        return p + name

    nodes = [
        helper.make_node("Transpose", [spec], [p + "xt"], perm=[0, 3, 1, 2], name=p + "transpose"),
        helper.make_node("Reshape", [p + "xt", c("xshape", np.array([B, T, 2 * F], np.int64))], [p + "x"], name=p + "reshape_x"),
        helper.make_node("MatMul", [p + "x", c("w", w)], [p + "frames"], name=p + "matmul"),  # [B, T, K]
        helper.make_node("Reshape", [p + "frames", c("fshape", np.array([B, T, hops, stride], np.int64))], [p + "f4"], name=p + "reshape_f"),
    ]
    parts = []
    for j in range(hops):
        q = f"{p}hop{j}_"
        nodes += [
            helper.make_node("Slice", [p + "f4", c(f"hop{j}_s", np.array([j], np.int64)), c(f"hop{j}_e", np.array([j + 1], np.int64)), c(f"hop{j}_ax", np.array([2], np.int64))], [q + "sl"], name=q + "slice"),
            helper.make_node("Reshape", [q + "sl", c(f"hop{j}_shape", np.array([B, T, stride], np.int64))], [q + "r"], name=q + "reshape"),
            # Frame t's j-th hop lands at output block t + j.
            helper.make_node("Pad", [q + "r", c(f"hop{j}_pads", np.array([0, j, 0, 0, hops - 1 - j, 0], np.int64))], [q + "p"], name=q + "pad"),
        ]
        parts.append(q + "p")
    acc = parts[0]
    for j, part in enumerate(parts[1:], 1):
        nodes.append(helper.make_node("Add", [acc, part], [f"{p}sum{j}"], name=f"{p}add{j}"))
        acc = f"{p}sum{j}"
    out_len = (T - 1) * stride + K
    nodes.append(helper.make_node("Reshape", [acc, c("oshape", np.array([B, 1, out_len], np.int64))], [add.output[0]], name=p + "reshape_out"))

    remove = {id(n) for n in convs + gathers + [add]}
    kept = [n for n in g.node if id(n) not in remove]
    at = next(i for i, n in enumerate(kept) if add.output[0] in n.input)
    g.ClearField("node")
    g.node.extend(kept[:at] + nodes + kept[at:])
    used = {i for n in g.node for i in n.input}
    keep = [i for i in g.initializer if i.name in used]
    g.ClearField("initializer")
    g.initializer.extend(keep)
    del g.value_info[:]


def check_no_double(model: onnx.ModelProto) -> None:
    inferred = onnx.shape_inference.infer_shapes(model)
    doubles = [
        v.name
        for v in list(inferred.graph.value_info) + list(inferred.graph.output)
        if v.type.tensor_type.elem_type == TensorProto.DOUBLE
    ]
    if doubles:
        raise SystemExit(f"DOUBLE tensors remain (not supported in ORT-web): {doubles[:5]}")


def compare(a: Path, b: Path) -> None:
    rng = np.random.default_rng(0)
    t = np.arange(N_SAMPLES) / 44100
    music_like = (0.3 * np.sin(2 * np.pi * 110 * t) + 0.1 * rng.standard_normal(N_SAMPLES)).astype(np.float32)
    x = np.stack([music_like, music_like * 0.8])[None]
    outs = []
    for p in (a, b):
        s = ort.InferenceSession(str(p), providers=["CPUExecutionProvider"])
        outs.append(s.run(["stems"], {"mix": x})[0])
    diff = np.abs(outs[0] - outs[1]).max()
    print(f"max abs diff vs original: {diff:.2e}")
    if not diff < 1e-3:
        raise SystemExit("Output differs from the original model")


def main() -> None:
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    with tempfile.TemporaryDirectory() as tmp:
        folded = Path(tmp) / "folded.onnx"
        fold(src, folded)
        model = onnx.load(str(folded))
    print(f"nodes after folding: {len(model.graph.node)}")
    check_no_double(model)
    istft_to_matmul(model)
    print("iSTFT: ConvTranspose -> MatMul + overlap-add")
    print(f"fp16 weights: {fp16_weights(model)} tensors")
    dst.parent.mkdir(parents=True, exist_ok=True)  # models/ isn't in git, so a fresh checkout has none
    onnx.save(model, str(dst))
    print(f"{src.stat().st_size / 1e6:.1f} MB -> {dst.stat().st_size / 1e6:.1f} MB")
    compare(src, dst)


if __name__ == "__main__":
    main()
