"""Converts MediaPipe's pose landmarker to the ONNX models Xenia's webcam Kinect
runs with ONNX Runtime.

  python convert_pose_models.py pose_landmarker_full.task OUT_DIR

pose_landmarker_full.task is Google's (Apache License 2.0):
https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/latest/pose_landmarker_full.task
It holds two TensorFlow Lite models, which become OUT_DIR/pose_detector.onnx and
OUT_DIR/pose_landmarks_detector.onnx: the same networks, with their float16
(and, in the detector, sparse) weights expanded to float32. Each is checked
against the original on the same input before it's written.

Needs: pip install tflite onnx onnxruntime ai-edge-litert numpy

The models use only a few TensorFlow Lite operators, which are translated
here directly. ONNX convolutions are channels-first, so 4-D tensors are kept
NCHW inside the graph and turned back to TensorFlow's NHWC where a reshape or
an output needs that order.
"""
import os
import sys
import zipfile

import numpy as np
import onnx
import tflite
from ai_edge_litert.interpreter import Interpreter, OpResolverType
from onnx import TensorProto, helper, numpy_helper

OPSET = 13
OP_NAMES = {v: k for k, v in vars(tflite.BuiltinOperator).items()
            if not k.startswith("_")}
NHWC_TO_NCHW = [0, 3, 1, 2]
NCHW_TO_NHWC = [0, 2, 3, 1]


