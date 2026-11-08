/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/onnx_model.h"

#include <mutex>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_LINUX
#include <dlfcn.h>
#endif

#include "third_party/onnxruntime/onnxruntime_c_api.h"

namespace xe {
namespace kernel {
namespace nui {

namespace {

// The oldest API version with everything used here, so that older ONNX
// Runtimes than the header's work too.
constexpr uint32_t kOrtApiVersion = 14;

struct Runtime {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtMemoryInfo* cpu_memory = nullptr;
};

Runtime runtime;
std::mutex runtime_mutex;
bool runtime_tried = false;

// Logs and frees a failed call's status: true if it failed.
bool Failed(OrtStatus* status, const char* what) {
  if (!status) {
    return false;
  }
  XELOGW("Kinect: ONNX Runtime couldn't {}: {}", what,
         runtime.api->GetErrorMessage(status));
  runtime.api->ReleaseStatus(status);
  return true;
}

const OrtApi* LoadRuntime(const std::filesystem::path& library_path,
                          const std::filesystem::path& model_dir) {
  std::lock_guard<std::mutex> lock(runtime_mutex);
  if (runtime_tried) {
    return runtime.api;
  }
  runtime_tried = true;
#if XE_PLATFORM_LINUX
  std::vector<std::string> candidates;
  if (!library_path.empty()) {
    candidates.push_back(xe::path_to_utf8(library_path));
  }
  candidates.push_back(xe::path_to_utf8(model_dir / "libonnxruntime.so.1"));
  candidates.push_back("libonnxruntime.so.1");
  candidates.push_back("libonnxruntime.so");
  void* library = nullptr;
  for (const std::string& candidate : candidates) {
    library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library) {
      XELOGI("Kinect: loaded ONNX Runtime from {}", xe::strip_pii(candidate));
      break;
    }
  }
  if (!library) {
    XELOGW(
        "Kinect: ONNX Runtime (libonnxruntime.so.1) isn't installed, so a "
        "webcam can't stand in for a Kinect");
    return nullptr;
  }
  auto get_api_base = reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(
      dlsym(library, "OrtGetApiBase"));
  const OrtApiBase* base = get_api_base ? get_api_base() : nullptr;
  const OrtApi* api = base ? base->GetApi(kOrtApiVersion) : nullptr;
  if (!api) {
    XELOGW("Kinect: the ONNX Runtime found is too old (version {} needed)",
           kOrtApiVersion);
    return nullptr;
  }
  runtime.api = api;
  if (Failed(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "xenia-kinect",
                            &runtime.env),
             "start") ||
      Failed(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                      &runtime.cpu_memory),
             "describe CPU memory")) {
    runtime.api = nullptr;
    return nullptr;
  }
  Failed(api->DisableTelemetryEvents(runtime.env), "turn telemetry off");
  return api;
#else
  XELOGW("Kinect: ONNX Runtime isn't supported on this platform yet");
  return nullptr;
#endif
}

}  // namespace

struct OnnxModel::Impl {
  OrtSession* session = nullptr;
  ~Impl() {
    if (session) {
      runtime.api->ReleaseSession(session);
    }
  }
};

OnnxModel::OnnxModel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
OnnxModel::~OnnxModel() = default;

std::unique_ptr<OnnxModel> OnnxModel::Load(
    const std::filesystem::path& model_path,
    const std::filesystem::path& library_path, int threads) {
  const OrtApi* api = LoadRuntime(library_path, model_path.parent_path());
  if (!api) {
    return nullptr;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(model_path, error)) {
    XELOGW("Kinect: no model at {}",
           xe::strip_pii(xe::path_to_utf8(model_path)));
    return nullptr;
  }
  OrtSessionOptions* options = nullptr;
  if (Failed(api->CreateSessionOptions(&options), "set up a session")) {
    return nullptr;
  }
  if (Failed(api->SetIntraOpNumThreads(options, threads), "set threads") ||
      Failed(api->SetInterOpNumThreads(options, 1), "set threads") ||
      Failed(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL),
             "set optimization")) {
    api->ReleaseSessionOptions(options);
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  const std::string path = xe::path_to_utf8(model_path);
  const bool failed = Failed(
      api->CreateSession(runtime.env, path.c_str(), options, &impl->session),
      "load a model");
  api->ReleaseSessionOptions(options);
  if (failed) {
    impl->session = nullptr;
    return nullptr;
  }
  return std::unique_ptr<OnnxModel>(new OnnxModel(std::move(impl)));
}

bool OnnxModel::Run(const char* input_name, const float* input,
                    const std::vector<int64_t>& shape,
                    const std::vector<const char*>& output_names,
                    std::vector<std::vector<float>>& outputs) {
  const OrtApi* api = runtime.api;
  size_t count = 1;
  for (int64_t dimension : shape) {
    count *= size_t(dimension);
  }
  OrtValue* input_value = nullptr;
  if (Failed(api->CreateTensorWithDataAsOrtValue(
                 runtime.cpu_memory, const_cast<float*>(input),
                 count * sizeof(float), shape.data(), shape.size(),
                 ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_value),
             "take an input")) {
    return false;
  }
  std::vector<OrtValue*> output_values(output_names.size(), nullptr);
  const bool failed = Failed(
      api->Run(impl_->session, nullptr, &input_name, &input_value, 1,
               output_names.data(), output_names.size(), output_values.data()),
      "run a model");
  api->ReleaseValue(input_value);
  outputs.resize(output_names.size());
  bool ok = !failed;
  for (size_t i = 0; i < output_values.size(); ++i) {
    OrtValue* value = output_values[i];
    if (!value) {
      ok = false;
      continue;
    }
    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t elements = 0;
    float* data = nullptr;
    if (!Failed(api->GetTensorTypeAndShape(value, &info), "read an output")) {
      Failed(api->GetTensorShapeElementCount(info, &elements),
             "read an output's size");
      api->ReleaseTensorTypeAndShapeInfo(info);
    }
    if (!Failed(
            api->GetTensorMutableData(value, reinterpret_cast<void**>(&data)),
            "read an output") &&
        data) {
      outputs[i].assign(data, data + elements);
    } else {
      ok = false;
    }
    api->ReleaseValue(value);
  }
  return ok;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
