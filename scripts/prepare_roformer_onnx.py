"""
Build models/bs_roformer_sw_6stem_opt.onnx (HQ mode) from bs_roformer_sw_6stem_fp16.onnx so that it
runs on the GPU through CoreML, with fp16 maths (and stays plain opset-17 ONNX for DirectML, CUDA
and CPU).

As exported, CoreML rejects ~490 of the graph's nodes and splits it into 174 partitions; compiling
those exhausts memory on a 16 GB Mac and the work falls back to the CPU. After these rewrites
CoreML takes 6591 of 6595 nodes in 2 partitions (the 4 left are small Gathers). Measured on an M4
per 4 s chunk (345 STFT frames): CPU 5.17 s, CoreML (CPUAndGPU) 1.09 s; a 5-minute song
splits in ~140 s once CoreML has compiled the model (~2.5 min, cached).

Steps (all exact rewrites, no retraining):
  1. Drop the zero-length Slices that feed Concats (the unrotated part of the rotary embedding is
     empty here); Concats left with one input become Identity. Remove nodes that become dead.
  2. Einsum 'bhid,bhjd->bhij' -> Transpose + MatMul; 'bhij,bhjd->bhid' -> MatMul;
     '...,f->...f' -> Unsqueeze + Mul.
  3. ReduceL2 (last axis, keepdims) -> Mul(x, x) + ReduceSum + Sqrt.
  4. Neg -> Mul by -1.
  5. Expand feeding only Div -> Identity (Div broadcasts).
  6. Check the result against the original model on CPU.
  7. Convert the maths to fp16 (inputs and outputs stay fp32). On an M4 GPU this is 1.7x faster
     (0.77 s vs 1.32 s per chunk; a 5-minute song in ~92 s) and the stems are 38-51 dB away from
     fp32, far below audibility. On CPU it is 1.7x slower (7.8 s vs 4.7 s per chunk), which only
     matters on the CPU fallback. The Pow ops (the attention scale) stay fp32: CoreML fails to
     compile them in fp16.

Usage:
  pip install onnx onnxruntime numpy onnxconverter-common
  python scripts/prepare_roformer_onnx.py bs_roformer_sw_6stem_fp16.onnx models/bs_roformer_sw_6stem_opt.onnx

Licence: the BS-RoFormer SW weights are CC-BY-NC-SA-4.0 (non-commercial, share-alike).
"""
from __future__ import annotations

import collections
import sys

import numpy as np
import onnx
import onnxruntime as ort
from onnx import helper, numpy_helper, shape_inference


def shapes(model: onnx.ModelProto) -> dict[str, list[int | None]]:
    return {
        v.name: [d.dim_value if d.HasField("dim_value") else None for d in v.type.tensor_type.shape.dim]
        for v in list(model.graph.value_info) + list(model.graph.input) + list(model.graph.output)
    }


def drop_empty_slices(model: onnx.ModelProto) -> int:
    empty = {name for name, s in shapes(model).items() if 0 in s}
    fixed = 0
    for n in model.graph.node:
        if n.op_type == "Concat" and any(i in empty for i in n.input):
            keep = [i for i in n.input if i not in empty]
            del n.input[:]
            n.input.extend(keep)
            if len(keep) == 1:
                n.op_type = "Identity"
                del n.attribute[:]
            fixed += 1
    return fixed


def remove_dead_nodes(model: onnx.ModelProto) -> int:
    nodes = list(model.graph.node)
    outputs = {o.name for o in model.graph.output}
    while True:
        needed = set(outputs)
        for n in nodes:
            needed.update(n.input)
        alive = [n for n in nodes if any(o in needed for o in n.output)]
        if len(alive) == len(nodes):
            break
        nodes = alive
    removed = len(model.graph.node) - len(nodes)
    del model.graph.node[:]
    model.graph.node.extend(nodes)
    return removed


