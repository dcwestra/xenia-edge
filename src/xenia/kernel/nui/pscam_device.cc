/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/pscam_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/kinect_camera.h"
#include "xenia/kernel/xthread.h"

DECLARE_bool(log_kinect);

DEFINE_bool(kinect_packed_depth, false,
            "Hand Kinect titles depth as the sensor sends it, 11 bits a pixel "
            "packed. Off, it's unpacked to 16 bits, the titles' main format.",
            "Kernel");

DEFINE_path(kinect_record_path, "",
            "Record what the Kinect streams to Kinect titles to this file, for "
            "kinect_playback_path. About 22 MB a second.",
            "Kernel");

DEFINE_bool(kinect_synthetic_depth, false,
            "Without a Kinect, stream empty depth frames to Kinect titles, for "
            "testing.",
            "Kernel");

namespace xe {
namespace kernel {
namespace nui {

namespace {
// The frame buffer request (code 0x05) a title queues.
struct X_PSCAM_FRAME_REQUEST {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown_04[3];
  xe::be<uint32_t> context;             // 0x10, the title's
  xe::be<uint32_t> completion_routine;  // 0x14
  xe::be<uint32_t> stream;              // 0x18
  xe::be<uint32_t> buffer_ptr;          // 0x1C
  xe::be<uint32_t> buffer_size;         // 0x20
  xe::be<uint32_t> unknown_24;
  xe::be<uint32_t> result_ptr;  // 0x28, the request's +0x40
  // For color, the generation of the color settings the frame was taken
  // with (see kColorSettingsGenerationOffset).
  uint8_t color_settings_generation;  // 0x2C
  uint8_t unknown_2d[19];
  // The frame's sequence number and timestamp, which become the skeleton
  // frame's dwFrameNumber and liTimeStamp: a timestamp that never changes
  // leaves everything timed from frame to frame, such as a hand's speed, at
  // nothing.
  xe::be<uint32_t> sequence;  // 0x40
  xe::be<uint32_t> unknown_44;
  xe::be<uint64_t> timestamp_ms;  // 0x48
};
static_assert_size(X_PSCAM_FRAME_REQUEST, 0x50);
// The completion routine a request names, at +0x14 in every request.
constexpr uint32_t kRequestCompletionOffset = 0x14;
// Color settings (0x23) completed get a generation, a byte counter, at +0x18,
// and color frames carry the generation in effect when they were taken. A
// title's NUI runtime applies new settings, such as the window its identity
// runtime pans onto a face, from the first frame of their generation, and
// Kinect Sports (4D5308C9) uses no color until that frame comes.
constexpr uint32_t kRequestSetColorSettings = 0x23;
constexpr uint32_t kColorSettingsGenerationOffset = 0x18;
// The streams frame buffers are queued for.
constexpr uint32_t kStreamColor = 2;
constexpr uint32_t kStreamDepth = 4;
constexpr auto kFrameInterval = std::chrono::microseconds(33333);
constexpr X_STATUS kStatusDeviceNotConnected = X_STATUS(0xC000009D);

// The Kinect's color camera sits beside its depth camera and sees a wider
// field. A title finds the color pixel showing what a depth pixel does with
// the depth-to-color registration in its sensor's calibration, which the
// calibration answered here (see xboxkrnl_usbcam.cc) makes the identity. So
// color is handed over registered to depth, each pixel what the color camera
// saw at the depth pixel in its place: color = kColorScale * (depth - center)
// + center + kColorOffset, as measured on a Kinect for Xbox 360 by matching
// the edges in its depth to the edges in its color, of people about 2 m away.
// Nearer things are a few pixels further over in color, its camera being
// 2.5 cm to the side.
constexpr float kColorScale = 0.915f;
constexpr float kColorOffsetX = -12.5f;
constexpr float kColorOffsetY = 26.5f;

// Makes the sensor's 8-bit Bayer (GRBG) color frame the one a title is
// handed, the same format: registered to depth, with the window on the
// quarter of it around the window's center at twice the size, and with the
// title's gains.
void ViewColorFrame(const uint8_t* in,
                    const PsCamDevice::ColorSettings& settings, uint8_t* out) {
  constexpr int kWidth = int(KinectCamera::kColorWidth);
  constexpr int kHeight = int(KinectCamera::kColorHeight);
  // The two pixels either side of where an output column or row comes from,
  // in one of the mosaic's two phases (a color's pixels are every other
  // one), and how far it is between them.
  struct Tap {
    int32_t i0, i1;
    float weight;
  };
  auto taps = [](int size, bool window, int32_t window_offset, float offset,
                 std::vector<Tap>& phase0, std::vector<Tap>& phase1) {
    phase0.resize(size);
    phase1.resize(size);
    const float center = (size - 1) * 0.5f;
    for (int i = 0; i < size; ++i) {
      // The depth pixel the output pixel stands for, and where the color
      // camera saw it.
      const float depth = window ? size * 0.25f + window_offset * 0.5f +
                                       (i + 0.5f) * 0.5f - 0.5f
                                 : float(i);
      const float color = kColorScale * (depth - center) + center + offset;
      for (int phase = 0; phase < 2; ++phase) {
        const int count = size / 2;
        const float f = (color - phase) * 0.5f;
        int k0 = int(std::floor(f));
        float weight = f - k0;
        if (k0 < 0) {
          k0 = 0;
          weight = 0.0f;
        } else if (k0 >= count - 1) {
          k0 = count - 1;
          weight = 0.0f;
        }
        const int k1 = std::min(k0 + 1, count - 1);
        (phase ? phase1 : phase0)[i] = {phase + 2 * k0, phase + 2 * k1, weight};
      }
    }
  };
  std::vector<Tap> columns[2], rows[2];
  taps(kWidth, settings.window, settings.window_x, kColorOffsetX, columns[0],
       columns[1]);
  taps(kHeight, settings.window, settings.window_y, kColorOffsetY, rows[0],
       rows[1]);
  auto sample = [&](int column_phase, int row_phase, int x, int y) {
    const Tap& column = columns[column_phase][x];
    const Tap& row = rows[row_phase][y];
    const uint8_t* row0 = in + row.i0 * kWidth;
    const uint8_t* row1 = in + row.i1 * kWidth;
    const float top =
        row0[column.i0] + (row0[column.i1] - row0[column.i0]) * column.weight;
    const float bottom =
        row1[column.i0] + (row1[column.i1] - row1[column.i0]) * column.weight;
    return top + (bottom - top) * row.weight;
  };
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      float value;
      if ((x ^ y) & 1) {
        // Red on even rows, blue on odd ones.
        value = y & 1 ? sample(0, 1, x, y) * settings.gains[2]
                      : sample(1, 0, x, y) * settings.gains[0];
      } else {
        value = (sample(0, 0, x, y) + sample(1, 1, x, y)) * 0.5f *
                settings.gains[1];
      }
      out[y * kWidth + x] = uint8_t(std::min(value + 0.5f, 255.0f));
    }
  }
}
}  // namespace

PsCamDevice::PsCamDevice(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

PsCamDevice::~PsCamDevice() { Shutdown(); }

bool PsCamDevice::IsAvailable() {
  std::lock_guard<std::mutex> lock(mutex_);
  OpenCamera();
  return camera_ || synthetic_;
}

void PsCamDevice::OpenCamera() {
  if (camera_opened_) {
    return;
  }
  camera_opened_ = true;
  camera_ = KinectCamera::Open();
  synthetic_ = !camera_ && cvars::kinect_synthetic_depth;
  if (synthetic_) {
    XELOGI("Kinect: no Kinect, streaming empty depth frames instead");
  }
}

X_STATUS PsCamDevice::QueueFrameBuffer(uint32_t request_address) {
  std::lock_guard<std::mutex> lock(mutex_);
  OpenCamera();
  if (!camera_ && !synthetic_) {
    return kStatusDeviceNotConnected;
  }
  if (stopping_) {
    return X_STATUS_UNSUCCESSFUL;
  }
  StartStreaming();
  auto* request =
      kernel_state_->memory()->TranslateVirtual<X_PSCAM_FRAME_REQUEST*>(
          request_address);
  switch (request->stream) {
    case kStreamDepth:
      depth_buffers_.push_back(request_address);
      break;
    case kStreamColor:
      color_buffers_.push_back(request_address);
      break;
    default:
      XELOGW("Kinect: frame buffer for unknown stream {}",
             uint32_t(request->stream));
      return X_STATUS_INVALID_PARAMETER;
  }
  frame_or_buffer_.notify_all();
  return X_STATUS_PENDING;
}

X_STATUS PsCamDevice::QueueAsyncCompletion(uint32_t request_address) {
  const uint32_t completion =
      xe::load_and_swap<uint32_t>(kernel_state_->memory()->TranslateVirtual(
          request_address + kRequestCompletionOffset));
  if (!completion) {
    return X_STATUS_SUCCESS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) {
    return X_STATUS_UNSUCCESSFUL;
  }
  StartWorker();
  async_requests_.push_back(request_address);
  frame_or_buffer_.notify_all();
  return X_STATUS_PENDING;
}

void PsCamDevice::SetColorSettings(const ColorSettings& settings) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cvars::log_kinect && settings.window != color_settings_.window) {
    XELOGI("Kinect: color window {}, at {}, {}", settings.window ? "on" : "off",
           settings.window_x, settings.window_y);
  }
  color_settings_ = settings;
  if (!settings.window) {
    color_settings_.window_x = 0;
    color_settings_.window_y = 0;
  }
}

