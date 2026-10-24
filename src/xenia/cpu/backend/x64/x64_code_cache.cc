/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_code_cache.h"

#include <cstring>

#if !XE_PLATFORM_WIN32
#include <unistd.h>

#include <cstdio>
#include <mutex>

#include "xenia/base/cvar.h"
#include "xenia/cpu/function.h"

DEFINE_bool(perf_map, false,
            "Write /tmp/perf-<pid>.map naming every JIT-compiled guest "
            "function, so Linux perf and similar profilers attribute samples "
            "in translated code to guest functions instead of anonymous "
            "memory. Diagnostic only.",
            "CPU");
#endif  // !XE_PLATFORM_WIN32

#if ENABLE_VTUNE
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/vtune/include/jitprofiling.h"
#pragma comment(lib, "../third_party/vtune/lib64/jitprofiling.lib")
#include "xenia/cpu/module.h"
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

void X64CodeCache::FillCode(void* write_address, size_t size) {
  std::memset(write_address, 0xCC, size);
}

void X64CodeCache::FlushCodeRange(void* address, size_t size) {
  // x86-64 has coherent I/D caches; no flush needed.
}

void X64CodeCache::OnCodePlaced(uint32_t guest_address,
                                GuestFunction* function_info,
                                void* code_execute_address, size_t code_size) {
#if !XE_PLATFORM_WIN32
  if (cvars::perf_map) {
    static std::mutex perf_map_mutex;
    static FILE* perf_map_file = nullptr;
    std::lock_guard<std::mutex> lock(perf_map_mutex);
    if (!perf_map_file) {
      char path[64];
      std::snprintf(path, sizeof(path), "/tmp/perf-%d.map", int(getpid()));
      perf_map_file = std::fopen(path, "w");
    }
    if (perf_map_file) {
      // "START SIZE name" in hex, the format perf reads for JIT code.
      if (function_info && !function_info->name().empty()) {
        std::fprintf(perf_map_file, "%zx %zx %s\n",
                     reinterpret_cast<size_t>(code_execute_address), code_size,
                     function_info->name().c_str());
      } else {
        std::fprintf(perf_map_file, "%zx %zx sub_%08X\n",
                     reinterpret_cast<size_t>(code_execute_address), code_size,
                     guest_address);
      }
      std::fflush(perf_map_file);
    }
  }
#endif  // !XE_PLATFORM_WIN32
#if ENABLE_VTUNE
  if (iJIT_IsProfilingActive() == iJIT_SAMPLING_ON) {
    std::string method_name;
    if (function_info && function_info->name().size() != 0) {
      method_name = function_info->name();
    } else {
      method_name = fmt::format("sub_{:08X}", guest_address);
    }

    iJIT_Method_Load_V2 method = {0};
    method.method_id = iJIT_GetNewMethodID();
    method.method_load_address = code_execute_address;
    method.method_size = uint32_t(code_size);
    method.method_name = const_cast<char*>(method_name.data());
    method.module_name = function_info
                             ? (char*)function_info->module()->name().c_str()
                             : nullptr;
    iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED_V2, (void*)&method);
  }
#endif
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
