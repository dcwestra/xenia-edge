/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_KINECT_LIBUSB_H_
#define XENIA_KERNEL_NUI_KINECT_LIBUSB_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_LINUX
#include <dlfcn.h>

// Only the declarations: the system's libusb is loaded at run time, so a
// build doesn't need its development package.
#include "third_party/libusb/libusb/libusb.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace nui {

#define XE_LIBUSB_FUNCTIONS(F)              \
  F(libusb_init)                            \
  F(libusb_exit)                            \
  F(libusb_open_device_with_vid_pid)        \
  F(libusb_close)                           \
  F(libusb_get_device)                      \
  F(libusb_get_active_config_descriptor)    \
  F(libusb_free_config_descriptor)          \
  F(libusb_kernel_driver_active)            \
  F(libusb_detach_kernel_driver)            \
  F(libusb_attach_kernel_driver)            \
  F(libusb_claim_interface)                 \
  F(libusb_release_interface)               \
  F(libusb_control_transfer)                \
  F(libusb_bulk_transfer)                   \
  F(libusb_alloc_transfer)                  \
  F(libusb_free_transfer)                   \
  F(libusb_submit_transfer)                 \
  F(libusb_cancel_transfer)                 \
  F(libusb_handle_events_timeout_completed) \
  F(libusb_error_name)

// libusb, loaded when a Kinect device is first opened so it isn't a dependency
// otherwise.
struct Libusb {
#define XE_LIBUSB_DECLARE(name) decltype(&::name) name = nullptr;
  XE_LIBUSB_FUNCTIONS(XE_LIBUSB_DECLARE)
#undef XE_LIBUSB_DECLARE

  bool Load() {
    void* library = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      return false;
    }
#define XE_LIBUSB_RESOLVE(fn)                               \
  fn = reinterpret_cast<decltype(fn)>(dlsym(library, #fn)); \
  if (!fn) {                                                \
    XELOGW("Kinect: libusb-1.0 has no {}", #fn);            \
    return false;                                           \
  }
    XE_LIBUSB_FUNCTIONS(XE_LIBUSB_RESOLVE)
#undef XE_LIBUSB_RESOLVE
    return true;
  }
};

#undef XE_LIBUSB_FUNCTIONS

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XE_PLATFORM_LINUX

#endif  // XENIA_KERNEL_NUI_KINECT_LIBUSB_H_
