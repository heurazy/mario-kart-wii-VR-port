#include <aurora/vulkan_interop.h>

#include "../internal.hpp"
#include "../stereo.hpp"
#include "gpu.hpp"

// The same-device requirement is documented in aurora/vulkan_interop.h. This
// file is the Vulkan sibling of d3d12_interop.cpp and deliberately mirrors its
// structure, including the "wire" technique for Dawn descriptors that are only
// reachable as native C++ types.
//
// Android is the only Vulkan target with a working interop path, because the
// handoff of an image between Dawn and a native Vulkan command buffer uses
// AHardwareBuffer: it is the one external-memory mechanism Dawn's Vulkan
// backend imports on Android, and the one the Quest driver supports.
#if defined(__ANDROID__) && defined(WEBGPU_DAWN) && defined(DAWN_ENABLE_BACKEND_VULKAN) && \
    __has_include(<dawn/native/VulkanBackend.h>)
#define AURORA_VULKAN_INTEROP_AVAILABLE 1
#else
#define AURORA_VULKAN_INTEROP_AVAILABLE 0
#endif

#if AURORA_VULKAN_INTEROP_AVAILABLE

#include <android/hardware_buffer.h>

// Must precede vulkan.h: it is what declares
// VK_ANDROID_external_memory_android_hardware_buffer's structures and entry
// points, which this bridge is built entirely around.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

#include <dawn/native/VulkanBackend.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace aurora::vulkan_interop {
namespace {

Module Log("aurora::vulkan_interop");

constexpr uint64_t kUnfencedSubmission = (std::numeric_limits<uint64_t>::max)();

// Every Vulkan entry point this file uses, resolved through Dawn's own
// instance proc addr so the bridge is guaranteed to be talking to the same
// loader and the same dispatch chain Dawn is. Resolving through a separately
// dlopen'd libvulkan would appear to work and then diverge on layers.
struct VulkanFunctions {
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;

  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindImageMemory2 BindImageMemory2 = nullptr;
  PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2 = nullptr;

  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkGetFenceStatus GetFenceStatus = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;

  // VK_ANDROID_external_memory_android_hardware_buffer
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAndroidHardwareBufferProperties = nullptr;

  bool complete() const noexcept {
    return GetDeviceProcAddr && GetPhysicalDeviceProperties &&
           GetPhysicalDeviceMemoryProperties && CreateImage && DestroyImage && AllocateMemory &&
           FreeMemory && BindImageMemory2 && GetImageMemoryRequirements2 && CreateCommandPool &&
           DestroyCommandPool && AllocateCommandBuffers && FreeCommandBuffers &&
           BeginCommandBuffer && EndCommandBuffer && CmdPipelineBarrier && CmdCopyImage &&
           QueueSubmit && QueueWaitIdle && CreateFence && DestroyFence && GetFenceStatus &&
           WaitForFences && ResetFences && GetAndroidHardwareBufferProperties;
  }
};

struct NativeObjects {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamilyIndex = 0;
  VulkanFunctions fn;
};

int32_t to_vk_format(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case wgpu::TextureFormat::RGBA16Float:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

// vkCmdCopyImage requires the two formats to be size-compatible, not identical.
// That is what lets an UNORM intermediate feed an SRGB OpenXR swapchain image,
// which is the normal case on a Quest: the runtime hands out SRGB images and
// Aurora renders linear.
bool same_copy_family(VkFormat left, VkFormat right) noexcept {
  const auto family = [](VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
      return 1;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
      return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return 3;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return 4;
    default:
      return 0;
    }
  };
  const int leftFamily = family(left);
  return leftFamily != 0 && leftFamily == family(right);
}

uint32_t to_ahardwarebuffer_format(VkFormat format) noexcept {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
  default:
    // AHardwareBuffer has no BGRA colour format. Dawn reports BGRA eye targets
    // on some desktop backends, but the Quest path renders RGBA; refusing here
    // is better than silently swapping channels.
    return 0;
  }
}

bool load_functions(VkInstance instance, VkDevice device, VulkanFunctions& fn) noexcept {
  const auto instanceProc = [&](const char* name) {
    return dawn::native::vulkan::GetInstanceProcAddr(webgpu::g_device.Get(), name);
  };
  fn.GetDeviceProcAddr =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceProc("vkGetDeviceProcAddr"));
  fn.GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      instanceProc("vkGetPhysicalDeviceProperties"));
  fn.GetPhysicalDeviceMemoryProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          instanceProc("vkGetPhysicalDeviceMemoryProperties"));
  if (fn.GetDeviceProcAddr == nullptr) {
    Log.error("Dawn did not resolve vkGetDeviceProcAddr");
    return false;
  }
  (void)instance;

  const auto deviceProc = [&](const char* name) { return fn.GetDeviceProcAddr(device, name); };
