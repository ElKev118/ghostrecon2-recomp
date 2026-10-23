/**
 * Ghost Recon 2 port - SDL2 window presented through Vulkan (phase 2a).
 *
 * Replaces the toolkit's GDI debug window (RECOMP_FB_WINDOW): that one
 * reopened when closed, hung on maximise and could not scale. This window
 * runs on a thread of its own, owns the SDL event loop and the Vulkan
 * device, and shows the frames the pushbuffer executor finishes:
 *
 *   executor thread  flip -> xbox_FramebufferSetPresentHook callback
 *                    -> copies the X8R8G8B8 frame into a shared buffer
 *   window thread    SDL events (keys into the pad table, close = exit)
 *                    -> new frame? upload, scale to the window keeping 4:3,
 *                       present
 *
 * Phase 2a only presents what the CPU rasteriser drew. The GPU renderer
 * (Nv2aBackend) comes next and will hand its own image to the same
 * swapchain.
 *
 * Knobs:
 *   RECOMP_WINDOW=sdl          use this window (run_gr2.bat sets it)
 *   RECOMP_WINDOW_SCALE=<n>    initial size n x 640x480 (default 2)
 *   RECOMP_PRESENT_VSYNC=0     present without waiting for the display
 *   F11 / Alt+Enter            toggle borderless fullscreen
 *   Delete / Home              the port options menu (port_menu.cpp)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_vulkan.h>
#include <volk.h>

#include "video_player.h"   /* xbox_FramebufferSetPresentHook */
#include "keyboard_sdl.h"   /* xbox_InputSdlEvent */
#include "vk_ctx.h"
#include "port_menu.h"
#include "port_settings.h"

#define VKW_MAX_IMAGES 8

static struct {
    SDL_Window      *win;
    VkInstance       inst;
    VkSurfaceKHR     surf;
    VkPhysicalDevice phys;
    VkDevice         dev;
    uint32_t         qfam;
    VkQueue          queue;

    VkSwapchainKHR   swap;
    VkFormat         swap_fmt;
    VkExtent2D       swap_ext;
    uint32_t         swap_count;
    VkImage          swap_img[VKW_MAX_IMAGES];
    VkImageView      swap_view[VKW_MAX_IMAGES];  /* the menu renders into these */
    int              swap_dirty;

    /* The guest frame: a host-visible staging buffer and the image it is
     * copied into before being scaled onto the swapchain. */
    VkBuffer         stage;
    VkDeviceMemory   stage_mem;
    void            *stage_map;
    VkImage          frame;
    VkDeviceMemory   frame_mem;
    uint32_t         fw, fh;

    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;
    VkSemaphore      sem_acquire, sem_done;

    /* VK_EXT_full_screen_exclusive: whether the driver may take the display
     * for itself (see create_swapchain). */
    int              fse;
    int              has_surf_caps2;
} V;

/* Shared with the executor thread. */
static SDL_mutex *s_lock;
static SDL_mutex *s_qlock;          /* the queue: vkQueueSubmit/Present */
static VkwContext s_ctx;
static volatile int s_ready;
/* The GPU renderer's frame, when there is one (vkw_set_gpu_frame). */
static VkImage    s_gpu_img;
static uint32_t   s_gpu_w, s_gpu_h;
static unsigned   s_gpu_frames, s_gpu_shown;
static uint32_t  *s_pending;        /* latest finished frame */
static uint32_t   s_pw, s_ph;
static int        s_have_frame;
static unsigned   s_flips;

static int vk_fail(const char *what, VkResult r)
{
    fprintf(stderr, "[VKW] %s failed (VkResult %d)\n", what, (int)r);
    return 0;
}

#define VKCHECK(call, what) do { VkResult r_ = (call); \
    if (r_ != VK_SUCCESS) return vk_fail(what, r_); } while (0)

static uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags want)
{
    return vkw_find_memory(type_bits, want);
}