void PsCamDevice::StartWorker() {
  if (worker_) {
    return;
  }
  worker_running_ = true;
  worker_ = object_ref<XHostThread>(new XHostThread(
      kernel_state_, 64 * 1024, 0,
      [this]() {
        WorkerMain();
        return 0;
      },
      kernel_state_->GetSystemProcess()));
  worker_->set_name("Kinect Camera");
  worker_->Create();
}

void PsCamDevice::StartStreaming() {
  if (streaming_) {
    return;
  }
  streaming_ = true;
  frame_.assign(KinectCamera::kDepthFrameBytes, 0xFF);
  if (camera_ && !cvars::kinect_record_path.empty()) {
    recorder_ = std::make_unique<KinectRecorder>(cvars::kinect_record_path);
  }
  if (camera_ &&
      !camera_->Start([this](const uint8_t* data, size_t size,
                             uint32_t) { OnDepthFrame(data, size); },
                      [this](const uint8_t* data, size_t size, uint32_t) {
                        OnColorFrame(data, size);
                      })) {
    camera_.reset();
    synthetic_ = cvars::kinect_synthetic_depth;
  }
  real_color_ = camera_ && camera_->IsColorStreaming();
  if (camera_) {
    if (KinectMotor* motor = kernel_state_->kinect_motor()) {
      motor->SetLed(KinectMotor::Led::kGreen);
    }
  }
  StartWorker();
}

