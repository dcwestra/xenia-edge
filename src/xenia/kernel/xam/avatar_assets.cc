/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_assets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <random>
#include <unordered_map>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/lzx.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/vfs/virtual_file_system.h"

DEFINE_path(avatar_asset_pack_path, "",
            "The system avatar asset package (FFFE07DF00000002 from a disc's "
            "$SystemUpdate folder, or the AvatarAssetPack on Kinect Sports' "
            "disc), or the AvatarAssetPack.toc from one. Without it, avatars "
            "come from the running title's disc when it has a package.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {

namespace {

constexpr uint8_t kTocMagic[16] = {0x8A, 0x76, 0x2D, 0xF4, 0xC0, 0x67,
                                   0x4B, 0x66, 0xB4, 0xEE, 0x56, 0x46,
                                   0x6B, 0x1B, 0x82, 0x80};
constexpr size_t kTocHeaderSize = 0x28;
constexpr size_t kTocRecordSize = 0xF8;

constexpr uint32_t kStrbBlockTexture = 2;
constexpr uint32_t kStrbBlockModel = 3;
constexpr uint32_t kStrbBlockColorTable = 7;
constexpr uint32_t kStrbBlockMetadata = 8;

constexpr uint32_t kSlotHead = 0x1;
constexpr uint32_t kSlotBody = 0x2;
constexpr uint32_t kSlotHair = 0x4;

// Asset y positions are quantized with a step sqrt(2/3) of x's and z's
// (checked against titles' own avatar meshes on their skeleton).
constexpr float kYStepScale = 0.816496581f;

// Results: a header, the skeleton (at 0x40, its bones at 0x100), then per
// model its asset ID (32 bytes), its entry (52), textures (44 each) and draw
// batches (512 each).
constexpr uint32_t kResultSize = 0x10000;

uint32_t LoadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}

float LoadLE32Float(const uint8_t* p) {
  const uint32_t bits = LoadLE32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreBE32Float(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  xe::store_and_swap<uint32_t>(p, bits);
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// Reads a file stored in consecutive blocks of an STFS package (PIRS, LIVE or
// CON), as the system avatar package is.
bool ReadStfsFile(std::span<const uint8_t> package, std::string_view name,
                  std::vector<uint8_t>* out) {
  if (package.size() < 0x1000) {
    return false;
  }
  const uint32_t magic = xe::load_and_swap<uint32_t>(package.data());
  if (magic != 0x50495253 && magic != 0x4C495645 && magic != 0x434F4E20) {
    return false;
  }
  const uint32_t header_size = xe::load_and_swap<uint32_t>(&package[0x340]);
  // The volume descriptor: block separation, file table block count and
  // first block (little-endian).
  const uint8_t* descriptor = &package[0x379];
  const uint32_t step = (descriptor[2] & 1) ? 1 : 2;
  const uint32_t table_blocks = descriptor[3] | descriptor[4] << 8;
  const uint32_t table_block =
      descriptor[5] | descriptor[6] << 8 | descriptor[7] << 16;
  const size_t base = xe::align<size_t>(header_size, 0x1000);
  auto block_offset = [&](uint32_t block) {
    // Hash tables come before every 0xAA data blocks, and higher level ones
    // before every 0x70E4.
    uint32_t n = block + (block / 0xAA + 1) * step;
    if (block >= 0xAA) {
      n += (block / 0x70E4 + 1) * step;
      if (block >= 0x70E4) {
        n += step;
      }
    }
    return base + size_t(n) * 0x1000;
  };
  for (uint32_t i = 0; i < table_blocks; ++i) {
    const size_t table = block_offset(table_block + i);
    for (uint32_t k = 0; k < 64; ++k) {
      const size_t entry = table + k * 0x40;
      if (entry + 0x40 > package.size()) {
        return false;
      }
      const uint8_t* e = &package[entry];
      const uint32_t name_length = e[0x28] & 0x3F;
      if (!name_length || std::string_view(reinterpret_cast<const char*>(e),
                                           name_length) != name) {
        continue;
      }
      if (!(e[0x28] & 0x40)) {
        XELOGE("Avatars: {} isn't stored in consecutive blocks", name);
        return false;
      }
      const uint32_t start = e[0x2F] | e[0x30] << 8 | e[0x31] << 16;
      const uint32_t size = xe::load_and_swap<uint32_t>(e + 0x34);
      out->resize(size);
      for (uint32_t b = 0; size_t(b) * 0x1000 < size; ++b) {
        const size_t offset = block_offset(start + b);
        const size_t length = std::min<size_t>(0x1000, size - b * 0x1000);
        if (offset + length > package.size()) {
          return false;
        }
        std::memcpy(out->data() + size_t(b) * 0x1000, &package[offset], length);
      }
      return true;
    }
  }
  return false;
}

bool ReadHostFile(const std::filesystem::path& path,
                  std::vector<uint8_t>* out) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  out->resize(size > 0 ? size_t(size) : 0);
  const bool ok = std::fread(out->data(), 1, out->size(), file) == out->size();
  std::fclose(file);
  return ok && !out->empty();
}

bool ReadGuestFile(std::string_view path, std::vector<uint8_t>* out) {
  vfs::File* file = nullptr;
  vfs::FileAction action;
  if (kernel_state()->file_system()->OpenFile(
          nullptr, path, vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead, false, true, &file,
          &action) != X_STATUS_SUCCESS) {
    return false;
  }
  out->resize(file->entry()->size());
  size_t read = 0;
  const X_STATUS status = file->ReadSync(std::span<uint8_t>(*out), 0, &read);
  file->Destroy();
  return XSUCCEEDED(status) && read == out->size() && !out->empty();
}

// The TOC itself, or the one in an avatar asset package.
bool ExtractToc(std::vector<uint8_t> data, std::vector<uint8_t>* toc) {
  if (data.size() >= sizeof(kTocMagic) &&
      !std::memcmp(data.data(), kTocMagic, sizeof(kTocMagic))) {
    *toc = std::move(data);
    return true;
  }
  return ReadStfsFile(data, "AvatarAssetPack.toc", toc);
}

struct StrbBlock {
  uint32_t id;
  size_t offset;
  size_t length;
};

// An STRB container's blocks (after Velocity's AvatarAsset): a header with
// the endianness, the sizes of block IDs and lengths and the alignment, then
// blocks of {ID, length, field size} and their data.
bool ParseStrb(std::span<const uint8_t> data, std::vector<StrbBlock>* blocks) {
  if (data.size() < 0x20 || std::memcmp(data.data(), "STRB", 4)) {
    return false;
  }
  const bool alignment_stored = data[4] != 0;
  const bool little_endian = data[5] != 0;
  const uint32_t id_size = data[22];
  const uint32_t span_size = data[23];
  const size_t alignment = alignment_stored ? std::max<size_t>(data[26], 1) : 1;
  if (!id_size || id_size > 4 || !span_size || span_size > 4) {
    return false;
  }
  auto round = [alignment](size_t value) {
    return (value + alignment - 1) / alignment * alignment;
  };
  auto read = [&](size_t offset, uint32_t size) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; ++i) {
      const uint32_t byte = data[offset + i];
      value |= little_endian ? byte << (8 * i) : byte << (8 * (size - 1 - i));
    }
    if (size < 4 && (value >> (8 * size - 1)) & 1) {
      value |= ~0u << (8 * size);
    }
    return int32_t(value);
  };
  const size_t header = round(id_size + 2 * span_size);
  size_t cursor = round(alignment_stored ? 30 : 0x1A);
  while (cursor + header <= data.size()) {
    const int32_t id = read(cursor, id_size);
    const int32_t length = read(cursor + id_size, span_size);
    const size_t offset = cursor + header;
    if (id == -1 || length < 0 || offset + size_t(length) > data.size()) {
      break;
    }
    blocks->push_back({uint32_t(id), offset, size_t(length)});
    cursor = offset + round(size_t(length));
  }
  return true;
}

