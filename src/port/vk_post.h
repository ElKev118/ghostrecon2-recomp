/**
 * Ghost Recon 2 port - the window's scaling pass (vk_post.c).
 *
 * Draws the renderer's frame onto the swapchain image with a shader instead
 * of a blit: a frame larger than its place on screen (internal resolution
 * above the window) is averaged over each screen pixel's whole footprint,
 * which a single bilinear blit cannot do -- it reads 2x2 texels however
 * much is being shrunk, so at 4x on a 1080p screen most of the detail was
 * skipped and edges shimmered. SMAA runs here too, on the frame at the
 * internal resolution before it is scaled, FSR 1 when the frame is
 * enlarged, and RCAS sharpening at the end.
 */
#ifndef GR2_VK_POST_H
#define GR2_VK_POST_H

#include <stdint.h>
#include <volk.h>

/* Filters for vkp_present. */
enum { VKP_BILINEAR = 0, VKP_AREA = 1 };

/* Once, after the device: 0 if the pass cannot be built (the caller keeps
 * blitting). */
int vkp_init(VkDevice dev, VkFormat target_format);

/* SMAA 1x on src (w x h, SHADER_READ_ONLY_OPTIMAL): returns a view of the
 * result in the same layout, or src itself if SMAA is unavailable. */
VkImageView vkp_smaa(VkCommandBuffer cmd, VkImageView src, uint32_t w, uint32_t h);

/* The frame (src, SHADER_READ_ONLY_OPTIMAL) into rect of dst (a view of the
 * swapchain image, COLOR_ATTACHMENT_OPTIMAL; the rest of dst is kept):
 *   - enlarged and fsr: FSR 1 EASU; else the scaling pass (filter: VKP_AREA
 *     averages when shrinking, VKP_BILINEAR);
 *   - sharpen > 0 (0..1, 1 = sharpest): RCAS on the result.
 * Returns 0 if nothing was drawn (the caller blits). */
int vkp_present(VkCommandBuffer cmd, VkImageView src, uint32_t src_w, uint32_t src_h,
                VkImageView dst, VkFormat dst_format, uint32_t dst_w, uint32_t dst_h,
                VkRect2D rect, int filter, int fsr, float sharpen);

#endif
