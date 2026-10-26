/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_data_store.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace nui {

std::string NuiDataStore::ProfileKey(uint64_t xuid) {
  return fmt::format("{:016X}", xuid);
}

std::filesystem::path NuiDataStore::PathFor(Kind kind,
                                            const std::string& key) const {
  std::string name;
  for (char c : key) {
    const bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') || c == '_' || c == '-';
    name += safe ? c : '_';
  }
  switch (kind) {
    case Kind::kNamed:
      return root_ / (name + ".named.bin");
    case Kind::kProfileCache:
      return root_ / (name + ".identity.bin");
    case Kind::kSystem:
      return root_ / (name + ".system.bin");
    case Kind::kProfileBiometric:
    default:
      return root_ / (name + ".biometric.bin");
  }
}

bool NuiDataStore::Read(Kind kind, const std::string& key,
                        uint64_t& version_out, std::vector<uint8_t>& data_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  FILE* file = xe::filesystem::OpenFile(PathFor(kind, key), "rb");
  if (!file) {
    return false;
  }
  // A big-endian version, then the data.
  uint8_t version[8];
  bool read = fread(version, 1, sizeof(version), file) == sizeof(version);
  if (read) {
    version_out = xe::load_and_swap<uint64_t>(version);
    data_out.clear();
    uint8_t chunk[4096];
    size_t chunk_size;
    while ((chunk_size = fread(chunk, 1, sizeof(chunk), file)) != 0) {
      data_out.insert(data_out.end(), chunk, chunk + chunk_size);
    }
  }
  fclose(file);
  return read;
}

bool NuiDataStore::Write(Kind kind, const std::string& key, uint64_t version,
                         const uint8_t* data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code error;
  std::filesystem::create_directories(root_, error);
  const std::filesystem::path path = PathFor(kind, key);
  std::filesystem::path temp_path = path;
  temp_path += ".tmp";
  FILE* file = xe::filesystem::OpenFile(temp_path, "wb");
  if (!file) {
    XELOGW("NUI: can't store {}", xe::strip_pii(xe::path_to_utf8(path)));
    return false;
  }
  uint8_t version_bytes[8];
  xe::store_and_swap<uint64_t>(version_bytes, version);
  bool written = fwrite(version_bytes, 1, sizeof(version_bytes), file) ==
                     sizeof(version_bytes) &&
                 (!size || fwrite(data, 1, size, file) == size);
  written = fclose(file) == 0 && written;
  if (written) {
    std::filesystem::rename(temp_path, path, error);
    written = !error;
  }
  if (!written) {
    std::filesystem::remove(temp_path, error);
    XELOGW("NUI: can't store {}", xe::strip_pii(xe::path_to_utf8(path)));
  }
  return written;
}

void NuiDataStore::Delete(Kind kind, const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code error;
  std::filesystem::remove(PathFor(kind, key), error);
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