// Model and texture block data: chunks of {u32 compressed size, u32 output
// offset, u32 output size} (little-endian), each an LZX stream of its own
// with a 32 KB window.
bool DecompressAssetData(std::span<const uint8_t> data,
                         std::vector<uint8_t>* out,
                         size_t max_chunks = SIZE_MAX) {
  out->clear();
  size_t cursor = 0;
  for (size_t chunk = 0; chunk < max_chunks && cursor + 12 <= data.size();
       ++chunk) {
    const uint32_t compressed_size = LoadLE32(&data[cursor]);
    const uint32_t output_offset = LoadLE32(&data[cursor + 4]);
    const uint32_t output_size = LoadLE32(&data[cursor + 8]);
    if (cursor + 12 + compressed_size > data.size() || !output_size ||
        output_size > 0x10000 || output_offset > (32u << 20)) {
      return false;
    }
    if (out->size() < size_t(output_offset) + output_size) {
      out->resize(size_t(output_offset) + output_size);
    }
    if (lzx_decompress(&data[cursor + 12], compressed_size,
                       out->data() + output_offset, output_size, 0x8000,
                       nullptr, 0)) {
      return false;
    }
    cursor += 12 + compressed_size;
  }
  return !out->empty();
}

// Fields of a bit stream, least significant bit first.
class BitReader {
 public:
  explicit BitReader(std::span<const uint8_t> data) : data_(data) {}
  uint32_t Read(size_t bit, uint32_t width) const {
    if (!width) {
      return 0;
    }
    const size_t byte = bit >> 3;
    uint64_t value = 0;
    for (size_t i = 0; i < 8 && byte + i < data_.size(); ++i) {
      value |= uint64_t(data_[byte + i]) << (8 * i);
    }
    value >>= bit & 7;
    return uint32_t(value & ((uint64_t(1) << width) - 1));
  }
  size_t size_bits() const { return data_.size() * 8; }

 private:
  std::span<const uint8_t> data_;
};

