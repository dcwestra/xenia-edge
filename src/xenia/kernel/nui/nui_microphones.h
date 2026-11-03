/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_MICROPHONES_H_
#define XENIA_KERNEL_NUI_NUI_MICROPHONES_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
class XHostThread;
namespace nui {

class KinectMicrophones;
struct MicrophoneWindow;

// The Kinect's microphone array as a title's NUI runtime reads it, after
// KinectAudio in Davi Carlos's kixenia (https://github.com/davicarlos/kixenia).
// The title asks for 16 ms windows with XamVoiceGetMicArrayAudioEx, and does
// its own echo cancellation in a (MEC) client the console's audio render driver
// calls every 256-sample frame of 5.33 ms, a tic; a window spans 3 tics and
// carries the tic it ends at. A host thread keeps the tic, completes a window
// request every 3 tics with the next window recorded (silence if none has come)
// and calls the MEC client.
class NuiMicrophones {
 public:
  explicit NuiMicrophones(KernelState* kernel_state);
  ~NuiMicrophones();

  // False with kinect_microphone off: titles then see no microphone array.
  // Otherwise there is one, silent if nothing can be recorded from.
  bool available() const;

  // XamVoiceGetMicArrayAudioEx: queues a window request, completed by
  // setting the X_KEVENT at event_ptr.
  X_STATUS ReadWindow(uint32_t request_ptr, uint32_t event_ptr);

  // XAudioRegisterRenderDriverMECClient and the rest.
  X_STATUS RegisterMecClient(uint32_t callback, uint32_t callback_arg,
                             uint32_t* handle);
  void UnregisterMecClient(uint32_t handle);
  uint16_t render_tic() const { return render_tic_; }

  // Stops the thread, which runs title code, before the title's fibers go.
  void Shutdown();

 private:
  struct Request {
    uint32_t request_ptr;
    uint32_t event_ptr;
  };

  void OpenSource();
  void StartThread();
  void ThreadMain();
  void Tick();
  void CompleteRequest(const Request& request, uint16_t tic);
  void LogLevels(const MicrophoneWindow& window, bool have_window);

  KernelState* kernel_state_;

  std::mutex mutex_;
  // Opens the source, off the title's thread.
  std::thread opener_;
  std::unique_ptr<KinectMicrophones> source_;
  bool reading_ = false;
  std::deque<Request> requests_;
  uint32_t mec_callback_ = 0;
  uint32_t mec_callback_arg_ = 0;
  bool stopping_ = false;

  // On the thread only.
  uint32_t log_requests_ = 0;
  uint32_t log_windows_ = 0;
  int32_t log_peak_ = 0;

  std::atomic<uint16_t> render_tic_ = 0;
  std::atomic<bool> thread_running_ = false;
  object_ref<XHostThread> thread_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_MICROPHONES_H_
