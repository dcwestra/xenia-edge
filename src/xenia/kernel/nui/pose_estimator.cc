/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/pose_estimator.h"

#include <algorithm>
#include <cmath>

#include "xenia/base/logging.h"
#include "xenia/kernel/nui/onnx_model.h"

namespace xe {
namespace kernel {
namespace nui {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kDetectorSize = 224;
constexpr int kLandmarkSize = 256;
// The landmark model's 39: the 33 of the pose, then two that frame the next
// frame's square (33, the hip center, and 34), and four more.
constexpr int kModelLandmarks = 39;
constexpr float kMinDetectionScore = 0.5f;
constexpr float kMinPresence = 0.5f;
constexpr float kNmsOverlap = 0.3f;

float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// The detector's anchors, its SSD configuration with fixed-size anchors: 2
// per cell of a 28x28 grid (stride 8), a 14x14 (16), and 6 of a 7x7 (the
// three stride-32 layers together).
std::vector<float> SsdAnchors() {
  const int strides[] = {8, 16, 32, 32, 32};
  constexpr int kLayers = 5;
  std::vector<float> anchors;
  for (int layer = 0; layer < kLayers;) {
    int last = layer;
    while (last + 1 < kLayers && strides[last + 1] == strides[layer]) {
      ++last;
    }
    const int per_cell = 2 * (last - layer + 1);
    const int size = (kDetectorSize + strides[layer] - 1) / strides[layer];
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        for (int i = 0; i < per_cell; ++i) {
          anchors.push_back((x + 0.5f) / size);
          anchors.push_back((y + 0.5f) / size);
        }
      }
    }
    layer = last + 1;
  }
  return anchors;
}

// A frame's RGB at (x, y), pixel centers on whole numbers, black outside.
void SampleBilinear(const uint8_t* rgb, int width, int height, size_t stride,
                    float x, float y, float out[3]) {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const int x0 = int(fx);
  const int y0 = int(fy);
  const float wx = x - fx;
  const float wy = y - fy;
  out[0] = out[1] = out[2] = 0.0f;
  for (int dy = 0; dy < 2; ++dy) {
    const int py = y0 + dy;
    if (py < 0 || py >= height) {
      continue;
    }
    const float weight_y = dy ? wy : 1.0f - wy;
    for (int dx = 0; dx < 2; ++dx) {
      const int px = x0 + dx;
      if (px < 0 || px >= width) {
        continue;
      }
      const float weight = weight_y * (dx ? wx : 1.0f - wx);
      const uint8_t* p = rgb + py * stride + px * 3;
      out[0] += p[0] * weight;
      out[1] += p[1] * weight;
      out[2] += p[2] * weight;
    }
  }
}

}  // namespace

PoseEstimator::Roi PoseEstimator::Roi::FromPoints(float x0, float y0, float x1,
                                                  float y1) {
  // MediaPipe's alignment points rect: centered on the first point, twice
  // their distance across, turned so the second is straight up from the
  // first, then enlarged 1.25 times.
  float rotation = kPi * 0.5f - std::atan2(-(y1 - y0), x1 - x0);
  rotation -= 2.0f * kPi * std::floor((rotation + kPi) / (2.0f * kPi));
  return {x0, y0, 2.0f * std::hypot(x1 - x0, y1 - y0) * 1.25f, rotation};
}

void PoseEstimator::Roi::ToImage(float u, float v, int out_size, float& x,
                                 float& y) const {
  const float c = std::cos(rotation);
  const float s = std::sin(rotation);
  const float scale = size / out_size;
  const float du = (u - out_size * 0.5f) * scale;
  const float dv = (v - out_size * 0.5f) * scale;
  x = cx + c * du - s * dv;
  y = cy + s * du + c * dv;
}

PoseEstimator::~PoseEstimator() = default;

