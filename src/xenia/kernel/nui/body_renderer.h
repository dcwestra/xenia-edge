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
#include <functional>
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

  // The first body's outline as a picture of the player shows it. Titles
  // show players the depth of their bodies, and a body drawn from simple
  // shapes alone looks like a mannequin: with an outline, the body takes the
  // player's, bone by bone, so it stays where its skeleton is.
  struct Outline {
    // Each joint's place in the picture, in its pixels.
    std::array<std::array<float, 2>, size_t(BodyJoint::kCount)> joints;
    // Whether the picture shows the joint where it's drawn: a bone between
    // joints it doesn't show keeps its shapes.
    std::array<bool, size_t(BodyJoint::kCount)> shown;
    // The picture's pixels per pixel of the sensor's view, across the body.
    float scale;
    // 0 to 1, how likely a picture pixel is to show the player.
    std::function<float(float x, float y)> mask;
  };

  BodyRenderer();

  // The sensor's height above the floor and its tilt up, in degrees.
  void SetCamera(float height, float tilt_degrees);
  // A room point in the sensor's view: x right, y up, z (depth) out.
  Vec3 ToCamera(const Vec3& room) const;
  // A point in the sensor's view to its pixel.
  static void Project(const Vec3& camera, float& x, float& y) {
    x = kCenterX + kFocalLength * camera.x / camera.z;
    y = kCenterY - kFocalLength * camera.y / camera.z;
  }
  // Raw depth values (KinectCamera's format, 2047 for no reading), for the
  // bodies given, sized for a person of the given height, the first in the
  // outline given if any.
  void Render(const Body* bodies, size_t body_count, float player_height,
              uint16_t* raw, const Outline* outline = nullptr);
  // After Render with an outline, for each pixel of the body (before the
  // view is mirrored), where in the picture it is: x, y pairs, NaN for
  // pixels that aren't the body's.
  const std::vector<float>& source() const { return source_; }
  // Whether the last Render had an outline.
  bool outlined() const { return outlined_; }

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
  void TraceOutline(const Body& body, float player_height,
                    const Outline& outline);
  void CastShadow();

  float camera_height_ = 0.85f;
  float tilt_ = 0.0f;
  bool room_valid_ = false;
  std::vector<float> room_;
  std::vector<float> depth_;
  std::vector<Ellipsoid> shapes_;
  std::vector<uint16_t> raw_from_mm_;
  // TraceOutline's, per pixel.
  struct Nearest {
    // The bone, -1 for none; how far outside its shape (negative inside),
    // in pixels; its axis' depth there; where it puts the pixel in the
    // picture; and whether the pixel is in the shape's core.
    int bone;
    float outside, axis_depth, x, y;
    bool core;
  };
  std::vector<Nearest> nearest_[2];
  std::vector<uint8_t> in_kept_shape_;
  std::vector<float> source_;
  bool outlined_ = false;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_BODY_RENDERER_H_