void PsCamDevice::OnDepthFrame(const uint8_t* data, size_t size) {
  if (recorder_) {
    recorder_->AddFrame(false, data, size);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::memcpy(frame_.data(), data, std::min(size, frame_.size()));
  frame_ready_ = true;
  frame_time_ms_ = FrameTimeMs();
  frame_or_buffer_.notify_all();
  // A frame from the sensor every 10 seconds, to check what it sees.
  if (cvars::log_kinect && !(++frames_received_ % 300)) {
    const std::filesystem::path path =
        kernel_state_->emulator()->cache_root() /
        fmt::format("kinect_depth_frame_{:05}.raw", frames_received_);
    if (FILE* file = xe::filesystem::OpenFile(path, "wb")) {
      fwrite(frame_.data(), 1, frame_.size(), file);
      fclose(file);
      XELOGI("Kinect: saved a raw depth frame to {}",
             xe::strip_pii(xe::path_to_utf8(path)));
    }
  }
}

void PsCamDevice::OnColorFrame(const uint8_t* data, size_t size) {
  if (recorder_) {
    recorder_->AddFrame(true, data, size);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  color_frame_.assign(data, data + size);
  color_frame_ready_ = true;
  color_frame_time_ms_ = FrameTimeMs();
  frame_or_buffer_.notify_all();
  if (cvars::log_kinect && !(++color_frames_received_ % 300)) {
    const std::filesystem::path path =
        kernel_state_->emulator()->cache_root() /
        fmt::format("kinect_color_frame_{:05}.raw", color_frames_received_);
    if (FILE* file = xe::filesystem::OpenFile(path, "wb")) {
      fwrite(color_frame_.data(), 1, color_frame_.size(), file);
      fclose(file);
      XELOGI("Kinect: saved a raw color frame to {}",
             xe::strip_pii(xe::path_to_utf8(path)));
    }
  }
}

uint64_t PsCamDevice::FrameTimeMs() {
  // The guest's uptime when the sensor delivered the frame, never 0.
  return std::max<uint64_t>(Clock::QueryGuestUptimeMillis(), 1);
}

void PsCamDevice::WorkerMain() {
  using clock = std::chrono::steady_clock;
  auto next_synthetic_depth = clock::now();
  // Without the sensor's color, a gray picture at its rate.
  auto next_color = clock::now();
  std::vector<uint8_t> depth;
  std::vector<uint8_t> color;
  std::vector<uint8_t> sensor_color;
  while (true) {
    uint32_t depth_request = 0;
    uint32_t depth_number = 0;
    uint64_t depth_time_ms = 0;
    uint32_t color_request = 0;
    uint32_t color_number = 0;
    uint64_t color_time_ms = 0;
    bool view_color = false;
    ColorSettings color_settings;
    std::deque<uint32_t> async_requests;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      frame_or_buffer_.wait_for(lock, std::chrono::milliseconds(5), [&]() {
        return stopping_ || !async_requests_.empty() ||
               (frame_ready_ && !depth_buffers_.empty()) ||
               (color_frame_ready_ && !color_buffers_.empty());
      });
      if (stopping_) {
        break;
      }
      async_requests.swap(async_requests_);
      const auto now = clock::now();
      if (synthetic_ && now >= next_synthetic_depth) {
        // An empty room: every pixel reads as no depth (all 11 bits set).
        next_synthetic_depth += kFrameInterval;
        frame_ready_ = true;
        frame_time_ms_ = FrameTimeMs();
      }
      if (frame_ready_ && !depth_buffers_.empty()) {
        depth_request = depth_buffers_.front();
        depth_buffers_.pop_front();
        depth_number = ++depth_frame_number_;
        depth_time_ms = frame_time_ms_;
        frame_ready_ = false;
        if (cvars::kinect_packed_depth) {
          depth = frame_;
        } else {
          // Big-endian 16-bit values, from the sensor's big-endian 11-bit
          // bitstream.
          depth.resize(KinectCamera::kDepthWidth * KinectCamera::kDepthHeight *
                       2);
          uint32_t bits = 0;
          uint32_t bit_count = 0;
          size_t in = 0;
          for (size_t out = 0; out < depth.size(); out += 2) {
            while (bit_count < 11) {
              bits = (bits << 8) | frame_[in++];
              bit_count += 8;
            }
            bit_count -= 11;
            const uint16_t value = uint16_t((bits >> bit_count) & 0x7FF);
            depth[out] = uint8_t(value >> 8);
            depth[out + 1] = uint8_t(value);
          }
        }
      }
      if (!color_buffers_.empty() &&
          (real_color_ ? color_frame_ready_ : now >= next_color)) {
        color_request = color_buffers_.front();
        color_buffers_.pop_front();
        color_number = ++color_frame_number_;
        auto* request =
            kernel_state_->memory()->TranslateVirtual<X_PSCAM_FRAME_REQUEST*>(
                color_request);
        if (real_color_) {
          color_frame_ready_ = false;
          sensor_color = color_frame_;
          color_time_ms = color_frame_time_ms_;
          view_color = sensor_color.size() == KinectCamera::kColorFrameBytes;
          if (!view_color) {
            color = sensor_color;
          }
          color_settings = color_settings_;
        } else {
          next_color =
              std::max(next_color + kFrameInterval, now - kFrameInterval);
          color.assign(request->buffer_size, 0x80);
          color_time_ms = FrameTimeMs();
        }
      }
    }
    for (uint32_t async_request : async_requests) {
      auto* words = kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          async_request);
      if (words[0] == kRequestSetColorSettings) {
        words[kColorSettingsGenerationOffset / 4] =
            ++color_settings_generation_;
      }
      uint64_t args[] = {async_request, uint64_t(X_STATUS_SUCCESS), 0};
      kernel_state_->processor()->Execute(
          worker_->thread_state(),
          xe::load_and_swap<uint32_t>(kernel_state_->memory()->TranslateVirtual(
              async_request + kRequestCompletionOffset)),
          args, xe::countof(args));
    }
    if (depth_request) {
      CompleteFrameBuffer(depth_request, depth.data(), uint32_t(depth.size()),
                          depth_number, depth_time_ms);
    }
    if (color_request) {
      if (view_color) {
        color.resize(sensor_color.size());
        ViewColorFrame(sensor_color.data(), color_settings, color.data());
        // A frame as handed over every 10 seconds, beside the sensor's.
        if (cvars::log_kinect && !(color_number % 300)) {
          const std::filesystem::path path =
              kernel_state_->emulator()->cache_root() /
              fmt::format("kinect_color_view_{:05}{}.raw", color_number,
                          color_settings.window ? "_window" : "");
          if (FILE* file = xe::filesystem::OpenFile(path, "wb")) {
            fwrite(color.data(), 1, color.size(), file);
            fclose(file);
          }
        }
      }
      kernel_state_->memory()
          ->TranslateVirtual<X_PSCAM_FRAME_REQUEST*>(color_request)
          ->color_settings_generation = color_settings_generation_;
      CompleteFrameBuffer(color_request, color.data(), uint32_t(color.size()),
                          color_number, color_time_ms);
    }
  }
  worker_running_ = false;
}