uint32_t vkw_find_memory(uint32_t type_bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(V.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* ---- instance / device ------------------------------------------------ */

static int create_device(void)
{
    const char *exts[16];
    unsigned n_ext = 16;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice devs[8];
    uint32_t n_dev = 8, d;

    VKCHECK(volkInitialize(), "volkInitialize (is a Vulkan driver installed?)");
    if (!SDL_Vulkan_GetInstanceExtensions(V.win, &n_ext, exts)) {
        fprintf(stderr, "[VKW] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return 0;
    }
    {
        /* VK_EXT_full_screen_exclusive needs this on the instance. */
        VkExtensionProperties ip[64];
        uint32_t n_ip = 64, k;
        if (vkEnumerateInstanceExtensionProperties(NULL, &n_ip, ip) >= 0)
            for (k = 0; k < n_ip && n_ext < 16; k++)
                if (!strcmp(ip[k].extensionName,
                            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
                    exts[n_ext++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
                    V.has_surf_caps2 = 1;
                }
    }
    app.pApplicationName = "Ghost Recon 2 (recompiled)";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = n_ext;
    ici.ppEnabledExtensionNames = exts;
    VKCHECK(vkCreateInstance(&ici, NULL, &V.inst), "vkCreateInstance");
    volkLoadInstance(V.inst);

    if (!SDL_Vulkan_CreateSurface(V.win, V.inst, &V.surf)) {
        fprintf(stderr, "[VKW] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }

    VKCHECK(vkEnumeratePhysicalDevices(V.inst, &n_dev, devs), "vkEnumeratePhysicalDevices");
    /* The first device with a queue that can draw and present; a discrete
     * GPU is preferred if there are several. */
    for (int pass = 0; pass < 2 && !V.phys; pass++) {
        for (d = 0; d < n_dev && !V.phys; d++) {
            VkPhysicalDeviceProperties pp;
            VkQueueFamilyProperties qf[16];
            uint32_t n_qf = 16, q;

            vkGetPhysicalDeviceProperties(devs[d], &pp);
            if (pass == 0 && pp.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                continue;
            vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &n_qf, qf);
            for (q = 0; q < n_qf; q++) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, V.surf, &present);
                if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    V.phys = devs[d];
                    V.qfam = q;
                    fprintf(stderr, "[VKW] GPU: %s (Vulkan %u.%u)\n", pp.deviceName,
                            VK_API_VERSION_MAJOR(pp.apiVersion),
                            VK_API_VERSION_MINOR(pp.apiVersion));
                    break;
                }
            }
        }
    }
    if (!V.phys) {
        fprintf(stderr, "[VKW] no GPU can present to this window\n");
        return 0;
    }

    {
        float prio = 1.0f;
        /* Swapchain for the window; push descriptors and (core 1.3)
         * dynamic rendering for the renderer, which binds a new set of
         * textures and a new target per batch. */
        const char *dev_ext[3] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                   VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, NULL };
        uint32_t n_dev_ext = 2;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        f2.pNext = &f13;
        f2.features.samplerAnisotropy = VK_TRUE;
        f2.features.depthClamp = VK_TRUE;
        qci.queueFamilyIndex = V.qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        dci.pNext = &f2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (V.has_surf_caps2) {
            VkExtensionProperties dp[256];
            uint32_t n_dp = 256, k;
            if (vkEnumerateDeviceExtensionProperties(V.phys, NULL, &n_dp, dp) >= 0)
                for (k = 0; k < n_dp; k++)
                    if (!strcmp(dp[k].extensionName,
                                VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)) {
                        dev_ext[n_dev_ext++] = VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME;
                        V.fse = 1;
                    }
        }
        dci.enabledExtensionCount = n_dev_ext;
        dci.ppEnabledExtensionNames = dev_ext;
        VKCHECK(vkCreateDevice(V.phys, &dci, NULL, &V.dev), "vkCreateDevice");
        volkLoadDevice(V.dev);
        vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
        fprintf(stderr, "[VKW] full-screen exclusive control: %s\n",
                V.fse ? "yes (borderless stays a window)" : "not offered by the driver");
        s_ctx.inst = V.inst;
        s_ctx.phys = V.phys;
        s_ctx.dev = V.dev;
        s_ctx.queue = V.queue;
        s_ctx.qfam = V.qfam;
        vkGetPhysicalDeviceProperties(V.phys, &s_ctx.props);
    }

    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = V.qfam;
        VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &V.pool), "vkCreateCommandPool");
        cai.commandPool = V.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(V.dev, &cai, &V.cmd), "vkAllocateCommandBuffers");
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(V.dev, &fci, NULL, &V.fence), "vkCreateFence");
        VKCHECK(vkCreateSemaphore(V.dev, &sci, NULL, &V.sem_acquire), "vkCreateSemaphore");
        VKCHECK(vkCreateSemaphore(V.dev, &sci, NULL, &V.sem_done), "vkCreateSemaphore");
    }
    return 1;
}