class Converter:
    def __init__(self, content):
        self.model = tflite.Model.GetRootAsModel(content, 0)
        self.graph = self.model.Subgraphs(0)
        # Every tensor's value after one run, so that weights the model stores
        # as float16 or sparse come out as the dense float32 the interpreter
        # expands them to. Without the XNNPACK delegate, which would take the
        # graph over and leave them uncomputed.
        self.interpreter = Interpreter(
            model_content=content, experimental_preserve_all_tensors=True,
            experimental_op_resolver_type=OpResolverType
            .BUILTIN_WITHOUT_DEFAULT_DELEGATES)
        self.interpreter.allocate_tensors()
        for d in self.interpreter.get_input_details():
            self.interpreter.set_tensor(d["index"],
                                        np.zeros(d["shape"], np.float32))
        self.interpreter.invoke()
        self.nodes = []
        self.initializers = []
        # Tensor index -> {layout: ONNX name}; 4-D activations may be held as
        # NCHW, NHWC or both.
        self.names = {}
        self.const = {}
        self.counter = 0

    def tensor(self, i):
        return self.graph.Tensors(i)

    def shape(self, i):
        return [int(d) for d in self.tensor(i).ShapeAsNumpy()]

    def value(self, i):
        if i not in self.const:
            self.const[i] = np.array(self.interpreter.get_tensor(i))
        return self.const[i]

    def fresh(self, hint):
        self.counter += 1
        return f"{hint}_{self.counter}"

    def add_const(self, array, hint="const"):
        name = self.fresh(hint)
        self.initializers.append(numpy_helper.from_array(np.asarray(array),
                                                         name))
        return name

    def node(self, op, inputs, hint, **attrs):
        out = self.fresh(hint)
        self.nodes.append(helper.make_node(op, inputs, [out], **attrs))
        return out

    def get(self, i, layout):
        """Tensor i as an ONNX name, in layout NCHW, NHWC or plain."""
        held = self.names.get(i)
        if held is None:
            # A constant.
            v = self.value(i).astype(np.float32) if self.value(i).dtype == \
                np.float16 else self.value(i)
            if layout == "NCHW" and v.ndim == 4:
                v = v.transpose(NHWC_TO_NCHW)
            return self.add_const(v)
        if layout in held:
            return held[layout]
        if layout == "NCHW" and "NHWC" in held:
            held["NCHW"] = self.node("Transpose", [held["NHWC"]], "nchw",
                                     perm=NHWC_TO_NCHW)
        elif layout == "NHWC" and "NCHW" in held:
            held["NHWC"] = self.node("Transpose", [held["NCHW"]], "nhwc",
                                     perm=NCHW_TO_NHWC)
        elif layout == "plain":
            return held.get("NHWC") or self.get(i, "NHWC")
        else:
            raise ValueError(f"tensor {i} has no {layout} form")
        return held[layout]

    def activation(self, name, fused):
        if fused == tflite.ActivationFunctionType.NONE:
            return name
        if fused == tflite.ActivationFunctionType.RELU:
            return self.node("Relu", [name], "relu")
        if fused == tflite.ActivationFunctionType.RELU6:
            return self.node("Clip", [name, self.add_const(np.float32(0)),
                                      self.add_const(np.float32(6))], "relu6")
        if fused == tflite.ActivationFunctionType.RELU_N1_TO_1:
            return self.node("Clip", [name, self.add_const(np.float32(-1)),
                                      self.add_const(np.float32(1))], "relu1")
        if fused == tflite.ActivationFunctionType.TANH:
            return self.node("Tanh", [name], "tanh")
        raise ValueError(f"fused activation {fused}")

    @staticmethod
    def options(op, cls):
        table = op.BuiltinOptions()
        opts = cls()
        opts.Init(table.Bytes, table.Pos)
        return opts

    def pads_attrs(self, padding):
        return {"auto_pad": "SAME_UPPER" if padding == tflite.Padding.SAME
                else "VALID"}

    def convert(self):
        produced_const = set()
        for k in range(self.graph.OperatorsLength()):
            op = self.graph.Operators(k)
            code = self.model.OperatorCodes(op.OpcodeIndex())
            name = OP_NAMES[max(code.BuiltinCode(), code.DeprecatedBuiltinCode())]
            ins = [int(i) for i in op.InputsAsNumpy()]
            outs = [int(i) for i in op.OutputsAsNumpy()]
            out = outs[0]
            if name in ("DEQUANTIZE", "DENSIFY"):
                # Weights: their expanded value is a constant.
                produced_const.add(out)
                continue
            if name == "CONV_2D":
                o = self.options(op, tflite.Conv2DOptions)
                w = self.value(ins[1]).astype(np.float32).transpose(0, 3, 1, 2)
                inputs = [self.get(ins[0], "NCHW"), self.add_const(w, "w")]
                if len(ins) > 2 and ins[2] >= 0:
                    inputs.append(self.add_const(
                        self.value(ins[2]).astype(np.float32), "b"))
                y = self.node("Conv", inputs, "conv",
                              strides=[o.StrideH(), o.StrideW()],
                              dilations=[o.DilationHFactor(), o.DilationWFactor()],
                              kernel_shape=list(w.shape[2:]),
                              **self.pads_attrs(o.Padding()))
                self.names[out] = {"NCHW": self.activation(
                    y, o.FusedActivationFunction())}
            elif name == "DEPTHWISE_CONV_2D":
                o = self.options(op, tflite.DepthwiseConv2DOptions)
                w = self.value(ins[1]).astype(np.float32).transpose(3, 0, 1, 2)
                groups = self.shape(ins[0])[3]
                inputs = [self.get(ins[0], "NCHW"), self.add_const(w, "w")]
                if len(ins) > 2 and ins[2] >= 0:
                    inputs.append(self.add_const(
                        self.value(ins[2]).astype(np.float32), "b"))
                y = self.node("Conv", inputs, "dwconv", group=groups,
                              strides=[o.StrideH(), o.StrideW()],
                              dilations=[o.DilationHFactor(), o.DilationWFactor()],
                              kernel_shape=list(w.shape[2:]),
                              **self.pads_attrs(o.Padding()))
                self.names[out] = {"NCHW": self.activation(
                    y, o.FusedActivationFunction())}
            elif name == "ADD":
                o = self.options(op, tflite.AddOptions)
                four_d = len(self.shape(out)) == 4
                operands = []
                for i in ins:
                    if i in self.names or not four_d:
                        operands.append(self.get(i, "NCHW" if four_d else "plain"))
                    else:
                        v = self.value(i).astype(np.float32)
                        if v.ndim == 4:
                            v = v.transpose(NHWC_TO_NCHW)
                        elif v.ndim == 1:
                            v = v.reshape(1, -1, 1, 1)
                        operands.append(self.add_const(v))
                y = self.node("Add", operands, "add")
                y = self.activation(y, o.FusedActivationFunction())
                self.names[out] = {"NCHW" if four_d else "plain": y}
            elif name == "PAD":
                p = self.value(ins[1]).astype(np.int64)
                pads = [p[0, 0], p[3, 0], p[1, 0], p[2, 0],
                        p[0, 1], p[3, 1], p[1, 1], p[2, 1]]
                y = self.node("Pad", [self.get(ins[0], "NCHW"),
                                      self.add_const(np.array(pads, np.int64))],
                              "pad")
                self.names[out] = {"NCHW": y}
            elif name == "RESIZE_BILINEAR":
                o = self.options(op, tflite.ResizeBilinearOptions)
                h, w = (int(v) for v in self.value(ins[1]))
                n, _, _, c = self.shape(ins[0])
                mode = ("align_corners" if o.AlignCorners() else
                        "half_pixel" if o.HalfPixelCenters() else "asymmetric")
                y = self.node("Resize", [self.get(ins[0], "NCHW"), "", "",
                                         self.add_const(np.array([n, c, h, w],
                                                                 np.int64))],
                              "resize", mode="linear",
                              coordinate_transformation_mode=mode)
                self.names[out] = {"NCHW": y}
            elif name == "RESHAPE":
                src = self.get(ins[0], "NHWC" if len(self.shape(ins[0])) == 4
                               else "plain")
                shape = np.array(self.shape(out), np.int64)
                y = self.node("Reshape", [src, self.add_const(shape)], "reshape")
                self.names[out] = {"NHWC" if len(shape) == 4 else "plain": y}
            elif name == "CONCATENATION":
                o = self.options(op, tflite.ConcatenationOptions)
                rank = len(self.shape(out))
                axis = o.Axis() % rank
                if rank == 4:
                    axis = NHWC_TO_NCHW.index(axis)
                    srcs = [self.get(i, "NCHW") for i in ins]
                else:
                    srcs = [self.get(i, "plain") for i in ins]
                y = self.node("Concat", srcs, "concat", axis=axis)
                y = self.activation(y, o.FusedActivationFunction())
                self.names[out] = {"NCHW" if rank == 4 else "plain": y}
            elif name == "DEPTH_TO_SPACE":
                o = self.options(op, tflite.DepthToSpaceOptions)
                y = self.node("DepthToSpace", [self.get(ins[0], "NCHW")], "d2s",
                              blocksize=o.BlockSize(), mode="DCR")
                self.names[out] = {"NCHW": y}
            elif name == "MAX_POOL_2D":
                o = self.options(op, tflite.Pool2DOptions)
                y = self.node("MaxPool", [self.get(ins[0], "NCHW")], "maxpool",
                              kernel_shape=[o.FilterHeight(), o.FilterWidth()],
                              strides=[o.StrideH(), o.StrideW()],
                              **self.pads_attrs(o.Padding()))
                self.names[out] = {"NCHW": self.activation(
                    y, o.FusedActivationFunction())}
            elif name == "LOGISTIC":
                rank = len(self.shape(ins[0]))
                layout = "NCHW" if rank == 4 else "plain"
                y = self.node("Sigmoid", [self.get(ins[0], layout)], "sigmoid")
                self.names[out] = {layout: y}
            else:
                raise ValueError(f"operator {name} isn't handled")

        graph_inputs, graph_outputs = [], []
        for i in self.graph.InputsAsNumpy():
            i = int(i)
            name = self.tensor(i).Name().decode()
            shape = self.shape(i)
            graph_inputs.append(helper.make_tensor_value_info(
                name, TensorProto.FLOAT, shape))
        for i in self.graph.OutputsAsNumpy():
            i = int(i)
            name = self.tensor(i).Name().decode()
            shape = self.shape(i)
            src = self.get(i, "NHWC" if len(shape) == 4 else "plain")
            self.nodes.append(helper.make_node("Identity", [src], [name]))
            graph_outputs.append(helper.make_tensor_value_info(
                name, TensorProto.FLOAT, shape))
        graph = helper.make_graph(self.nodes, "pose", graph_inputs,
                                  graph_outputs, self.initializers)
        model = helper.make_model(graph, opset_imports=[
            helper.make_opsetid("", OPSET)], producer_name="xenia-kinect")
        model.ir_version = 8
        onnx.checker.check_model(model)
        return model


