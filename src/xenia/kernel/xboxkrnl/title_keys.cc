/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/title_keys.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"

namespace xe {
namespace kernel {
namespace xboxkrnl {

static int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

std::optional<TitleKey> ParseTitleKey(std::string_view text) {
  TitleKey key;
  if (text.size() != key.size() * 2) {
    return std::nullopt;
  }
  for (size_t i = 0; i < key.size(); ++i) {
    const int high = HexDigit(text[i * 2]);
    const int low = HexDigit(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    key[i] = uint8_t(high << 4 | low);
  }
  return key;
}

std::string FormatTitleKey(const TitleKey& key) {
  std::string text;
  text.reserve(key.size() * 2);
  for (uint8_t byte : key) {
    fmt::format_to(std::back_inserter(text), "{:02X}", byte);
  }
  return text;
}

std::vector<TitleKeyPair> ParseTitleKeys(std::string_view text,
                                         std::vector<std::string>* problems) {
  std::vector<TitleKeyPair> pairs;
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& e) {
    problems->push_back(
        fmt::format("line {}: {}", e.source().begin.line, e.description()));
    return pairs;
  }
  const toml::node* entries = root.get("title_key");
  if (!entries) {
    return pairs;
  }
  if (!entries->is_array_of_tables()) {
    problems->push_back(
        fmt::format("line {}: title_key isn't a list of [[title_key]] entries",
                    entries->source().begin.line));
    return pairs;
  }
  for (const toml::node& entry : *entries->as_array()) {
    const toml::table& table = *entry.as_table();
    const auto line = entry.source().begin.line;
    const auto set_text = table["set"].value<std::string>();
    const auto use_text = table["use"].value<std::string>();
    if (!set_text || !use_text) {
      problems->push_back(fmt::format(
          "line {}: a [[title_key]] needs both set and use, as strings", line));
      continue;
    }
    const auto set = ParseTitleKey(*set_text);
    const auto use = ParseTitleKey(*use_text);
    if (!set || !use) {
      problems->push_back(
          fmt::format("line {}: set and use must each be 32 hex digits", line));
      continue;
    }
    if (std::any_of(pairs.begin(), pairs.end(), [&](const TitleKeyPair& pair) {
          return pair.set == *set;
        })) {
      problems->push_back(fmt::format(
          "line {}: set {} is listed more than once, the first is used", line,
          *set_text));
      continue;
    }
    pairs.push_back({*set, *use});
  }
  return pairs;
}

std::vector<TitleKeyPair> LoadTitleKeys(const std::filesystem::path& path,
                                        std::vector<std::string>* problems) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    problems->push_back("it can't be read");
    return {};
  }
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  return ParseTitleKeys(text, problems);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
