/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_ASSETS_H_
#define XENIA_KERNEL_XAM_AVATAR_ASSETS_H_

#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {

// The avatar asset pack's table of contents, AvatarAssetPack.toc, from the
// system avatar package (FFFE07DF00000002) or a disc's copy of it. A 0x28-byte
// header, then 0xF8-byte records, one per asset. An asset ID's high 16 bits
// are its record's index, the low 16 bits its gender.
class AvatarAssetPack {
 public:
  struct Record {
    // The avatar slots it fills: 1 head, 2 body, 4 hair, 8 shirt, 0x10
    // trousers, 0x20 shoes, ...; facial features from 0x2000.
    uint32_t slots = 0;
    // 1 male, 2 female, 3 both.
    uint8_t gender = 0;
    // Bit 0: offered in the avatar editor (not a hat variant of a hairstyle).
    uint8_t flags = 0;
    uint32_t subcategory = 0;
    // The asset's STRB container in the table, 0 if this pack lacks it.
    uint32_t strb_offset = 0;
    uint32_t strb_size = 0;
    // From the STRB metadata: 1 component, 2 texture, 3 shape override, 4
    // animation; and the details (for textures, the facial feature kind).
    int32_t type = 0;
    uint32_t details = 0;
  };

  // The pack, loaded on first use from avatar_asset_pack_path or the running
  // title's disc. Null if there's none.
  static const AvatarAssetPack* Get();

  const std::vector<Record>& records() const { return records_; }
  const Record* GetRecord(uint32_t index) const {
    return index < records_.size() && records_[index].strb_offset
               ? &records_[index]
               : nullptr;
  }
  std::string GetName(uint32_t index) const;
  // The 8 bytes every asset ID of the pack has after its first word.
  const uint8_t* id_namespace() const { return &toc_[0x10]; }

  // The data of a record's first STRB block of the type (2 texture, 3 model,
  // 7 color table), decompressed.
  bool ReadBlock(uint32_t index, uint32_t block_id,
                 std::vector<uint8_t>* out) const;
  // A model's GPU data size (its header's total), for result sizes.
  uint32_t GetModelGpuSize(uint32_t index) const;

 private:
  bool Load(std::vector<uint8_t> toc);
  bool FindBlock(uint32_t index, uint32_t block_id, size_t* offset,
                 size_t* length) const;

  std::vector<uint8_t> toc_;
  std::vector<Record> records_;
  mutable std::mutex gpu_sizes_mutex_;
  mutable std::vector<uint32_t> gpu_sizes_;
};

// An avatar's component models as XamAvatarGetAssets returns them. Addresses
// in textures and batches are offsets into gpu_data.
struct AvatarAssetModel {
  uint32_t slots = 0;
  // The asset ID from the manifest.
  std::array<uint8_t, 16> asset_id = {};
  // The model header's first 7 words (sizes, mesh and texture counts).
  std::array<uint32_t, 7> header = {};
  // Its texture data, vertex and index buffers in gpu_data.
  uint32_t gpu_offset = 0;
  uint32_t vb_offset = 0;
  uint32_t ib_offset = 0;
  // {format, width, height, size, 0, slice size, 0, 1, slices, address, 0}.
  std::vector<std::array<uint32_t, 11>> textures;
  // A draw batch per mesh, big-endian: +0 pass, +4 draw records (texture
  // bindings and color parameters), +484 triangle count, +488 vertex count,
  // +492 UV sets, +496 vertex stride, +500 2, +504 VB, +508 IB.
  std::vector<std::array<uint8_t, 512>> batches;
};

struct AvatarAssets {
  std::vector<AvatarAssetModel> models;
  std::vector<uint8_t> gpu_data;
};

// The avatar manifest (X_AVATAR_METADATA) is 1000 bytes.
constexpr size_t kAvatarManifestSize = 1000;

// Builds an avatar's models with the components in component_mask.
bool BuildAvatarAssets(std::span<const uint8_t> manifest,
                       uint32_t component_mask, AvatarAssets* out);
// The result and GPU data sizes that hold any avatar's components in the mask.
void GetAvatarAssetsSizes(uint32_t component_mask, uint32_t* result_size,
                          uint32_t* gpu_size);
// 1 male, 2 female.
uint8_t GetAvatarManifestBodyType(std::span<const uint8_t> manifest);
// A random avatar of the pack's editor items: body_type 1 male, 2 female,
// anything else either.
bool GenerateAvatarManifest(uint64_t seed, uint32_t body_type,
                            std::span<uint8_t> manifest);

// An avatar's contents as a manifest holds them, for editing.
struct AvatarDescription {
  // 1 male, 2 female.
  uint8_t gender = 1;
  // From -1 to 1 (not applied to the skeleton yet).
  float height = 0.0f;
  float weight = 0.0f;
  // Pack record indices of its items: hair, clothes, accessories and facial
  // features (the head and the body are implied).
  std::vector<uint32_t> items;
  // Skin, hair, lips, eyes, eyebrows, eye shadow, facial hair, face paint 1
  // and 2 (ARGB).
  std::array<uint32_t, 9> colors = {};
};

bool ReadAvatarManifest(std::span<const uint8_t> manifest,
                        AvatarDescription* avatar);
bool WriteAvatarManifest(const AvatarDescription& avatar,
                         std::span<uint8_t> manifest);
bool RandomizeAvatar(uint64_t seed, uint32_t body_type,
                     AvatarDescription* avatar);
// Puts an item on, taking off the ones sharing a slot with it (an outfit
// replaces the top and bottoms, new eyes the old ones).
void SetAvatarItem(AvatarDescription* avatar, uint32_t index);
// Takes off the items occupying any of the slots.
void RemoveAvatarItems(AvatarDescription* avatar, uint32_t slots);
// The pack's avatar editor items with exactly one of these slot masks, for
// the gender, in the pack's order.
std::vector<uint32_t> GetAvatarEditorItems(std::span<const uint32_t> slots,
                                           uint8_t gender);

// One slice of a result texture ({format, width, height, ..., address} with
// the address in gpu_data) as RGBA8, from its GPU layout.
bool DecodeAvatarTexture(const AvatarAssets& assets,
                         const std::array<uint32_t, 11>& texture,
                         uint32_t slice, std::vector<uint32_t>* rgba);

// Renders the avatar in an idle pose on the CPU as titles' avatar shaders
// color it (the body's base, tint mask and overlay textures; the head's
// facial feature layers over the skin color), turned by yaw radians: RGB8,
// width by height, for the avatar editor's preview.
bool RenderAvatarPreview(const AvatarAssets& assets, uint32_t width,
                         uint32_t height, float yaw, std::vector<uint8_t>* rgb);

// The avatar skeleton titles render with, as the result's skeleton holds it:
// hierarchy (0xFF none) and the bind pose in model and parent space.
struct AvatarBone {
  uint8_t parent;
  uint8_t first_child;
  uint8_t next_sibling;
  float model_position[3];
  float model_rotation[4];
  float local_position[3];
  float local_rotation[4];
};
extern const std::array<AvatarBone, 71> kAvatarSkeleton;

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_ASSETS_H_
