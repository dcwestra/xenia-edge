/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_microphones.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/kinect_microphones.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DECLARE_bool(log_kinect);
DECLARE_string(kinect_microphone);

namespace xe {
namespace kernel {
namespace nui {

namespace {

// A window request, big-endian (xam 0x816FBCF8, from kixenia's notes): its
// size at +0, the buffers for the processed mix (16-bit) and the four
// microphones (32-bit), their sizes in bytes, and only when the size says the
// request has room for them (xam checks > 0x40 and > 0x64) the tic the window
// was captured at and the time. Sonic Free Riders keeps two 0x44-byte
// requests back to back, so the time would land on the next.
constexpr uint32_t kRequestSize = 0x00;
constexpr uint32_t kRequestBuffers = 0x04;
constexpr uint32_t kRequestSizes = 0x18;
constexpr uint32_t kRequestTic = 0x40;
constexpr uint32_t kRequestTimeHigh = 0x44;
constexpr uint32_t kRequestTimeLow = 0x48;
constexpr uint32_t kRequestBufferCount = 1 + MicrophoneWindow::kMicrophones;
constexpr uint32_t kRequestSizeWithTic = 0x40;
constexpr uint32_t kRequestSizeWithTime = 0x64;

constexpr uint32_t kTicsPerWindow = 3;
constexpr auto kTicPeriod = std::chrono::nanoseconds(5333333);
constexpr uint32_t kMecClientHandle = 0x4D454301;

}  // namespace

NuiMicrophones::NuiMicrophones(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

NuiMicrophones::~NuiMicrophones() { Shutdown(); }

bool NuiMicrophones::available() const {
  return cvars::kinect_microphone != "off";
}

// On opener_: opening the Kinect's audio device can take seconds, while its
// firmware is uploaded and it comes back. Requests get silence meanwhile.
void NuiMicrophones::OpenSource() {
  Emulator* emulator = kernel_state_->emulator();
  std::unique_ptr<KinectMicrophones> source = KinectMicrophones::Open(
      emulator->audio_system(), emulator->content_root());
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stopping_) {
    source_ = std::move(source);
  }
}

X_STATUS NuiMicrophones::ReadWindow(uint32_t request_ptr, uint32_t event_ptr) {
  if (!available()) {
    return X_STATUS_DEVICE_NOT_CONNECTED;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return X_STATUS_DEVICE_NOT_CONNECTED;
    }
    if (!reading_) {
      reading_ = true;
      XELOGI("Kinect: the title is reading the microphones");
      opener_ = std::thread([this]() { OpenSource(); });
    }
    requests_.push_back({request_ptr, event_ptr});
  }
  StartThread();
  return X_STATUS_SUCCESS;
}

X_STATUS NuiMicrophones::RegisterMecClient(uint32_t callback,
                                           uint32_t callback_arg,
                                           uint32_t* handle) {
  if (!available()) {
    return X_STATUS_DEVICE_NOT_CONNECTED;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return X_STATUS_DEVICE_NOT_CONNECTED;
    }
    mec_callback_ = callback;
    mec_callback_arg_ = callback_arg;
  }
  XELOGI("Kinect: echo cancellation client {:08X}", callback);
  *handle = kMecClientHandle;
  StartThread();
  return X_STATUS_SUCCESS;
}

void NuiMicrophones::UnregisterMecClient(uint32_t handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  mec_callback_ = 0;
  mec_callback_arg_ = 0;
}

void NuiMicrophones::StartThread() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_ || stopping_) {
    return;
  }
  thread_running_ = true;
  thread_ = object_ref<XHostThread>(new XHostThread(
      kernel_state_, 64 * 1024, 0,
      [this]() {
        ThreadMain();
        return 0;
      },
      kernel_state_->GetSystemProcess()));
  thread_->set_name("Kinect Microphones");
  thread_->Create();
}

void NuiMicrophones::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  for (int i = 0; i < 200 && thread_running_; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (opener_.joinable()) {
    opener_.join();
  }
  std::unique_ptr<KinectMicrophones> source;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    source = std::move(source_);
  }
}

// The tic on a fixed schedule of 5.33 ms, as the console's audio render
// driver keeps it.
void NuiMicrophones::ThreadMain() {
  auto deadline = std::chrono::steady_clock::now();
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        break;
      }
    }
    deadline += kTicPeriod;
    const auto now = std::chrono::steady_clock::now();
    if (deadline > now) {
      xe::threading::NanoSleepPrecise(
          std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
              .count());
    } else if (now - deadline > kTicPeriod * 8) {
      // Far behind (stopped in a debugger): don't race to catch up.
      deadline = now;
    }
    Tick();
  }
  thread_running_ = false;
}

