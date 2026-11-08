/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_HUD_H_
#define XENIA_KERNEL_NUI_NUI_HUD_H_

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace xe {
namespace kernel {
namespace nui {

// What the system's Kinect UI sees of the players: the skeleton frames a
// Kinect title's NUI runtime hands xam every frame (message 0x2B002), from
// which a hand cursor drives Kinect dialogs as on a console.
class NuiHud {
 public:
  static constexpr int kSkeletonCount = 6;
  static constexpr int kJointCount = 20;

  ~NuiHud();

  // The message's skeletons, as the title sent them (big-endian).
  void OnSkeletonFrame(const uint8_t* message, uint32_t size);

  // Where the hand of the player with the tracking ID (any tracked player for
  // 0 or an ID not in view) points, 0 to 1 across and down. False with no
  // tracked player, or none recently.
  bool GetHandCursor(uint32_t tracking_id, float& x_out, float& y_out);

  // A player the title tracks, as it last sent them, for the Kinect view:
  // skeleton space, meters from the sensor, x mirrored against its picture.
  struct TrackedSkeleton {
    float joints[kJointCount][3];
    uint32_t states[kJointCount];  // 0 not tracked, 1 inferred, 2 tracked.
  };
  // The players tracked in the last frame, if one came recently.
  std::vector<TrackedSkeleton> GetTrackedSkeletons();

 private:
  struct Joint {
    float x, y, z;
    uint32_t state;  // 0 not tracked, 1 inferred, 2 tracked.
  };
  struct Skeleton {
    uint32_t state;  // 2 tracked.
    uint32_t tracking_id;
    uint32_t quality_flags;
    Joint joints[kJointCount];
  };

  // Appends the tracked skeletons to kinect_skeleton_log, if it's set.
  void LogSkeletons();

  std::mutex mutex_;
  Skeleton skeletons_[kSkeletonCount] = {};
  std::chrono::steady_clock::time_point frame_time_;
  // The cursor's hand and its smoothed position.
  uint32_t cursor_tracking_id_ = 0;
  int cursor_hand_ = -1;
  float cursor_x_ = 0.5f;
  float cursor_y_ = 0.5f;

  FILE* skeleton_log_ = nullptr;
  bool skeleton_log_failed_ = false;
  std::chrono::steady_clock::time_point skeleton_log_start_;
  uint32_t skeleton_log_frames_ = 0;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_HUD_H_
