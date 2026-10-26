/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_PSCAM_DEVICE_H_
#define XENIA_KERNEL_NUI_PSCAM_DEVICE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class KernelState;
class XHostThread;

namespace nui {

class KinectCamera;
class KinectRecorder;

// The console's Kinect camera driver as titles see it through
// PsCamDeviceRequest: frame buffers a title's NUI runtime queues are filled
// with depth from the host's Kinect and handed back through the completion
// routine each one names.
class PsCamDevice {
 public:
  explicit PsCamDevice(KernelState* kernel_state);
  ~PsCamDevice();

  // Whether there's a sensor to stream from, opening the host's Kinect the
  // first time.
  bool IsAvailable();
  // Whether frames are being streamed, from the first queued frame buffer.
  bool IsStreaming() {
    std::lock_guard<std::mutex> lock(mutex_);
    return streaming_;
  }

  // Queues the frame buffer described by the request (code 0x05) at
  // request_address. Its completion routine runs once a frame fills it.
  X_STATUS QueueFrameBuffer(uint32_t request_address);
  // For a request completed later (0x0F, 0x23): its completion routine is
  // called on the camera's thread. X_STATUS_PENDING, or X_STATUS_SUCCESS for
  // one without a routine.
  X_STATUS QueueAsyncCompletion(uint32_t request_address);
  // What a title sets of the color stream (0x23).
  struct ColorSettings {
    // With the window on, color is the 640x480 of the sensor's full
    // 1280x960 picture centered window_x, window_y full-resolution pixels
    // from its center, rather than all of it at half the resolution. A
    // title's identity runtime keeps it on a player's face.
    bool window = false;
    int32_t window_x = 0;
    int32_t window_y = 0;
    // The red, green and blue gains, which the identity runtime's exposure
    // control raises until the face is bright enough. The sensor exposes
    // itself, so they're applied on top of its own exposure.
    float gains[3] = {1.0f, 1.0f, 1.0f};
  };
  void SetColorSettings(const ColorSettings& settings);

  void Shutdown();

 private:
  void OpenCamera();
  void StartWorker();
  void StartStreaming();
  static uint64_t FrameTimeMs();
  void OnDepthFrame(const uint8_t* data, size_t size);
  void OnColorFrame(const uint8_t* data, size_t size);
  void WorkerMain();
  // Fills the request's buffer from data, sets its sequence number and
  // timestamp and calls its completion routine. Without the lock held.
  void CompleteFrameBuffer(uint32_t request_address, const uint8_t* data,
                           uint32_t size, uint32_t frame_number,
                           uint64_t timestamp_ms);

  KernelState* kernel_state_;
  std::mutex mutex_;
  std::condition_variable frame_or_buffer_;
  bool camera_opened_ = false;
  std::unique_ptr<KinectCamera> camera_;
  std::unique_ptr<KinectRecorder> recorder_;
  bool synthetic_ = false;
  bool streaming_ = false;
  bool stopping_ = false;
  // Frame buffers waiting, per stream the request names.
  std::deque<uint32_t> depth_buffers_;
  std::deque<uint32_t> color_buffers_;
  std::deque<uint32_t> async_requests_;
  std::vector<uint8_t> frame_;
  bool frame_ready_ = false;
  uint64_t frame_time_ms_ = 0;
  uint64_t color_frame_time_ms_ = 0;
  // Whether color comes from the sensor rather than being a placeholder.
  bool real_color_ = false;
  std::vector<uint8_t> color_frame_;
  bool color_frame_ready_ = false;
  ColorSettings color_settings_;
  uint32_t color_frames_received_ = 0;
  uint32_t depth_frame_number_ = 0;
  uint32_t color_frame_number_ = 0;
  uint32_t frames_received_ = 0;
  std::atomic<bool> worker_running_ = false;
  object_ref<XHostThread> worker_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_PSCAM_DEVICE_H_
