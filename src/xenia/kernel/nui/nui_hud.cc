/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"

DEFINE_path(kinect_skeleton_log, "",
            "Write the skeletons a Kinect title tracks, every frame it hands "
            "them to the system, to this CSV file: each tracked player's "
            "joints with their tracking states.",
            "Kernel");

namespace xe {
namespace kernel {
namespace nui {

namespace {
// Message 0x2B002: a 48-byte header (runtime version, floor plane, gravity),
// then NUI_SKELETON_DATA for six skeletons.
constexpr uint32_t kSkeletonsOffset = 48;
constexpr uint32_t kSkeletonSize = 448;
// Within NUI_SKELETON_DATA.
constexpr uint32_t kJointPositionsOffset = 32;
constexpr uint32_t kJointStatesOffset = 352;
// NUI_SKELETON_QUALITY flags: clipped at the right, left, top or bottom of
// the view.
constexpr uint32_t kQualityFlagsOffset = 432;
constexpr uint32_t kTrackingStateTracked = 2;
// NUI_SKELETON_POSITION_INDEX.
constexpr int kShoulderCenter = 2;
constexpr int kHandLeft = 7;
constexpr int kHandRight = 11;
// The hand's reach, around a point just below the shoulders, that spans the
// cursor's range: meters across and down.
constexpr float kReachX = 0.5f;
constexpr float kReachY = 0.4f;
constexpr float kCenterBelowShoulders = 0.15f;
// A hand has to be this much further forward than the other to take over.
constexpr float kHandSwitchDistance = 0.1f;
constexpr float kSmoothing = 0.35f;
constexpr auto kFrameTimeout = std::chrono::milliseconds(300);
}  // namespace

void NuiHud::OnSkeletonFrame(const uint8_t* message, uint32_t size) {
  if (size && size < kSkeletonsOffset + kSkeletonCount * kSkeletonSize) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < kSkeletonCount; ++i) {
    const uint8_t* data = message + kSkeletonsOffset + i * kSkeletonSize;
    Skeleton& skeleton = skeletons_[i];
    skeleton.state = xe::load_and_swap<uint32_t>(data);
    skeleton.tracking_id = xe::load_and_swap<uint32_t>(data + 4);
    skeleton.quality_flags =
        xe::load_and_swap<uint32_t>(data + kQualityFlagsOffset);
    for (int j = 0; j < kJointCount; ++j) {
      const uint8_t* position = data + kJointPositionsOffset + j * 16;
      skeleton.joints[j].x = xe::load_and_swap<float>(position);
      skeleton.joints[j].y = xe::load_and_swap<float>(position + 4);
      skeleton.joints[j].z = xe::load_and_swap<float>(position + 8);
      skeleton.joints[j].state =
          xe::load_and_swap<uint32_t>(data + kJointStatesOffset + j * 4);
    }
  }
  frame_time_ = std::chrono::steady_clock::now();
  LogSkeletons();
}

void NuiHud::LogSkeletons() {
  if (cvars::kinect_skeleton_log.empty()) {
    return;
  }
  if (!skeleton_log_) {
    if (skeleton_log_failed_) {
      return;
    }
    skeleton_log_ = xe::filesystem::OpenFile(cvars::kinect_skeleton_log, "w");
    if (!skeleton_log_) {
      skeleton_log_failed_ = true;
      XELOGW("Kinect: can't write the skeleton log {}",
             xe::strip_pii(xe::path_to_utf8(cvars::kinect_skeleton_log)));
      return;
    }
    skeleton_log_start_ = frame_time_;
    std::fputs("ms,frame,slot,tracking_id,quality", skeleton_log_);
    for (int j = 0; j < kJointCount; ++j) {
      std::fprintf(skeleton_log_, ",s%d,x%d,y%d,z%d", j, j, j, j);
    }
    std::fputc('\n', skeleton_log_);
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      frame_time_ - skeleton_log_start_)
                      .count();
  for (int i = 0; i < kSkeletonCount; ++i) {
    const Skeleton& skeleton = skeletons_[i];
    if (skeleton.state != kTrackingStateTracked) {
      continue;
    }
    std::fprintf(skeleton_log_, "%lld,%u,%d,%u,%X", static_cast<long long>(ms),
                 skeleton_log_frames_, i, skeleton.tracking_id,
                 skeleton.quality_flags);
    for (const Joint& joint : skeleton.joints) {
      std::fprintf(skeleton_log_, ",%u,%.3f,%.3f,%.3f", joint.state, joint.x,
                   joint.y, joint.z);
    }
    std::fputc('\n', skeleton_log_);
  }
  ++skeleton_log_frames_;
}

std::vector<NuiHud::TrackedSkeleton> NuiHud::GetTrackedSkeletons() {
  std::vector<TrackedSkeleton> tracked;
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::chrono::steady_clock::now() - frame_time_ > kFrameTimeout) {
    return tracked;
  }
  for (const Skeleton& skeleton : skeletons_) {
    if (skeleton.state != kTrackingStateTracked) {
      continue;
    }
    TrackedSkeleton& out = tracked.emplace_back();
    for (int j = 0; j < kJointCount; ++j) {
      out.joints[j][0] = skeleton.joints[j].x;
      out.joints[j][1] = skeleton.joints[j].y;
      out.joints[j][2] = skeleton.joints[j].z;
      out.states[j] = skeleton.joints[j].state;
    }
  }
  return tracked;
}

NuiHud::~NuiHud() {
  if (skeleton_log_) {
    std::fclose(skeleton_log_);
  }
}

bool NuiHud::GetHandCursor(uint32_t tracking_id, float& x_out, float& y_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::chrono::steady_clock::now() - frame_time_ > kFrameTimeout) {
    return false;
  }
  const Skeleton* skeleton = nullptr;
  for (const Skeleton& candidate : skeletons_) {
    if (candidate.state != kTrackingStateTracked) {
      continue;
    }
    if (candidate.tracking_id == tracking_id) {
      skeleton = &candidate;
      break;
    }
    // Otherwise the player the cursor followed, else the first one.
    if (!skeleton || candidate.tracking_id == cursor_tracking_id_) {
      skeleton = &candidate;
    }
  }
  if (!skeleton) {
    return false;
  }
  if (skeleton->tracking_id != cursor_tracking_id_) {
    cursor_tracking_id_ = skeleton->tracking_id;
    cursor_hand_ = -1;
  }
  const Joint& shoulders = skeleton->joints[kShoulderCenter];
  const Joint& left = skeleton->joints[kHandLeft];
  const Joint& right = skeleton->joints[kHandRight];
  // The hand held out further toward the sensor points.
  const bool left_seen = left.state != 0;
  const bool right_seen = right.state != 0;
  if (!left_seen && !right_seen) {
    return false;
  }
  int hand = cursor_hand_;
  if (hand < 0 || (hand == kHandLeft ? !left_seen : !right_seen)) {
    hand = !left_seen         ? kHandRight
           : !right_seen      ? kHandLeft
           : left.z < right.z ? kHandLeft
                              : kHandRight;
  } else {
    const Joint& current = skeleton->joints[hand];
    const Joint& other = hand == kHandLeft ? right : left;
    if (other.state != 0 && other.z < current.z - kHandSwitchDistance) {
      hand = hand == kHandLeft ? kHandRight : kHandLeft;
    }
  }
  const Joint& pointing = skeleton->joints[hand];
  const float x =
      std::clamp(0.5f + (pointing.x - shoulders.x) / kReachX, 0.0f, 1.0f);
  const float y = std::clamp(
      0.5f - (pointing.y - (shoulders.y - kCenterBelowShoulders)) / kReachY,
      0.0f, 1.0f);
  if (hand != cursor_hand_) {
    cursor_hand_ = hand;
    cursor_x_ = x;
    cursor_y_ = y;
  } else {
    cursor_x_ += (x - cursor_x_) * kSmoothing;
    cursor_y_ += (y - cursor_y_) * kSmoothing;
  }
  x_out = cursor_x_;
  y_out = cursor_y_;
  return true;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