// texture_util's GetTiledOffset2D: a block's offset in a tiled texture.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch,
                      uint32_t bytes_per_block_log2) {
  pitch = xe::align<uint32_t>(pitch, 32);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5))
                        << (bytes_per_block_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  const int32_t offset =
      macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// A DXT texture format's block size (D3DFORMAT: the GPU format in the low 6
// bits, tiled with bit 8), 0 for anything else.
uint32_t TextureBlockBytes(uint32_t format) {
  switch (format & 0x3F) {
    case 0x12:  // DXT1
      return 8;
    case 0x13:  // DXT2/3
    case 0x14:  // DXT4/5
      return 16;
    default:
      return 0;
  }
}

// One slice from packed linear block rows to the GPU layout: rows padded to
// 32 blocks, and tiled for tiled formats.
void LayOutTextureSlice(const uint8_t* packed, uint32_t width, uint32_t height,
                        uint32_t format, uint8_t* out, size_t out_size) {
  const uint32_t block_bytes = TextureBlockBytes(format);
  const uint32_t block_bytes_log2 = block_bytes == 8 ? 3 : 4;
  const bool tiled = (format & 0x100) != 0;
  const uint32_t blocks_x = (width + 3) / 4;
  const uint32_t blocks_y = (height + 3) / 4;
  const uint32_t pitch = xe::align<uint32_t>(blocks_x, 32);
  for (uint32_t y = 0; y < blocks_y; ++y) {
    for (uint32_t x = 0; x < blocks_x; ++x) {
      const size_t offset =
          tiled ? size_t(TiledOffset2D(x, y, pitch, block_bytes_log2))
                : (size_t(y) * pitch + x) * block_bytes;
      if (offset + block_bytes <= out_size) {
        std::memcpy(out + offset,
                    packed + (size_t(y) * blocks_x + x) * block_bytes,
                    block_bytes);
      }
    }
  }
}

struct ModelMesh {
  uint32_t pass = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t uv_sets = 0;
  uint32_t stride = 0;
  // In the model's GPU data.
  uint32_t vb_offset = 0;
  uint32_t ib_offset = 0;
  // 24-byte draw records (6 words): type 1 binds a texture, type 3 is a
  // color parameter {3, id, r, g, b, a}.
  std::vector<uint32_t> records;
};

struct ModelTexture {
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t size = 0;
  uint32_t slice_size = 0;
  uint32_t slices = 0;
  uint32_t offset = 0;
};

struct DecodedModel {
  std::array<uint32_t, 10> header = {};
  std::vector<ModelMesh> meshes;
  std::vector<ModelTexture> textures;
  // header[1] bytes: texture data, vertex buffers, index buffers.
  std::vector<uint8_t> gpu;
};

// A model block (little-endian): the header {header size, GPU data size,
// texture bytes, VB bytes, IB bytes, mesh count, texture count, VB offset, IB
// offset, ?}, two words, then per mesh {pass, ?, vertex count * 32, UV bits,
// 4 words}, a byte, draw records up to the vertex count, the position step
// and minimum, a 274-bit descriptor (3 6-bit position widths, base values
// and widths of the normal, weights, bone indices and color), the vertex
// stream, {index count, 0, index bits} and the index stream; then textures
// {GPU offset, size, 32-byte header, a byte, packed data unless XAM fills it
// from another asset}.
bool DecodeModel(std::span<const uint8_t> raw, DecodedModel* model) {
  if (raw.size() < 0x40) {
    return false;
  }
  for (size_t i = 0; i < model->header.size(); ++i) {
    model->header[i] = LoadLE32(&raw[4 * i]);
  }
  const uint32_t gpu_size = model->header[1];
  const uint32_t mesh_count = model->header[5];
  const uint32_t texture_count = model->header[6];
  if (!gpu_size || gpu_size > (16u << 20) || !mesh_count || mesh_count > 16 ||
      texture_count > 16) {
    return false;
  }
  model->gpu.assign(gpu_size, 0);
  const BitReader bits(raw);
  size_t cursor = 0x30;
  uint32_t vb_cursor = model->header[7];
  uint32_t ib_cursor = model->header[8];
  for (uint32_t m = 0; m < mesh_count; ++m) {
    if (cursor + 33 > raw.size()) {
      return false;
    }
    ModelMesh mesh;
    mesh.pass = LoadLE32(&raw[cursor]);
    const uint32_t vertex_bits_x32 = LoadLE32(&raw[cursor + 8]);
    const uint32_t uv_bits = LoadLE32(&raw[cursor + 12]);
    if (vertex_bits_x32 % 32 || uv_bits % 32 || uv_bits > 32 * 8) {
      return false;
    }
    const uint32_t vertex_count = vertex_bits_x32 / 32;
    size_t record = cursor + 33;
    while (record + 24 <= raw.size() &&
           LoadLE32(&raw[record]) != vertex_count) {
      const uint32_t type = LoadLE32(&raw[record]);
      if ((type != 1 && type != 3) || mesh.records.size() >= 19 * 6) {
        return false;
      }
      for (size_t i = 0; i < 6; ++i) {
        mesh.records.push_back(LoadLE32(&raw[record + 4 * i]));
      }
      record += 24;
    }
    if (record + 20 > raw.size()) {
      return false;
    }
    const float step = 2.0f * LoadLE32Float(&raw[record + 4]);
    const float minimum[3] = {LoadLE32Float(&raw[record + 8]),
                              LoadLE32Float(&raw[record + 12]),
                              LoadLE32Float(&raw[record + 16])};
    const size_t descriptor = (record + 20) * 8;
    uint32_t position_widths[3];
    for (uint32_t i = 0; i < 3; ++i) {
      position_widths[i] = bits.Read(descriptor + 6 * i, 6);
    }
    uint32_t words[8];
    for (uint32_t i = 0; i < 8; ++i) {
      words[i] = bits.Read(descriptor + 18 + 32 * i, 32);
    }
    // Normal, weights, bone indices and color: base value and delta width.
    const uint32_t attribute_bases[4] = {words[0], words[2], words[4],
                                         words[6]};
    const uint32_t attribute_widths[4] = {words[1], words[3], words[5],
                                          words[7]};
    uint32_t vertex_bits =
        position_widths[0] + position_widths[1] + position_widths[2] + uv_bits;
    for (uint32_t width : attribute_widths) {
      if (width > 32) {
        return false;
      }
      vertex_bits += width;
    }
    mesh.vertex_count = vertex_count;
    mesh.uv_sets = uv_bits / 32;
    mesh.stride = 28 + 4 * mesh.uv_sets;
    const size_t stream = descriptor + 274;
    if (stream + size_t(vertex_count) * vertex_bits > bits.size_bits() ||
        size_t(vb_cursor) + size_t(vertex_count) * mesh.stride > gpu_size) {
      return false;
    }
    // GPU vertices, big-endian: float3 position, the normal (11:11:10),
    // weights, bone indices and color words, and half2 UV sets.
    for (uint32_t v = 0; v < vertex_count; ++v) {
      size_t bit = stream + size_t(v) * vertex_bits;
      uint8_t* out = &model->gpu[vb_cursor + size_t(v) * mesh.stride];
      float position[3];
      for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t q = bits.Read(bit, position_widths[i]);
        bit += position_widths[i];
        position[i] =
            minimum[i] + float(q) * step * (i == 1 ? kYStepScale : 1.0f);
        StoreBE32Float(out + 4 * i, position[i]);
      }
      for (uint32_t i = 0; i < 4; ++i) {
        xe::store_and_swap<uint32_t>(
            out + 12 + 4 * i,
            attribute_bases[i] + bits.Read(bit, attribute_widths[i]));
        bit += attribute_widths[i];
      }
      for (uint32_t i = 0; i < mesh.uv_sets; ++i) {
        const uint32_t uv = bits.Read(bit + 32 * i, 32);
        xe::store_and_swap<uint16_t>(out + 28 + 4 * i, uint16_t(uv));
        xe::store_and_swap<uint16_t>(out + 30 + 4 * i, uint16_t(uv >> 16));
      }
    }
    const size_t vertices_end =
        (stream + size_t(vertex_count) * vertex_bits + 7) / 8;
    if (vertices_end + 8 > raw.size()) {
      return false;
    }
    const uint32_t index_count = LoadLE32(&raw[vertices_end]);
    const uint32_t index_bits = raw[vertices_end + 6] | raw[vertices_end + 7]
                                                            << 8;
    const size_t index_stream = (vertices_end + 8) * 8;
    if (index_count % 3 || !index_bits || index_bits > 16 ||
        index_stream + size_t(index_count) * index_bits > bits.size_bits() ||
        size_t(ib_cursor) + size_t(index_count) * 2 > gpu_size) {
      return false;
    }
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint32_t index =
          bits.Read(index_stream + size_t(i) * index_bits, index_bits);
      if (index >= vertex_count) {
        return false;
      }
      xe::store_and_swap<uint16_t>(&model->gpu[ib_cursor + 2 * size_t(i)],
                                   uint16_t(index));
    }
    mesh.index_count = index_count;
    mesh.vb_offset = vb_cursor;
    mesh.ib_offset = ib_cursor;
    vb_cursor = xe::align<uint32_t>(vb_cursor + vertex_count * mesh.stride, 4);
    ib_cursor += index_count * 2;
    cursor = (index_stream + size_t(index_count) * index_bits + 7) / 8;
    model->meshes.push_back(std::move(mesh));
  }
  for (uint32_t t = 0; t < texture_count; ++t) {
    if (cursor + 41 > raw.size()) {
      return false;
    }
    ModelTexture texture;
    texture.offset = LoadLE32(&raw[cursor]);
    texture.size = LoadLE32(&raw[cursor + 4]);
    texture.format = LoadLE32(&raw[cursor + 8]);
    texture.width = LoadLE32(&raw[cursor + 12]);
    texture.height = LoadLE32(&raw[cursor + 16]);
    texture.slice_size = LoadLE32(&raw[cursor + 24]);
    texture.slices = LoadLE32(&raw[cursor + 28]);
    const uint32_t block_bytes = TextureBlockBytes(texture.format);
    if (!block_bytes || size_t(texture.offset) + texture.size > gpu_size ||
        !texture.slices || texture.slices > 64) {
      return false;
    }
    const size_t slice_packed = size_t((texture.width + 3) / 4) *
                                ((texture.height + 3) / 4) * block_bytes;
    const size_t packed = slice_packed * texture.slices;
    const size_t data = cursor + 41;
    bool has_data = data + packed + 8 <= raw.size();
    if (has_data && t + 1 < texture_count) {
      // No data when the next texture's block follows at once.
      const uint32_t next_offset = LoadLE32(&raw[data]);
      const uint32_t next_format = LoadLE32(&raw[data + 8]);
      if (next_offset == texture.offset + texture.size &&
          (next_format & 0xFFFFFE00) == 0x1A200000) {
        has_data = false;
      }
    }
    if (has_data) {
      for (uint32_t s = 0; s < texture.slices; ++s) {
        const size_t slice_offset =
            texture.offset + size_t(s) * texture.slice_size;
        if (slice_offset >= size_t(texture.offset) + texture.size) {
          break;
        }
        LayOutTextureSlice(
            &raw[data + s * slice_packed], texture.width, texture.height,
            texture.format, &model->gpu[slice_offset],
            std::min<size_t>(texture.slice_size,
                             texture.offset + texture.size - slice_offset));
      }
    }
    cursor = data + (has_data ? packed : 0);
    model->textures.push_back(texture);
  }
  return true;
}

