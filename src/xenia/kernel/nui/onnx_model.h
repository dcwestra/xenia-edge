/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_ONNX_MODEL_H_
#define XENIA_KERNEL_NUI_ONNX_MODEL_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace xe {
namespace kernel {
namespace nui {

// A neural network model run with ONNX Runtime, which is loaded when first
// needed so it isn't a dependency otherwise: from library_path, or a copy
// beside the model, or the system's.
class OnnxModel {
 public:
  // Null, having logged why, if ONNX Runtime or the model can't be loaded.
  static std::unique_ptr<OnnxModel> Load(
      const std::filesystem::path& model_path,
      const std::filesystem::path& library_path, int threads);
  ~OnnxModel();

  // Runs the model on its one float input, of the given shape, filling each
  // named float output.
  bool Run(const char* input_name, const float* input,
           const std::vector<int64_t>& shape,
           const std::vector<const char*>& output_names,
           std::vector<std::vector<float>>& outputs);

 private:
  struct Impl;
  explicit OnnxModel(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_ONNX_MODEL_H_