std::unique_ptr<PoseEstimator> PoseEstimator::Create(
    const std::filesystem::path& model_dir,
    const std::filesystem::path& onnx_runtime_path, int threads) {
  std::unique_ptr<PoseEstimator> estimator(new PoseEstimator());
  estimator->detector_ = OnnxModel::Load(model_dir / "pose_detector.onnx",
                                         onnx_runtime_path, threads);
  if (!estimator->detector_) {
    return nullptr;
  }
  estimator->landmarker_ = OnnxModel::Load(
      model_dir / "pose_landmarks_detector.onnx", onnx_runtime_path, threads);
  if (!estimator->landmarker_) {
    return nullptr;
  }
  estimator->anchors_ = SsdAnchors();
  return estimator;
}

bool PoseEstimator::Detect(const Frame& frame, Roi& roi) {
  // The frame letterboxed into the detector's square, values -1 to 1.
  const float scale =
      float(kDetectorSize) / std::max(frame.width, frame.height);
  const float pad_x = (kDetectorSize - frame.width * scale) * 0.5f;
  const float pad_y = (kDetectorSize - frame.height * scale) * 0.5f;
  input_.resize(kDetectorSize * kDetectorSize * 3);
  float* out = input_.data();
  for (int v = 0; v < kDetectorSize; ++v) {
    for (int u = 0; u < kDetectorSize; ++u, out += 3) {
      SampleBilinear(frame.rgb, frame.width, frame.height, frame.stride,
                     (u - pad_x) / scale, (v - pad_y) / scale, out);
      for (int c = 0; c < 3; ++c) {
        out[c] = out[c] / 127.5f - 1.0f;
      }
    }
  }
  if (!detector_->Run("input_1", input_.data(),
                      {1, kDetectorSize, kDetectorSize, 3},
                      {"Identity", "Identity_1"}, outputs_)) {
    return false;
  }
  const std::vector<float>& boxes = outputs_[0];
  const std::vector<float>& scores = outputs_[1];
  const size_t count = anchors_.size() / 2;
  if (boxes.size() < count * 12 || scores.size() < count) {
    return false;
  }
  // Each candidate's box (center, size) and first two keypoints, in 0 to 1
  // of the letterboxed square.
  struct Candidate {
    float score, x0, y0, x1, y1, kp[4];
  };
  std::vector<Candidate> candidates;
  size_t best = 0;
  for (size_t i = 0; i < count; ++i) {
    const float score = Sigmoid(std::clamp(scores[i], -100.0f, 100.0f));
    if (score < kMinDetectionScore) {
      continue;
    }
    const float* b = &boxes[i * 12];
    const float ax = anchors_[i * 2];
    const float ay = anchors_[i * 2 + 1];
    const float cx = b[0] / kDetectorSize + ax;
    const float cy = b[1] / kDetectorSize + ay;
    const float w = b[2] / kDetectorSize;
    const float h = b[3] / kDetectorSize;
    Candidate c = {score,
                   cx - w / 2,
                   cy - h / 2,
                   cx + w / 2,
                   cy + h / 2,
                   {b[4] / kDetectorSize + ax, b[5] / kDetectorSize + ay,
                    b[6] / kDetectorSize + ax, b[7] / kDetectorSize + ay}};
    if (candidates.empty() || score > candidates[best].score) {
      best = candidates.size();
    }
    candidates.push_back(c);
  }
  if (candidates.empty()) {
    return false;
  }
  // Weighted NMS around the best: its keypoints averaged with those of the
  // boxes overlapping it, by score.
  const Candidate& top = candidates[best];
  const float top_area = (top.x1 - top.x0) * (top.y1 - top.y0);
  float kp[4] = {};
  float total = 0.0f;
  for (const Candidate& c : candidates) {
    const float iw = std::min(c.x1, top.x1) - std::max(c.x0, top.x0);
    const float ih = std::min(c.y1, top.y1) - std::max(c.y0, top.y0);
    const float inter = std::max(iw, 0.0f) * std::max(ih, 0.0f);
    const float uni = (c.x1 - c.x0) * (c.y1 - c.y0) + top_area - inter;
    if (uni <= 0.0f || inter / uni <= kNmsOverlap) {
      continue;
    }
    for (int k = 0; k < 4; ++k) {
      kp[k] += c.kp[k] * c.score;
    }
    total += c.score;
  }
  for (int k = 0; k < 4; ++k) {
    kp[k] /= total;
  }
  // Back to frame pixels: the hip center, and a point above it whose
  // distance is the body's size.
  const float x0 = (kp[0] * kDetectorSize - pad_x) / scale;
  const float y0 = (kp[1] * kDetectorSize - pad_y) / scale;
  const float x1 = (kp[2] * kDetectorSize - pad_x) / scale;
  const float y1 = (kp[3] * kDetectorSize - pad_y) / scale;
  roi = Roi::FromPoints(x0, y0, x1, y1);
  return true;
}

