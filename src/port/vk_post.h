/**
 * Ghost Recon 2 port - the window's scaling pass (vk_post.c).
 *
 * Draws the renderer's frame onto the swapchain image with a shader instead
 * of a blit: a frame larger than its place on screen (internal resolution
 * above the window) is averaged over each screen pixel's whole footprint,
 * which a single bilinear blit cannot do -- it reads 2x2 texels however
 * much is being shrunk, so at 4x on a 1080p screen most of the detail was
 * skipped and edges shimmered. SMAA runs here too, on the frame at the
 * internal resolution before it is scaled; FSR comes next.
 */
#ifndef GR2_VK_POST_H
#define GR2_VK_POST_H

#include <stdint.h>
#include <volk.h>

/* Filters for vkp_draw. */
enum { VKP_BILINEAR = 0, VKP_AREA = 1 };

/* Once, after the device: 0 if the pass cannot be built (the caller keeps
 * blitting). */
int vkp_init(VkDevice dev, VkFormat target_format);

/* SMAA 1x on src (w x h, SHADER_READ_ONLY_OPTIMAL): returns a view of the
 * result in the same layout, or src itself if SMAA is unavailable. */
VkImageView vkp_smaa(VkCommandBuffer cmd, VkImageView src, uint32_t w, uint32_t h);

/* Record the pass into cmd. src is in SHADER_READ_ONLY_OPTIMAL, dst (a view
 * of the swapchain image) in COLOR_ATTACHMENT_OPTIMAL; the frame lands in
 * rect, the rest of dst is left as it was. A target format other than the
 * one given to vkp_init rebuilds the pipeline. Returns 0 if nothing was
 * drawn. */
int vkp_draw(VkCommandBuffer cmd, VkImageView src, uint32_t src_w, uint32_t src_h,
             VkImageView dst, VkFormat dst_format, uint32_t dst_w, uint32_t dst_h,
             VkRect2D rect, int filter);

#endif
