/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_identity.h"

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/nui_data_store.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace nui {

namespace {
constexpr uint32_t kMessageEnroll = 0x0002C006;
constexpr uint32_t kMessageBindingsChanged = 0x0002C00F;
}  // namespace

NuiIdentity::NuiIdentity(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

NuiIdentity::~NuiIdentity() = default;

void NuiIdentity::SetHandler(uint32_t handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handler_ = handler;
}

void NuiIdentity::RequestEnrollment(uint32_t tracking_id,
                                    uint32_t user_index) {
  uint32_t handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handler = handler_;
    if (!handler || !tracking_id) {
      XELOGW(
          "NUI identity: can't enroll tracking ID {:X} for user {} without "
          "{}",
          tracking_id, user_index, handler ? "a player" : "a title handler");
      return;
    }
    pending_tracking_id_ = tracking_id;
    pending_user_index_ = user_index;
    // The title checks the slot it enrolls them in is theirs before it
    // answers, so it's bound now, to the slot their identification was
    // given.
    for (const MatchAnswer& answer : match_answers_) {
      if (answer.tracking_id == tracking_id &&
          answer.slot < kSlotCount) {
        BindSlot(answer.slot, user_index);
        break;
      }
    }
  }
  XELOGI("NUI identity: asking the title to enroll tracking ID {:X} for "
         "user {}",
         tracking_id, user_index);
  // {tracking ID, user index, flags}.
  CallHandler(handler, kMessageEnroll, {tracking_id, user_index, 1});
}

void NuiIdentity::CallHandler(uint32_t handler, uint32_t message,
                              std::vector<uint32_t> words) {
  KernelState* kernel_state = kernel_state_;
  auto thread = object_ref<XHostThread>(new XHostThread(
      kernel_state, 64 * 1024, 0,
      [kernel_state, handler, message, words]() {
        Memory* memory = kernel_state->memory();
        uint32_t buffer = 0;
        if (!words.empty()) {
          buffer = memory->SystemHeapAlloc(uint32_t(words.size() * 4));
          auto* out = memory->TranslateVirtual<xe::be<uint32_t>*>(buffer);
          for (size_t i = 0; i < words.size(); ++i) {
            out[i] = words[i];
          }
        }
        uint64_t args[] = {message, buffer};
        const uint32_t result = uint32_t(kernel_state->processor()->Execute(
            XThread::GetCurrentThread()->thread_state(), handler, args,
            xe::countof(args)));
        XELOGI("NUI identity: the title's handler took {:08X}: {:08X}",
               message, result);
        // Refused (the player out of view, say), so no answer is coming.
        if (message == kMessageEnroll && int32_t(result) < 0) {
          kernel_state->nui_identity()->OnEnrollmentResult(
              result, words.empty() ? 0 : words[0], kSlotCount);
        }
        if (buffer) {
          memory->SystemHeapFree(buffer);
        }
        return 0;
      },
      kernel_state->GetSystemProcess()));
  thread->set_name("NUI Identity");
  thread->Create();
  std::lock_guard<std::mutex> lock(mutex_);
  request_thread_ = thread;
}

void NuiIdentity::WaitForEnrollment(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!enrollment_answered_.wait_for(lock, timeout, [this]() {
        return pending_user_index_ == kNoUser;
      })) {
    XELOGW("NUI identity: the title didn't answer the enrollment in time");
  }
}

void NuiIdentity::OnEnrollmentResult(uint32_t result, uint32_t tracking_id,
                                     uint32_t slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  enrollment_answered_.notify_all();
  XELOGI("NUI identity: enrollment of tracking ID {:X} finished: {:08X}, "
         "slot {}",
         tracking_id, result, slot);
  if (int32_t(result) < 0 || slot >= kSlotCount ||
      pending_user_index_ == kNoUser) {
    pending_user_index_ = kNoUser;
    return;
  }
  BindSlot(slot, pending_user_index_);
  pending_user_index_ = kNoUser;
  pending_tracking_id_ = 0;
  // Have the title read the bindings again.
  if (handler_) {
    const uint32_t handler = handler_;
    mutex_.unlock();
    CallHandler(handler, kMessageBindingsChanged, {});
    mutex_.lock();
  }
}

void NuiIdentity::OnMatchAnswered(uint32_t tracking_id, uint32_t slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (MatchAnswer& answer : match_answers_) {
    if (answer.tracking_id == tracking_id) {
      answer.slot = slot;
      return;
    }
  }
  match_answers_[next_match_answer_] = {tracking_id, slot};
  next_match_answer_ = (next_match_answer_ + 1) % xe::countof(match_answers_);
}

void NuiIdentity::BindSlot(uint32_t slot, uint32_t user_index) {
  const auto* profile = kernel_state_->xam_state()->GetUserProfile(user_index);
  if (!profile || slot >= kSlotCount) {
    return;
  }
  LoadBindings();
  for (uint64_t& xuid : slot_xuids_) {
    if (xuid == profile->xuid()) {
      xuid = 0;
    }
  }
  slot_xuids_[slot] = profile->xuid();
  XELOGI("NUI identity: slot {} is now {:016X}", slot, profile->xuid());
  SaveBindings();
}

bool NuiIdentity::GetUserIndex(uint32_t slot, uint32_t& user_index_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  LoadBindings();
  if (slot >= kSlotCount || !slot_xuids_[slot]) {
    return false;
  }
  const uint8_t user_index =
      kernel_state_->xam_state()->profile_manager()
          ->GetUserIndexAssignedToProfile(slot_xuids_[slot]);
  if (user_index >= XUserMaxUserCount) {
    return false;
  }
  user_index_out = user_index;
  return true;
}

bool NuiIdentity::GetSlot(uint32_t user_index, uint32_t& slot_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  LoadBindings();
  const auto* profile = kernel_state_->xam_state()->GetUserProfile(user_index);
  if (!profile) {
    return false;
  }
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    if (slot_xuids_[slot] == profile->xuid()) {
      slot_out = slot;
      return true;
    }
  }
  return false;
}

void NuiIdentity::LoadBindings() {
  if (bindings_loaded_) {
    return;
  }
  bindings_loaded_ = true;
  uint64_t version;
  std::vector<uint8_t> data;
  if (!kernel_state_->nui_data_store()->Read(NuiDataStore::Kind::kSystem,
                                             "enrollment_profiles", version,
                                             data) ||
      data.size() < sizeof(slot_xuids_)) {
    return;
  }
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    slot_xuids_[slot] = xe::load_and_swap<uint64_t>(data.data() + slot * 8);
  }
}

void NuiIdentity::SaveBindings() {
  uint8_t data[sizeof(slot_xuids_)];
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    xe::store_and_swap<uint64_t>(data + slot * 8, slot_xuids_[slot]);
  }
  kernel_state_->nui_data_store()->Write(NuiDataStore::Kind::kSystem,
                                         "enrollment_profiles", 1, data,
                                         sizeof(data));
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