std::mutex model_cache_mutex;
std::unordered_map<uint32_t, std::shared_ptr<const DecodedModel>> model_cache;

std::shared_ptr<const DecodedModel> GetModel(const AvatarAssetPack& pack,
                                             uint32_t index) {
  {
    std::lock_guard<std::mutex> lock(model_cache_mutex);
    auto it = model_cache.find(index);
    if (it != model_cache.end()) {
      return it->second;
    }
  }
  std::vector<uint8_t> raw;
  auto model = std::make_shared<DecodedModel>();
  if (!pack.ReadBlock(index, kStrbBlockModel, &raw) ||
      !DecodeModel(raw, model.get())) {
    XELOGE("Avatars: couldn't decode the model of asset {:04X} ({})", index,
           pack.GetName(index));
    model.reset();
  }
  std::lock_guard<std::mutex> lock(model_cache_mutex);
  if (model_cache.size() >= 96) {
    model_cache.clear();
  }
  model_cache[index] = model;
  return model;
}

// An avatar manifest item: a component (from 0x120, 32 bytes each: slots,
// the 16-byte asset ID, 3 custom colors) or a facial feature (from 0x0C: the
// slots and the asset ID's first 12 bytes; textures have 16 more bytes).
struct ManifestItem {
  uint32_t slots = 0;
  uint32_t index = 0;
  std::array<uint8_t, 16> asset_id = {};
  std::array<uint32_t, 3> custom_colors = {};
};

struct ParsedManifest {
  std::vector<ManifestItem> components;
  std::vector<ManifestItem> features;
  // Skin, hair, lips, eyes, eyebrows, eye shadow, facial hair, face paint 1
  // and 2 (ARGB).
  std::array<uint32_t, 9> colors = {};
};

// An asset ID: {index << 16 | gender, the pack's 8 bytes, ...}.
bool IsPackAssetId(const AvatarAssetPack& pack, const uint8_t* id) {
  // The first byte's top bits differ between manifests and the TOC header.
  const uint8_t* ns = pack.id_namespace();
  return (id[4] & 0x3F) == (ns[0] & 0x3F) && !std::memcmp(id + 5, ns + 1, 7);
}