#define AURORA_VK_LOAD(member, name)                                                               \
  fn.member = reinterpret_cast<PFN_vk##name>(deviceProc("vk" #name))
  AURORA_VK_LOAD(CreateImage, CreateImage);
  AURORA_VK_LOAD(DestroyImage, DestroyImage);
  AURORA_VK_LOAD(AllocateMemory, AllocateMemory);
  AURORA_VK_LOAD(FreeMemory, FreeMemory);
  AURORA_VK_LOAD(BindImageMemory2, BindImageMemory2);
  AURORA_VK_LOAD(GetImageMemoryRequirements2, GetImageMemoryRequirements2);
  AURORA_VK_LOAD(CreateCommandPool, CreateCommandPool);
  AURORA_VK_LOAD(DestroyCommandPool, DestroyCommandPool);
  AURORA_VK_LOAD(AllocateCommandBuffers, AllocateCommandBuffers);
  AURORA_VK_LOAD(FreeCommandBuffers, FreeCommandBuffers);
  AURORA_VK_LOAD(BeginCommandBuffer, BeginCommandBuffer);
  AURORA_VK_LOAD(EndCommandBuffer, EndCommandBuffer);
  AURORA_VK_LOAD(CmdPipelineBarrier, CmdPipelineBarrier);
  AURORA_VK_LOAD(CmdCopyImage, CmdCopyImage);
  AURORA_VK_LOAD(QueueSubmit, QueueSubmit);
  AURORA_VK_LOAD(QueueWaitIdle, QueueWaitIdle);
  AURORA_VK_LOAD(CreateFence, CreateFence);
  AURORA_VK_LOAD(DestroyFence, DestroyFence);
  AURORA_VK_LOAD(GetFenceStatus, GetFenceStatus);
  AURORA_VK_LOAD(WaitForFences, WaitForFences);
  AURORA_VK_LOAD(ResetFences, ResetFences);
  AURORA_VK_LOAD(GetAndroidHardwareBufferProperties, GetAndroidHardwareBufferPropertiesANDROID);
#undef AURORA_VK_LOAD

  if (!fn.complete()) {
    Log.error("The Vulkan device does not expose every entry point the OpenXR bridge needs; "
              "VK_ANDROID_external_memory_android_hardware_buffer is required");
    return false;
  }
  return true;
}

bool get_native_objects(NativeObjects& objects) noexcept {
  if (!webgpu::g_device || webgpu::g_backendType != wgpu::BackendType::Vulkan) {
    return false;
  }
  WGPUDevice device = webgpu::g_device.Get();
  objects.instance = dawn::native::vulkan::GetInstance(device);
  objects.physicalDevice = dawn::native::vulkan::GetVkPhysicalDevice(device);
  objects.device = dawn::native::vulkan::GetVkDevice(device);
  objects.queue = dawn::native::vulkan::GetVkQueue(device);
  objects.queueFamilyIndex = dawn::native::vulkan::GetGraphicsQueueFamily(device);
  if (objects.instance == VK_NULL_HANDLE || objects.physicalDevice == VK_NULL_HANDLE ||
      objects.device == VK_NULL_HANDLE || objects.queue == VK_NULL_HANDLE) {
    Log.error("Dawn returned an incomplete set of Vulkan handles");
    return false;
  }
  return load_functions(objects.instance, objects.device, objects.fn);
}

// Unlike d3d12_interop.cpp, this file uses Dawn's own descriptor types rather
// than hand-spelled wire layouts. The D3D12 side needs the wire trick because
// its descriptor embeds a ComPtr whose constructor would have to be linked; the
// Vulkan/AHardwareBuffer descriptors are plain structs, and one of them chains
// through ChainedStructOut rather than ChainedStruct, which is exactly the kind
// of detail a hand-written layout gets wrong.

