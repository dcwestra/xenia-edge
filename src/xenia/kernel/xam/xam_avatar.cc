/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_avatar.h"

#include <algorithm>
#include <atomic>
#include <random>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/avatar_assets.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"

DEFINE_bool(allow_avatar_initialization, false,
            "Enable Avatar Initialization\n"
            "Only set true when testing Avatar games. Certain games may crash "
            "due to requirement of full avatar implementation.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {

namespace {
// Writes GPU data as file reads do, so the GPU drops what it had of the pages.
void WriteGpuData(uint32_t address, const uint8_t* data, size_t size) {
  Memory* memory = kernel_state()->memory();
  BaseHeap* heap = memory->LookupHeap(address);
  if (heap && heap->heap_type() == HeapType::kGuestPhysical &&
      memory->LookupHeap(address + uint32_t(size) - 1) == heap) {
    auto physical_heap = static_cast<PhysicalHeap*>(heap);
    physical_heap->TriggerCallbacks(global_critical_region::AcquireDirect(),
                                    address, uint32_t(size), true, true);
    std::memcpy(
        memory->TranslatePhysical(physical_heap->GetPhysicalAddress(address)),
        data, size);
    return;
  }
  std::memcpy(memory->TranslateVirtual(address), data, size);
}

// XamAvatarGetAssets' result, as titles read it: +0 the skeleton {+0 bone
// count, +4 bones of 96 bytes}, +8 the model count, +12 their asset IDs (32
// bytes each), +16 their entries (52 bytes each: the model header's first 7
// words, the batch count at +20; +28 and +48 the textures, 44 bytes each;
// +32 the GPU data, +36 the vertex buffer, +40 the index buffer, +44 the
// draw batches, 512 bytes each). The data is in the GPU resource buffer.
void WriteAvatarAssetsResult(uint32_t result, uint32_t result_size,
                             uint32_t gpu, uint32_t gpu_size,
                             const AvatarAssets& assets) {
  constexpr uint32_t kSkeletonOffset = 0x40;
  constexpr uint32_t kBonesOffset = 0x100;
  Memory* memory = kernel_state()->memory();
  std::memset(memory->TranslateVirtual(result), 0, result_size);
  auto words = [memory](uint32_t address) {
    return memory->TranslateVirtual<xe::be<uint32_t>*>(address);
  };
  auto float_bits = [](float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  };
  words(result)[0] = result + kSkeletonOffset;
  words(result + kSkeletonOffset)[0] = uint32_t(kAvatarSkeleton.size());
  words(result + kSkeletonOffset)[1] = result + kBonesOffset;
  // Bones: {parent, first child, next sibling, 0}, then float4 model
  // position, model rotation, local position, local rotation and scale.
  for (size_t i = 0; i < kAvatarSkeleton.size(); ++i) {
    const AvatarBone& bone = kAvatarSkeleton[i];
    xe::be<uint32_t>* b = words(result + kBonesOffset + uint32_t(96 * i));
    b[0] = bone.parent;
    b[1] = bone.first_child;
    b[2] = bone.next_sibling;
    for (size_t k = 0; k < 3; ++k) {
      b[4 + k] = float_bits(bone.model_position[k]);
      b[12 + k] = float_bits(bone.local_position[k]);
    }
    for (size_t k = 0; k < 4; ++k) {
      b[8 + k] = float_bits(bone.model_rotation[k]);
      b[16 + k] = float_bits(bone.local_rotation[k]);
      b[20 + k] = float_bits(1.0f);
    }
    b[7] = b[15] = float_bits(1.0f);
  }
  uint32_t cursor =
      result + kBonesOffset + uint32_t(96 * kAvatarSkeleton.size());
  if (!gpu || assets.gpu_data.empty() || assets.gpu_data.size() > gpu_size) {
    if (!assets.gpu_data.empty()) {
      XELOGE("Avatars: the avatar's {} bytes of GPU data don't fit in {}",
             assets.gpu_data.size(), gpu_size);
    }
    return;
  }
  uint32_t needed = cursor - result;
  for (const AvatarAssetModel& model : assets.models) {
    needed += 32 + 52 + 44 * uint32_t(model.textures.size()) +
              512 * uint32_t(model.batches.size());
  }
  if (needed > result_size) {
    XELOGE("Avatars: the avatar's {} bytes of models don't fit in {}", needed,
           result_size);
    return;
  }
  WriteGpuData(gpu, assets.gpu_data.data(), assets.gpu_data.size());
  const uint32_t count = uint32_t(assets.models.size());
  const uint32_t ids = cursor;
  const uint32_t entries = ids + 32 * count;
  cursor = entries + 52 * count;
  words(result)[2] = count;
  words(result)[3] = ids;
  words(result)[4] = entries;
  for (uint32_t i = 0; i < count; ++i) {
    const AvatarAssetModel& model = assets.models[i];
    std::memcpy(memory->TranslateVirtual(ids + 32 * i), model.asset_id.data(),
                model.asset_id.size());
    words(ids + 32 * i)[4] = model.slots << 16;
    words(ids + 32 * i)[5] = 0xFF008000;
    xe::be<uint32_t>* e = words(entries + 52 * i);
    for (size_t k = 0; k < model.header.size(); ++k) {
      e[k] = model.header[k];
    }
    const uint32_t textures = cursor;
    for (const auto& texture : model.textures) {
      xe::be<uint32_t>* t = words(cursor);
      for (size_t k = 0; k < texture.size(); ++k) {
        t[k] = texture[k];
      }
      t[9] = gpu + texture[9];
      cursor += 44;
    }
    const uint32_t batches = cursor;
    for (const auto& batch : model.batches) {
      std::memcpy(memory->TranslateVirtual(cursor), batch.data(), batch.size());
      xe::be<uint32_t>* b = words(cursor);
      b[126] = gpu + b[126];
      b[127] = gpu + b[127];
      cursor += 512;
    }
    e[5] = uint32_t(model.batches.size());
    e[7] = textures;
    e[8] = gpu + model.gpu_offset;
    e[9] = gpu + model.vb_offset;
    e[10] = gpu + model.ib_offset;
    e[11] = batches;
    e[12] = textures;
  }
}
}  // namespace

// Start/End
dword_result_t XamAvatarInitialize_entry(
    dword_t coordinate_system,  // 1, 2, 4, etc
    dword_t unk2,               // 0 or 1
    dword_t processor_number,   // for thread creation?
    lpdword_t function_ptrs,    // 20b, 5 pointers
    lpdword_t unk5,             // data segment ptr
    dword_t unk6                // flags - 0x00300000, 0x30, etc
) {
  if (kernel_state()->title_id() == kAvatarEditorID) {
    return X_STATUS_SUCCESS;
  }

  return cvars::allow_avatar_initialization ? X_STATUS_SUCCESS : ~0u;
}
DECLARE_XAM_EXPORT1(XamAvatarInitialize, kAvatars, kStub);

void XamAvatarShutdown_entry() {
  // Calls XMsgStartIORequestEx(0xf3,0x600002,0,0,0,0).
  // in 12611 its XamUnloadSysApp(0xf2,1)
}
DECLARE_XAM_EXPORT1(XamAvatarShutdown, kAvatars, kStub);

// Get & Set
dword_result_t XamAvatarGetManifestLocalUser_entry(
    dword_t user_index, pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    if (user_index >= XUserMaxUserCount) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }

    if (!avatar_metadata_ptr) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }

    const auto user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user_profile) {
      extended_error = X_E_NO_SUCH_USER;
      return X_ERROR_FUNCTION_FAILED;
    }

    const uint32_t avatar_info_id =
        static_cast<uint32_t>(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1);

    X_USER_PROFILE_SETTING avatar_info_setting_data = {};

    avatar_info_setting_data.user_index = static_cast<uint32_t>(user_index);
    avatar_info_setting_data.setting_id = avatar_info_id;
    avatar_info_setting_data.data.type = X_USER_DATA_TYPE::BINARY;
    avatar_info_setting_data.data.data.binary.size = kMaxUserDataSize;
    avatar_info_setting_data.data.data.binary.ptr = 0;

    uint32_t avatar_metadata_address = avatar_metadata_ptr.guest_address();

    kernel_state()->xam_state()->user_tracker()->GetUserSetting(
        user_profile->xuid(), kDashboardID, avatar_info_id,
        &avatar_info_setting_data, avatar_metadata_address);

    // A profile without an avatar gets one made from its XUID, saved to the
    // profile as XamAvatarSetManifest saves one, so every title shows the
    // same avatar. GetUserSetting has moved avatar_metadata_address past the
    // setting.
    if (!avatar_info_setting_data.data.data.binary.ptr) {
      if (!cvars::allow_avatar_initialization ||
          !GenerateAvatarManifest(
              user_profile->xuid(), 0,
              std::span<uint8_t>(
                  kernel_state()->memory()->TranslateVirtual<uint8_t*>(
                      avatar_metadata_ptr.guest_address()),
                  kAvatarManifestSize))) {
        extended_error = X_E_FAIL;
        return X_ERROR_FUNCTION_FAILED;
      }
      X_USER_PROFILE_SETTING setting_data = {};
      setting_data.user_index = static_cast<uint32_t>(user_index);
      setting_data.setting_id = avatar_info_id;
      setting_data.data.type = X_USER_DATA_TYPE::BINARY;
      setting_data.data.data.binary.size = uint32_t(kAvatarManifestSize);
      setting_data.data.data.binary.ptr = avatar_metadata_ptr.guest_address();
      const UserSetting setting(&setting_data);
      kernel_state()->xam_state()->user_tracker()->UpsertSetting(
          user_profile->xuid(), kDashboardID, &setting);
      XELOGI(
          "Avatars: profile {:016X} had no avatar; it has a generated one "
          "now",
          user_profile->xuid());
    }

    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarGetManifestLocalUser, kAvatars, kStub);

