/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_POSE_ESTIMATOR_H_
#define XENIA_KERNEL_NUI_POSE_ESTIMATOR_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace xe {
namespace kernel {
namespace nui {

class OnnxModel;

// A person's pose as MediaPipe's pose landmarker (BlazePose GHUM 3D) gives
// it: 33 landmarks, in the picture and in meters around the hips.
struct PoseLandmarks {
  static constexpr int kCount = 33;
  enum Index {
    kNose = 0,
    kEarLeft = 7,
    kEarRight = 8,
    kShoulderLeft = 11,
    kShoulderRight = 12,
    kElbowLeft = 13,
    kElbowRight = 14,
    kWristLeft = 15,
    kWristRight = 16,
    kPinkyLeft = 17,
    kPinkyRight = 18,
    kIndexLeft = 19,
    kIndexRight = 20,
    kHipLeft = 23,
    kHipRight = 24,
    kKneeLeft = 25,
    kKneeRight = 26,
    kAnkleLeft = 27,
    kAnkleRight = 28,
    kHeelLeft = 29,
    kHeelRight = 30,
    kFootLeft = 31,
    kFootRight = 32,
  };
  // Pixels, x right and y down.
  float image[kCount][2];
  // Meters from between the hips, oriented as the camera sees them: x right,
  // y down, z away from the camera.
  float world[kCount][3];
  // How likely each is to be in view and not hidden, 0 to 1.
  float visibility[kCount];
};

// Finds one person's pose in each frame of a video, with MediaPipe's pose
// models converted to ONNX (tools/kinect/convert_pose_models.py), following
// its pose detection and landmark graphs: a detector finds the person and a
// rotated square around them, the landmark model reads that square, and the
// landmarks give the next frame's square, so the detector runs only when the
// person is lost.
class PoseEstimator {
 public:
  // Null, having logged why, if the models or ONNX Runtime can't be loaded.
  static std::unique_ptr<PoseEstimator> Create(
      const std::filesystem::path& model_dir,
      const std::filesystem::path& onnx_runtime_path, int threads);
  ~PoseEstimator();

  // An RGB frame, 3 bytes a pixel. False if no one is found.
  bool Process(const uint8_t* rgb, int width, int height, size_t stride,
               PoseLandmarks& pose);

 private:
  struct Roi {
    float cx, cy, size, rotation;
    static Roi FromPoints(float x0, float y0, float x1, float y1);
    // A pixel of the model's size x size input, in the frame.
    void ToImage(float u, float v, int size, float& x, float& y) const;
  };
  struct Frame {
    const uint8_t* rgb;
    int width, height;
    size_t stride;
  };

  PoseEstimator() = default;
  bool Detect(const Frame& frame, Roi& roi);
  bool Landmarks(const Frame& frame, const Roi& roi, PoseLandmarks& pose,
                 Roi& next);

  std::unique_ptr<OnnxModel> detector_;
  std::unique_ptr<OnnxModel> landmarker_;
  std::vector<float> anchors_;  // x, y pairs, 0 to 1.
  std::vector<float> input_;
  std::vector<std::vector<float>> outputs_;
  bool tracking_ = false;
  Roi roi_ = {};
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_POSE_ESTIMATOR_H_