// One eye's handoff buffer: a single AHardwareBuffer seen by Dawn as a
// SharedTextureMemory and by this file as a plain VkImage. Both views alias the
// same pages, so the copy below reads exactly what Aurora rendered.
struct IntermediateEye {
  AHardwareBuffer* buffer = nullptr;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  wgpu::SharedTextureMemory sharedMemory;
  wgpu::Texture texture;
  wgpu::TextureFormat webgpuFormat = wgpu::TextureFormat::Undefined;
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
  bool accessBegun = false;
  // The layout Dawn says it left the image in, fed back into the next
  // BeginAccess. VK_IMAGE_LAYOUT_UNDEFINED is correct for the first use.
  int32_t lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct PendingTarget {
  VkImage image = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

struct InFlightCommand {
  VkFence fence = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
};

class StereoBridge final {
public:
  StereoBridge(NativeObjects objects, AuroraVulkanStereoSubmittedCallback callback,
               void* userdata) noexcept
      : m_objects(std::move(objects)), m_callback(callback), m_userdata(userdata) {}

  ~StereoBridge() {
    std::lock_guard lock(m_mutex);
    (void)WaitForGpuLocked();
    DestroyIntermediatesLocked();
    if (m_commandPool != VK_NULL_HANDLE) {
      m_objects.fn.DestroyCommandPool(m_objects.device, m_commandPool, nullptr);
      m_commandPool = VK_NULL_HANDLE;
    }
  }

  bool Initialize() noexcept {
    if (!webgpu::g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer)) {
      Log.error("Dawn device lacks SharedTextureMemoryAHardwareBuffer; the OpenXR Vulkan bridge "
                "cannot hand an image to a native command buffer");
      return false;
    }
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = m_objects.queueFamilyIndex,
    };
    if (m_objects.fn.CreateCommandPool(m_objects.device, &poolInfo, nullptr, &m_commandPool) !=
        VK_SUCCESS) {
      Log.error("Could not create the Vulkan stereo copy command pool");
      return false;
    }
    return true;
  }

  bool PrepareForDestruction() noexcept {
    std::lock_guard lock(m_mutex);
    return WaitForGpuLocked();
  }

  bool SetTargets(uint64_t token, const AuroraVulkanStereoTarget* targets,
                  uint32_t targetCount) noexcept {
    if (token == 0 || targets == nullptr || targetCount == 0 ||
        targetCount > AURORA_VULKAN_STEREO_MAX_TARGETS) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_framePending || m_encoded) {
      return false;
    }
    for (uint32_t eye = 0; eye < targetCount; ++eye) {
      const auto format = static_cast<VkFormat>(targets[eye].format);
      if (targets[eye].image == 0 || targets[eye].width == 0 || targets[eye].height == 0 ||
          format == VK_FORMAT_UNDEFINED) {
        return false;
      }
      m_targets[eye] = {
          .image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(targets[eye].image)),
          .width = targets[eye].width,
          .height = targets[eye].height,
          .format = format,
      };
    }
    for (uint32_t eye = targetCount; eye < m_targets.size(); ++eye) {
      m_targets[eye] = {};
    }
    m_frameToken = token;
    m_targetCount = targetCount;
    m_framePending = true;
    return true;
  }

  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || m_encoded || frame.frameToken != m_frameToken) {
      return false;
    }
    if (EncodeLocked(encoder, frame)) {
      m_encoded = true;
      return true;
    }
    PublishAndClearFrameLocked(frame.frameToken, false);
    return false;
  }

  void Submitted(const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || !m_encoded || frame.frameToken != m_frameToken) {
      return;
    }
    const bool success = EndAccessLocked() && EnqueueNativeCopyLocked();
    PublishAndClearFrameLocked(frame.frameToken, success);
  }

  void CancelPending() noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending) {
      return;
    }
    if (m_encoded) {
      EndAccessLocked();
    }
    PublishAndClearFrameLocked(m_frameToken, false);
  }

  bool CancelBeforeEncode(uint64_t token) noexcept {
    // Never make the XR pacing thread wait behind an in-progress Encode. A
    // failed try-lock means Aurora may already own GPU-relevant work, so the
    // submitted callback remains authoritative.
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      return false;
    }
    if (token == 0 || !m_framePending || m_encoded || token != m_frameToken) {
      return false;
    }
    ClearFrameLocked();
    return true;
  }