dword_result_t XamAvatarGetManifestsByXuid_entry(
    dword_t user_index, dword_t xuid_count, lpqword_t xuid, dword_t unk,
    dword_t avatar_info_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // set unk4 to 0 or 0x80990001 after

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetManifestsByXuid, kAvatars, kStub)

dword_result_t XamAvatarGetAssetsResultSize_entry(
    dword_t avatar_component_mask, lpdword_t result_buffer_size_ptr,
    lpdword_t gpu_resource_buffer_size_ptr) {
  // Sizes that hold any avatar with these components (see XamAvatarGetAssets).
  uint32_t result_size = 0, gpu_size = 0;
  if (cvars::allow_avatar_initialization) {
    GetAvatarAssetsSizes(avatar_component_mask, &result_size, &gpu_size);
  }
  *result_buffer_size_ptr = result_size;
  *gpu_resource_buffer_size_ptr = gpu_size;

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetsResultSize, kAvatars, kSketchy);

dword_result_t XamAvatarGetAssets_entry(
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    dword_t avatar_component_mask, dword_t flags, lpdword_t result_buffer_ptr,
    lpdword_t gpu_resource_buffer_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!cvars::allow_avatar_initialization) {
    // 58410907 doesn't crash if we return failure.
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediateEx(
          overlapped_ptr, X_ERROR_FUNCTION_FAILED, X_E_FAIL, 0);
      return X_ERROR_IO_PENDING;
    }
    return X_STATUS_SUCCESS;
  }
  // The avatar's models from the system avatar asset pack. A title that
  // initialized the avatar system takes the result as given whether or not
  // the request succeeded (Kinect Sports, 4D5308C9, corrupts its heap
  // otherwise), so without the assets it's a valid result of nothing.
  std::vector<uint8_t> manifest(kAvatarManifestSize);
  if (avatar_metadata_ptr) {
    std::memcpy(manifest.data(),
                kernel_state()->memory()->TranslateVirtual(
                    avatar_metadata_ptr.guest_address()),
                manifest.size());
  }
  // Titles' custom assets in it, read now as the title has them.
  std::vector<std::vector<uint8_t>> custom_assets;
  for (const auto& [address, size] : GetAvatarManifestCustomAssets(manifest)) {
    const auto* data =
        kernel_state()->memory()->TranslateVirtual<const uint8_t*>(address);
    custom_assets.emplace_back(data, data + size);
  }
  const uint32_t component_mask = avatar_component_mask;
  const uint32_t result = result_buffer_ptr.guest_address();
  const uint32_t gpu = gpu_resource_buffer_ptr.guest_address();
  auto run = [manifest, custom_assets, component_mask, result, gpu](
                 uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;
    if (!result) {
      return X_ERROR_SUCCESS;
    }
    uint32_t result_size, gpu_size;
    GetAvatarAssetsSizes(component_mask, &result_size, &gpu_size);
    AvatarAssets assets;
    BuildAvatarAssets(manifest, component_mask, &assets, custom_assets);
    XELOGI("Avatars: {} models for components {:08X}", assets.models.size(),
           component_mask);
    WriteAvatarAssetsResult(result, result_size, gpu, gpu_size, assets);
    return X_ERROR_SUCCESS;
  };
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
  uint32_t extended_error, length;
  return run(extended_error, length);
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssets, kAvatars, kSketchy);

