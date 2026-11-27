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
#include <iterator>
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

// The bones an outline is traced along, with the shapes AddBody draws for
// them: a radius at each end (meters, for a body 1.70 m tall), and how far
// the shapes reach past the ends (the pelvis below the hips, the top of the
// head, fingers, heels and toes).
struct OutlineBone {
  BodyJoint from, to;
  float radius_from, radius_to;
  float before, after;
  // The bone this one hangs from, -1 for none.
  int parent;
};
constexpr OutlineBone kOutlineBones[] = {
    {BodyJoint::kHipCenter, BodyJoint::kShoulderCenter, 0.16f, 0.16f, 0.13f,
     0.0f, -1},
    {BodyJoint::kShoulderCenter, BodyJoint::kHead, 0.055f, 0.09f, 0.0f, 0.14f,
     0},
    {BodyJoint::kShoulderLeft, BodyJoint::kElbowLeft, 0.06f, 0.043f, 0.0f, 0.0f,
     0},
    {BodyJoint::kElbowLeft, BodyJoint::kWristLeft, 0.04f, 0.03f, 0.0f, 0.0f, 2},
    {BodyJoint::kWristLeft, BodyJoint::kHandLeft, 0.035f, 0.032f, 0.0f, 0.09f,
     3},
    {BodyJoint::kShoulderRight, BodyJoint::kElbowRight, 0.06f, 0.043f, 0.0f,
     0.0f, 0},
    {BodyJoint::kElbowRight, BodyJoint::kWristRight, 0.04f, 0.03f, 0.0f, 0.0f,
     5},
    {BodyJoint::kWristRight, BodyJoint::kHandRight, 0.035f, 0.032f, 0.0f, 0.09f,
     6},
    {BodyJoint::kHipLeft, BodyJoint::kKneeLeft, 0.085f, 0.055f, 0.0f, 0.0f, 0},
    {BodyJoint::kKneeLeft, BodyJoint::kAnkleLeft, 0.052f, 0.037f, 0.0f, 0.0f,
     8},
    {BodyJoint::kAnkleLeft, BodyJoint::kFootLeft, 0.04f, 0.032f, 0.07f, 0.05f,
     9},
    {BodyJoint::kHipRight, BodyJoint::kKneeRight, 0.085f, 0.055f, 0.0f, 0.0f,
     0},
    {BodyJoint::kKneeRight, BodyJoint::kAnkleRight, 0.052f, 0.037f, 0.0f, 0.0f,
     11},
    {BodyJoint::kAnkleRight, BodyJoint::kFootRight, 0.04f, 0.032f, 0.07f, 0.05f,
     12},
};
constexpr int kOutlineBoneCount = int(std::size(kOutlineBones));
// How far past a bone's shapes its outline may reach, for clothes and hair,
// in meters.
constexpr float kOutlineReach = 0.10f;
// Within this of a pixel's nearest bone, the bone joined to it there shares
// the pixel, their pictures blended, so the outline doesn't tear at joints.
constexpr float kOutlineBlend = 0.05f;
// The middle of a shape, as a fraction of its radius, is kept whatever the
// mask says: a limb the mask misses, as when blurred, still has a body.
constexpr float kOutlineCore = 0.5f;
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

