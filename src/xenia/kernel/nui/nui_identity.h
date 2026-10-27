/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_IDENTITY_H_
#define XENIA_KERNEL_NUI_NUI_IDENTITY_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "xenia/kernel/xobject.h"

namespace xe {
namespace kernel {

class KernelState;
class XHostThread;

namespace nui {

// The system's side of Kinect ID. A Kinect title's identity runtime
// recognizes players itself, in eight enrollment slots it fills from the
// profiles' biometric data, and registers a handler (message 0x2C009) for what
// the system asks of it. Signing a Kinect player in asks it to enroll them for
// the profile (0x2C006); it answers with the slot it enrolled them in
// (0x2C003), which is how the system knows which slot is whose.
class NuiIdentity {
 public:
  static constexpr uint32_t kSlotCount = 8;

  explicit NuiIdentity(KernelState* kernel_state);
  ~NuiIdentity();

  void SetHandler(uint32_t handler);
  // Asks the title to enroll the player with the tracking ID for the profile
  // signed in as user_index.
  void RequestEnrollment(uint32_t tracking_id, uint32_t user_index);
  // The slot the system gave the player with the tracking ID when the title
  // asked who they are (0x2C00D): theirs, or the free one to enroll them in.
  void OnMatchAnswered(uint32_t tracking_id, uint32_t slot);
  // Waits for the enrollment asked for to be answered, if there is one.
  void WaitForEnrollment(std::chrono::milliseconds timeout);
  // What the title answered an enrollment with (0x2C003).
  void OnEnrollmentResult(uint32_t result, uint32_t tracking_id, uint32_t slot);

  bool GetUserIndex(uint32_t slot, uint32_t& user_index_out);
  bool GetSlot(uint32_t user_index, uint32_t& slot_out);

 private:
  // Calls the title's handler with the message and a buffer of the words, on
  // a thread of its own: it's guest code, and the title may be holding its
  // identity runtime's lock where it tells the system something.
  void CallHandler(uint32_t handler, uint32_t message,
                   std::vector<uint32_t> words);

  static constexpr uint32_t kNoUser = 0xFF;

  // The bindings are kept with the identity runtime's database, which titles
  // keep from run to run.
  void LoadBindings();
  void SaveBindings();

  KernelState* kernel_state_;
  std::mutex mutex_;
  std::condition_variable enrollment_answered_;
  uint32_t handler_ = 0;
  // The profile each slot is bound to, or 0.
  bool bindings_loaded_ = false;
  uint64_t slot_xuids_[kSlotCount] = {};
  void BindSlot(uint32_t slot, uint32_t user_index);

  // The last slot answered for each tracking ID.
  struct MatchAnswer {
    uint32_t tracking_id;
    uint32_t slot;
  };
  MatchAnswer match_answers_[8] = {};
  uint32_t next_match_answer_ = 0;
  // The enrollment asked for and not answered yet.
  uint32_t pending_tracking_id_ = 0;
  uint32_t pending_user_index_ = kNoUser;
  object_ref<XHostThread> request_thread_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_IDENTITY_H_