dword_result_t XamAvatarSetCustomAsset_entry(
    dword_t buffer_size, lpdword_t asset_data_ptr, dword_t custom_color_count,
    lpdword_t custom_colors_ptr, dword_t unknown,  // 0 from XAvatar
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  // A title's own avatar item (an STRB container) in place of the manifest's
  // in its slots; Kinect Sports dresses players for its sports this way
  // (empty clothes whose hiding templates hide the body under its outfits).
  if (!avatar_metadata_ptr || !asset_data_ptr || !buffer_size) {
    return X_E_INVALIDARG;
  }
  Memory* memory = kernel_state()->memory();
  std::vector<uint32_t> colors;
  for (uint32_t i = 0;
       custom_colors_ptr && i < std::min<uint32_t>(custom_color_count, 3);
       ++i) {
    colors.push_back(xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual(custom_colors_ptr.guest_address() + 4 * i)));
  }
  const bool set = SetAvatarManifestCustomAsset(
      std::span<uint8_t>(memory->TranslateVirtual<uint8_t*>(
                             avatar_metadata_ptr.guest_address()),
                         kAvatarManifestSize),
      std::span<const uint8_t>(memory->TranslateVirtual<const uint8_t*>(
                                   asset_data_ptr.guest_address()),
                               buffer_size),
      asset_data_ptr.guest_address(), colors);
  if (!set) {
    XELOGW("Avatars: a title's custom avatar asset ({} bytes) wasn't usable",
           uint32_t(buffer_size));
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarSetCustomAsset, kAvatars, kStub)