bool PoseEstimator::Landmarks(const Frame& frame, const Roi& roi,
                              PoseLandmarks& pose, Roi& next) {
  input_.resize(kLandmarkSize * kLandmarkSize * 3);
  float* out = input_.data();
  for (int v = 0; v < kLandmarkSize; ++v) {
    for (int u = 0; u < kLandmarkSize; ++u, out += 3) {
      float x, y;
      roi.ToImage(float(u), float(v), kLandmarkSize, x, y);
      SampleBilinear(frame.rgb, frame.width, frame.height, frame.stride, x, y,
                     out);
      for (int c = 0; c < 3; ++c) {
        out[c] *= 1.0f / 255.0f;
      }
    }
  }
  if (!landmarker_->Run("input_1", input_.data(),
                        {1, kLandmarkSize, kLandmarkSize, 3},
                        {"Identity", "Identity_1", "Identity_4"}, outputs_)) {
    return false;
  }
  const std::vector<float>& raw = outputs_[0];
  const std::vector<float>& flag = outputs_[1];
  const std::vector<float>& world = outputs_[2];
  if (raw.size() < kModelLandmarks * 5 || flag.empty() ||
      world.size() < kModelLandmarks * 3) {
    return false;
  }
  // The model applies its own sigmoid to the presence of a person.
  if (flag[0] < kMinPresence) {
    return false;
  }
  for (int i = 0; i < PoseLandmarks::kCount; ++i) {
    const float* r = &raw[i * 5];
    roi.ToImage(r[0], r[1], kLandmarkSize, pose.image[i][0], pose.image[i][1]);
    pose.visibility[i] = std::min(Sigmoid(r[3]), Sigmoid(r[4]));
  }
  // World landmarks come in the square's orientation: turned back to the
  // camera's.
  const float c = std::cos(roi.rotation);
  const float s = std::sin(roi.rotation);
  for (int i = 0; i < PoseLandmarks::kCount; ++i) {
    const float* w = &world[i * 3];
    pose.world[i][0] = c * w[0] - s * w[1];
    pose.world[i][1] = s * w[0] + c * w[1];
    pose.world[i][2] = w[2];
  }
  float x0, y0, x1, y1;
  roi.ToImage(raw[33 * 5], raw[33 * 5 + 1], kLandmarkSize, x0, y0);
  roi.ToImage(raw[34 * 5], raw[34 * 5 + 1], kLandmarkSize, x1, y1);
  next = Roi::FromPoints(x0, y0, x1, y1);
  return true;
}

bool PoseEstimator::Process(const uint8_t* rgb, int width, int height,
                            size_t stride, PoseLandmarks& pose) {
  const Frame frame = {rgb, width, height, stride};
  Roi next;
  // Following the person from the last frame.
  if (tracking_) {
    if (Landmarks(frame, roi_, pose, next)) {
      roi_ = next;
      return true;
    }
    tracking_ = false;
  }
  // Else finding them.
  if (!Detect(frame, roi_) || !Landmarks(frame, roi_, pose, next)) {
    return false;
  }
  roi_ = next;
  tracking_ = true;
  return true;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
