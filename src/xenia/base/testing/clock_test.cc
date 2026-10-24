/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <cstdint>

#include "xenia/base/clock.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace base {
namespace test {

// A guest mftb reads the guest clock, and some titles spin on it millions of
// times a second. Guest time must keep pace with real time however often it
// is read: a converter that drops each read's fraction of a tick slows it.
TEST_CASE("Guest clock keeps real time under constant polling", "[clock]") {
  constexpr uint64_t kXenonTimebaseHz = 49875000;
  const uint64_t saved_frequency = Clock::guest_tick_frequency();
  Clock::set_guest_tick_frequency(kXenonTimebaseHz);

  const auto host_start = std::chrono::steady_clock::now();
  const uint64_t guest_start = Clock::QueryGuestTickCount();
  uint64_t previous = guest_start;
  bool monotonic = true;
  std::chrono::steady_clock::time_point host_now;
  do {
    const uint64_t guest_now = Clock::QueryGuestTickCount();
    monotonic = monotonic && guest_now >= previous;
    previous = guest_now;
    host_now = std::chrono::steady_clock::now();
  } while (host_now - host_start < std::chrono::milliseconds(200));
  const uint64_t guest_end = Clock::QueryGuestTickCount();
  const auto host_end = std::chrono::steady_clock::now();

  Clock::set_guest_tick_frequency(saved_frequency);

  const double host_seconds =
      std::chrono::duration<double>(host_end - host_start).count();
  const double guest_seconds =
      double(guest_end - guest_start) / double(kXenonTimebaseHz);
  INFO("host " << host_seconds << " s, guest " << guest_seconds << " s");
  REQUIRE(monotonic);
  REQUIRE(guest_seconds > host_seconds * 0.99);
  REQUIRE(guest_seconds < host_seconds * 1.01);
}

}  // namespace test
}  // namespace base
}  // namespace xe
