/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_BODY_RENDERER_H_
#define XENIA_KERNEL_NUI_BODY_RENDERER_H_

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace xe {
namespace kernel {
namespace nui {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  Vec3() = default;
  constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 Cross(const Vec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
  float Length() const { return std::sqrt(Dot(*this)); }
  Vec3 Normalized() const {
    const float length = Length();
    return length > 0.0f ? *this * (1.0f / length) : Vec3(0.0f, 1.0f, 0.0f);
  }
};

// A body's joints, in the order of a title's NUI skeleton.
enum class BodyJoint {
  kHipCenter,
  kSpine,
  kShoulderCenter,
  kHead,
  kShoulderLeft,
  kElbowLeft,
  kWristLeft,
  kHandLeft,
  kShoulderRight,
  kElbowRight,
  kWristRight,
  kHandRight,
  kHipLeft,
  kKneeLeft,
  kAnkleLeft,
  kFootLeft,
  kHipRight,
  kKneeRight,
  kAnkleRight,
  kFootRight,
  kCount,
};

struct Body {
  // In the room: meters, x to the sensor's right as it looks out, y up from
  // the floor, z out from the sensor. A person facing the sensor has their
  // left at +x.
  std::array<Vec3, size_t(BodyJoint::kCount)> joints;
  Vec3& operator[](BodyJoint joint) { return joints[size_t(joint)]; }
  const Vec3& operator[](BodyJoint joint) const {
    return joints[size_t(joint)];
  }
};

// Depth as a Kinect for Xbox 360 would see a body standing in a plain room:
// for titles' own skeletal trackers, which find people in depth alone and
// take a body drawn from simple shapes as they would a real one. The view is
// the sensor's nominal one, which titles assume.
class BodyRenderer {
 public:
  static constexpr int kWidth = 640;
  static constexpr int kHeight = 480;
  // NUI_CAMERA_DEPTH_NOMINAL_FOCAL_LENGTH_IN_PIXELS, at 640x480.
  static constexpr float kFocalLength = 571.26f;
  static constexpr float kCenterX = 319.5f;
  static constexpr float kCenterY = 239.5f;

  BodyRenderer();

  // The sensor's height above the floor and its tilt up, in degrees.
  void SetCamera(float height, float tilt_degrees);
  // A room point in the sensor's view: x right, y up, z (depth) out.
  Vec3 ToCamera(const Vec3& room) const;
  // Raw depth values (KinectCamera's format, 2047 for no reading), for the
  // bodies given, sized for a person of the given height.
  void Render(const Body* bodies, size_t body_count, float player_height,
              uint16_t* raw);

 private:
  struct Ellipsoid {
    Vec3 center;
    Vec3 axes[3];  // Unit vectors.
    Vec3 radii;
  };
  void AddBody(const Body& body, float player_height);
  void AddSphere(const Vec3& center, float radius);
  void AddLimb(const Vec3& p0, const Vec3& p1, float r0, float r1);
  void DrawRoom();
  void Draw(const Ellipsoid& e);
  void CastShadow();

  float camera_height_ = 0.85f;
  float tilt_ = 0.0f;
  bool room_valid_ = false;
  std::vector<float> room_;
  std::vector<float> depth_;
  std::vector<Ellipsoid> shapes_;
  std::vector<uint16_t> raw_from_mm_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_BODY_RENDERER_H_
