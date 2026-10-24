/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_provider.h"

#include <cstdint>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/system.h"
#include "xenia/ui/vulkan/vulkan_immediate_drawer.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"

DEFINE_int32(
    vulkan_validation, 0,
    "Vulkan validation level. 0=off, 1=standard (VK_LAYER_KHRONOS_validation: "
    "API correctness, object lifetime, parameter checks), 2=+synchronization "
    "validation (catches missing barriers and RAW/WAW hazards; 2-5x slowdown), "
    "3=+GPU-assisted (catches OOB shader buffer accesses; 3-10x slowdown). "
    "Messages go to the Xenia log if 'vulkan_log_debug_messages' is enabled, "
    "or to the OS debug output otherwise.",
    "Vulkan");

DEFINE_int32(vulkan_device, -1,
             "Index of the preferred Vulkan physical device, or -1 to use any "
             "compatible device.",
             "Vulkan");

namespace xe {
namespace ui {
namespace vulkan {

std::unique_ptr<VulkanProvider> VulkanProvider::Create(
    const bool with_gpu_emulation, const bool with_presentation) {
  std::unique_ptr<VulkanProvider> provider(new VulkanProvider());

  provider->vulkan_instance_ =
      VulkanInstance::Create(with_presentation, cvars::vulkan_validation);
  if (!provider->vulkan_instance_) {
    return nullptr;
  }

  std::vector<VkPhysicalDevice> physical_devices;
  provider->vulkan_instance_->EnumeratePhysicalDevices(physical_devices);

  if (physical_devices.empty()) {
    XELOGW("No Vulkan physical devices available");
    return nullptr;
  }

  const VulkanInstance::Functions& ifn =
      provider->vulkan_instance_->functions();

  XELOGW(
      "Available Vulkan physical devices (use the 'vulkan_device' "
      "configuration variable to force a specific device):");
  std::vector<bool> physical_devices_cpu(physical_devices.size());
  for (size_t physical_device_index = 0;
       physical_device_index < physical_devices.size();
       ++physical_device_index) {
    VkPhysicalDeviceProperties physical_device_properties;
    ifn.vkGetPhysicalDeviceProperties(physical_devices[physical_device_index],
                                      &physical_device_properties);
    physical_devices_cpu[physical_device_index] =
        physical_device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    XELOGW("* {}: {}{}", physical_device_index,
           physical_device_properties.deviceName,
           physical_devices_cpu[physical_device_index]
               ? " (software rasterizer)"
               : "");
  }

  size_t physical_device_index = SIZE_MAX;
  const int32_t preferred_physical_device_index = cvars::vulkan_device;
  if (preferred_physical_device_index >= 0 &&
      uint32_t(preferred_physical_device_index) < physical_devices.size()) {
    provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
        provider->vulkan_instance_.get(),
        physical_devices[preferred_physical_device_index], with_gpu_emulation,
        with_presentation);
    if (provider->vulkan_device_) {
      physical_device_index = size_t(preferred_physical_device_index);
    }
  }

  if (!provider->vulkan_device_) {
    // Software rasterizers such as lavapipe are far too slow for emulation, so
    // try them only after every hardware device. They may be listed first, and
    // they are the only device left when the GPU's own driver fails to load.
    for (const bool cpu : {false, true}) {
      for (size_t i = 0; i < physical_devices.size(); ++i) {
        if (physical_devices_cpu[i] != cpu) {
          continue;
        }
        provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
            provider->vulkan_instance_.get(), physical_devices[i],
            with_gpu_emulation, with_presentation);
        if (provider->vulkan_device_) {
          physical_device_index = i;
          break;
        }
      }
      if (provider->vulkan_device_) {
        break;
      }
    }

    if (!provider->vulkan_device_) {
      XELOGW(
          "Couldn't choose a compatible Vulkan physical device or initialize a "
          "Vulkan logical device");
      return nullptr;
    }

    // Without this, games silently show a black screen, as emulation on the
    // CPU takes seconds per frame. A common cause is a driver update that
    // replaced the userspace libraries while the old kernel module is still
    // loaded, which the driver refuses until the computer restarts.
    if (with_gpu_emulation && physical_devices_cpu[physical_device_index]) {
      std::string message = fmt::format(
          "No GPU Vulkan driver is available, so Xenia is using '{}', a "
          "software renderer that draws on the CPU. Games will be extremely "
          "slow or show only a black screen.\n\n"
          "Your GPU's driver likely failed to load. If it was just installed "
          "or updated, restart the computer. Otherwise, make sure its kernel "
          "module and its Vulkan libraries are installed and of the same "
          "version.",
          provider->vulkan_device_->properties().deviceName);
      XELOGE("{}", message);
      xe::ShowSimpleMessageBox(xe::SimpleMessageBoxType::Warning, message);
    }
  }

  if (with_presentation) {
    provider->ui_samplers_ = UISamplers::Create(provider->vulkan_device_.get());
    if (!provider->ui_samplers_) {
      return nullptr;
    }
  }

  return provider;
}

std::unique_ptr<Presenter> VulkanProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return VulkanPresenter::Create(host_gpu_loss_callback, vulkan_device(),
                                 ui_samplers());
}

std::unique_ptr<ImmediateDrawer> VulkanProvider::CreateImmediateDrawer() {
  return VulkanImmediateDrawer::Create(vulkan_device(), ui_samplers());
}

}  // namespace vulkan
}  // namespace ui
}  // namespace xe
