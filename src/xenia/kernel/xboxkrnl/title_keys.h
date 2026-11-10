/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_TITLE_KEYS_H_
#define XENIA_KERNEL_XBOXKRNL_TITLE_KEYS_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
namespace xboxkrnl {

using TitleKey = std::array<uint8_t, 16>;

// A key a title hands the hypervisor (XeKeysSetKey), and the key the
// hypervisor uses in its place. The hypervisor derives that key in a way that
// isn't known, so a user supplies these pairs in a title keys file:
//
//   [[title_key]]
//   set = "<the 16 bytes the title sets, as 32 hex digits>"
//   use = "<the 16 bytes the hypervisor uses, as 32 hex digits>"
struct TitleKeyPair {
  TitleKey set;
  TitleKey use;
};

// The key that 32 hex digits spell, or nothing if text is anything else.
std::optional<TitleKey> ParseTitleKey(std::string_view text);

// The 32 hex digits that spell key, in capitals.
std::string FormatTitleKey(const TitleKey& key);

// The pairs a title keys file's TOML text lists. An entry that isn't a pair of
// keys is left out, and why goes in problems.
std::vector<TitleKeyPair> ParseTitleKeys(std::string_view text,
                                         std::vector<std::string>* problems);

// The same for the file at path, which must exist.
std::vector<TitleKeyPair> LoadTitleKeys(const std::filesystem::path& path,
                                        std::vector<std::string>* problems);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_TITLE_KEYS_H_