ParsedManifest ParseManifest(const AvatarAssetPack& pack,
                             std::span<const uint8_t> manifest) {
  ParsedManifest parsed;
  for (size_t i = 0; i < parsed.colors.size(); ++i) {
    parsed.colors[i] = xe::load_and_swap<uint32_t>(&manifest[0xFC + 4 * i]);
  }
  auto accept = [&](uint32_t slots, const uint8_t* id, ManifestItem* item) {
    const uint32_t index = xe::load_and_swap<uint32_t>(id) >> 16;
    const AvatarAssetPack::Record* record = pack.GetRecord(index);
    if (!record || record->slots != slots) {
      XELOGW("Avatars: no asset {:08X} with slots {:X} in the pack",
             xe::load_and_swap<uint32_t>(id), slots);
      return false;
    }
    item->slots = slots;
    item->index = index;
    return true;
  };
  for (size_t offset = 0x0C; offset + 16 <= 0xFC;) {
    const uint32_t slots = xe::load_and_swap<uint32_t>(&manifest[offset]);
    if (slots >= 0x2000 && !(slots & (slots - 1)) &&
        IsPackAssetId(pack, &manifest[offset + 4])) {
      ManifestItem item;
      std::memcpy(item.asset_id.data(), &manifest[offset + 4], 12);
      if (accept(slots, &manifest[offset + 4], &item)) {
        parsed.features.push_back(item);
      }
      offset += 16;
    } else {
      offset += 4;
    }
  }
  for (size_t offset = 0x120; offset + 32 <= 0x300; offset += 32) {
    const uint32_t slots = xe::load_and_swap<uint32_t>(&manifest[offset]);
    if (!slots) {
      break;
    }
    ManifestItem item;
    std::memcpy(item.asset_id.data(), &manifest[offset + 4], 16);
    for (size_t i = 0; i < 3; ++i) {
      item.custom_colors[i] =
          xe::load_and_swap<uint32_t>(&manifest[offset + 20 + 4 * i]);
    }
    if (IsPackAssetId(pack, &manifest[offset + 4]) &&
        accept(slots, &manifest[offset + 4], &item)) {
      parsed.components.push_back(item);
    }
  }
  return parsed;
}

// The color parameter words of an ARGB color.
void SetColorRecord(uint32_t* record, uint32_t argb) {
  for (uint32_t i = 0; i < 3; ++i) {
    record[2 + i] = FloatBits(float((argb >> (16 - 8 * i)) & 0xFF) / 255.0f);
  }
}

// Where a facial feature texture goes in the head's textures (as its pixel
// shader layers them): by the texture asset's kind.
int32_t HeadTextureSlot(uint32_t texture_details) {
  switch (texture_details) {
    case 5:  // Face paint, wrinkles.
      return 0;
    case 3:  // Facial hair.
      return 1;
    case 2:  // Eyebrows.
      return 2;
    case 1:  // Eyes.
      return 3;
    case 0:  // Mouth.
      return 4;
    case 4:  // Eye shadow.
      return 5;
    default:
      return -1;
  }
}

}  // namespace

const AvatarAssetPack* AvatarAssetPack::Get() {
  static std::once_flag once;
  static std::unique_ptr<AvatarAssetPack> pack;
  std::call_once(once, []() {
    struct Candidate {
      std::string source;
      // The package's name, to install a disc's copy under.
      std::string package;
      std::unique_ptr<AvatarAssetPack> pack;
      size_t present = 0;
    };
    std::vector<Candidate> candidates;
    auto add = [&candidates](std::string source, std::string package,
                             std::vector<uint8_t> data) {
      std::vector<uint8_t> toc;
      auto loaded = std::make_unique<AvatarAssetPack>();
      if (!ExtractToc(std::move(data), &toc) || !loaded->Load(std::move(toc))) {
        return false;
      }
      size_t present = 0;
      for (const Record& record : loaded->records_) {
        present += record.strb_offset != 0;
      }
      candidates.push_back(
          {std::move(source), std::move(package), std::move(loaded), present});
      return true;
    };
    std::vector<uint8_t> data;
    if (!cvars::avatar_asset_pack_path.empty()) {
      const std::string path =
          xe::strip_pii(xe::path_to_utf8(cvars::avatar_asset_pack_path));
      if (!ReadHostFile(cvars::avatar_asset_pack_path, &data) ||
          !add(path, std::string(), std::move(data))) {
        XELOGE("Avatars: no avatar asset pack in {}", path);
      }
    }
    // Installed packs, where installing the system avatar package puts its
    // files; then the disc's: discs with Kinect system updates carry the
    // system package, and Kinect Sports also has a copy of its own.
    const std::filesystem::path installed =
        kernel_state()->emulator()->content_root() / "0000000000000000" /
        "FFFE07DF" / "00008000";
    if (candidates.empty()) {
      std::error_code ec;
      for (const auto& entry :
           std::filesystem::directory_iterator(installed, ec)) {
        const std::filesystem::path toc = entry.path() / "AvatarAssetPack.toc";
        if (entry.is_directory(ec) && ReadHostFile(toc, &data)) {
          add(xe::strip_pii(xe::path_to_utf8(toc)), std::string(),
              std::move(data));
        }
      }
      const size_t installed_count = candidates.size();
      static const struct {
        const char* package;
        const char* path;
      } kDiscPacks[] = {
          {"FFFE07DF00000002", "game:\\$SystemUpdate\\FFFE07DF00000002"},
          {"AvatarAssetPack", "game:\\AvatarAssetPack"},
      };
      for (const auto& disc_pack : kDiscPacks) {
        if (ReadGuestFile(disc_pack.path, &data)) {
          add(disc_pack.path, disc_pack.package, std::move(data));
        }
      }
      // The disc's pack, installed when it has more assets than any installed
      // one, so titles without one have avatars too.
      for (size_t i = installed_count; i < candidates.size(); ++i) {
        const Candidate& disc = candidates[i];
        bool better = true;
        for (size_t k = 0; k < installed_count; ++k) {
          better = better && disc.present > candidates[k].present;
        }
        if (!better) {
          continue;
        }
        const std::filesystem::path folder = installed / disc.package;
        const std::filesystem::path file = folder / "AvatarAssetPack.toc";
        const std::filesystem::path temporary =
            folder / "AvatarAssetPack.toc.tmp";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        bool written = false;
        if (FILE* f = xe::filesystem::OpenFile(temporary, "wb")) {
          written =
              std::fwrite(disc.pack->toc_.data(), 1, disc.pack->toc_.size(),
                          f) == disc.pack->toc_.size();
          written = std::fclose(f) == 0 && written;
        }
        if (written) {
          std::filesystem::rename(temporary, file, ec);
          written = !ec;
        }
        if (written) {
          XELOGI("Avatars: installed the avatar asset pack from {} to {}",
                 disc.source, xe::strip_pii(xe::path_to_utf8(file)));
        } else {
          XELOGW("Avatars: couldn't install the avatar asset pack to {}",
                 xe::strip_pii(xe::path_to_utf8(file)));
        }
        break;
      }
    }
    if (candidates.empty()) {
      XELOGW("Avatars: no avatar asset pack, avatars will be empty");
      return;
    }
    // The most complete one.
    auto best = std::max_element(candidates.begin(), candidates.end(),
                                 [](const Candidate& a, const Candidate& b) {
                                   return a.present < b.present;
                                 });
    XELOGI("Avatars: asset pack from {}: {} assets", best->source,
           best->present);
    pack = std::move(best->pack);
  });
  return pack.get();
}

