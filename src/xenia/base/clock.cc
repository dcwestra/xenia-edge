/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/clock.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"

#if defined(_WIN32)

#include "xenia/base/platform_win.h"

#endif

#if XE_COMPILER_MSVC
#include <intrin.h>
#endif

DEFINE_bool(clock_no_scaling, false,
            "Disable scaling code. Time management and locking is bypassed. "
            "Guest system time is directly pulled from host.",
            "CPU");
DEFINE_bool(clock_source_raw, false,
            "On x64, Use the RDTSC instruction as the time source. Requires "
            "invariant TSC. "
            "On a64, Use the CNTVCT_EL0 register as the time source",
            "CPU");

namespace xe {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();

std::pair<uint64_t, uint64_t> guest_system_time_ratio_ =
    []() -> std::pair<uint64_t, uint64_t> {
  std::pair<uint64_t, uint64_t> frac(uint64_t(10000000), guest_tick_frequency_);
  reduce_fraction(frac);
  return frac;
}();

// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks, as of a recent read. Exposed to the JIT for
// inline_loadclock, which accepts a slightly stale value.
uint64_t last_guest_tick_count_ = 0;

using tick_mutex_type = std::mutex;

// Serializes the rare writers of the ratios and the conversion below.
static tick_mutex_type tick_mutex_;

// (a * b) >> shift over the full 128-bit product, shift in [0, 63].
static inline uint64_t MulShift64(uint64_t a, uint64_t b, uint32_t shift) {
#if XE_COMPILER_MSVC && XE_ARCH_AMD64
  uint64_t high;
  uint64_t low = _umul128(a, b, &high);
  return __shiftright128(low, high, static_cast<unsigned char>(shift));
#elif XE_COMPILER_MSVC && XE_ARCH_ARM64
  uint64_t low = a * b;
  uint64_t high = __umulh(a, b);
  return shift ? (low >> shift) | (high << (64 - shift)) : low;
#else
  return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) >>
                               shift);
#endif
}

// Host to guest tick conversion, guest = guest_base + (host - host_base) *
// mult >> shift, with mult a fixed-point form of guest_tick_ratio_. A guest
// mftb reads this, which some titles do millions of times a second, so it is
// published through a sequence lock rather than a mutex. Converting from a
// fixed base also keeps the fraction of a tick: the accumulator this replaced
// dropped it on every read, so polling slowed guest time down, by a fifth at
// reads 100 ns apart and to a standstill under one guest tick apart.
static std::atomic<uint32_t> guest_clock_seq_{0};
static std::atomic<uint64_t> guest_clock_host_base_{0};
static std::atomic<uint64_t> guest_clock_guest_base_{0};
static std::atomic<uint64_t> guest_clock_mult_{1};
static std::atomic<uint32_t> guest_clock_shift_{0};
// Guest time counts from startup, as the accumulator it replaced did.
[[maybe_unused]] static const bool guest_clock_based_ = [] {
  guest_clock_host_base_.store(Clock::QueryHostTickCount(),
                               std::memory_order_relaxed);
  return true;
}();

// Guest ticks at host tick count |host_ticks|.
static uint64_t GuestTicksAtHostTicks(uint64_t host_ticks) {
  uint32_t seq;
  uint64_t host_base, guest_base, mult;
  uint32_t shift;
  do {
    seq = guest_clock_seq_.load(std::memory_order_acquire);
    host_base = guest_clock_host_base_.load(std::memory_order_relaxed);
    guest_base = guest_clock_guest_base_.load(std::memory_order_relaxed);
    mult = guest_clock_mult_.load(std::memory_order_relaxed);
    shift = guest_clock_shift_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
  } while ((seq & 1) ||
           guest_clock_seq_.load(std::memory_order_relaxed) != seq);
  // A host tick count from before the base (another source) adds nothing.
  uint64_t host_delta = host_ticks > host_base ? host_ticks - host_base : 0;
  return guest_base + MulShift64(host_delta, mult, shift);
}

// Rebases the conversion on the current guest time and switches it to
// |ratio|, keeping guest time continuous. Caller holds tick_mutex_.
static void PublishGuestClockRatio(std::pair<uint64_t, uint64_t> ratio) {
  uint64_t host_now = Clock::QueryHostTickCount();
  uint64_t guest_now = GuestTicksAtHostTicks(host_now);
  // As many fraction bits as keep ratio.first << shift within 64 bits.
  int shift = std::min(40, 63 - int(std::bit_width(ratio.first)));
  shift = std::max(shift, 0);
  uint64_t mult = ratio.second ? (ratio.first << shift) / ratio.second : 0;
  uint32_t seq = guest_clock_seq_.load(std::memory_order_relaxed);
  guest_clock_seq_.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  guest_clock_host_base_.store(host_now, std::memory_order_relaxed);
  guest_clock_guest_base_.store(guest_now, std::memory_order_relaxed);
  guest_clock_mult_.store(mult, std::memory_order_relaxed);
  guest_clock_shift_.store(uint32_t(shift), std::memory_order_relaxed);
  guest_clock_seq_.store(seq + 2, std::memory_order_release);
}

static void RecomputeGuestSystemTimeRatio() {
  std::pair<uint64_t, uint64_t> frac(uint64_t(10000000), guest_tick_frequency_);
  reduce_fraction(frac);
  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  guest_system_time_ratio_ = frac;
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac =
      std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
  PublishGuestClockRatio(frac);
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (cvars::clock_no_scaling) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  uint64_t guest_tick_count = GuestTicksAtHostTicks(host_tick_count);
  // Refresh the copy inline_loadclock reads. Only once it has fallen a little
  // behind, so threads polling the clock don't all keep writing one line.
  std::atomic_ref<uint64_t> published(last_guest_tick_count_);
  if (guest_tick_count > published.load(std::memory_order_relaxed) + 64) {
    published.store(guest_tick_count, std::memory_order_relaxed);
  }
  return guest_tick_count;
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  return guest_tick_count * guest_system_time_ratio_.first /
         guest_system_time_ratio_.second;
}
uint64_t Clock::QueryHostTickFrequency() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() { return guest_time_scalar_; }

void Clock::set_guest_time_scalar(double scalar) {
  if (cvars::clock_no_scaling) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() { return guest_tick_frequency_; }

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestSystemTimeRatio();
  RecomputeGuestTickScalar();
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t* Clock::GetGuestTickCountPointer() { return &last_guest_tick_count_; }
uint64_t Clock::QueryGuestSystemTime() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint64_t Clock::QueryGuestInterruptTime() {
  return Clock::QueryHostInterruptTime();
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(
      std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                         std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (cvars::clock_no_scaling) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (cvars::clock_no_scaling) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms = static_cast<uint64_t>(
      (static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (cvars::clock_no_scaling) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time =
        static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time = static_cast<uint64_t>(
        (static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (cvars::clock_no_scaling) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) *
                                              guest_time_scalar_);
  uint64_t scaled_usec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) *
                                               guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace xe