void BodyRenderer::TraceOutline(const Body& body, float player_height,
                                const Outline& outline) {
  const float s = player_height / 1.70f;
  const size_t pixels = size_t(kWidth) * kHeight;
  if (nearest_[0].size() != pixels) {
    nearest_[0].assign(pixels, {-1});
    nearest_[1].assign(pixels, {-1});
    in_kept_shape_.assign(pixels, 0);
  }
  source_.assign(pixels * 2, std::numeric_limits<float>::quiet_NaN());
  auto traced = [&](int bone) {
    return outline.shown[size_t(kOutlineBones[bone].from)] &&
           outline.shown[size_t(kOutlineBones[bone].to)];
  };
  // Each bone in the view, and the pixels it may reach.
  struct InView {
    bool valid;
    float ax, ay, bx, by;
    int x0, y0, x1, y1;
  };
  InView in_view[kOutlineBoneCount];
  int bx0 = kWidth, by0 = kHeight, bx1 = 0, by1 = 0;
  for (int i = 0; i < kOutlineBoneCount; ++i) {
    const OutlineBone& bone = kOutlineBones[i];
    const Vec3& a = body[bone.from];
    const Vec3& b = body[bone.to];
    InView& v = in_view[i];
    v.valid = a.z > 0.3f && b.z > 0.3f;
    if (!v.valid) {
      continue;
    }
    Project(a, v.ax, v.ay);
    Project(b, v.bx, v.by);
    const float pixels_per_meter = kFocalLength / std::min(a.z, b.z);
    const float margin = (std::max(bone.radius_from, bone.radius_to) +
                          kOutlineReach + std::max(bone.before, bone.after)) *
                             s * pixels_per_meter +
                         2.0f;
    v.x0 = std::max(int(std::min(v.ax, v.bx) - margin), 0);
    v.x1 = std::min(int(std::max(v.ax, v.bx) + margin) + 1, kWidth);
    v.y0 = std::max(int(std::min(v.ay, v.by) - margin), 0);
    v.y1 = std::min(int(std::max(v.ay, v.by) + margin) + 1, kHeight);
    if (v.x0 >= v.x1 || v.y0 >= v.y1) {
      v.valid = false;
      continue;
    }
    bx0 = std::min(bx0, v.x0);
    by0 = std::min(by0, v.y0);
    bx1 = std::max(bx1, v.x1);
    by1 = std::max(by1, v.y1);
  }
  if (bx0 >= bx1 || by0 >= by1) {
    return;
  }
  for (int y = by0; y < by1; ++y) {
    const size_t row = size_t(y) * kWidth;
    for (int x = bx0; x < bx1; ++x) {
      nearest_[0][row + x].bone = -1;
      nearest_[1][row + x].bone = -1;
      in_kept_shape_[row + x] = 0;
    }
  }
  // Nearer: inside both, the one in front; else the one less outside.
  auto nearer = [](const Nearest& n, const Nearest& m) {
    if (m.bone < 0) {
      return true;
    }
    if (n.outside < 0.0f && m.outside < 0.0f) {
      return n.axis_depth < m.axis_depth;
    }
    return n.outside < m.outside;
  };
  const float scale = outline.scale;
  for (int i = 0; i < kOutlineBoneCount; ++i) {
    const InView& v = in_view[i];
    if (!v.valid) {
      continue;
    }
    const OutlineBone& bone = kOutlineBones[i];
    const Vec3& a = body[bone.from];
    const Vec3& b = body[bone.to];
    const bool bone_traced = traced(i);
    // Along the bone and across it, in the view and in the picture.
    const float dx = v.bx - v.ax;
    const float dy = v.by - v.ay;
    const float length = std::sqrt(dx * dx + dy * dy);
    const float ux = length > 0.5f ? dx / length : 0.0f;
    const float uy = length > 0.5f ? dy / length : 1.0f;
    const auto& pa = outline.joints[size_t(bone.from)];
    const auto& pb = outline.joints[size_t(bone.to)];
    const float pdx = pb[0] - pa[0];
    const float pdy = pb[1] - pa[1];
    const float picture_length = std::sqrt(pdx * pdx + pdy * pdy);
    const float pux = picture_length > 0.5f ? pdx / picture_length : ux;
    const float puy = picture_length > 0.5f ? pdy / picture_length : uy;
    const float pixels_per_meter = kFocalLength * 2.0f / (a.z + b.z);
    const float before = bone.before * s * pixels_per_meter;
    const float after = bone.after * s * pixels_per_meter;
    const float reach = kOutlineReach * s * pixels_per_meter;
    // Beyond this from the axis, no pixel can be reached: tested first, as
    // most of the box around a slanting bone is.
    const float farthest = std::max(bone.radius_from, bone.radius_to) * s *
                               kFocalLength / std::min(a.z, b.z) +
                           reach;
    const float farthest_squared = farthest * farthest;
    for (int y = v.y0; y < v.y1; ++y) {
      const float ry = y - v.ay;
      const size_t row = size_t(y) * kWidth;
      for (int x = v.x0; x < v.x1; ++x) {
        const float rx = x - v.ax;
        const float along = rx * ux + ry * uy;
        const float across = ry * ux - rx * uy;
        const float on_axis = std::clamp(along, -before, length + after);
        const float off_along = along - on_axis;
        const float off_axis_squared = off_along * off_along + across * across;
        if (off_axis_squared > farthest_squared) {
          continue;
        }
        const float t =
            length > 0.5f ? std::clamp(along / length, 0.0f, 1.0f) : 0.0f;
        const float z = a.z + (b.z - a.z) * t;
        const float radius =
            (bone.radius_from + (bone.radius_to - bone.radius_from) * t) * s *
            kFocalLength / z;
        const float off_axis = std::sqrt(off_axis_squared);
        const float outside = off_axis - radius;
        if (outside > reach) {
          continue;
        }
        const size_t index = row + x;
        if (!bone_traced && outside < 0.0f) {
          in_kept_shape_[index] = 1;
        }
        // In the picture: along the bone in proportion between its ends,
        // and past them and across it at the picture's scale.
        float picture_along;
        if (along < 0.0f) {
          picture_along = along * scale;
        } else if (along > length) {
          picture_along = picture_length + (along - length) * scale;
        } else {
          picture_along =
              length > 0.5f ? along / length * picture_length : 0.0f;
        }
        const Nearest n = {i,
                           outside,
                           z,
                           pa[0] + pux * picture_along - puy * across * scale,
                           pa[1] + puy * picture_along + pux * across * scale,
                           off_axis < kOutlineCore * radius};
        Nearest& first = nearest_[0][index];
        if (nearer(n, first)) {
          nearest_[1][index] = first;
          first = n;
        } else if (nearer(n, nearest_[1][index])) {
          nearest_[1][index] = n;
        }
      }
    }
  }
  for (int y = by0; y < by1; ++y) {
    const size_t row = size_t(y) * kWidth;
    for (int x = bx0; x < bx1; ++x) {
      const size_t index = row + x;
      const Nearest& n = nearest_[0][index];
      if (n.bone < 0 || !traced(n.bone) || in_kept_shape_[index]) {
        continue;
      }
      float px = n.x, py = n.y;
      const Nearest& m = nearest_[1][index];
      if (m.bone >= 0 && traced(m.bone) &&
          (kOutlineBones[m.bone].parent == n.bone ||
           kOutlineBones[n.bone].parent == m.bone)) {
        const float gap = m.outside - n.outside;
        const float blend = kOutlineBlend * s * kFocalLength / n.axis_depth;
        if (gap >= 0.0f && gap < blend) {
          const float w = 0.5f * (1.0f - gap / blend);
          px += (m.x - px) * w;
          py += (m.y - py) * w;
        }
      }
      const bool drawn = depth_[index] < room_[index] - 0.005f;
      const bool player = outline.mask(px, py) >= 0.5f;
      if (drawn) {
        if (!player && !n.core) {
          depth_[index] = room_[index];
          continue;
        }
      } else if (player) {
        depth_[index] = std::min(room_[index], n.axis_depth);
      } else {
        continue;
      }
      source_[index * 2] = px;
      source_[index * 2 + 1] = py;
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
                          float player_height, uint16_t* raw,
                          const Outline* outline) {
  if (!room_valid_) {
    DrawRoom();
  }
  std::copy(room_.begin(), room_.end(), depth_.begin());
  shapes_.clear();
  Body first;
  for (size_t i = 0; i < body_count; ++i) {
    Body camera;
    for (size_t j = 0; j < camera.joints.size(); ++j) {
      camera.joints[j] = ToCamera(bodies[i].joints[j]);
    }
    AddBody(camera, player_height);
    if (!i) {
      first = camera;
    }
  }
  for (const Ellipsoid& shape : shapes_) {
    Draw(shape);
  }
  outlined_ = outline && body_count && outline->mask && outline->scale > 0.0f;
  if (outlined_) {
    TraceOutline(first, player_height, *outline);
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
