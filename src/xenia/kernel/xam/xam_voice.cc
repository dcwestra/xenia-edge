/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cfloat>

#include "xenia/base/byte_order.h"
#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/nui_microphones.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xevent.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamVoiceIsActiveProcess_entry() {
  // Returning 0 here will short-circuit a bunch of voice stuff.
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceIsActiveProcess, kNone, kStub);

dword_result_t XamVoiceCreate_entry(dword_t user_index,
                                    dword_t max_attached_packets,  // 0xF
                                    lpdword_t out_voice_ptr) {
  // Null out the ptr.
  out_voice_ptr.Zero();
  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamVoiceCreate, kNone, kStub);

dword_result_t XamVoiceClose_entry(lpunknown_t voice_ptr) { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceClose, kNone, kStub);

dword_result_t XamVoiceHeadsetPresent_entry(lpunknown_t voice_ptr) { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceHeadsetPresent, kNone, kStub);

dword_result_t XamVoiceSubmitPacket_entry(lpdword_t unk1, dword_t unk2,
                                          lpdword_t unk3) {
  // also may return 0xD000009D
  return 0x800700AA;
}
DECLARE_XAM_EXPORT1(XamVoiceSubmitPacket, kNone, kStub);

// The Kinect's microphone array, after Davi Carlos's research into xam in
// kixenia (https://github.com/davicarlos/kixenia, notes under CC BY 4.0). The
// addresses are the functions in xam 17559.

// 2 without the array (0x816FBBE8); Dance Central 2 checks for that.
dword_result_t XamVoiceGetMicArrayStatus_entry() {
  return kernel_state()->nui_microphones()->available() ? 1 : 2;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayStatus, kNone, kImplemented);

// Queues a request for the next 16 ms of the microphones, the event set once
// it's filled; see NuiMicrophones for the request.
dword_result_t XamVoiceGetMicArrayAudioEx_entry(dword_t request,
                                                dword_t event) {
  if (!request) {
    return X_E_INVALIDARG;
  }
  return kernel_state()->nui_microphones()->ReadWindow(request, event);
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayAudioEx, kNone, kImplemented);

// The older form (0x816FBCA0): an event handle instead of the event, and xam
// fills the request's words at +0x2C to +0x3C with 0, 5, 5, 5, 5 before
// handing it to the same worker.
dword_result_t XamVoiceGetMicArrayAudio_entry(lpdword_t request,
                                              dword_t event_handle) {
  if (!request) {
    return X_E_INVALIDARG;
  }
  request[0xB] = 0;
  for (uint32_t i = 0xC; i <= 0xF; ++i) {
    request[i] = 5;
  }
  uint32_t event_ptr = 0;
  if (event_handle) {
    auto event =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    if (!event) {
      return X_STATUS_INVALID_HANDLE;
    }
    event_ptr = event->guest_object();
  }
  return kernel_state()->nui_microphones()->ReadWindow(request.guest_address(),
                                                       event_ptr);
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayAudio, kNone, kImplemented);

// Windows that would overrun are dropped on the host and missing ones come
// as silence, so the title never sees an underrun (0x816FBD50).
dword_result_t XamVoiceGetMicArrayUnderrunStatus_entry(lpdword_t flags,
                                                       lpdword_t count) {
  if (flags) {
    *flags = 0;
  }
  if (count) {
    *count = 0;
  }
  return 0x10000000;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayUnderrunStatus, kNone, kImplemented);

// Points the array's beam, in radians from straight ahead within 50 degrees
// either way, or back to the sensor's choice with FLT_MAX (0x816FBD60). Only
// the sensor's processed mix is beamed, and it isn't used.
dword_result_t XamVoiceSetMicArrayBeamAngle_entry(double_t angle) {
  const float radians = float(angle);
  constexpr float kMaxBeamAngle = 0.873f;
  if (radians != FLT_MAX &&
      (radians < -kMaxBeamAngle || radians > kMaxBeamAngle)) {
    return X_E_INVALIDARG;
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayBeamAngle, kNone, kSketchy);

// Who the title considers idle at the array, kept for xam's own voice
// processing (0x816FB9C0).
dword_result_t XamVoiceSetMicArrayIdleUsers_entry(dword_t idle_users,
                                                  dword_t signal) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayIdleUsers, kNone, kSketchy);

// Muting only times xam's chat talk timers (0x816FBA08), which aren't here.
dword_result_t XamVoiceMuteMicArray_entry(dword_t mute) {
  return kernel_state()->nui_microphones()->available() ? 0 : 2;
}
DECLARE_XAM_EXPORT1(XamVoiceMuteMicArray, kNone, kSketchy);

// The array's name in the title's language, xam's string 100 (0x816FBE10);
// here always English.
dword_result_t XamVoiceGetMicArrayFilenameDesc_entry() {
  static uint32_t name_ptr = 0;
  if (!name_ptr) {
    static const char16_t kName[] = u"Kinect Sensor Microphone Array";
    const uint32_t ptr = kernel_memory()->SystemHeapAlloc(sizeof(kName));
    auto* out = kernel_memory()->TranslateVirtual<uint8_t*>(ptr);
    for (size_t i = 0; i < xe::countof(kName); ++i) {
      xe::store_and_swap<uint16_t>(out + i * 2, uint16_t(kName[i]));
    }
    name_ptr = ptr;
  }
  return name_ptr;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayFilenameDesc, kNone, kSketchy);

// Asks with a system dialog to turn the array off (0x816C3750), and returns
// its answer. No dialog here: the array stays on.
dword_result_t XamVoiceDisableMicArray_entry(dword_t user_index) {
  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamVoiceDisableMicArray, kNone, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Voice);