private:
  bool EnsureIntermediate(uint32_t eye, const stereo::EyeImage& source) noexcept {
    auto& intermediate = m_intermediates[eye];
    const auto sourceFormat = static_cast<VkFormat>(to_vk_format(source.format));
    if (source.texture == nullptr || sourceFormat == VK_FORMAT_UNDEFINED ||
        source.size.width != m_targets[eye].width ||
        source.size.height != m_targets[eye].height ||
        !same_copy_family(sourceFormat, m_targets[eye].format)) {
      Log.error("Stereo eye {} does not match its OpenXR Vulkan target", eye);
      return false;
    }
    if (intermediate.texture && intermediate.width == source.size.width &&
        intermediate.height == source.size.height && intermediate.webgpuFormat == source.format) {
      return true;
    }
    if (intermediate.accessBegun) {
      return false;
    }

    // Resizing replaces the buffer, so nothing may still be reading it.
    if (!WaitForGpuLocked()) {
      return false;
    }
    DestroyIntermediateLocked(intermediate);

    const uint32_t ahbFormat = to_ahardwarebuffer_format(sourceFormat);
    if (ahbFormat == 0) {
      Log.error("Eye format has no AHardwareBuffer equivalent; cannot bridge eye {}", eye);
      return false;
    }

    const AHardwareBuffer_Desc bufferDesc{
        .width = source.size.width,
        .height = source.size.height,
        .layers = 1,
        .format = ahbFormat,
        // COLOR_OUTPUT so Dawn may render/copy into it; SAMPLED_IMAGE so the
        // native side may read it as a transfer source. Both sides bind the
        // same allocation.
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        .stride = 0,
        .rfu0 = 0,
        .rfu1 = 0,
    };
    if (AHardwareBuffer_allocate(&bufferDesc, &intermediate.buffer) != 0 ||
        intermediate.buffer == nullptr) {
      Log.error("Could not allocate the AHardwareBuffer for stereo eye {}", eye);
      return false;
    }

    if (!ImportBufferAsImageLocked(intermediate, sourceFormat, source.size.width,
                                   source.size.height)) {
      DestroyIntermediateLocked(intermediate);
      return false;
    }
    if (!ImportBufferIntoDawnLocked(intermediate, eye, source)) {
      DestroyIntermediateLocked(intermediate);
      return false;
    }

    intermediate.webgpuFormat = source.format;
    intermediate.vkFormat = sourceFormat;
    intermediate.width = source.size.width;
    intermediate.height = source.size.height;
    intermediate.lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    intermediate.initialized = false;
    return true;
  }

  // Creates this file's own VkImage view of the AHardwareBuffer, on Dawn's
  // device, so the native copy below can use it as a transfer source.
  bool ImportBufferAsImageLocked(IntermediateEye& intermediate, VkFormat format, uint32_t width,
                                 uint32_t height) noexcept {
    const auto& fn = m_objects.fn;

    VkAndroidHardwareBufferPropertiesANDROID properties{
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = nullptr,
    };
    if (fn.GetAndroidHardwareBufferProperties(m_objects.device, intermediate.buffer,
                                              &properties) != VK_SUCCESS) {
      Log.error("The Vulkan driver rejected the stereo AHardwareBuffer");
      return false;
    }

    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (fn.CreateImage(m_objects.device, &imageInfo, nullptr, &intermediate.image) != VK_SUCCESS) {
      Log.error("Could not create the Vulkan alias of the stereo AHardwareBuffer");
      return false;
    }

    // An AHardwareBuffer import is always a dedicated allocation.
    const VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = intermediate.image,
        .buffer = VK_NULL_HANDLE,
    };
    const VkImportAndroidHardwareBufferInfoANDROID importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicated,
        .buffer = intermediate.buffer,
    };
    uint32_t memoryTypeIndex = 0;
    if (!SelectMemoryTypeLocked(properties.memoryTypeBits, memoryTypeIndex)) {
      Log.error("No Vulkan memory type accepts the stereo AHardwareBuffer");
      return false;
    }
    const VkMemoryAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = properties.allocationSize,
        .memoryTypeIndex = memoryTypeIndex,
    };
    if (fn.AllocateMemory(m_objects.device, &allocateInfo, nullptr, &intermediate.memory) !=
        VK_SUCCESS) {
      Log.error("Could not import the stereo AHardwareBuffer into Vulkan memory");
      return false;
    }

    const VkBindImageMemoryInfo bindInfo{
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext = nullptr,
        .image = intermediate.image,
        .memory = intermediate.memory,
        .memoryOffset = 0,
    };
    if (fn.BindImageMemory2(m_objects.device, 1, &bindInfo) != VK_SUCCESS) {
      Log.error("Could not bind the imported stereo AHardwareBuffer memory");
      return false;
    }
    return true;
  }

  bool SelectMemoryTypeLocked(uint32_t typeBits, uint32_t& index) const noexcept {
    VkPhysicalDeviceMemoryProperties memory{};
    m_objects.fn.GetPhysicalDeviceMemoryProperties(m_objects.physicalDevice, &memory);
    for (uint32_t candidate = 0; candidate < memory.memoryTypeCount; ++candidate) {
      if ((typeBits & (1u << candidate)) == 0) {
        continue;
      }
      // An imported AHardwareBuffer must land in device-local memory to be
      // usable as a colour attachment; the host-visible types the mask may also
      // advertise are for CPU access this bridge never performs.
      if ((memory.memoryTypes[candidate].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        index = candidate;
        return true;
      }
    }
    // Fall back to the first permitted type rather than failing outright: some
    // drivers report imported types without the device-local bit.
    for (uint32_t candidate = 0; candidate < memory.memoryTypeCount; ++candidate) {
      if ((typeBits & (1u << candidate)) != 0) {
        index = candidate;
        return true;
      }
    }
    return false;
  }

  bool ImportBufferIntoDawnLocked(IntermediateEye& intermediate, uint32_t eye,
                                  const stereo::EyeImage& source) noexcept {
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor bufferDescriptor{};
    bufferDescriptor.handle = intermediate.buffer;
    const wgpu::SharedTextureMemoryDescriptor memoryDescriptor{
        .nextInChain = &bufferDescriptor,
        .label = eye == 0 ? "OpenXR left eye intermediate" : "OpenXR right eye intermediate",
    };
    intermediate.sharedMemory = webgpu::g_device.ImportSharedTextureMemory(&memoryDescriptor);
    if (!intermediate.sharedMemory) {
      Log.error("Dawn rejected the stereo AHardwareBuffer for eye {}", eye);
      return false;
    }

    wgpu::SharedTextureMemoryProperties properties{};
    if (intermediate.sharedMemory.GetProperties(&properties) != wgpu::Status::Success ||
        properties.size.width != source.size.width ||
        properties.size.height != source.size.height ||
        (properties.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
      Log.error("Dawn reported incompatible shared-texture properties for eye {}", eye);
      return false;
    }

    const wgpu::TextureDescriptor textureDescriptor{
        .label = eye == 0 ? "OpenXR left eye shared texture" : "OpenXR right eye shared texture",
        .usage = wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {source.size.width, source.size.height, 1},
        .format = properties.format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    intermediate.texture = intermediate.sharedMemory.CreateTexture(&textureDescriptor);
    if (!intermediate.texture) {
      Log.error("Dawn could not wrap the stereo AHardwareBuffer for eye {}", eye);
      return false;
    }
    return true;
  }

  bool EncodeLocked(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    CollectCompletedCommandsLocked();
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      if (!EnsureIntermediate(eye, frame.eyes[eye])) {
        return false;
      }
    }
    // Acquire every shared texture before recording any command that refers to
    // one, so a later BeginAccess failure can roll the earlier ones back
    // without leaving an unsubmitted copy that uses a texture past its access
    // interval.
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      auto& intermediate = m_intermediates[eye];
      // Tell Dawn the layout this file left the image in. On the first frame
      // that is UNDEFINED; afterwards it is the layout the native copy restored.
      wgpu::SharedTextureMemoryVkImageLayoutBeginState beginLayout{};
      beginLayout.oldLayout = intermediate.lastLayout;
      beginLayout.newLayout = intermediate.lastLayout;

      wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
      begin.nextInChain = &beginLayout;
      begin.initialized = intermediate.initialized;
      // No fences: Dawn's queue and the bridge's submissions are the same
      // VkQueue, so submission order is the dependency. This mirrors the
      // same-queue reasoning in d3d12_interop.cpp.
      begin.fenceCount = 0;
      if (intermediate.sharedMemory.BeginAccess(intermediate.texture, &begin) !=
          wgpu::Status::Success) {
        Log.error("Dawn BeginAccess failed for stereo eye {}", eye);
        for (uint32_t begunEye = 0; begunEye < eye; ++begunEye) {
          RollBackAccessLocked(m_intermediates[begunEye]);
        }
        return false;
      }
      intermediate.accessBegun = true;
    }
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      const auto& intermediate = m_intermediates[eye];
      const wgpu::TexelCopyTextureInfo source{
          .texture = *frame.eyes[eye].texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::TexelCopyTextureInfo destination{
          .texture = intermediate.texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::Extent3D extent{intermediate.width, intermediate.height, 1};
      encoder.CopyTextureToTexture(&source, &destination, &extent);
    }
    return true;
  }

  void RollBackAccessLocked(IntermediateEye& intermediate) noexcept {
    wgpu::SharedTextureMemoryVkImageLayoutEndState endLayout{};
    wgpu::SharedTextureMemoryEndAccessState end{};
    end.nextInChain = &endLayout;
    intermediate.sharedMemory.EndAccess(intermediate.texture, &end);
    intermediate.initialized = end.initialized;
    intermediate.lastLayout = endLayout.newLayout;
    intermediate.accessBegun = false;
  }

  bool EndAccessLocked() noexcept {
    bool success = true;
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      auto& intermediate = m_intermediates[eye];
      if (!intermediate.accessBegun) {
        success = false;
        continue;
      }
      wgpu::SharedTextureMemoryVkImageLayoutEndState endLayout{};
      wgpu::SharedTextureMemoryEndAccessState end{};
      end.nextInChain = &endLayout;
      if (intermediate.sharedMemory.EndAccess(intermediate.texture, &end) !=
          wgpu::Status::Success) {
        Log.error("Dawn EndAccess failed for stereo eye {}", eye);
        success = false;
      } else {
        intermediate.initialized = end.initialized;
        // Dawn hands the image back in this layout; the copy below must barrier
        // from exactly it, and restore it before the next BeginAccess.
        intermediate.lastLayout = endLayout.newLayout;
      }
      intermediate.accessBegun = false;
    }
    return success;
  }

  bool EnqueueNativeCopyLocked() noexcept {
    const auto& fn = m_objects.fn;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = m_commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (fn.AllocateCommandBuffers(m_objects.device, &allocateInfo, &commandBuffer) != VK_SUCCESS) {
      Log.error("Could not allocate the Vulkan stereo copy command buffer");
      return false;
    }

    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (fn.BeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      Log.error("Could not begin the Vulkan stereo copy command buffer");
      fn.FreeCommandBuffers(m_objects.device, m_commandPool, 1, &commandBuffer);
      return false;
    }

    constexpr VkImageSubresourceRange kColorRange{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      auto& source = m_intermediates[eye];
      const auto& destination = m_targets[eye];

      // The intermediate comes back from Dawn in whatever layout EndAccess
      // reported; the OpenXR image is owned by the runtime, which per the
      // XR_KHR_vulkan_enable2 contract hands it over in COLOR_ATTACHMENT_OPTIMAL.
      const std::array<VkImageMemoryBarrier, 2> toTransfer{
          VkImageMemoryBarrier{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .pNext = nullptr,
              .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
              .oldLayout = static_cast<VkImageLayout>(source.lastLayout),
              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = source.image,
              .subresourceRange = kColorRange,
          },
          VkImageMemoryBarrier{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .pNext = nullptr,
              .srcAccessMask = 0,
              .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = destination.image,
              .subresourceRange = kColorRange,
          },
      };
      fn.CmdPipelineBarrier(commandBuffer,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                            static_cast<uint32_t>(toTransfer.size()), toTransfer.data());

      const VkImageCopy region{
          .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .srcOffset = {0, 0, 0},
          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .dstOffset = {0, 0, 0},
          .extent = {source.width, source.height, 1},
      };
      fn.CmdCopyImage(commandBuffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

      // Restore both images. The OpenXR image must go back to the layout the
      // runtime expects at xrReleaseSwapchainImage; the intermediate must go
      // back to the layout the next BeginAccess will be told about.
      const std::array<VkImageMemoryBarrier, 2> restore{
          VkImageMemoryBarrier{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .pNext = nullptr,
              .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = source.image,
              .subresourceRange = kColorRange,
          },
          VkImageMemoryBarrier{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .pNext = nullptr,
              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = destination.image,
              .subresourceRange = kColorRange,
          },
      };
      fn.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                            nullptr, static_cast<uint32_t>(restore.size()), restore.data());
      source.lastLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (fn.EndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      Log.error("Could not end the Vulkan stereo copy command buffer");
      fn.FreeCommandBuffers(m_objects.device, m_commandPool, 1, &commandBuffer);
      return false;
    }

    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    if (fn.CreateFence(m_objects.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
      Log.error("Could not create the Vulkan stereo copy fence");
      fn.FreeCommandBuffers(m_objects.device, m_commandPool, 1, &commandBuffer);
      return false;
    }

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    // Dawn submits to this queue from its own thread. aurora_vulkan_lock_queue
    // is held by the caller for the duration of the sink's submit callback, so
    // no additional lock is taken here.
    if (fn.QueueSubmit(m_objects.queue, 1, &submitInfo, fence) != VK_SUCCESS) {
      Log.error("Could not submit the Vulkan stereo copy");
      fn.DestroyFence(m_objects.device, fence, nullptr);
      fn.FreeCommandBuffers(m_objects.device, m_commandPool, 1, &commandBuffer);
      return false;
    }

    m_commands.push_back({fence, commandBuffer});
    m_gpuIdle = false;
    return true;
  }

  void CollectCompletedCommandsLocked() noexcept {
    const auto& fn = m_objects.fn;
    auto it = m_commands.begin();
    while (it != m_commands.end()) {
      if (fn.GetFenceStatus(m_objects.device, it->fence) == VK_SUCCESS) {
        fn.DestroyFence(m_objects.device, it->fence, nullptr);
        fn.FreeCommandBuffers(m_objects.device, m_commandPool, 1, &it->commandBuffer);
        it = m_commands.erase(it);
      } else {
        ++it;
      }
    }
    if (m_commands.empty()) {
      m_gpuIdle = true;
    }
  }

  bool WaitForGpuLocked() noexcept {
    if (m_commands.empty()) {
      m_gpuIdle = true;
      return true;
    }
    const auto& fn = m_objects.fn;
    std::vector<VkFence> fences;
    fences.reserve(m_commands.size());
    for (const auto& command : m_commands) {
      fences.push_back(command.fence);
    }
    // Five seconds is far longer than any legitimate copy; a timeout means the
    // device is lost, and returning false keeps the bridge (and the caller's XR
    // images) alive rather than freeing resources with unknown GPU use.
    constexpr uint64_t kTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;
    const VkResult waited = fn.WaitForFences(m_objects.device, static_cast<uint32_t>(fences.size()),
                                             fences.data(), VK_TRUE, kTimeoutNs);
    if (waited != VK_SUCCESS) {
      Log.error("Timed out waiting for the Vulkan stereo copies to complete");
      return false;
    }
    CollectCompletedCommandsLocked();
    m_gpuIdle = true;
    return true;
  }

  void DestroyIntermediateLocked(IntermediateEye& intermediate) noexcept {
    const auto& fn = m_objects.fn;
    intermediate.texture = nullptr;
    intermediate.sharedMemory = nullptr;
    if (intermediate.image != VK_NULL_HANDLE) {
      fn.DestroyImage(m_objects.device, intermediate.image, nullptr);
    }
    if (intermediate.memory != VK_NULL_HANDLE) {
      fn.FreeMemory(m_objects.device, intermediate.memory, nullptr);
    }
    if (intermediate.buffer != nullptr) {
      AHardwareBuffer_release(intermediate.buffer);
    }
    intermediate = {};
  }

  void DestroyIntermediatesLocked() noexcept {
    for (auto& intermediate : m_intermediates) {
      DestroyIntermediateLocked(intermediate);
    }
  }

  void PublishAndClearFrameLocked(uint64_t token, bool success) noexcept {
    const auto callback = m_callback;
    void* const userdata = m_userdata;
    ClearFrameLocked();
    if (callback != nullptr && token != 0) {
      callback(token, success, userdata);
    }
  }

  void ClearFrameLocked() noexcept {
    m_framePending = false;
    m_encoded = false;
    m_frameToken = 0;
    m_targetCount = 0;
    for (auto& target : m_targets) {
      target = {};
    }
  }

  NativeObjects m_objects;
  AuroraVulkanStereoSubmittedCallback m_callback = nullptr;
  void* m_userdata = nullptr;

  std::recursive_mutex m_mutex;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  std::array<IntermediateEye, AURORA_VULKAN_STEREO_MAX_TARGETS> m_intermediates{};
  std::array<PendingTarget, AURORA_VULKAN_STEREO_MAX_TARGETS> m_targets{};
  std::vector<InFlightCommand> m_commands;
  uint64_t m_frameToken = 0;
  uint32_t m_targetCount = 0;
  bool m_framePending = false;
  bool m_encoded = false;
  bool m_gpuIdle = true;
};

std::unique_ptr<StereoBridge> g_bridge;
std::recursive_mutex g_queueMutex;

bool sink_encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame,
                 void* userdata) noexcept {
  auto* bridge = static_cast<StereoBridge*>(userdata);
  return bridge != nullptr && bridge->Encode(encoder, frame);
}

void sink_submitted(const stereo::SinkFrame& frame, void* userdata) noexcept {
  if (auto* bridge = static_cast<StereoBridge*>(userdata); bridge != nullptr) {
    bridge->Submitted(frame);
  }
}

} // namespace
} // namespace aurora::vulkan_interop