bool AvatarAssetPack::Load(std::vector<uint8_t> toc) {
  if (toc.size() < kTocHeaderSize ||
      std::memcmp(toc.data(), kTocMagic, sizeof(kTocMagic))) {
    return false;
  }
  toc_ = std::move(toc);
  const uint32_t count = xe::load_and_swap<uint32_t>(&toc_[0x1C]);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t offset = kTocHeaderSize + size_t(i) * kTocRecordSize;
    if (offset + kTocRecordSize > toc_.size()) {
      break;
    }
    const uint8_t* r = &toc_[offset];
    Record record;
    record.slots = xe::load_and_swap<uint32_t>(r);
    record.gender = r[4];
    record.flags = r[6];
    record.subcategory = xe::load_and_swap<uint32_t>(r + 0x98);
    const uint32_t strb = xe::load_and_swap<uint32_t>(r + 0xF0);
    const uint32_t size = xe::load_and_swap<uint32_t>(r + 0xF4);
    std::vector<StrbBlock> blocks;
    if (strb && size >= 0x20 && size_t(strb) + size <= toc_.size() &&
        ParseStrb(std::span<const uint8_t>(&toc_[strb], size), &blocks)) {
      record.strb_offset = strb;
      record.strb_size = size;
      // Metadata v2: {2, gender, i32 type, u32 details, i32 category, u8
      // skeleton version}, little-endian.
      for (const StrbBlock& block : blocks) {
        if (block.id == kStrbBlockMetadata && block.length >= 10 &&
            toc_[strb + block.offset] == 2) {
          record.type = int32_t(LoadLE32(&toc_[strb + block.offset + 2]));
          record.details = LoadLE32(&toc_[strb + block.offset + 6]);
          break;
        }
      }
    }
    records_.push_back(record);
  }
  gpu_sizes_.assign(records_.size(), UINT32_MAX);
  return !records_.empty();
}

std::string AvatarAssetPack::GetName(uint32_t index) const {
  if (index >= records_.size()) {
    return std::string();
  }
  // The first of 13 localized names (UTF-16BE).
  const size_t record = kTocHeaderSize + size_t(index) * kTocRecordSize;
  const uint32_t offset = xe::load_and_swap<uint32_t>(&toc_[record + 0xBC]);
  std::string name;
  for (size_t i = offset; i + 1 < toc_.size() && name.size() < 64; i += 2) {
    const uint16_t c = xe::load_and_swap<uint16_t>(&toc_[i]);
    if (!c) {
      break;
    }
    name.push_back(c < 0x80 ? char(c) : '?');
  }
  return name;
}

bool AvatarAssetPack::FindBlock(uint32_t index, uint32_t block_id,
                                size_t* offset, size_t* length) const {
  const Record* record = GetRecord(index);
  if (!record) {
    return false;
  }
  std::vector<StrbBlock> blocks;
  ParseStrb(
      std::span<const uint8_t>(&toc_[record->strb_offset], record->strb_size),
      &blocks);
  for (const StrbBlock& block : blocks) {
    if (block.id == block_id) {
      *offset = record->strb_offset + block.offset;
      *length = block.length;
      return true;
    }
  }
  return false;
}

bool AvatarAssetPack::ReadBlock(uint32_t index, uint32_t block_id,
                                std::vector<uint8_t>* out) const {
  size_t offset, length;
  if (!FindBlock(index, block_id, &offset, &length)) {
    return false;
  }
  const std::span<const uint8_t> data(&toc_[offset], length);
  if (block_id == kStrbBlockModel || block_id == kStrbBlockTexture) {
    return DecompressAssetData(data, out);
  }
  out->assign(data.begin(), data.end());
  return true;
}

uint32_t AvatarAssetPack::GetModelGpuSize(uint32_t index) const {
  std::lock_guard<std::mutex> lock(gpu_sizes_mutex_);
  if (index >= gpu_sizes_.size()) {
    return 0;
  }
  if (gpu_sizes_[index] == UINT32_MAX) {
    // The model header is in the first chunk.
    size_t offset, length;
    std::vector<uint8_t> first_chunk;
    gpu_sizes_[index] = FindBlock(index, kStrbBlockModel, &offset, &length) &&
                                DecompressAssetData(std::span<const uint8_t>(
                                                        &toc_[offset], length),
                                                    &first_chunk, 1) &&
                                first_chunk.size() >= 8
                            ? LoadLE32(&first_chunk[4])
                            : 0;
  }
  return gpu_sizes_[index];
}

