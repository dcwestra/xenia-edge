/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/kinect_camera.h"
#include "xenia/kernel/nui/pscam_device.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

DEFINE_bool(log_kinect, false,
            "Log the Kinect camera requests titles make, with their "
            "parameters.",
            "Kernel");
DECLARE_bool(allow_nui_initialization);

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XUsbcamCreate_entry(dword_t buffer,
                                   dword_t buffer_size,  // 0x4B000 640x480?
                                   lpdword_t handle_out) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamCreate, kNone, kStub);

enum class XUsbCamState : uint32_t {
  CamNotConnected = 0x00000000,
  CamInitilizied = 0x00000001,
  CamConnected = 0x00000002,
};

dword_result_t XUsbcamGetState_entry() {
  return static_cast<uint32_t>(XUsbCamState::CamNotConnected);
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamGetState, kNone, kStub);

// The Kinect camera (PrimeSense) interface. A title's NUI runtime makes every
// request through it, each a structure starting with the request code, +16
// the title's own context, and +20 a completion routine for those completed
// later. See also docs/nui.md of ryantimjohn/xenia-canary, which agrees with
// what Fable: The Journey (4D530A1A) asks.
enum class PsCamRequest : uint32_t {
  // Out +4: the sensor's state, polled every frame. 4 is ready, on which a
  // title starts its streams, and 2 is streaming. Still 4 once a title has
  // started, it takes the sensor as stopped and its NUI runtime goes idle.
  kGetState = 0x00,
  // A frame buffer to fill, completed through the routine it names.
  kQueueFrameBuffer = 0x05,
  // Sets the camera's timing, completed later.
  kSetTiming = 0x0F,
  // A 8192-entry table of the millimeters each raw depth value stands for,
  // which the skeletal tracker takes as its metric depth. Without it, the
  // title makes an identity table.
  kGetDepthTable = 0x18,
  // Fourteen timing values in microseconds, which the runtime multiplies by
  // 50 into timebase ticks.
  kGetTiming = 0x19,
  // Sets the color stream's settings, completed later: from +28, flags, and
  // at +36 1 for a window of the sensor's full resolution rather than all of
  // it, whose center is +40, +44 full-resolution pixels from the sensor's
  // (see PsCamDevice::ColorSettings), then the exposure: red, green and blue
  // gains at +76..+84 and the exposure time at +88, 16.16 fixed point, valid
  // where the flags' bits 1-3 and 0 are set.
  kSetColorSettings = 0x23,
  // Reads back the settings 0x23 set.
  kGetColorSettings = 0x24,
};
constexpr uint32_t kPsCamStateStreaming = 2;
constexpr uint32_t kPsCamStateReady = 4;
// Of the timing values, only the frame period (+36) is ever tested: zero
// skips the end of the camera's configuration and the runtime's per-frame
// graphics and exposure work.
constexpr uint32_t kPsCamTimingFramePeriodIndex = 8;
constexpr uint32_t kPsCamFramePeriodUs = 33333;

dword_result_t PsCamDeviceRequest_entry(lpvoid_t request_ptr,
                                        const ppc_context_t& ctx) {
  if (!request_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto request = request_ptr.as<xe::be<uint32_t>*>();
  const uint32_t code = request[0];
  if (cvars::log_kinect && code != uint32_t(PsCamRequest::kGetState)) {
    std::string bytes;
    const uint8_t* data = request_ptr.as<const uint8_t*>();
    for (uint32_t i = 0; i < 64; ++i) {
      bytes += fmt::format("{}{:02X}", i && !(i & 3) ? " " : "", data[i]);
    }
    XELOGI("log_kinect: PsCamDeviceRequest {:02X} at {:08X} from {:08X}: {}",
           code, request_ptr.guest_address(), uint32_t(ctx->lr), bytes);
  }
  if (!cvars::allow_nui_initialization) {
    return X_E_FAIL;
  }
  nui::PsCamDevice* device = kernel_state()->pscam_device();
  switch (PsCamRequest(code)) {
    case PsCamRequest::kGetState:
      request[1] = !device->IsAvailable()  ? 0
                   : device->IsStreaming() ? kPsCamStateStreaming
                                           : kPsCamStateReady;
      return X_STATUS_SUCCESS;
    case PsCamRequest::kQueueFrameBuffer:
      return device->QueueFrameBuffer(request_ptr.guest_address());
    case PsCamRequest::kGetDepthTable: {
      // +8 the table, +0xC its entry count. The title reads distances under
      // 4000 as valid, so millimeters.
      const uint32_t table_ptr = request[2];
      const uint32_t count = std::min(uint32_t(request[3]), uint32_t(8192));
      if (!table_ptr) {
        return X_STATUS_INVALID_PARAMETER;
      }
      auto table =
          kernel_state()->memory()->TranslateVirtual<xe::be<uint16_t>*>(
              table_ptr);
      for (uint32_t raw = 0; raw < count; ++raw) {
        table[raw] = nui::KinectRawDepthToMillimeters(raw);
      }
      return X_STATUS_SUCCESS;
    }
    case PsCamRequest::kGetTiming:
      // +4: the values.
      for (uint32_t i = 0; i < 14; ++i) {
        request[1 + i] =
            i == kPsCamTimingFramePeriodIndex ? kPsCamFramePeriodUs : 0;
      }
      return X_STATUS_SUCCESS;
    case PsCamRequest::kGetColorSettings:
      // The settings as 0x23 sends them, at +28: flags, whose low four bits
      // mark the four exposure values at +76 valid (or the runtime asks
      // again), the resolution, the window (off), then the exposure's 16.16
      // fixed point limits and values. A value of 0 would read back as no
      // exposure at all.
      request[7] = 0x8000000F;
      request[8] = 2;
      for (uint32_t i = 9; i < 15; ++i) {
        request[i] = 0;
      }
      request[15] = 0x1999;
      request[16] = 0x1999;
      request[17] = 0xCCCC;
      request[18] = 0xCCCC;
      for (uint32_t i = 19; i < 23; ++i) {
        request[i] = 0x10000;
      }
      request[23] = 0;
      return X_STATUS_SUCCESS;
    case PsCamRequest::kSetColorSettings: {
      nui::PsCamDevice::ColorSettings settings;
      settings.window = request[9] == 1;
      settings.window_x = int32_t(uint32_t(request[10]));
      settings.window_y = int32_t(uint32_t(request[11]));
      const uint32_t flags = request[7];
      for (uint32_t i = 0; i < 3; ++i) {
        if (flags & (2u << i)) {
          settings.gains[i] = float(request[19 + i]) / 65536.0f;
        }
      }
      device->SetColorSettings(settings);
      return device->QueueAsyncCompletion(request_ptr.guest_address());
    }
    case PsCamRequest::kSetTiming:
      // Nothing to set: the camera times and exposes itself. The runtime
      // waits for the completion routine, though, which clears its "a change
      // is in flight" flags.
      return device->QueueAsyncCompletion(request_ptr.guest_address());
    default:
      // Not understood yet - accepted, to see what the title asks next.
      return X_STATUS_SUCCESS;
  }
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Usbcam);
