/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>
#include <vector>

#include "xenia/kernel/xboxkrnl/title_keys.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::FormatTitleKey;
using xe::kernel::xboxkrnl::ParseTitleKey;
using xe::kernel::xboxkrnl::ParseTitleKeys;
using xe::kernel::xboxkrnl::TitleKey;

// Made-up keys: the format is all these tests are about.
constexpr char kSetA[] = "000102030405060708090A0B0C0D0E0F";
constexpr char kUseA[] = "F0E0D0C0B0A090807060504030201000";
constexpr char kSetB[] = "1111111111111111111111111111111F";
constexpr char kUseB[] = "2222222222222222222222222222222F";

TEST_CASE("Title keys are 32 hex digits", "[title_keys]") {
  const auto key = ParseTitleKey(kSetA);
  REQUIRE(key);
  for (size_t i = 0; i < key->size(); ++i) {
    REQUIRE((*key)[i] == i);
  }
  REQUIRE(FormatTitleKey(*key) == kSetA);
  // Lower case reads the same.
  REQUIRE(ParseTitleKey("000102030405060708090a0b0c0d0e0f") == key);
  REQUIRE_FALSE(ParseTitleKey(""));
  REQUIRE_FALSE(ParseTitleKey("000102030405060708090A0B0C0D0E"));
  REQUIRE_FALSE(ParseTitleKey("000102030405060708090A0B0C0D0E0F00"));
  REQUIRE_FALSE(ParseTitleKey("000102030405060708090A0B0C0D0E0G"));
  REQUIRE_FALSE(ParseTitleKey("00 01 02 03 04 05 06 07 08 09 0A"));
}

TEST_CASE("A title keys file lists set and use pairs", "[title_keys]") {
  std::vector<std::string> problems;

  SECTION("Each [[title_key]] is a pair") {
    const std::string text =
        std::string("# Comments are fine.\n") + "[[title_key]]\n" + "set = \"" +
        kSetA + "\"\n" + "use = \"" + kUseA + "\"\n\n" + "[[title_key]]\n" +
        "set = \"" + kSetB + "\"\n" + "use = \"" + kUseB + "\"\n";
    const auto pairs = ParseTitleKeys(text, &problems);
    REQUIRE(problems.empty());
    REQUIRE(pairs.size() == 2);
    REQUIRE(FormatTitleKey(pairs[0].set) == kSetA);
    REQUIRE(FormatTitleKey(pairs[0].use) == kUseA);
    REQUIRE(FormatTitleKey(pairs[1].set) == kSetB);
    REQUIRE(FormatTitleKey(pairs[1].use) == kUseB);
  }

  SECTION("An empty file lists none") {
    REQUIRE(ParseTitleKeys("", &problems).empty());
    REQUIRE(problems.empty());
  }

  SECTION("Bad entries are left out and reported, good ones kept") {
    const std::string text =
        std::string("[[title_key]]\n") + "set = \"" + kSetA + "\"\n" +
        "\n[[title_key]]\n" + "set = \"" + kSetA + "\"\n" + "use = \"F0E0\"\n" +
        "\n[[title_key]]\n" + "set = \"" + kSetB + "\"\n" + "use = \"" + kUseB +
        "\"\n" + "\n[[title_key]]\n" + "set = \"" + kSetB + "\"\n" +
        "use = \"" + kUseA + "\"\n";
    const auto pairs = ParseTitleKeys(text, &problems);
    REQUIRE(pairs.size() == 1);
    REQUIRE(FormatTitleKey(pairs[0].use) == kUseB);
    // No use, a short use, and kSetB a second time.
    REQUIRE(problems.size() == 3);
    REQUIRE(problems[0].rfind("line 1:", 0) == 0);
  }

  SECTION("Text that isn't TOML is reported with its line") {
    REQUIRE(ParseTitleKeys("[[title_key]]\nset = \n", &problems).empty());
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].rfind("line 2:", 0) == 0);
  }

  SECTION("title_key must be [[title_key]] entries") {
    const std::string text = std::string("title_key = \"") + kSetA + "\"\n";
    REQUIRE(ParseTitleKeys(text, &problems).empty());
    REQUIRE(problems.size() == 1);
  }
}

}  // namespace