void PsCamDevice::CompleteFrameBuffer(uint32_t request_address,
                                      const uint8_t* data, uint32_t size,
                                      uint32_t frame_number,
                                      uint64_t timestamp_ms) {
  auto* request =
      kernel_state_->memory()->TranslateVirtual<X_PSCAM_FRAME_REQUEST*>(
          request_address);
  const bool filled = request->buffer_ptr && request->buffer_size >= size;
  if (filled) {
    std::memcpy(kernel_state_->memory()->TranslateVirtual(request->buffer_ptr),
                data, size);
  }
  request->sequence = frame_number;
  request->timestamp_ms = timestamp_ms;
  const uint32_t routine = request->completion_routine;
  if (!routine) {
    return;
  }
  uint64_t args[] = {
      request_address,
      filled ? uint64_t(X_STATUS_SUCCESS) : uint64_t(X_STATUS_BUFFER_TOO_SMALL),
      filled ? uint64_t(size) : 0};
  kernel_state_->processor()->Execute(worker_->thread_state(), routine, args,
                                      xe::countof(args));
}

void PsCamDevice::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    frame_or_buffer_.notify_all();
  }
  for (int i = 0; i < 200 && worker_running_; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (camera_) {
    camera_->Stop();
    camera_.reset();
    if (KinectMotor* motor = kernel_state_->kinect_motor()) {
      motor->SetLed(KinectMotor::Led::kBlinkGreen);
    }
  }
  recorder_.reset();
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
