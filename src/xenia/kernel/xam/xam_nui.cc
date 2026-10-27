/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui.h"
#include "xenia/kernel/nui/kinect_camera.h"
#include "xenia/kernel/nui/nui_data_store.h"
#include "xenia/kernel/nui/nui_identity.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

DEFINE_bool(kinect_tilt, true,
            "Let Kinect titles tilt the sensor with its motor, as they do to "
            "find the floor and frame their players.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {
// https://web.cs.ucdavis.edu/~okreylos/ResDev/Kinect/MainPage.html

struct X_NUI_DEVICE_STATUS {
  /* Notes:
     - for one side func of XamNuiGetDeviceStatus
       - if some data address is less than zero then tilt_status = it
       - else another func is called and its return can set
     tilt_status = c0051200 or some value involving DetroitDeviceRequest
       - next PsCamDeviceRequest is called and if its return is less than zero
     then camera_status = return of PsCamDeviceRequest
       - else it equals an unknown local_1c
       - finally McaDeviceRequest is called and if its return is less than zero
     then mic_status = return of McaDeviceRequest
       - else it equals an unknown local_14
     - status_bits can be set to status_bits | 0x44 or | 0x40
  */
  xe::be<uint32_t> camera_status;
  xe::be<uint32_t> tilt_status;
  xe::be<uint32_t> mic_status;
  xe::be<uint32_t> status_bits;
};
static_assert_size(X_NUI_DEVICE_STATUS, 0x10);
// X_NUI_DEVICE_STATUS::status_bits bits titles test.
constexpr uint32_t kNuiDeviceStatusPresent = 0x1;
constexpr uint32_t kNuiDeviceStatusReady = 0x2;

// Get
dword_result_t XamNuiGetDeviceStatus_entry(
    pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  /* Notes:
     - it does return a value that is not always used
     - returns values are X_ERROR_SUCCESS, 0xC0050006, and others
     - 1) On func start *status_ptr = 0, status_ptr->tilt_status = 0,
     status_ptr->mic_status = 0, and status_ptr->status_bits = 0
     - 2) calls XamXStudioRequest(6,&var <- = 0);
     - if return is greater than -1 && var & 0x80000000 != 0 then set
     status_ptr->tilt_status = 0xC000009D, status_ptr->mic_status = 0xC000009D,
     and status_ptr->status_bits = 0x20
     - lots of branching functions after
  */

  status_ptr.Zero();

  const bool kinect_initialized = kernel_state()->nui()->NuiInitialized();

  // Kinect titles (Fable: The Journey) take the sensor as connected only with
  // both of these set, 0x2 being ready to stream.
  status_ptr->status_bits =
      kinect_initialized ? kNuiDeviceStatusPresent | kNuiDeviceStatusReady : 0;
  return kinect_initialized ? X_ERROR_SUCCESS : 0xC0050006;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kStub);

dword_result_t XamXStudioRequest_entry(dword_t request_type,
                                       lpdword_t result_ptr) {
  // Kinect Studio, a development tool for recording and playing back what a
  // Kinect sees, isn't attached. Kinect titles go to the sensor itself on
  // E_INVALIDARG: on another failure, Fable: The Journey (4D530A1A) never
  // reads the tilt motor's status.
  static bool logged = false;
  if (!logged) {
    logged = true;
    XELOGI("XamXStudioRequest({:X}): no Kinect Studio", uint32_t(request_type));
  }
  return X_E_INVALIDARG;
}
DECLARE_XAM_EXPORT2(XamXStudioRequest, kNone, kStub, kHighFrequency);

// Which signed-in user a Kinect ID enrollment slot of the title's identity
// runtime is bound to.
dword_result_t XamUserNuiGetUserIndex_entry(dword_t slot,
                                            lpdword_t user_index_ptr) {
  uint32_t user_index;
  const bool bound =
      kernel_state()->nui_identity()->GetUserIndex(slot, user_index) &&
      kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!bound) {
    return X_E_NO_SUCH_USER;
  }
  if (user_index_ptr) {
    *user_index_ptr = user_index;
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kSketchy);

dword_result_t XamUserNuiGetUserIndexForSignin_entry(lpdword_t user_index) {
  if (!user_index) {
    return X_E_INVALIDARG;
  }

  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    auto profile = kernel_state()->xam_state()->GetUserProfile(i);
    if (profile) {
      *user_index = i;
      return X_E_SUCCESS;
    }
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForSignin, kNone, kImplemented);

dword_result_t XamUserNuiGetUserIndexForBind_entry(lpdword_t user_index) {
  if (!user_index) {
    return X_E_INVALIDARG;
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

struct X_NUI_DEPTH_CALIBRATION {
  uint8_t data[0x30];
};
static_assert_size(X_NUI_DEPTH_CALIBRATION, 0x30);

dword_result_t XamNuiGetDepthCalibration_entry(
    pointer_t<X_NUI_DEPTH_CALIBRATION> depth_data) {
  // always cleared before new data written
  std::memset(depth_data, 0, sizeof(X_NUI_DEPTH_CALIBRATION));
  return X_STATUS_NO_SUCH_FILE | X_FACILITY_NT_BIT;
}
DECLARE_XAM_EXPORT1(XamNuiGetDepthCalibration, kNone, kStub);

// Skeleton
qword_result_t XamNuiSkeletonGetBestSkeletonIndex_entry(int_t unk) {
  return 0xffffffffffffffff;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetBestSkeletonIndex, kNone, kStub);

/* XamNuiCamera Notes
   - most require message calls to xam in 0x0002Bxxx area
*/
dword_result_t XamNuiCameraTiltSetCallback_entry(dword_t callback) {
  kernel_state()->nui()->SetTiltCallback(callback);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

// The Kinect's tilt, which Kinect titles' NUI runtime drives to frame its
// players: when it doesn't know where the floor is, it sweeps the sensor from
// low to high to find it, then aims from the camera's height above it. The
// motor tilts to angles relative to gravity, measured by its accelerometer.
// X_NUI_TILT_STATUS::flags. 0x2 is a move something else made, 0x8 a fault
// and 0x10 a request to search for the floor again, none of which is set.
constexpr uint32_t kNuiTiltMoving = TILT_STATUS_UNK1;
constexpr uint32_t kNuiTiltAtLimit = TILT_STATUS_STALLED;

namespace {
struct NuiTiltState {
  std::mutex mutex;
  // A move just asked for counts as under way for a moment, before the motor
  // reports it.
  std::chrono::steady_clock::time_point tilt_requested;
  bool floor_known = false;
  float floor_normal[3] = {};
  float camera_height = 0.0f;
  int32_t floor_accel[3] = {};
};
NuiTiltState nui_tilt_state;
constexpr auto kNuiTiltStartTime = std::chrono::milliseconds(300);

nui::KinectMotor* GetTiltMotor() {
  return cvars::kinect_tilt ? kernel_state()->kinect_motor() : nullptr;
}

X_HRESULT GetTiltStatus(X_NUI_TILT_STATUS& status) {
  nui::KinectMotor* motor = GetTiltMotor();
  nui::KinectMotor::State state;
  if (!motor || !motor->GetState(state)) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard<std::mutex> lock(nui_tilt_state.mutex);
  std::memset(&status.unknown_08, 0,
              sizeof(status) - offsetof(X_NUI_TILT_STATUS, unknown_08));
  for (int i = 0; i < 3; ++i) {
    status.gravity_long_avg[i] = state.accel[i];
  }
  uint32_t flags = 0;
  if (state.status == nui::KinectMotor::TiltStatus::kMoving ||
      std::chrono::steady_clock::now() - nui_tilt_state.tilt_requested <
          kNuiTiltStartTime) {
    flags |= kNuiTiltMoving;
  } else if (state.status == nui::KinectMotor::TiltStatus::kAtLimit) {
    flags |= kNuiTiltAtLimit;
  }
  if (nui_tilt_state.floor_known) {
    for (int i = 0; i < 3; ++i) {
      status.floor_normal[i] = nui_tilt_state.floor_normal[i];
      status.floor_accel[i] = nui_tilt_state.floor_accel[i];
    }
    status.camera_height = nui_tilt_state.camera_height;
  }
  status.flags = X_TILT_STATUS_FLAGS(flags);
  return X_E_SUCCESS;
}

int32_t TiltDegreesFromAccel(const int32_t accel[3]) {
  return int32_t(std::lround(std::atan2(double(accel[2]), double(accel[1])) *
                             180.0 / 3.14159265358979));
}
}  // namespace

dword_result_t XamNuiCameraTiltGetStatus_entry(
    pointer_t<X_NUI_TILT_STATUS> status_ptr) {
  if (!status_ptr || status_ptr->buffer_size < sizeof(X_NUI_TILT_STATUS)) {
    return X_E_INVALIDARG;
  }
  return GetTiltStatus(*status_ptr);
}
DECLARE_XAM_EXPORT2(XamNuiCameraTiltGetStatus, kNone, kImplemented,
                    kHighFrequency);

// The second output is a status word of its own, not the tilt status block's
// flags: with 0x1 a title gives up tilting for the frame, and with 0x2 its
// engagement takes the sensor as needing attention and drops tracked
// players. Nothing calls for either.
dword_result_t XamNuiCameraElevationGetAngle_entry(lpdword_t angle_ptr,
                                                   lpdword_t status_ptr) {
  X_NUI_TILT_STATUS status = {};
  const X_HRESULT result = GetTiltStatus(status);
  if (XSUCCEEDED(result)) {
    const int32_t accel[3] = {status.gravity_long_avg[0],
                              status.gravity_long_avg[1],
                              status.gravity_long_avg[2]};
    if (angle_ptr) {
      *angle_ptr = uint32_t(TiltDegreesFromAccel(accel));
    }
    if (status_ptr) {
      *status_ptr = 0;
    }
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kImplemented);

dword_result_t XamNuiCameraElevationSetAngle_entry(int_t degrees) {
  nui::KinectMotor* motor = GetTiltMotor();
  if (!motor) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  XELOGI("XamNuiCameraElevationSetAngle({})", int32_t(degrees));
  {
    std::lock_guard<std::mutex> lock(nui_tilt_state.mutex);
    nui_tilt_state.tilt_requested = std::chrono::steady_clock::now();
  }
  return motor->SetTilt(degrees) ? X_E_SUCCESS : X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kImplemented);

dword_result_t XamNuiCameraElevationStopMovement_entry() {
  // The motor has no stop, so it's sent to where it is.
  X_NUI_TILT_STATUS status = {};
  if (XFAILED(GetTiltStatus(status))) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  if (!(status.flags & kNuiTiltMoving)) {
    return X_E_SUCCESS;
  }
  const int32_t accel[3] = {status.gravity_long_avg[0],
                            status.gravity_long_avg[1],
                            status.gravity_long_avg[2]};
  return XamNuiCameraElevationSetAngle_entry(TiltDegreesFromAccel(accel));
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kImplemented);

dword_result_t XamNuiCameraRememberFloor_entry(qword_t normal_xy,
                                               qword_t normal_z_distance,
                                               dword_t unknown) {
  // The floor plane of a NUI skeleton frame: its normal, and the camera's
  // distance from it in millimeters.
  auto as_float = [](uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };
  const float normal[3] = {as_float(uint32_t(normal_xy >> 32)),
                           as_float(uint32_t(normal_xy)),
                           as_float(uint32_t(normal_z_distance >> 32))};
  const float distance = as_float(uint32_t(normal_z_distance));

  const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                 normal[2] * normal[2]);
  if (!(distance > 0.0f && distance < 10000.0f) ||
      std::abs(length - 1.0f) > 0.1f) {
    return X_E_INVALIDARG;
  }
  X_NUI_TILT_STATUS status = {};
  if (XFAILED(GetTiltStatus(status))) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard<std::mutex> lock(nui_tilt_state.mutex);
  const float camera_height = distance;
  if (!nui_tilt_state.floor_known ||
      std::abs(nui_tilt_state.camera_height - camera_height) > 50.0f) {
    XELOGI("XamNuiCameraRememberFloor: the camera is {:.0f} mm above the floor",
           camera_height);
  }
  nui_tilt_state.floor_known = true;
  for (int i = 0; i < 3; ++i) {
    nui_tilt_state.floor_normal[i] = normal[i];
    nui_tilt_state.floor_accel[i] = status.gravity_long_avg[i];
  }
  nui_tilt_state.camera_height = camera_height;
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kImplemented);

dword_result_t XamNuiCameraTiltReportStatus_entry(dword_t message,
                                                  lpvoid_t status_ptr) {
  // How the title's tilting is going, for the system's Kinect UI.
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kSketchy);

dword_result_t XamNuiCameraGetTiltControllerType_entry() {
  /* Notes:
     - Earlier decompile notes: DetroitDeviceRequest and
       (LZCOUNT(result) << 0x20) >> 0x25.
  */
  // TODO(boma): Get the controller type from connected device.
  // Currently only reports no controller or type 1.
  const bool kinect_initialized =
      kernel_state()->xconfig()->ReadSetting<uint32_t>(
          X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS) &
      X_RETAIL_FLAGS::KinectInitialized;
  return kinect_initialized;
}
DECLARE_XAM_EXPORT1(XamNuiCameraGetTiltControllerType, kNone, kStub);

dword_result_t XamNuiCameraSetFlags_entry(qword_t unk1, dword_t unk2) {
  X_STATUS result = X_E_DEVICE_NOT_CONNECTED;
  int controller_type = XamNuiCameraGetTiltControllerType_entry();

  if (controller_type == 1) {
    X_NUI_TILT_STATUS tilt_status = {};
    tilt_status.buffer_size = sizeof(tilt_status);
    tilt_status.signature = make_fourcc("XtSs");  // (XtSs)?
    result = XamNuiCameraTiltGetStatus_entry(&tilt_status);
    if (!XSUCCEEDED(result)) {
      return result;
    }
    // TODO(knuckleslee): Apply the flags once their meaning is known.
    return X_E_FAIL;  // until implementation improves
  }
  if (controller_type == 2) {
    return X_ERROR_SUCCESS;
  }
  if (controller_type >= 3) {
    return X_E_FAIL;  // until implementation improves
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

dword_result_t XamIsNuiUIActive_entry() {
  return kernel_state()->xam_state()->is_xam_dialog_present_.load();
}
DECLARE_XAM_EXPORT1(XamIsNuiUIActive, kNone, kImplemented);

dword_result_t XamNuiIsDeviceReady_entry() {
  /* device_state Notes:
   - used with XNotifyBroadcast(kXNotificationSystemNUIHardwareStatusChanged,
   device_state)
   - known values:
     - 0x0001
     - 0x0004
     - 0x0040
  */

  return kernel_state()->nui()->NuiInitialized();
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamIsNuiAutomationEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNuiAutomationEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamIsNatalPlaybackEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNatalPlaybackEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamNuiIsChatMicEnabled_entry() {
  /* Notes:
     - call ExGetXConfigSetting(7,9,&xconfig_nui,0x1c,local_40);
     - check xconfig_nui.flag & 0x8
     - return True or false based on result
  */
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiIsChatMicEnabled, kNone, kImplemented);

/* HUD Notes:
   - XamNuiHudGetEngagedTrackingID, XamNuiHudIsEnabled,
   XamNuiHudSetEngagedTrackingID, XamNuiHudInterpretFrame, and
   XamNuiHudGetEngagedEnrollmentIndex all utilize the same data address
*/
dword_result_t XamNuiHudSetEngagedTrackingID_entry(dword_t id) {
  auto nui_ = kernel_state()->nui();
  if (id) {
    if (!nui_->GetNUIDataPtr()) {
      return X_E_FAIL;
    }
    nui_->SetEngagedTrackingId(id);
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudSetEngagedTrackingID, kNone, kImplemented);

qword_result_t XamNuiHudGetEngagedTrackingID_entry() {
  auto nui_ = kernel_state()->nui();
  if (nui_->GetNUIDataPtr()) {
    return nui_->GetEngagedTrackingId();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);

dword_result_t XamNuiHudIsEnabled_entry() {
  auto nui_ = kernel_state()->nui();
  bool result = XamNuiIsDeviceReady_entry();
  if (nui_->GetNUIDataPtr() && nui_->GetUnknown2() && result) {
    return true;
  }
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

dword_result_t XamNuiHudGetInitializeFlags_entry() {
  return kernel_state()->nui()->GetInitFlags();
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

void XamNuiHudGetVersions_entry(lpqword_t xam_version_ptr,
                                lpqword_t xtl_version_ptr) {
  auto nui_ = kernel_state()->nui();
  if (xam_version_ptr) {
    *xam_version_ptr = nui_->GetNUIVersion(0);
  }
  if (xtl_version_ptr) {
    *xtl_version_ptr = nui_->GetNUIVersion(1);
  }
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

/* XamNuiIdentity Notes:
   - most require message calls to xam in 0x0002Cxxx area
*/
qword_result_t XamNuiIdentityGetSessionId_entry() {
  auto nui_ = kernel_state()->nui();
  if (nui_->GetSessionId() == 0) {
    // xboxkrnl::XeCryptRandom_entry(NUI_Session_Id, 8);
    nui_->SetSessionId(0xDEADF00DDEADF00D);
  }
  return nui_->GetSessionId();
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kImplemented);

dword_result_t XamNuiIdentityEnrollForSignIn_entry(dword_t unk1, qword_t unk2,
                                                   qword_t unk3, dword_t unk4) {
  /* Notes:
     - Decompiler issues so double check
  */
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [2]
  // buffer[0] = unk
  // var = unk4
  // return func(0xfe,0x2c010,buffer,0xc,unk3);
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityEnrollForSignIn, kNone, kStub);

dword_result_t XamNuiIdentityAbort_entry(dword_t unk) {
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [4]
  // buffer[0] = unk
  // return func(0xfe,0x2c00e,buffer,4,0)
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityAbort, kNone, kStub);

// A profile's biometric (Kinect ID) data, which Kinect titles' identity
// runtime writes when it enrolls a player and reads to recognize them.
static X_RESULT CompleteBiometricRequest(uint32_t overlapped_ptr,
                                         X_RESULT result) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

dword_result_t XamReadBiometricData_entry(qword_t xuid, lpdword_t version_ptr,
                                          lpvoid_t buffer_ptr,
                                          lpdword_t size_ptr,
                                          dword_t overlapped_ptr) {
  if (!size_ptr) {
    return CompleteBiometricRequest(overlapped_ptr, X_ERROR_INVALID_PARAMETER);
  }
  uint64_t version = 0;
  std::vector<uint8_t> data;
  if (!kernel_state()->nui_data_store()->Read(
          nui::NuiDataStore::Kind::kProfileBiometric,
          nui::NuiDataStore::ProfileKey(xuid), version, data)) {
    return CompleteBiometricRequest(overlapped_ptr, X_ERROR_NOT_FOUND);
  }
  if (version_ptr) {
    *version_ptr = uint32_t(version);
  }
  const uint32_t capacity = *size_ptr;
  *size_ptr = uint32_t(data.size());
  if (!buffer_ptr || capacity < data.size()) {
    return CompleteBiometricRequest(overlapped_ptr,
                                    X_ERROR_INSUFFICIENT_BUFFER);
  }
  std::memcpy(buffer_ptr, data.data(), data.size());
  return CompleteBiometricRequest(overlapped_ptr, X_ERROR_SUCCESS);
}
DECLARE_XAM_EXPORT1(XamReadBiometricData, kUserProfiles, kImplemented);

dword_result_t XamWriteBiometricData_entry(qword_t xuid, lpdword_t version_ptr,
                                           lpvoid_t buffer_ptr,
                                           lpdword_t size_ptr,
                                           dword_t overlapped_ptr) {
  if (!version_ptr || !size_ptr || (*size_ptr && !buffer_ptr)) {
    return CompleteBiometricRequest(overlapped_ptr, X_ERROR_INVALID_PARAMETER);
  }
  XELOGI("XamWriteBiometricData({:016X}): version {:X}, {} bytes",
         uint64_t(xuid), uint32_t(*version_ptr), uint32_t(*size_ptr));
  const bool written = kernel_state()->nui_data_store()->Write(
      nui::NuiDataStore::Kind::kProfileBiometric,
      nui::NuiDataStore::ProfileKey(xuid), *version_ptr,
      buffer_ptr.as<const uint8_t*>(), *size_ptr);
  return CompleteBiometricRequest(
      overlapped_ptr, written ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED);
}
DECLARE_XAM_EXPORT1(XamWriteBiometricData, kUserProfiles, kImplemented);

dword_result_t XamUserNuiEnableBiometric_entry(dword_t user_index,
                                               int_t enable) {
  // Whether the profile signs in with Kinect ID. Titles enrolling a player
  // turn it on once they've written the player's biometric data.
  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }
  XELOGI("XamUserNuiEnableBiometric({}, {})", uint32_t(user_index),
         int32_t(enable));
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kSketchy);

dword_result_t XamUserNuiGetEnrollmentIndex_entry(dword_t user_index,
                                                  lpdword_t index_ptr) {
  // Which of the identity runtime's enrollments is the profile's, for one
  // the console has already matched. Titles load the profile's identity data
  // themselves without it.
  uint32_t slot;
  const bool bound = kernel_state()->nui_identity()->GetSlot(user_index, slot);
  if (!bound) {
    return X_E_NO_SUCH_USER;
  }
  if (index_ptr) {
    *index_ptr = slot;
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kSketchy);

struct X_NUI_PLAYER_ENGAGEMENT_UPDATE {
  uint8_t data[0x1C];
};
static_assert_size(X_NUI_PLAYER_ENGAGEMENT_UPDATE, 0x1C);

void XamNuiPlayerEngagementUpdate_entry(
    lpunknown_t unk1, unknown_t unk2,
    pointer_t<X_NUI_PLAYER_ENGAGEMENT_UPDATE> engagement_data) {
  // always cleared before new data written
  std::memset(engagement_data, 0, sizeof(X_NUI_PLAYER_ENGAGEMENT_UPDATE));
}
DECLARE_XAM_EXPORT1(XamNuiPlayerEngagementUpdate, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
