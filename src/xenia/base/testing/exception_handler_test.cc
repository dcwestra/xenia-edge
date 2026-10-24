/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/exception_handler.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#include "xenia/base/memory.h"
#include "xenia/base/platform.h"

#include "third_party/catch/include/catch.hpp"

#if XE_ARCH_AMD64
#include <xmmintrin.h>
#elif XE_ARCH_ARM64 && XE_COMPILER_MSVC
#include <intrin.h>
#endif

#if XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#endif

namespace xe {
namespace test {

namespace {

// Round toward +inf with flush-to-zero: what a guest with FPSCR RN=2 and NI
// set leaves in the FP control register.
#if XE_ARCH_AMD64
constexpr uint64_t kGuestLikeFpControl = 0xDF80;
#elif XE_ARCH_ARM64
constexpr uint64_t kGuestLikeFpControl = (0b01 << 22) | (1 << 24);
#endif

uint64_t ReadFpControl() {
#if XE_ARCH_AMD64
  return _mm_getcsr();
#elif XE_ARCH_ARM64 && XE_COMPILER_MSVC
  return _ReadStatusReg(ARM64_FPCR);
#elif XE_ARCH_ARM64
  uint64_t fpcr;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#endif
}

void WriteFpControl(uint64_t value) {
#if XE_ARCH_AMD64
  _mm_setcsr(static_cast<unsigned int>(value));
#elif XE_ARCH_ARM64 && XE_COMPILER_MSVC
  _WriteStatusReg(ARM64_FPCR, value);
#elif XE_ARCH_ARM64
  asm volatile("msr fpcr, %0" ::"r"(value) : "memory");
#endif
}

struct FaultState {
  uint8_t* page = nullptr;
  bool faulted = false;
  float sum = 0.0f;
  float denormal_product = 0.0f;
};

bool RecordFpAndUnprotect(Exception* ex, void* data) {
  auto* state = static_cast<FaultState*>(data);
  if (ex->code() != Exception::Code::kAccessViolation ||
      ex->fault_address() != reinterpret_cast<uint64_t>(state->page)) {
    return false;
  }
  state->faulted = true;
  volatile float tiny = std::ldexp(1.0f, -24);
  volatile float denormal = std::ldexp(1.0f, -140);
  state->sum = 1.0f + tiny;
  state->denormal_product = denormal * 2.0f;
  return memory::Protect(state->page, memory::page_size(),
                         memory::PageAccess::kReadWrite);
}

}  // namespace

// A fault in guest code runs host handlers (write watches, MMIO) on the
// faulting thread, which may hold the guest's FP control.
TEST_CASE("EXCEPTION_HANDLER_RUNS_IN_HOST_FP_MODE", "[exception]") {
  FaultState state;
  const size_t page_size = memory::page_size();
  state.page = static_cast<uint8_t*>(memory::AllocFixed(
      nullptr, page_size, memory::AllocationType::kReserveCommit,
      memory::PageAccess::kNoAccess));
  REQUIRE(state.page != nullptr);
  ExceptionHandler::Install(RecordFpAndUnprotect, &state);

  const uint64_t host_fp_control = ReadFpControl();
  WriteFpControl(kGuestLikeFpControl);
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const uint8_t value = *reinterpret_cast<volatile uint8_t*>(state.page);
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const uint64_t fp_control_after = ReadFpControl();
  WriteFpControl(host_fp_control);

  ExceptionHandler::Uninstall(RecordFpAndUnprotect, &state);
  memory::DeallocFixed(state.page, page_size,
                       memory::DeallocationType::kRelease);

  REQUIRE(state.faulted);
  REQUIRE(value == 0);
  // The handler rounded to nearest and kept the denormal.
  REQUIRE(state.sum == 1.0f);
  REQUIRE(state.denormal_product == std::ldexp(1.0f, -139));
  // The interrupted code got its own FP control back.
  REQUIRE(fp_control_after == kGuestLikeFpControl);
}

#if XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID
namespace {
bool DeclineEveryException(Exception*, void*) { return false; }
}  // namespace

// An unhandled host crash has to leave a report behind, as the asynchronous
// logger never runs again. Crashes a child and reads what it wrote to stderr.
TEST_CASE("UNHANDLED_FAULT_WRITES_CRASH_REPORT", "[exception]") {
  int pipe_fds[2];
  REQUIRE(pipe(pipe_fds) == 0);
  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // RLIMIT_CORE of 1 makes the kernel skip piped core dumps, so the expected
    // crash doesn't reach systemd-coredump or a desktop crash reporter.
    rlimit core_limit = {1, 1};
    setrlimit(RLIMIT_CORE, &core_limit);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    // The test runner's own handler would claim the signal otherwise.
    signal(SIGSEGV, SIG_DFL);
    ExceptionHandler::Install(DeclineEveryException, nullptr);
    *reinterpret_cast<volatile int*>(uintptr_t(16)) = 1;
    _exit(0);
  }
  close(pipe_fds[1]);
  std::string output;
  char buffer[4096];
  while (true) {
    const ssize_t read_size = read(pipe_fds[0], buffer, sizeof(buffer));
    if (read_size < 0 && errno == EINTR) {
      continue;
    }
    if (read_size <= 0) {
      break;
    }
    output.append(buffer, size_t(read_size));
  }
  close(pipe_fds[0]);
  int status = 0;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  INFO(output);
  // Still dies of the original signal after reporting.
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGSEGV);
  REQUIRE(output.find("FATAL HOST CRASH: SIGSEGV") != std::string::npos);
  REQUIRE(output.find("END OF HOST CRASH REPORT") != std::string::npos);
}
#endif  // XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID

}  // namespace test
}  // namespace xe