bool BuildAvatarAssets(std::span<const uint8_t> manifest,
                       uint32_t component_mask, AvatarAssets* out) {
  out->models.clear();
  out->gpu_data.clear();
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  if (!pack || manifest.size() < kAvatarManifestSize) {
    return false;
  }
  const ParsedManifest parsed = ParseManifest(*pack, manifest);
  // The body first, then the head, then the rest as the manifest lists them.
  std::vector<const ManifestItem*> components;
  for (uint32_t first : {kSlotBody, kSlotHead, 0u}) {
    for (const ManifestItem& item : parsed.components) {
      const bool is_first = item.slots == kSlotBody || item.slots == kSlotHead;
      if ((first ? item.slots == first : !is_first) &&
          (item.slots & component_mask) &&
          pack->GetRecord(item.index)->type == 1) {
        components.push_back(&item);
      }
    }
  }
  for (const ManifestItem* item : components) {
    std::shared_ptr<const DecodedModel> model = GetModel(*pack, item->index);
    if (!model) {
      continue;
    }
    AvatarAssetModel result;
    result.slots = item->slots;
    result.asset_id = item->asset_id;
    std::copy_n(model->header.begin(), result.header.size(),
                result.header.begin());
    // Texture fetch constants address 4 KB units.
    const uint32_t base =
        xe::align<uint32_t>(uint32_t(out->gpu_data.size()), 0x1000);
    out->gpu_data.resize(base + model->gpu.size());
    std::memcpy(&out->gpu_data[base], model->gpu.data(), model->gpu.size());
    result.gpu_offset = base;
    result.vb_offset = base + model->header[7];
    result.ib_offset = base + model->header[8];
    std::vector<ModelTexture> textures = model->textures;
    if (item->slots & kSlotHead) {
      // XAM fills the head's empty textures with the facial features: their
      // assets are slices of packed 128x64 masks (the head declares
      // placeholders of the same size).
      for (const ManifestItem& feature : parsed.features) {
        const AvatarAssetPack::Record* record = pack->GetRecord(feature.index);
        const int32_t slot = HeadTextureSlot(record->details);
        std::vector<uint8_t> data;
        if (record->type != 2 || slot < 0 || slot >= int32_t(textures.size()) ||
            !pack->ReadBlock(feature.index, kStrbBlockTexture, &data) ||
            data.size() < 33) {
          continue;
        }
        ModelTexture& texture = textures[slot];
        ModelTexture asset;
        asset.format = LoadLE32(&data[0]);
        asset.width = LoadLE32(&data[4]);
        asset.height = LoadLE32(&data[8]);
        asset.slice_size = LoadLE32(&data[16]);
        asset.slices = LoadLE32(&data[20]);
        const uint32_t block_bytes = TextureBlockBytes(asset.format);
        const size_t slice_packed = size_t((asset.width + 3) / 4) *
                                    ((asset.height + 3) / 4) * block_bytes;
        if (asset.format != texture.format || !block_bytes ||
            size_t(asset.slice_size) * asset.slices > texture.size ||
            33 + slice_packed * asset.slices > data.size()) {
          XELOGW("Avatars: facial feature {} doesn't fit the head",
                 pack->GetName(feature.index));
          continue;
        }
        for (uint32_t s = 0; s < asset.slices; ++s) {
          LayOutTextureSlice(&data[33 + s * slice_packed], asset.width,
                             asset.height, asset.format,
                             &out->gpu_data[base + texture.offset +
                                            size_t(s) * asset.slice_size],
                             asset.slice_size);
        }
        texture.width = asset.width;
        texture.height = asset.height;
        texture.slice_size = asset.slice_size;
        texture.slices = asset.slices;
      }
    }
    for (const ModelTexture& texture : textures) {
      result.textures.push_back({texture.format, texture.width, texture.height,
                                 texture.size, 0, texture.slice_size, 0, 1,
                                 texture.slices, base + texture.offset, 0});
    }
    // The tint colors (parameters 0x16-0x18): skin for the body, the hair
    // color for hair, the item's color table (or the manifest's custom
    // colors) otherwise. The head's parameters 0xD-0x15 are the manifest's
    // 9 colors.
    std::array<uint32_t, 3> tints = {};
    if (item->slots & kSlotBody) {
      tints.fill(parsed.colors[0]);
    } else if (item->slots & kSlotHair) {
      tints.fill(parsed.colors[1]);
    } else if (item->custom_colors[0] || item->custom_colors[1] ||
               item->custom_colors[2]) {
      tints = item->custom_colors;
    } else {
      // {u32 count, then entries of 3 {color, ?}}, little-endian.
      std::vector<uint8_t> table;
      if (pack->ReadBlock(item->index, kStrbBlockColorTable, &table) &&
          table.size() >= 28 && LoadLE32(&table[0])) {
        for (size_t i = 0; i < 3; ++i) {
          tints[i] = LoadLE32(&table[4 + 8 * i]);
        }
      }
    }
    for (const ModelMesh& mesh : model->meshes) {
      std::array<uint8_t, 512> batch = {};
      auto* words = reinterpret_cast<xe::be<uint32_t>*>(batch.data());
      words[0] = mesh.pass;
      for (size_t r = 0; r + 6 <= mesh.records.size(); r += 6) {
        uint32_t record[6];
        std::copy_n(&mesh.records[r], 6, record);
        if (record[0] == 3) {
          if (record[1] >= 0xD && record[1] <= 0x15) {
            SetColorRecord(record, parsed.colors[record[1] - 0xD]);
          } else if (record[1] >= 0x16 && record[1] <= 0x18) {
            SetColorRecord(record, tints[record[1] - 0x16]);
          }
        }
        for (size_t i = 0; i < 6; ++i) {
          words[1 + r + i] = record[i];
        }
      }
      words[121] = mesh.index_count / 3;
      words[122] = mesh.vertex_count;
      words[123] = mesh.uv_sets;
      words[124] = mesh.stride;
      words[125] = 2;
      words[126] = base + mesh.vb_offset;
      words[127] = base + mesh.ib_offset;
      result.batches.push_back(batch);
    }
    out->models.push_back(std::move(result));
  }
  return !out->models.empty();
}

void GetAvatarAssetsSizes(uint32_t component_mask, uint32_t* result_size,
                          uint32_t* gpu_size) {
  *result_size = kResultSize;
  *gpu_size = 0;
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  if (!pack) {
    return;
  }
  // The largest model of each slot.
  uint32_t total = 0x1000;
  for (uint32_t bit = 0; bit < 24; ++bit) {
    const uint32_t slot = 1u << bit;
    if (!(component_mask & slot)) {
      continue;
    }
    uint32_t largest = 0;
    const auto& records = pack->records();
    for (uint32_t i = 0; i < records.size(); ++i) {
      if (records[i].strb_offset && records[i].type == 1 &&
          (records[i].slots & slot)) {
        largest = std::max(largest, pack->GetModelGpuSize(i));
      }
    }
    total += xe::align<uint32_t>(largest, 0x1000);
  }
  *gpu_size = total;
}

uint8_t GetAvatarManifestBodyType(std::span<const uint8_t> manifest) {
  if (manifest.size() < kAvatarManifestSize) {
    return 1;
  }
  for (size_t offset = 0x120; offset + 32 <= 0x300; offset += 32) {
    const uint32_t slots = xe::load_and_swap<uint32_t>(&manifest[offset]);
    if (!slots) {
      break;
    }
    if (slots == kSlotBody) {
      // The body's asset ID ends with its gender.
      const uint16_t gender =
          xe::load_and_swap<uint16_t>(&manifest[offset + 6]);
      return gender == 2 ? 2 : 1;
    }
  }
  return 1;
}