def convert(content):
    c = Converter(content)
    # Name the graph input first, so the ops reading it find it.
    for i in c.graph.InputsAsNumpy():
        i = int(i)
        c.names[i] = {"NHWC": c.tensor(i).Name().decode()}
    return c.convert()


def check(content, model):
    import onnxruntime as ort
    it = Interpreter(model_content=content)
    it.allocate_tensors()
    (inp,) = it.get_input_details()
    rng = np.random.default_rng(1)
    x = rng.uniform(-1, 1, inp["shape"]).astype(np.float32)
    it.set_tensor(inp["index"], x)
    it.invoke()
    sess = ort.InferenceSession(model.SerializeToString(),
                                providers=["CPUExecutionProvider"])
    got = sess.run(None, {inp["name"]: x})
    worst = 0.0
    for d, g in zip(it.get_output_details(), got):
        want = it.get_tensor(d["index"])
        err = float(np.abs(want - g).max() / (np.abs(want).max() + 1e-6))
        print(f"  {d['name']:10s} {list(want.shape)}: worst difference "
              f"{err:.2e} of its range")
        worst = max(worst, err)
    # The segmentation mask's final upsampling rounds a little differently at
    # its edges; everything else agrees to about 1e-5.
    if worst > 5e-3:
        raise SystemExit("the converted model doesn't match the original")


def main():
    task, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with zipfile.ZipFile(task) as bundle:
        for name in ("pose_detector", "pose_landmarks_detector"):
            content = bundle.read(f"{name}.tflite")
            print(f"{name}:")
            model = convert(content)
            check(content, model)
            path = os.path.join(out_dir, f"{name}.onnx")
            onnx.save(model, path)
            print(f"  wrote {path} ({os.path.getsize(path) // 1024} KiB)")


if __name__ == "__main__":
    main()