/* ---- swapchain -------------------------------------------------------- */

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[32];
    VkPresentModeKHR modes[8];
    uint32_t n_fmt = 32, n_mode = 8, i;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = V.swap;
    int vsync = port_settings()->vsync;
    int w, h;

    vkDeviceWaitIdle(V.dev);
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, V.surf, &caps), "surface caps");
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surf, &n_fmt, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surf, &n_mode, modes);

    SDL_Vulkan_GetDrawableSize(V.win, &w, &h);
    if (caps.currentExtent.width != UINT32_MAX) {
        V.swap_ext = caps.currentExtent;
    } else {
        V.swap_ext.width = (uint32_t)w;
        V.swap_ext.height = (uint32_t)h;
    }
    if (!V.swap_ext.width || !V.swap_ext.height)
        return 1;                              /* minimised: try later */

    V.swap_fmt = fmts[0].format;
    for (i = 0; i < n_fmt; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            V.swap_fmt = fmts[i].format;
            sci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    if (i == n_fmt)
        sci.imageColorSpace = fmts[0].colorSpace;

    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync)
        for (i = 0; i < n_mode; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR
                || modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                sci.presentMode = modes[i];

    sci.surface = V.surf;
    sci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
        sci.minImageCount = caps.maxImageCount;
    sci.imageFormat = V.swap_fmt;
    sci.imageExtent = V.swap_ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    /* Without this the driver decides, and AMD's and NVIDIA's take a window
     * that covers the screen as exclusive fullscreen: borderless then
     * behaved like old exclusive mode -- no taskbar over it, Alt+Tab
     * switching the display. Windowed and borderless are disallowed;
     * exclusive fullscreen leaves it to the driver. */
    VkSurfaceFullScreenExclusiveInfoEXT fsi = {
        VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    if (V.fse) {
        fsi.fullScreenExclusive = port_settings()->window_mode == PS_FULLSCREEN
            ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
            : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
        sci.pNext = &fsi;
    }
    VKCHECK(vkCreateSwapchainKHR(V.dev, &sci, NULL, &V.swap), "vkCreateSwapchainKHR");
    if (old)
        vkDestroySwapchainKHR(V.dev, old, NULL);

    V.swap_count = VKW_MAX_IMAGES;
    vkGetSwapchainImagesKHR(V.dev, V.swap, &V.swap_count, V.swap_img);
    for (i = 0; i < VKW_MAX_IMAGES; i++) {
        if (V.swap_view[i])
            vkDestroyImageView(V.dev, V.swap_view[i], NULL);
        V.swap_view[i] = VK_NULL_HANDLE;
    }
    for (i = 0; i < V.swap_count; i++) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = V.swap_img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = V.swap_fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        VKCHECK(vkCreateImageView(V.dev, &vci, NULL, &V.swap_view[i]), "swapchain view");
    }
    pmenu_set_image_count(V.swap_count);
    V.swap_dirty = 0;
    return 1;
}

/* ---- the guest frame image ------------------------------------------- */

static void destroy_frame(void)
{
    if (V.stage_map) vkUnmapMemory(V.dev, V.stage_mem);
    if (V.stage) vkDestroyBuffer(V.dev, V.stage, NULL);
    if (V.stage_mem) vkFreeMemory(V.dev, V.stage_mem, NULL);
    if (V.frame) vkDestroyImage(V.dev, V.frame, NULL);
    if (V.frame_mem) vkFreeMemory(V.dev, V.frame_mem, NULL);
    V.stage_map = NULL; V.stage = VK_NULL_HANDLE; V.stage_mem = VK_NULL_HANDLE;
    V.frame = VK_NULL_HANDLE; V.frame_mem = VK_NULL_HANDLE;
}

