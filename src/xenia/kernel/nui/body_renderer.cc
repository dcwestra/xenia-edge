/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/body_renderer.h"

#include <algorithm>
#include <limits>

#include "xenia/kernel/nui/kinect_camera.h"

namespace xe {
namespace kernel {
namespace nui {

namespace {
constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kPi = 3.14159265358979f;
// The room: a back wall, and the sensor's range.
constexpr float kWallDistance = 4.2f;
constexpr float kMaxDepth = 8.0f;
// The Kinect's IR projector is beside its IR camera, so background just to
// one side of a nearer object gets no pattern and reads as nothing.
constexpr float kProjectorBaseline = 0.075f;
constexpr uint32_t kMaxMillimeters = 8191;
}  // namespace

BodyRenderer::BodyRenderer()
    : room_(size_t(kWidth) * kHeight),
      depth_(size_t(kWidth) * kHeight),
      raw_from_mm_(kMaxMillimeters + 1) {
  for (uint32_t mm = 0; mm <= kMaxMillimeters; ++mm) {
    raw_from_mm_[mm] = MillimetersToKinectRawDepth(mm);
  }
}

void BodyRenderer::SetCamera(float height, float tilt_degrees) {
  const float tilt = tilt_degrees * kPi / 180.0f;
  if (height != camera_height_ || tilt != tilt_) {
    camera_height_ = height;
    tilt_ = tilt;
    room_valid_ = false;
  }
}

Vec3 BodyRenderer::ToCamera(const Vec3& room) const {
  const Vec3 p = room - Vec3(0.0f, camera_height_, 0.0f);
  const float c = std::cos(tilt_);
  const float s = std::sin(tilt_);
  return {p.x, c * p.y - s * p.z, s * p.y + c * p.z};
}

void BodyRenderer::DrawRoom() {
  const float c = std::cos(tilt_);
  const float s = std::sin(tilt_);
  for (int v = 0; v < kHeight; ++v) {
    const float ray_y = -(v - kCenterY) / kFocalLength;
    // The ray's direction in the room, with the camera's z 1.
    const float room_y = ray_y * c + s;
    const float room_z = -ray_y * s + c;
    float t = kInfinity;
    if (room_y < -1e-6f) {
      const float floor_t = -camera_height_ / room_y;
      if (floor_t * room_z <= kWallDistance) {
        t = floor_t;
      }
    }
    if (room_z > 1e-6f) {
      t = std::min(t, kWallDistance / room_z);
    }
    if (t > kMaxDepth) {
      t = kInfinity;
    }
    std::fill_n(room_.begin() + size_t(v) * kWidth, kWidth, t);
  }
  room_valid_ = true;
}

void BodyRenderer::AddSphere(const Vec3& center, float radius) {
  shapes_.push_back({center,
                     {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)},
                     Vec3(radius, radius, radius)});
}

void BodyRenderer::AddLimb(const Vec3& p0, const Vec3& p1, float r0, float r1) {
  // A tapered limb as a chain of spheres, close enough together that the
  // ridges between them are a few millimeters.
  const float length = (p1 - p0).Length();
  const int count = std::max(2, int(length / (0.5f * std::min(r0, r1))) + 1);
  for (int i = 0; i < count; ++i) {
    const float t = float(i) / (count - 1);
    AddSphere(p0 + (p1 - p0) * t, r0 + (r1 - r0) * t);
  }
}

void BodyRenderer::AddBody(const Body& b, float player_height) {
  using J = BodyJoint;
  const float s = player_height / 1.70f;
  const Vec3 up = (b[J::kShoulderCenter] - b[J::kHipCenter]).Normalized();
  Vec3 left = (b[J::kShoulderLeft] - b[J::kShoulderRight]).Normalized();
  left = (left - up * left.Dot(up)).Normalized();
  const Vec3 front = up.Cross(left);
  auto torso = [&](const Vec3& center, float rx, float ry, float rz) {
    shapes_.push_back({center, {left, up, front}, Vec3(rx, ry, rz) * s});
  };
  const Vec3 spine = b[J::kShoulderCenter] - b[J::kHipCenter];
  torso(b[J::kHipCenter] - up * (0.02f * s), 0.165f, 0.11f, 0.105f);
  torso(b[J::kHipCenter] + spine * 0.42f, 0.145f, 0.15f, 0.10f);
  torso(b[J::kHipCenter] + spine * 0.75f, 0.165f, 0.15f, 0.11f);
  // The neck and head.
  const Vec3 head_up = (b[J::kHead] - b[J::kShoulderCenter]).Normalized();
  AddLimb(b[J::kShoulderCenter], b[J::kHead] - head_up * (0.06f * s),
          0.055f * s, 0.05f * s);
  const Vec3 head_left = (left - head_up * left.Dot(head_up)).Normalized();
  shapes_.push_back({b[J::kHead],
                     {head_left, head_up, head_up.Cross(head_left)},
                     Vec3(0.078f, 0.11f, 0.095f) * s});
  struct Side {
    J shoulder, elbow, wrist, hand, hip, knee, ankle, foot;
  };
  for (const Side& side :
       {Side{J::kShoulderLeft, J::kElbowLeft, J::kWristLeft, J::kHandLeft,
             J::kHipLeft, J::kKneeLeft, J::kAnkleLeft, J::kFootLeft},
        Side{J::kShoulderRight, J::kElbowRight, J::kWristRight, J::kHandRight,
             J::kHipRight, J::kKneeRight, J::kAnkleRight, J::kFootRight}}) {
    AddSphere(b[side.shoulder], 0.06f * s);
    AddLimb(b[side.shoulder], b[side.elbow], 0.052f * s, 0.043f * s);
    AddLimb(b[side.elbow], b[side.wrist], 0.04f * s, 0.03f * s);
    AddLimb(b[side.wrist], b[side.hand], 0.035f * s, 0.032f * s);
    AddLimb(b[side.hip], b[side.knee], 0.085f * s, 0.055f * s);
    AddLimb(b[side.knee], b[side.ankle], 0.052f * s, 0.037f * s);
    AddLimb(b[side.ankle], b[side.foot], 0.04f * s, 0.032f * s);
  }
}

void BodyRenderer::Draw(const Ellipsoid& e) {
  const float bound = std::max({e.radii.x, e.radii.y, e.radii.z});
  if (e.center.z - bound < 0.2f) {
    return;
  }
  const float u = kCenterX + kFocalLength * e.center.x / e.center.z;
  const float v = kCenterY - kFocalLength * e.center.y / e.center.z;
  const float extent = kFocalLength * bound / (e.center.z - bound) + 2.0f;
  const int x0 = std::max(int(u - extent), 0);
  const int x1 = std::min(int(u + extent) + 1, kWidth);
  const int y0 = std::max(int(v - extent), 0);
  const int y1 = std::min(int(v + extent) + 1, kHeight);
  // In the ellipsoid's unit sphere space, where a ray from the sensor is
  // origin + t * direction and t is the depth (the ray's z is 1).
  const Vec3 inv(1.0f / e.radii.x, 1.0f / e.radii.y, 1.0f / e.radii.z);
  auto local = [&](const Vec3& p) {
    return Vec3(e.axes[0].Dot(p) * inv.x, e.axes[1].Dot(p) * inv.y,
                e.axes[2].Dot(p) * inv.z);
  };
  const Vec3 origin = local(e.center * -1.0f);
  const float cc = origin.Dot(origin) - 1.0f;
  for (int y = y0; y < y1; ++y) {
    float* row = &depth_[size_t(y) * kWidth];
    const float ray_y = -(y - kCenterY) / kFocalLength;
    for (int x = x0; x < x1; ++x) {
      const Vec3 d = local(Vec3((x - kCenterX) / kFocalLength, ray_y, 1.0f));
      const float a = d.Dot(d);
      const float b = origin.Dot(d);
      const float disc = b * b - a * cc;
      if (disc <= 0.0f) {
        continue;
      }
      const float t = (-b - std::sqrt(disc)) / a;
      if (t > 0.0f && t < row[x]) {
        row[x] = t;
      }
    }
  }
}

void BodyRenderer::CastShadow() {
  // A pixel is in shadow if a pixel to its right lands further left in the
  // projector's view.
  for (int y = 0; y < kHeight; ++y) {
    float* row = &depth_[size_t(y) * kWidth];
    float right_min = kInfinity;
    for (int x = kWidth - 1; x >= 0; --x) {
      if (row[x] <= 0.0f || row[x] == kInfinity) {
        continue;
      }
      const float projector = x - kFocalLength * kProjectorBaseline / row[x];
      const bool shadowed = projector > right_min + 0.5f;
      right_min = std::min(right_min, projector);
      if (shadowed) {
        row[x] = 0.0f;
      }
    }
  }
}

void BodyRenderer::Render(const Body* bodies, size_t body_count,
                          float player_height, uint16_t* raw) {
  if (!room_valid_) {
    DrawRoom();
  }
  std::copy(room_.begin(), room_.end(), depth_.begin());
  shapes_.clear();
  for (size_t i = 0; i < body_count; ++i) {
    Body camera;
    for (size_t j = 0; j < camera.joints.size(); ++j) {
      camera.joints[j] = ToCamera(bodies[i].joints[j]);
    }
    AddBody(camera, player_height);
  }
  for (const Ellipsoid& shape : shapes_) {
    Draw(shape);
  }
  CastShadow();
  for (size_t i = 0; i < depth_.size(); ++i) {
    const float meters = depth_[i];
    if (meters <= 0.0f || meters == kInfinity) {
      raw[i] = 2047;
      continue;
    }
    const uint32_t mm = uint32_t(meters * 1000.0f + 0.5f);
    raw[i] = mm <= kMaxMillimeters ? raw_from_mm_[mm] : 2047;
  }
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
