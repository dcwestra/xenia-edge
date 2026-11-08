The C API header of [ONNX Runtime](https://github.com/microsoft/onnxruntime)
1.22.0 (MIT, see LICENSE), for the webcam Kinect's pose model. Xenia loads the
ONNX Runtime library at run time (src/xenia/kernel/nui/onnx_model.cc) and asks
it for an older API version, so any ONNX Runtime from 1.14 on works.
