/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_KINECT_CAMERA_H_
#define XENIA_KERNEL_NUI_KINECT_CAMERA_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>

namespace xe {
namespace kernel {
namespace nui {

// The distance a Kinect's raw 11-bit depth value stands for, 0 for no reading.
// An approximation of the sensor's curve, good to a couple of percent, rather
// than one from the sensor's own calibration.
uint16_t KinectRawDepthToMillimeters(uint32_t raw);

// A Kinect for Xbox 360 (model 1414) on the host, read over USB with libusb,
// which is loaded when first needed so it isn't a dependency otherwise.
// Streams in the sensor's own formats, which is what the console's camera
// driver hands titles: depth at 640x480, 11 bits per pixel packed big-endian,
// and color at 640x480, 8-bit Bayer.
class KinectCamera {
 public:
  static constexpr uint32_t kDepthWidth = 640;
  static constexpr uint32_t kDepthHeight = 480;
  static constexpr uint32_t kDepthFrameBytes =
      kDepthWidth * kDepthHeight * 11 / 8;
  static constexpr uint32_t kColorWidth = 640;
  static constexpr uint32_t kColorHeight = 480;
  static constexpr uint32_t kColorFrameBytes = kColorWidth * kColorHeight;

  // Called on the USB event thread with each complete frame and the sensor's
  // timestamp for it.
  using FrameCallback =
      std::function<void(const uint8_t* data, size_t size, uint32_t timestamp)>;

  // Null if there is no Kinect, no libusb, or no permission to open it.
  static std::unique_ptr<KinectCamera> Open();
  virtual ~KinectCamera() = default;

  // Starts depth, and color too if color_callback isn't null - without
  // failing if only color can't be started.
  virtual bool Start(FrameCallback depth_callback,
                     FrameCallback color_callback) = 0;
  virtual bool IsColorStreaming() const = 0;
  virtual void Stop() = 0;
};

// The Kinect's motor device: its tilt motor, which tilts the sensor to an
// angle relative to gravity it measures with its accelerometer, and its LED.
class KinectMotor {
 public:
  static constexpr int kMaxTiltDegrees = 27;
  // About this many accelerometer counts make 1 g.
  static constexpr float kCountsPerG = 819.0f;

  enum class Led : uint16_t {
    kOff = 0,
    kGreen = 1,
    kRed = 2,
    kYellow = 3,
    // What the Kinect shows when nothing on the host drives it.
    kBlinkGreen = 4,
    kBlinkRedYellow = 6,
  };
  enum class TiltStatus : uint8_t {
    kStopped = 0,
    kAtLimit = 1,
    kMoving = 4,
  };
  struct State {
    // x, y and z: with the sensor level, y is about kCountsPerG and z grows as
    // it tilts up.
    int16_t accel[3];
    int8_t tilt_half_degrees;
    TiltStatus status;
  };

  // Null if there is no Kinect motor, no libusb, or no permission to open it.
  // With a recording playing (kinect_playback_path), a simulated one.
  static std::unique_ptr<KinectMotor> Open();
  virtual ~KinectMotor() = default;

  virtual bool SetLed(Led led) = 0;
  // Starts tilting to the angle, clamped to kMaxTiltDegrees either way.
  virtual bool SetTilt(int degrees) = 0;
  virtual bool GetState(State& state) = 0;
};

// Writes the frames a Kinect streams to a file, for playing back later with
// kinect_playback_path - to test titles without anyone in front of the sensor.
class KinectRecorder {
 public:
  explicit KinectRecorder(const std::filesystem::path& path);
  ~KinectRecorder();
  void AddFrame(bool color, const uint8_t* data, size_t size);

 private:
  std::mutex mutex_;
  FILE* file_ = nullptr;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_KINECT_CAMERA_H_
