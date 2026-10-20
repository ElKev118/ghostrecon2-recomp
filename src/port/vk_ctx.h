/**
 * Ghost Recon 2 port - the Vulkan context shared by the window
 * (vk_window.c, its own thread) and the renderer (vk_backend.c, on the
 * pushbuffer executor's thread).
 *
 * One device and one queue. vkQueueSubmit / vkQueuePresentKHR need the
 * queue externally synchronised, so both sides take vkw_queue_lock()
 * around them. Ordering between the renderer's work and the window's blit
 * comes from same-queue submission order plus pipeline barriers.
 */
#ifndef GR2_VK_CTX_H
#define GR2_VK_CTX_H

#include <stdint.h>
#include <volk.h>

typedef struct {
    VkInstance       inst;
    VkPhysicalDevice phys;
    VkDevice         dev;
    VkQueue          queue;
    uint32_t         qfam;
    VkPhysicalDeviceProperties props;
} VkwContext;

/* NULL until the window thread has created the device; vkw_wait_ready
 * blocks (up to timeout_ms) for it. */
const VkwContext *vkw_context(void);
const VkwContext *vkw_wait_ready(uint32_t timeout_ms);

void vkw_queue_lock(void);
void vkw_queue_unlock(void);

/* The renderer's finished frame: an image in TRANSFER_SRC_OPTIMAL layout
 * that stays valid until the next call. From then on the window shows it
 * instead of the CPU frame. */
void vkw_set_gpu_frame(VkImage img, uint32_t w, uint32_t h);

uint32_t vkw_find_memory(uint32_t type_bits, VkMemoryPropertyFlags want);

#endif
