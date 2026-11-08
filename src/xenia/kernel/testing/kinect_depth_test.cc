/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "xenia/kernel/nui/kinect_camera.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::nui::KinectCamera;
using xe::kernel::nui::KinectRawDepthToMillimeters;
using xe::kernel::nui::MillimetersToKinectRawDepth;
using xe::kernel::nui::PackKinectDepth;
using xe::kernel::nui::UnpackKinectDepth;

constexpr uint16_t kNoReading = 2047;

TEST_CASE("Kinect raw depth from millimeters", "[kinect]") {
  SECTION("A distance comes back within one raw step") {
    for (uint32_t mm = 500; mm <= 8000; mm += 7) {
      const uint16_t raw = MillimetersToKinectRawDepth(mm);
      REQUIRE(raw != kNoReading);
      const uint32_t back = KinectRawDepthToMillimeters(raw);
      // The step between raw values, which grows with distance from under a
      // millimeter.
      const uint32_t step = KinectRawDepthToMillimeters(raw + 1) -
                            KinectRawDepthToMillimeters(raw);
      REQUIRE(uint32_t(std::abs(int32_t(back) - int32_t(mm))) <=
              std::max<uint32_t>(step, 1));
    }
  }

  SECTION("Each raw value maps back to itself") {
    // Where raw values are at least 2 mm apart, so their millimeters are
    // distinct.
    for (uint16_t raw = 1; raw < 1083; ++raw) {
      const uint16_t mm = KinectRawDepthToMillimeters(raw);
      const uint16_t below = KinectRawDepthToMillimeters(raw - 1);
      const uint16_t above = KinectRawDepthToMillimeters(raw + 1);
      if (mm && above && mm - below >= 2 && above - mm >= 2) {
        REQUIRE(MillimetersToKinectRawDepth(mm) == raw);
      }
    }
  }

  SECTION("Nothing, too near and too far read as no reading") {
    REQUIRE(MillimetersToKinectRawDepth(0) == kNoReading);
    REQUIRE(MillimetersToKinectRawDepth(100) == kNoReading);
    REQUIRE(MillimetersToKinectRawDepth(9000) == kNoReading);
  }
}

TEST_CASE("Kinect depth packing", "[kinect]") {
  constexpr size_t kPixels =
      size_t(KinectCamera::kDepthWidth) * KinectCamera::kDepthHeight;
  std::vector<uint16_t> raw(kPixels);
  uint32_t seed = 12345;
  for (uint16_t& value : raw) {
    seed = seed * 1103515245 + 12345;
    value = uint16_t((seed >> 16) & 0x7FF);
  }
  std::vector<uint8_t> packed(KinectCamera::kDepthFrameBytes);
  PackKinectDepth(raw.data(), packed.data());

  SECTION("It's the sensor's big-endian bitstream") {
    // The first pixel's 11 bits, then the second's top 5.
    REQUIRE(packed[0] == uint8_t(raw[0] >> 3));
    REQUIRE(packed[1] == uint8_t((raw[0] & 0x7) << 5 | raw[1] >> 6));
  }

  SECTION("Unpacking gives the frame back") {
    std::vector<uint16_t> unpacked(kPixels);
    UnpackKinectDepth(packed.data(), unpacked.data());
    REQUIRE(unpacked == raw);
  }
}

}  // namespace