// Every third tic completes the oldest window request, then the echo
// cancellation client runs, on this thread as on the console's driver thread.
void NuiMicrophones::Tick() {
  const uint16_t tic = uint16_t(render_tic_ + 1);
  render_tic_ = tic;
  uint32_t callback = 0;
  uint32_t callback_arg = 0;
  Request request = {};
  bool complete = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tic % kTicsPerWindow == 0 && !requests_.empty()) {
      request = requests_.front();
      requests_.pop_front();
      complete = true;
    }
    callback = mec_callback_;
    callback_arg = mec_callback_arg_;
  }
  if (complete) {
    CompleteRequest(request, tic);
  }
  if (callback) {
    uint64_t args[] = {callback_arg};
    kernel_state_->processor()->Execute(thread_->thread_state(), callback, args,
                                        xe::countof(args));
  }
}

// Every 10 seconds, how many windows came and the loudest sample of the
// processed mix, to see what the title hears.
void NuiMicrophones::LogLevels(const MicrophoneWindow& window,
                               bool have_window) {
  constexpr uint32_t kWindowsPerLog = 625;
  for (int16_t sample : window.processed) {
    log_peak_ = std::max(log_peak_, std::abs(int32_t(sample)));
  }
  log_windows_ += have_window;
  if (++log_requests_ < kWindowsPerLog) {
    return;
  }
  XELOGI("Kinect: microphones sent {} of {} windows, peak {:.1f} dBFS",
         log_windows_, log_requests_,
         log_peak_ ? 20.0 * std::log10(log_peak_ / 32768.0) : -INFINITY);
  log_requests_ = 0;
  log_windows_ = 0;
  log_peak_ = 0;
}

void NuiMicrophones::CompleteRequest(const Request& request, uint16_t tic) {
  Memory* memory = kernel_state_->memory();
  uint8_t* fields = memory->TranslateVirtual<uint8_t*>(request.request_ptr);
  MicrophoneWindow window;
  bool have_window = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_) {
      have_window = source_->ReadWindow(window);
    }
  }
  // A request is due every third tic whether a window has come or not;
  // without one it gets silence.
  if (!have_window) {
    std::memset(&window, 0, sizeof(window));
  }
  if (cvars::log_kinect) {
    LogLevels(window, have_window);
  }
  for (uint32_t buffer_index = 0; buffer_index < kRequestBufferCount;
       ++buffer_index) {
    const uint32_t buffer = xe::load_and_swap<uint32_t>(
        fields + kRequestBuffers + buffer_index * 4);
    const uint32_t size =
        xe::load_and_swap<uint32_t>(fields + kRequestSizes + buffer_index * 4);
    if (!buffer) {
      continue;
    }
    uint8_t* dest = memory->TranslateVirtual<uint8_t*>(buffer);
    if (buffer_index == 0) {
      const uint32_t count = std::min<uint32_t>(size / sizeof(int16_t),
                                                MicrophoneWindow::kSamples);
      for (uint32_t i = 0; i < count; ++i) {
        xe::store_and_swap<int16_t>(dest + i * sizeof(int16_t),
                                    window.processed[i]);
      }
    } else {
      const int32_t* samples = window.microphones[buffer_index - 1];
      const uint32_t count = std::min<uint32_t>(size / sizeof(int32_t),
                                                MicrophoneWindow::kSamples);
      for (uint32_t i = 0; i < count; ++i) {
        xe::store_and_swap<int32_t>(dest + i * sizeof(int32_t), samples[i]);
      }
    }
  }
  const uint32_t request_size =
      xe::load_and_swap<uint32_t>(fields + kRequestSize);
  if (request_size > kRequestSizeWithTic) {
    xe::store_and_swap<uint16_t>(fields + kRequestTic, tic);
  }
  if (request_size > kRequestSizeWithTime) {
    const uint64_t time = Clock::QueryGuestSystemTime();
    xe::store_and_swap<uint32_t>(fields + kRequestTimeHigh,
                                 uint32_t(time >> 32));
    xe::store_and_swap<uint32_t>(fields + kRequestTimeLow, uint32_t(time));
  }
  if (request.event_ptr) {
    xboxkrnl::xeKeSetEvent(
        memory->TranslateVirtual<X_KEVENT*>(request.event_ptr), 1, 0);
  }
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