extern "C" {

bool aurora_vulkan_lock_queue(void) {
  aurora::vulkan_interop::g_queueMutex.lock();
  return true;
}

void aurora_vulkan_unlock_queue(void) { aurora::vulkan_interop::g_queueMutex.unlock(); }

bool aurora_vulkan_get_native_handles(AuroraVulkanNativeHandles* handles) {
  using namespace aurora::vulkan_interop;
  if (handles == nullptr) {
    return false;
  }
  *handles = {};

  NativeObjects objects;
  if (!get_native_objects(objects)) {
    return false;
  }

  VkPhysicalDeviceProperties properties{};
  objects.fn.GetPhysicalDeviceProperties(objects.physicalDevice, &properties);

  handles->instance = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(objects.instance));
  handles->physicalDevice =
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(objects.physicalDevice));
  handles->device = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(objects.device));
  handles->queue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(objects.queue));
  handles->queueFamilyIndex = objects.queueFamilyIndex;
  // Dawn creates exactly one queue per family for its default WGPUQueue.
  handles->queueIndex = 0;
  handles->apiVersion = properties.apiVersion;
  handles->colorFormat =
      to_vk_format(aurora::webgpu::g_graphicsConfig.surfaceConfiguration.format);
  return true;
}

bool aurora_vulkan_enable_stereo_bridge(AuroraVulkanStereoSubmittedCallback submitted,
                                        void* userdata) {
  using namespace aurora::vulkan_interop;
  if (g_bridge) {
    return false;
  }
  NativeObjects objects;
  if (!get_native_objects(objects)) {
    return false;
  }
  auto bridge = std::make_unique<StereoBridge>(std::move(objects), submitted, userdata);
  if (!bridge->Initialize()) {
    return false;
  }
  g_bridge = std::move(bridge);
  aurora::stereo::set_sink(&sink_encode, &sink_submitted, g_bridge.get());
  return true;
}