dword_result_t XamAvatarSetManifest_entry(
    dword_t user_index, dword_t avatar_info_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;
    // Update and save settings.
    const auto& user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);

    // Skip writing data about users with id != 0 they're not supported
    if (!user_profile) {
      extended_error = X_E_NO_SUCH_USER;
      return X_ERROR_FUNCTION_FAILED;
    }

    const uint32_t avatar_info_id =
        static_cast<uint32_t>(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1);

    X_USER_PROFILE_SETTING setting_data;
    setting_data.user_index = static_cast<uint32_t>(user_index);
    setting_data.setting_id = avatar_info_id;
    setting_data.data.type = X_USER_DATA_TYPE::BINARY;
    setting_data.data.data.binary.size = kMaxUserDataSize;
    setting_data.data.data.binary.ptr = static_cast<uint32_t>(avatar_info_ptr);

    const UserSetting setting = UserSetting(&setting_data);

    kernel_state()->xam_state()->user_tracker()->UpsertSetting(
        user_profile->xuid(), kDashboardID, &setting);

    return X_STATUS_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarSetManifest, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataRandom_entry(
    dword_t body_type, dword_t avatars_count,
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  static std::atomic<uint64_t> counter = std::random_device()();
  if (avatar_metadata_ptr && cvars::allow_avatar_initialization) {
    for (uint32_t i = 0; i < avatars_count; ++i) {
      GenerateAvatarManifest(
          counter++ * 0x9E3779B97F4A7C15ull, body_type,
          std::span<uint8_t>(
              kernel_state()->memory()->TranslateVirtual<uint8_t*>(
                  avatar_metadata_ptr.guest_address() +
                  i * uint32_t(sizeof(X_AVATAR_METADATA))),
              kAvatarManifestSize));
    }
  }
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataRandom, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataSignedOutProfileCount_entry(
    lpdword_t profile_count_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataSignedOutProfileCount, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataSignedOutProfile_entry(
    dword_t profile_index, pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataSignedOutProfile, kAvatars, kStub);

dword_result_t XamAvatarManifestGetBodyType_entry(
    pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  if (!avatar_metadata_ptr) {
    return static_cast<uint8_t>(X_AVATAR_BODY_TYPE::Male);
  }
  return GetAvatarManifestBodyType(std::span<const uint8_t>(
      kernel_state()->memory()->TranslateVirtual<const uint8_t*>(
          avatar_metadata_ptr.guest_address()),
      kAvatarManifestSize));
}
DECLARE_XAM_EXPORT1(XamAvatarManifestGetBodyType, kAvatars, kStub);

dword_result_t XamAvatarGetInstrumentation_entry(qword_t unk1, lpdword_t unk2) {
  /* Notes:
     - unk1 not used?
     - unk1 recieves values of 1, 2, and 6
     - mark implemented once confirmed first param not used and params named
  */
  if (unk2) {
    *unk2 = 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstrumentation, kAvatars, kStub);

dword_result_t XamAvatarGetAssetIcon_entry(
    lpqword_t unk1, dword_t unk2, lpqword_t unk3, lpqword_t unk4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetIcon, kAvatars, kStub);

dword_result_t XamAvatarGetAssetBinary_entry(
    lpvoid_t asset_metadata, dword_t unk2, dword_t unk3, dword_t unk4,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetBinary, kAvatars, kStub);

void XamAvatarGetInstalledAssetPackageDescription_entry(
    pointer_t<X_ASSET_ID> asset_id_ptr,
    pointer_t<XCONTENT_AGGREGATE_DATA>
        content_data_ptr  // pointer_t<XCONTENT_DATA_INTERNAL>
) {
  XCONTENT_AGGREGATE_DATA content_data;
  content_data.content_type = XContentType::kAvatarItem;
  content_data.xuid = 0;
  content_data.title_id = asset_id_ptr->title_id;
  std::string file_name =
      fmt::format("{:016X}{:08X}{:08X}", asset_id_ptr->data,
                  asset_id_ptr->data2, asset_id_ptr->title_id);
  content_data.set_file_name(file_name);
  *content_data_ptr = content_data;
  XELOGD("Looking for avatar asset: {:X}", file_name);
}
DECLARE_XAM_EXPORT1(XamAvatarGetInstalledAssetPackageDescription, kAvatars,
                    kSketchy);

void XamAvatarSetMocks_entry() {
  // No-op.
}
DECLARE_XAM_EXPORT1(XamAvatarSetMocks, kAvatars, kStub);

// Animation
const static std::map<uint64_t, std::string> XAnimationTypeMap = {
    // Animation Generic Stand
    {0x0040000000030003, "Animation Generic Stand 0"},
    {0x0040000000040003, "Animation Generic Stand 1"},
    {0x0040000000050003, "Animation Generic Stand 2"},
    {0x0040000000270003, "Animation Generic Stand 3"},
    {0x0040000000280003, "Animation Generic Stand 4"},
    {0x0040000000290003, "Animation Generic Stand 5"},
    {0x00400000002A0003, "Animation Generic Stand 6"},
    {0x00400000002B0003, "Animation Generic Stand 7"},
    // Animation Idle
    {0x0040000000130001, "Animation Male Idle Looks Around"},
    {0x0040000000140001, "Animation Male Idle Stretch"},
    {0x0040000000150001, "Animation Male Idle Shifts Weight"},
    {0x0040000000260001, "Animation Male Idle Checks Hand"},
    {0x0040000000090002, "Animation Female Idle Check Nails"},
    {0x00400000000A0002, "Animation Female Idle Looks Around"},
    {0x00400000000B0002, "Animation Female Idle Shifts Weight"},
    {0x00400000000C0002, "Animation Female Idle Fixes Shoe"},
};

// https://github.com/xenia-canary/xenia-canary/commit/212c99eee2724de15f471148d10197d89794ff32
dword_result_t XamAvatarLoadAnimation_entry(
    lpqword_t asset_id_ptr, dword_t flags, lpvoid_t output,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  /* Notes:
      - unknown[4] & unknown[0] = 0x10000000
      - Calls XMsgStartIORequestEx(0xf3, 0x60000F, overlapped_ptr, stack1, 0x18,
     unknown) 0xf2 12611
  */
  assert_true(asset_id_ptr);

  std::string summary = "Request to load avatar animation: ";

  if (XAnimationTypeMap.contains(*asset_id_ptr)) {
    summary += XAnimationTypeMap.at(*asset_id_ptr);
  } else {
    summary += fmt::format("Unknown animation 0x{:016x}",
                           static_cast<uint64_t>(*asset_id_ptr));
  }

  XELOGD("{}", summary);

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarLoadAnimation, kAvatars, kStub);

dword_result_t XamAvatarGenerateMipMaps_entry(
    lpdword_t avatar_assets_ptr, dword_t flags, dword_t buffer_size,
    lpdword_t mip_map_buffer_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGenerateMipMaps, kAvatars, kStub);

// Enum
dword_result_t XamAvatarBeginEnumAssets_entry(
    dword_t unk1, dword_t unk2, dword_t unk3, word_t unk4, dword_t unk5,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // unknown[4] & unknown[0] = 0x20080002
  // buffer_ptr[8]
  // XMsgStartIORequestEx(0xf3, 0x60000c, overlapped_ptr, buffer_ptr, 0x14,
  // unknown) 0xf2 12611

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarBeginEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEnumAssets_entry(
    lpvoid_t unk1, lpqword_t unk2, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // unk1 pointer to a struct of size 0x8a48
  // buffer_ptr = concat(unk1, unk2)
  // unknown = 0x20080002
  // XMsgStartIORequestEx(0xf3, 0x60000d, overlapped_ptr, &buffer_ptr, 8,
  // &unknown) 0xf2 12611

  return X_E_NO_MORE_FILES;  // Stop it from calling endlessly
}
DECLARE_XAM_EXPORT1(XamAvatarEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEndEnumAssets_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // unknown[4]
  // unknown[0] = 0x20080002
  // XMsgStartIORequestEx(0xf2,0x60000e,overlapped_ptr,0,0,unknown); 12611

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarEndEnumAssets, kAvatars, kStub);

// Other
dword_result_t XamAvatarWearNow_entry(
    qword_t unk1, lpdword_t unk2, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  if (kernel_state()->title_id() == kAvatarEditorID) {
    /*
      - ops
    XamSendMessageToLoadedApps(0xffffffff8000000e,0xffffffff80050018,lVar5);
    XNotifyBroadcast(0xffffffff80050018,lVar5);
    if (overlapped_ptr) {
      XMsgCompleteIORequest(overlapped_ptr,0,0,0);
    }
    */
  } else {
    // buffer_ptr = concat(unk1, *unk2);
    //  XMsgStartIORequestEx(0xf3,0x600018,overlapped_ptr,&buffer_ptr,0x14,0);
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
      return X_ERROR_IO_PENDING;
    }
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamAvatarWearNow, kAvatars, kStub);

dword_result_t XamAvatarReinstallAwardedAsset_entry(lpstring_t string_out_ptr,
                                                    dword_t string_size,
                                                    lpdword_t unk_ptr) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarReinstallAwardedAsset, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Avatar);