def rewrite_ops(model: onnx.ModelProto) -> collections.Counter:
    g = model.graph
    consumers = collections.defaultdict(list)
    for n in g.node:
        for i in n.input:
            consumers[i].append(n)
    dtype = {v.name: v.type.tensor_type.elem_type for v in list(g.value_info) + list(g.input) + list(g.output)}
    dtype.update({t.name: t.data_type for t in g.initializer})

    inits: list[onnx.TensorProto] = []

    def const(name: str, arr: np.ndarray) -> str:
        inits.append(numpy_helper.from_array(arr, name))
        return name

    last_axis = const("rw_axes_last", np.array([-1], np.int64))
    out, count = [], collections.Counter()
    for k, n in enumerate(g.node):
        p = f"rw{k}"
        if n.op_type == "Einsum":
            eq = n.attribute[0].s.decode().replace(" ", "")
            a, b = n.input
            y = n.output[0]
            if eq == "bhid,bhjd->bhij":
                out += [helper.make_node("Transpose", [b], [p + "_t"], perm=[0, 1, 3, 2]),
                        helper.make_node("MatMul", [a, p + "_t"], [y])]
            elif eq == "bhij,bhjd->bhid":
                out.append(helper.make_node("MatMul", [a, b], [y]))
            elif eq == "...,f->...f":
                out += [helper.make_node("Unsqueeze", [a, last_axis], [p + "_u"]),
                        helper.make_node("Mul", [p + "_u", b], [y])]
            else:
                raise SystemExit(f"unhandled Einsum '{eq}'")
            count["Einsum"] += 1
        elif n.op_type == "ReduceL2":
            x = n.input[0]
            out += [helper.make_node("Mul", [x, x], [p + "_sq"]),
                    helper.make_node("ReduceSum", [p + "_sq", last_axis], [p + "_sum"], keepdims=1),
                    helper.make_node("Sqrt", [p + "_sum"], [n.output[0]])]
            count["ReduceL2"] += 1
        elif n.op_type == "Neg":
            np_type = np.float16 if dtype.get(n.input[0]) == onnx.TensorProto.FLOAT16 else np.float32
            minus_one = const(p + "_minus_one", np.array(-1, np_type))
            out.append(helper.make_node("Mul", [n.input[0], minus_one], [n.output[0]]))
            count["Neg"] += 1
        elif n.op_type == "Expand" and consumers[n.output[0]] and all(c.op_type == "Div" for c in consumers[n.output[0]]):
            out.append(helper.make_node("Identity", [n.input[0]], [n.output[0]]))
            count["Expand"] += 1
        elif n.op_type == "Concat" and len(n.input) == 1:
            out.append(helper.make_node("Identity", [n.input[0]], [n.output[0]]))
            count["Concat"] += 1
        else:
            out.append(n)
    del g.node[:]
    g.node.extend(out)
    g.initializer.extend(inits)
    return count


def check(original: str, rewritten: str) -> float:
    ref = ort.InferenceSession(original, providers=["CPUExecutionProvider"])
    new = ort.InferenceSession(rewritten, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(0)
    feed = {i.name: (rng.standard_normal(i.shape) * 0.1).astype(np.float32) for i in ref.get_inputs()}
    return max(float(np.abs(a - b).max()) for a, b in zip(ref.run(None, feed), new.run(None, feed)))


def to_fp16_compute(model: onnx.ModelProto) -> onnx.ModelProto:
    from onnxconverter_common import float16

    # The weights are already fp16 (with Casts up to fp32), hence check_fp16_ready=False.
    m16 = float16.convert_float_to_float16(model, keep_io_types=True, check_fp16_ready=False, op_block_list=["Pow"])
    # Those weight Casts still go up to fp32 and would leave fp32 islands: point them at fp16, except
    # the Casts that feed the blocked Pows, and the graph outputs.
    consumers = collections.defaultdict(list)
    for n in m16.graph.node:
        for i in n.input:
            consumers[i].append(n.op_type)
    keep = {o.name for o in m16.graph.output}
    for n in m16.graph.node:
        if n.op_type == "Cast" and n.output[0] not in keep and "Pow" not in consumers[n.output[0]]:
            for a in n.attribute:
                if a.name == "to" and a.i == onnx.TensorProto.FLOAT:
                    a.i = onnx.TensorProto.FLOAT16
    return m16


def snr_db(reference: str, candidate: str) -> float:
    ref = ort.InferenceSession(reference, providers=["CPUExecutionProvider"])
    new = ort.InferenceSession(candidate, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(0)
    feed = {i.name: (rng.standard_normal(i.shape) * 0.1).astype(np.float32) for i in ref.get_inputs()}
    a = np.concatenate([x.ravel() for x in ref.run(None, feed)]).astype(np.float64)
    b = np.concatenate([x.ravel() for x in new.run(None, feed)]).astype(np.float64)
    return float(10 * np.log10((a**2).sum() / max(((a - b) ** 2).sum(), 1e-30)))


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1:]

    model = shape_inference.infer_shapes(onnx.load(src))
    print(f"empty slices dropped from {drop_empty_slices(model)} Concats")
    print(f"dead nodes removed: {remove_dead_nodes(model)}")
    del model.graph.value_info[:]
    model = shape_inference.infer_shapes(model)

    print(f"rewritten: {dict(rewrite_ops(model))}")
    del model.graph.value_info[:]
    model = shape_inference.infer_shapes(model)
    onnx.checker.check_model(model)
    onnx.save(model, dst)
    diff = check(src, dst)
    print(f"rewrites: max difference from the original on CPU: {diff:.2e}")
    if diff > 1e-4:
        raise SystemExit("rewritten model doesn't match the original")

    onnx.save(to_fp16_compute(model), dst)
    snr = snr_db(src, dst)
    print(f"fp16 maths: {snr:.1f} dB from the original on CPU")
    if snr < 30:
        raise SystemExit("fp16 model is too far from the original")


if __name__ == "__main__":
    main()
