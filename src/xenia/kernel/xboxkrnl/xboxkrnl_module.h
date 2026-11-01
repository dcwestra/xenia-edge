/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULE_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULE_H_

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ordinals.h"

// All of the exported functions:
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"

namespace xe {
class Exception;

namespace kernel {
namespace xboxkrnl {

// Hands an access violation in guest code at pc to a __try around it, by
// diverting the thread to the guest's filter. False if no guest handler can
// take it, and once the guest's handlers have all declined it.
bool DivertGuestAccessViolation(Exception* ex, uint32_t pc);

class XboxkrnlModule : public KernelModule {
 public:
  static constexpr size_t kExLoadedImageNameSize = 255 + 1;
  static constexpr size_t kExLoadedCommandLineSize =
      1024;  // Based on max size provided for XamLoaderGetLaunchData

  XboxkrnlModule(Emulator* emulator, KernelState* kernel_state);
  virtual ~XboxkrnlModule();

  static void RegisterExportTable(xe::cpu::ExportResolver* export_resolver);

  bool SendPIXCommand(const char* cmd);

  void set_pix_function(uint32_t addr) { pix_function_ = addr; }

 protected:
  uint32_t pix_function_ = 0;
};

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULE_H_