bool aurora_vulkan_set_stereo_targets(uint64_t frameToken,
                                      const AuroraVulkanStereoTarget* targets,
                                      uint32_t targetCount) {
  using namespace aurora::vulkan_interop;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount);
}

bool aurora_vulkan_cancel_stereo_targets(uint64_t frameToken) {
  using namespace aurora::vulkan_interop;
  return g_bridge && g_bridge->CancelBeforeEncode(frameToken);
}

bool aurora_vulkan_disable_stereo_bridge(void) {
  using namespace aurora::vulkan_interop;
  if (!g_bridge) {
    return true;
  }
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  g_bridge->CancelPending();
  if (!g_bridge->PrepareForDestruction()) {
    // Queued work could not be fenced. Deliberately leak the bridge rather than
    // destroying images the GPU may still be reading.
    (void)g_bridge.release();
    return false;
  }
  g_bridge.reset();
  return true;
}

} // extern "C"

#else // !AURORA_VULKAN_INTEROP_AVAILABLE

extern "C" {

bool aurora_vulkan_lock_queue(void) { return false; }

void aurora_vulkan_unlock_queue(void) {}

bool aurora_vulkan_get_native_handles(AuroraVulkanNativeHandles* handles) {
  if (handles != nullptr) {
    *handles = {};
  }
  return false;
}

bool aurora_vulkan_enable_stereo_bridge(AuroraVulkanStereoSubmittedCallback, void*) {
  return false;
}

bool aurora_vulkan_set_stereo_targets(uint64_t, const AuroraVulkanStereoTarget*, uint32_t) {
  return false;
}

bool aurora_vulkan_cancel_stereo_targets(uint64_t) { return false; }

bool aurora_vulkan_disable_stereo_bridge(void) { return true; }

} // extern "C"

#endif
