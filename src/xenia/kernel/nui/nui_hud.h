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
#include <mutex>

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

  // The message's skeletons, as the title sent them (big-endian).
  void OnSkeletonFrame(const uint8_t* message, uint32_t size);

  // Where the hand of the player with the tracking ID (any tracked player for
  // 0 or an ID not in view) points, 0 to 1 across and down. False with no
  // tracked player, or none recently.
  bool GetHandCursor(uint32_t tracking_id, float& x_out, float& y_out);

 private:
  struct Joint {
    float x, y, z;
    uint32_t state;  // 0 not tracked, 1 inferred, 2 tracked.
  };
  struct Skeleton {
    uint32_t state;  // 2 tracked.
    uint32_t tracking_id;
    Joint joints[kJointCount];
  };

  std::mutex mutex_;
  Skeleton skeletons_[kSkeletonCount] = {};
  std::chrono::steady_clock::time_point frame_time_;
  // The cursor's hand and its smoothed position.
  uint32_t cursor_tracking_id_ = 0;
  int cursor_hand_ = -1;
  float cursor_x_ = 0.5f;
  float cursor_y_ = 0.5f;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_HUD_H_