static int create_frame(uint32_t w, uint32_t h)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };

    vkDeviceWaitIdle(V.dev);
    destroy_frame();

    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &V.stage), "staging buffer");
    vkGetBufferMemoryRequirements(V.dev, V.stage, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &mai, NULL, &V.stage_mem), "staging memory");
    vkBindBufferMemory(V.dev, V.stage, V.stage_mem, 0);
    VKCHECK(vkMapMemory(V.dev, V.stage_mem, 0, VK_WHOLE_SIZE, 0, &V.stage_map), "map");

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;     /* X8R8G8B8 in memory order */
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(V.dev, &ici, NULL, &V.frame), "frame image");
    vkGetImageMemoryRequirements(V.dev, V.frame, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &mai, NULL, &V.frame_mem), "frame memory");
    vkBindImageMemory(V.dev, V.frame, V.frame_mem, 0);
    V.fw = w;
    V.fh = h;
    return 1;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(V.cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- one present ------------------------------------------------------ */

static int present(int upload)
{
    VkImage gpu_img;
    uint32_t gpu_w, gpu_h;
    uint32_t idx;
    VkResult r;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT
                                    | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    if (V.swap_dirty && !create_swapchain())
        return 0;
    if (!V.swap || !V.swap_ext.width)
        return 1;
    SDL_LockMutex(s_lock);
    gpu_img = s_gpu_img;
    gpu_w = s_gpu_w;
    gpu_h = s_gpu_h;
    s_gpu_shown = s_gpu_frames;
    SDL_UnlockMutex(s_lock);
    if (gpu_img)
        upload = 0;                           /* the renderer owns the picture */

    vkWaitForFences(V.dev, 1, &V.fence, VK_TRUE, UINT64_MAX);
    r = vkAcquireNextImageKHR(V.dev, V.swap, UINT64_MAX, V.sem_acquire,
                              VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { V.swap_dirty = 1; return 1; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return vk_fail("vkAcquireNextImageKHR", r);
    vkResetFences(V.dev, 1, &V.fence);

    vkResetCommandBuffer(V.cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(V.cmd, &bi);

    if (upload) {
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = V.fw;
        c.imageExtent.height = V.fh;
        c.imageExtent.depth = 1;
        barrier(V.frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(V.cmd, V.stage, V.frame,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(V.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    barrier(V.swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(V.cmd, V.swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &all);

    if (gpu_img || (V.frame && s_flips)) {
        /* Scale the 4:3 frame to fit the window, centred (letterbox or
         * pillarbox), filtered. */
        VkImageBlit b;
        VkImage src = gpu_img ? gpu_img : V.frame;
        uint32_t sw = gpu_img ? gpu_w : V.fw, sh = gpu_img ? gpu_h : V.fh;
        uint32_t ww = V.swap_ext.width, wh = V.swap_ext.height;
        /* The display is 4:3 whatever the surface: an anti-aliased surface
         * is twice as wide (and maybe as tall) as what it shows. */
        uint32_t dw = ww, dh = ww * 3 / 4;

        if (dh > wh) { dh = wh; dw = wh * 4 / 3; }
        memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[1].x = (int32_t)sw;
        b.srcOffsets[1].y = (int32_t)sh;
        b.srcOffsets[1].z = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[0].x = (int32_t)((ww - dw) / 2);
        b.dstOffsets[0].y = (int32_t)((wh - dh) / 2);
        b.dstOffsets[1].x = b.dstOffsets[0].x + (int32_t)dw;
        b.dstOffsets[1].y = b.dstOffsets[0].y + (int32_t)dh;
        b.dstOffsets[1].z = 1;
        if (!upload || gpu_img)
            /* The renderer's copy into its frame image was submitted
             * earlier on this queue: make its writes visible. */
            barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdBlitImage(V.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       V.swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, VK_FILTER_LINEAR);
    }

    if (pmenu_is_open()) {
        /* The options overlay, drawn over the frame. Under the queue lock:
         * ImGui uploads its font atlas with a submit of its own. */
        barrier(V.swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        vkw_queue_lock();
        pmenu_draw(V.cmd, V.swap_view[idx], V.swap_ext.width, V.swap_ext.height);
        vkw_queue_unlock();
        barrier(V.swap_img[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        barrier(V.swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(V.cmd);

    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &V.sem_acquire;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &V.sem_done;
    vkw_queue_lock();
    r = vkQueueSubmit(V.queue, 1, &si, V.fence);
    if (r != VK_SUCCESS) {
        vkw_queue_unlock();
        return vk_fail("vkQueueSubmit", r);
    }

    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &V.sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &V.swap;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(V.queue, &pi);
    vkw_queue_unlock();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        V.swap_dirty = 1;
    else if (r != VK_SUCCESS)
        return vk_fail("vkQueuePresentKHR", r);
    return 1;
}

/* ---- executor side ---------------------------------------------------- */

static void on_flip(const uint32_t *xrgb, uint32_t w, uint32_t h)
{
    SDL_LockMutex(s_lock);
    if (!s_pending || s_pw != w || s_ph != h) {
        free(s_pending);
        s_pending = (uint32_t *)malloc((size_t)w * h * 4);
        s_pw = w;
        s_ph = h;
    }
    if (s_pending) {
        memcpy(s_pending, xrgb, (size_t)w * h * 4);
        s_have_frame = 1;
        /* With the Vulkan renderer every flip also arrives as
         * vkw_set_gpu_frame, which counts it: counting here too showed
         * twice the real frame rate in the title. */
        if (!s_gpu_img)
            s_flips++;
    }
    SDL_UnlockMutex(s_lock);
}

/* ---- window thread ---------------------------------------------------- */

/* PS_WINDOWED / PS_BORDERLESS / PS_FULLSCREEN. Exclusive fullscreen takes
 * the desktop's own mode, so the display does not switch resolution. */
static void set_window_mode(int mode)
{
    if (mode == PS_FULLSCREEN) {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(V.win), &dm) == 0)
            SDL_SetWindowDisplayMode(V.win, &dm);
        SDL_SetWindowFullscreen(V.win, SDL_WINDOW_FULLSCREEN);
    } else {
        SDL_SetWindowFullscreen(V.win, mode == PS_BORDERLESS
                                ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
    V.swap_dirty = 1;
}

static void set_vsync(int on)
{
    (void)on;                  /* create_swapchain reads port_settings() */
    V.swap_dirty = 1;
}

static int window_thread(void *unused)
{
    const char *sc = getenv("RECOMP_WINDOW_SCALE");
    int scale = sc ? atoi(sc) : 2;
    Uint32 last_title = SDL_GetTicks();
    unsigned flips_at_title = 0;
    int fullscreen = 0;

    (void)unused;
    if (scale < 1) scale = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[VKW] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    V.win = SDL_CreateWindow("Ghost Recon 2", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, 640 * scale, 480 * scale,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!V.win) {
        fprintf(stderr, "[VKW] SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    xbox_InputSdlInit();
    if (!create_device() || !create_swapchain()) {
        fprintf(stderr, "[VKW] Vulkan unavailable; no window\n");
        SDL_DestroyWindow(V.win);
        return 1;
    }
    fprintf(stderr, "[VKW] window up: %ux%u, swapchain %u images\n",
            V.swap_ext.width, V.swap_ext.height, V.swap_count);
    {
        PortMenuInit mi;
        memset(&mi, 0, sizeof mi);
        mi.win = V.win;
        mi.inst = V.inst;
        mi.phys = V.phys;
        mi.dev = V.dev;
        mi.qfam = V.qfam;
        mi.queue = V.queue;
        mi.format = V.swap_fmt;
        mi.image_count = V.swap_count;
        mi.set_window_mode = set_window_mode;
        mi.set_vsync = set_vsync;
        if (!pmenu_init(&mi))
            fprintf(stderr, "[VKW] options menu unavailable\n");
    }
    fullscreen = port_settings()->window_mode != PS_WINDOWED;
    if (fullscreen)
        set_window_mode(port_settings()->window_mode);
    xbox_FramebufferSetPresentHook(on_flip);
    s_ready = 1;

    for (;;) {
        SDL_Event ev;
        int upload = 0;

        while (SDL_PollEvent(&ev)) {
            if (pmenu_event(&ev))
                continue;             /* the options menu's, not the game's */
            if (ev.type == SDL_QUIT) {
                /* Lifted code cannot be stopped mid-flight; closing the
                 * window ends the process, as the user expects. */
                fprintf(stderr, "[VKW] window closed, exiting\n");
                fflush(stderr);
                _exit(0);
            }
            if (ev.type == SDL_KEYDOWN
                && (ev.key.keysym.sym == SDLK_F11
                    || (ev.key.keysym.sym == SDLK_RETURN
                        && (ev.key.keysym.mod & KMOD_ALT)))) {
                fullscreen = !fullscreen;
                port_settings()->window_mode = fullscreen ? PS_BORDERLESS : PS_WINDOWED;
                set_window_mode(port_settings()->window_mode);
                continue;
            }
            if (ev.type == SDL_WINDOWEVENT
                && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                    || ev.window.event == SDL_WINDOWEVENT_RESIZED))
                V.swap_dirty = 1;
            xbox_InputSdlEvent(&ev);
        }

        SDL_LockMutex(s_lock);
        if (s_have_frame) {
            if ((V.fw != s_pw || V.fh != s_ph) && !create_frame(s_pw, s_ph)) {
                SDL_UnlockMutex(s_lock);
                break;
            }
            memcpy(V.stage_map, s_pending, (size_t)s_pw * s_ph * 4);
            s_have_frame = 0;
            upload = 1;
        }
        SDL_UnlockMutex(s_lock);

        if (upload || V.swap_dirty || s_gpu_frames != s_gpu_shown) {
            if (!present(upload))
                break;
        } else if (pmenu_is_open()) {
            /* The menu animates and answers the mouse even when the game
             * shows no new frame (loading, paused): redraw it at ~60 Hz. */
            static Uint32 last_menu;
            if (SDL_GetTicks() - last_menu >= 16) {
                last_menu = SDL_GetTicks();
                if (!present(0))
                    break;
            } else {
                SDL_Delay(2);
            }
        } else {
            SDL_Delay(2);
        }

        if (SDL_GetTicks() - last_title >= 1000) {
            char title[96];
            unsigned f = s_flips;
            snprintf(title, sizeof(title), "Ghost Recon 2 | %u FPS | %ux%u | %s",
                     f - flips_at_title, V.swap_ext.width, V.swap_ext.height,
                     s_gpu_img ? "Vulkan" : "CPU");
            SDL_SetWindowTitle(V.win, title);
            flips_at_title = f;
            last_title = SDL_GetTicks();
        }
    }
    fprintf(stderr, "[VKW] presenter stopped\n");
    return 1;
}

/* Called from main() before the guest starts. Returns 1 if the window is
 * being opened (RECOMP_WINDOW=sdl). */
int gr2_window_start(void)
{
    const char *w = getenv("RECOMP_WINDOW");

    if (!w || strcmp(w, "sdl") != 0)
        return 0;
    SDL_SetMainReady();
    s_lock = SDL_CreateMutex();
    s_qlock = SDL_CreateMutex();
    SDL_CreateThread(window_thread, "gr2-window", NULL);
    return 1;
}

/* ---- shared context (vk_ctx.h) ---------------------------------------- */

const VkwContext *vkw_context(void)
{
    return s_ready ? &s_ctx : NULL;
}

const VkwContext *vkw_wait_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!s_ready && waited < timeout_ms) {
        SDL_Delay(10);
        waited += 10;
    }
    return vkw_context();
}

void vkw_queue_lock(void)   { SDL_LockMutex(s_qlock); }
void vkw_queue_unlock(void) { SDL_UnlockMutex(s_qlock); }

void vkw_set_gpu_frame(VkImage img, uint32_t w, uint32_t h)
{
    SDL_LockMutex(s_lock);
    s_gpu_img = img;
    s_gpu_w = w;
    s_gpu_h = h;
    s_gpu_frames++;
    s_flips++;
    SDL_UnlockMutex(s_lock);
}
