/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_KINECT_MICROPHONES_H_
#define XENIA_KERNEL_NUI_KINECT_MICROPHONES_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace xe {
namespace apu {
class AudioSystem;
}  // namespace apu
namespace kernel {
namespace nui {

// 16 ms of the Kinect's microphone array at 16 kHz, as the console's audio
// driver hands it to titles: the array's processed mix, and each of its four
// microphones.
struct MicrophoneWindow {
  static constexpr uint32_t kSamples = 256;
  static constexpr uint32_t kMicrophones = 4;
  int16_t processed[kSamples];
  int32_t microphones[kMicrophones][kSamples];
};

// Windows between whoever records them and the title reading them, which reads
// one every 16 ms on its own clock: a couple are held back first so that
// arrival jitter doesn't leave the title without one, and the oldest are
// dropped once the title falls about 100 ms behind.
class MicrophoneWindowQueue {
 public:
  void Push(const MicrophoneWindow& window);
  bool Pop(MicrophoneWindow& window);

 private:
  static constexpr uint64_t kCapacity = 16;
  static constexpr uint64_t kPrime = 2;
  static constexpr uint64_t kMaxBacklog = 6;
  static_assert(kMaxBacklog < kCapacity);
  std::mutex mutex_;
  MicrophoneWindow windows_[kCapacity];
  uint64_t written_ = 0;
  uint64_t read_ = 0;
  bool primed_ = false;
};

// Where the microphone windows come from: the Kinect's own microphone array,
// or a host recording device standing in for it (kinect_microphone).
class KinectMicrophones {
 public:
  static constexpr uint32_t kSampleRate = 16000;

  // Null if there's nothing to record from. The Kinect's audio device needs
  // its firmware, which comes from kinect_audio_firmware_path or the system
  // package FFFE07DF00000001 installed under `content_root`.
  static std::unique_ptr<KinectMicrophones> Open(
      apu::AudioSystem* audio_system,
      const std::filesystem::path& content_root);
  virtual ~KinectMicrophones() = default;

  virtual std::string name() const = 0;
  // The next window; false if none is ready.
  virtual bool ReadWindow(MicrophoneWindow& window) = 0;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_KINECT_MICROPHONES_H_
