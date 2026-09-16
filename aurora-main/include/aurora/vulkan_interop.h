#ifndef AURORA_VULKAN_INTEROP_H
#define AURORA_VULKAN_INTEROP_H

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include "stdbool.h"
#include "stdint.h"
#endif

enum { AURORA_VULKAN_STEREO_MAX_TARGETS = 2 };

/**
 * Vulkan counterpart to aurora/d3d12_interop.h, for the standalone Meta Quest
 * target, where Vulkan is the only backend.
 *
 * OpenXR requires the application to submit eye images on the SAME Vulkan
 * device the compositor was told about at session creation. Aurora owns that
 * device through Dawn, so the XR backend cannot create its own: it borrows
 * Dawn's. Dawn's stock VulkanBackend.h exposes only VkInstance, so the vendored
 * build applies aurora-main/cmake/patches/dawn-vulkan-native-handles.patch to
 * publish the rest.
 *
 * Handles are borrowed. They remain valid until aurora_shutdown() and must not
 * be destroyed by the caller.
 */
typedef struct {
  /* VkInstance, VkPhysicalDevice, VkDevice, VkQueue. Carried as uint64_t so
   * this header does not drag the Vulkan headers into every consumer; Vulkan
   * dispatchable and non-dispatchable handles both fit in 64 bits on every
   * target this project supports. */
  uint64_t instance;
  uint64_t physicalDevice;
  uint64_t device;
  uint64_t queue;
  uint32_t queueFamilyIndex;
  uint32_t queueIndex;
  /* VK_MAKE_API_VERSION-encoded version of the instance/device. */
  uint32_t apiVersion;
  /* VkFormat matching Aurora's single-sample eye output. */
  int32_t colorFormat;
} AuroraVulkanNativeHandles;

/** One acquired OpenXR swapchain image for the next Aurora stereo sink. */
typedef struct {
  /* VkImage owned by the OpenXR runtime. */
  uint64_t image;
  uint32_t width;
  uint32_t height;
  /* VkFormat the swapchain was created with. */
  int32_t format;
} AuroraVulkanStereoTarget;

/**
 * Fired when Aurora either finishes or abandons the stereo sink. `success`
 * guarantees that the final same-queue Vulkan copy and its completion fence
 * were submitted. A false result may occur after vkQueueSubmit, so callers must
 * conservatively retain externally owned targets until graphics/session
 * teardown. The callback must not wait for the GPU or re-enter Aurora.
 */
typedef void (*AuroraVulkanStereoSubmittedCallback)(uint64_t frameToken, bool success,
                                                    void* userdata);

/**
 * A Vulkan queue is not internally synchronized, and Dawn submits to it from
 * its own thread. These bracket every queue operation the bridge performs.
 * aurora_vulkan_lock_queue returns false when the queue cannot be acquired, in
 * which case the caller must not touch it.
 */
bool aurora_vulkan_lock_queue(void);
void aurora_vulkan_unlock_queue(void);

/** Returns false unless the active Aurora backend is Dawn Vulkan. */
bool aurora_vulkan_get_native_handles(AuroraVulkanNativeHandles* handles);

/**
 * Installs the internal zero-readback stereo sink. Call while Aurora's frame
 * worker is idle, after aurora_initialize().
 */
bool aurora_vulkan_enable_stereo_bridge(AuroraVulkanStereoSubmittedCallback submitted,
                                        void* userdata);

/**
 * Publishes the acquired OpenXR image(s) for frameToken. Immersive projection
 * frames supply two targets; virtual-screen quad frames supply one. Exactly
 * one frame may be pending at a time.
 */
bool aurora_vulkan_set_stereo_targets(uint64_t frameToken,
                                      const AuroraVulkanStereoTarget* targets,
                                      uint32_t targetCount);

/**
 * Withdraws frameToken only while its target has not been encoded. Safe to race
 * with Aurora's frame worker: false means the worker already owns encoded work
 * (or the token is no longer pending), so the submitted callback remains the
 * only completion authority.
 */
bool aurora_vulkan_cancel_stereo_targets(uint64_t frameToken);

/**
 * Removes the sink and drains bridge resources. The worker must be idle.
 * Returns false when queued work cannot be fenced; the bridge is then retained
 * for the process lifetime and the caller must likewise retain its graphics/XR
 * owners rather than destroying resources with unknown GPU use.
 */
bool aurora_vulkan_disable_stereo_bridge(void);

#ifdef __cplusplus
}
#endif

#endif