bool GenerateAvatarManifest(uint64_t seed, uint32_t body_type,
                            std::span<uint8_t> manifest) {
  const AvatarAssetPack* pack = AvatarAssetPack::Get();
  if (!pack || manifest.size() < kAvatarManifestSize) {
    return false;
  }
  std::mt19937_64 random(seed);
  const uint8_t gender = body_type == 1 || body_type == 2
                             ? uint8_t(body_type)
                             : uint8_t(1 + random() % 2);
  // A random editor item with exactly these slots.
  auto pick = [&](std::initializer_list<uint32_t> slots, int32_t type,
                  uint32_t* id) {
    std::vector<uint32_t> candidates;
    const auto& records = pack->records();
    for (uint32_t i = 0; i < records.size(); ++i) {
      const AvatarAssetPack::Record& r = records[i];
      if (r.strb_offset && r.type == type && (r.flags & 1) &&
          (r.gender == gender || r.gender == 3) &&
          std::find(slots.begin(), slots.end(), r.slots) != slots.end()) {
        candidates.push_back(i);
      }
    }
    if (candidates.empty()) {
      return false;
    }
    const uint32_t index = candidates[random() % candidates.size()];
    *id = index << 16 | records[index].gender;
    return true;
  };
  std::fill(manifest.begin(), manifest.end(), uint8_t(0));
  uint8_t id_namespace[8];
  std::memcpy(id_namespace, pack->id_namespace(), 8);
  id_namespace[0] |= 0xC0;
  auto store_id = [&](size_t offset, uint32_t slots, uint32_t id) {
    xe::store_and_swap<uint32_t>(&manifest[offset], slots);
    xe::store_and_swap<uint32_t>(&manifest[offset + 4], id);
    std::memcpy(&manifest[offset + 8], id_namespace, 8);
  };
  static const float kShapes[] = {-0.5f, 0.0f, 0.5f};
  StoreBE32Float(&manifest[4], kShapes[random() % 3]);
  StoreBE32Float(&manifest[8], kShapes[random() % 3]);
  // Facial features: shapes {slots, ID} (16 bytes), then textures {slots,
  // ID, 1.0, 0...} (32 bytes).
  size_t offset = 0x0C;
  uint32_t id;
  for (uint32_t slots : {0x100000u, 0x80000u, 0x200000u}) {
    if (pick({slots}, 3, &id)) {
      store_id(offset, slots, id);
      offset += 16;
    }
  }
  std::vector<uint32_t> textures = {0x8000, 0x2000, 0x4000};
  if (gender == 1 && random() % 3 == 0) {
    textures.push_back(0x10000);
  }
  for (uint32_t slots : textures) {
    if (pick({slots}, 2, &id)) {
      store_id(offset, slots, id);
      StoreBE32Float(&manifest[offset + 16], 1.0f);
      offset += 32;
    }
  }
  // Colors (from titles' default avatars).
  static const uint32_t kSkin[] = {0xFF40280E, 0xFF64411B, 0xFF98652F,
                                   0xFFBE8749, 0xFFD7AA71, 0xFFD7B971};
  static const uint32_t kHair[] = {0xFF0D0D0D, 0xFF1F120B, 0xFF493421,
                                   0xFF523436, 0xFF553921, 0xFF92340E};
  static const uint32_t kLips[] = {0xFF83463F, 0xFF8A1A0E, 0xFFB56157,
                                   0xFFD46552, 0xFFD59282, 0xFFEB7D80};
  static const uint32_t kEyes[] = {0xFF243F53, 0xFF2A72A4, 0xFF312114,
                                   0xFF595D5C, 0xFF82501D, 0xFFAE9843};
  static const uint32_t kEyeShadow[] = {0xFF195381, 0xFF904F33, 0xFFA9CEF0,
                                        0xFFB2B05D, 0xFFC1C335};
  static const uint32_t kPaint[] = {0xFF399251, 0xFF4265BC, 0xFF654428,
                                    0xFFAD5F47, 0xFFD13927};
  const uint32_t hair = kHair[random() % std::size(kHair)];
  const uint32_t colors[9] = {kSkin[random() % std::size(kSkin)],
                              hair,
                              kLips[random() % std::size(kLips)],
                              kEyes[random() % std::size(kEyes)],
                              hair,
                              kEyeShadow[random() % std::size(kEyeShadow)],
                              hair,
                              kPaint[random() % std::size(kPaint)],
                              kPaint[random() % std::size(kPaint)]};
  for (size_t i = 0; i < 9; ++i) {
    xe::store_and_swap<uint32_t>(&manifest[0xFC + 4 * i], colors[i]);
  }
  // Components {slots, ID (its last word the slots << 16), 3 custom colors};
  // the clothes again in reverse at 0x300.
  struct Component {
    uint32_t slots;
    uint32_t id;
  };
  std::vector<Component> components = {
      {kSlotHead, 2u << 16 | 3},
      {kSlotBody, gender == 1 ? 0u << 16 | 1 : 1u << 16 | 2}};
  std::vector<Component> clothes;
  if (pick({kSlotHair}, 1, &id)) {
    clothes.push_back({kSlotHair, id});
  }
  if (pick({0x8, 0x208}, 1, &id)) {
    clothes.push_back({pack->GetRecord(id >> 16)->slots, id});
  }
  for (uint32_t slots : {0x10u, 0x20u}) {
    if (pick({slots}, 1, &id)) {
      clothes.push_back({slots, id});
    }
  }
  components.insert(components.end(), clothes.begin(), clothes.end());
  offset = 0x120;
  for (const Component& component : components) {
    store_id(offset, component.slots, component.id);
    xe::store_and_swap<uint32_t>(&manifest[offset + 16], component.slots << 16);
    offset += 32;
  }
  offset = 0x300;
  for (auto it = clothes.rbegin(); it != clothes.rend(); ++it) {
    store_id(offset, it->slots, it->id);
    xe::store_and_swap<uint32_t>(&manifest[offset + 16], it->slots << 16);
    offset += 32;
  }
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
