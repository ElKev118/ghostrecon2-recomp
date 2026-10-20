/**
 * Ghost Recon 2 port - Vulkan renderer (phase 2c, docs/VULKAN_PLAN.md).
 *
 * An Nv2aBackend: the pushbuffer executor decodes the title's GPU commands
 * and runs its vertex programs on the CPU, and hands the result here instead
 * of rasterising it itself. Everything below runs on the executor's thread.
 *
 *   surfaces   every colour / depth target the title draws into is a
 *              VkImage, keyed by guest address. Nothing is written back to
 *              guest memory: a texture that names a rendered surface samples
 *              that image (render-to-texture: Ghost Recon 2's scene, glow
 *              chain and colour-grade table), and the flipped surface is
 *              copied to the window's frame image.
 *   textures   anything else is decoded from guest memory by the executor's
 *              own decoder (swizzle, DXT, palettes), uploaded, and cached by
 *              address + format + a hash of the bytes, rechecked once a
 *              frame.
 *   pipelines  one per (blend, depth, stencil, cull, colour mask, target
 *              formats, shader), created on first use.
 *   shaders    slice 1: stage 0 times diffuse, alpha test. The register
 *              combiners come next (xemu's psh.c generator).
 *
 * RECOMP_RENDERER=vk turns it on (needs RECOMP_WINDOW=sdl); anything else
 * leaves the CPU rasteriser in charge.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>   /* GetTickCount */

#include "vk_ctx.h"
#include "vk_glsl.h"
#include "nv2a_backend.h"

#define VKB_FRAMES        2
#define VKB_VBUF_BYTES    (48u << 20)          /* vertex ring per frame */
#define VKB_UBUF_BYTES    (8u << 20)           /* combiner-state ring per frame */
#define VKB_MAX_SURFACES  48
#define VKB_MAX_TEXTURES  1024
#define VKB_MAX_PIPELINES 1024

typedef struct {
    uint32_t va;                /* guest address (low 28 bits compared) */
    int      depth;             /* depth/stencil target */
    uint32_t w, h;
    VkImage  img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageLayout layout;
    uint32_t last_used;
} Surface;

typedef struct {
    uint32_t va, color, w, h, pitch;
    uint64_t hash;
    uint32_t checked_frame;
    VkImage  img;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t last_used;
} Tex;

typedef struct {
    uint64_t key[3];
    VkPipeline pipe;
} Pipe;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer vbuf;
    VkDeviceMemory vmem;
    uint8_t *vmap;
    uint32_t vused;
    VkBuffer ubuf;                  /* per-draw combiner state */
    VkDeviceMemory umem;
    uint8_t *umap;
    uint32_t uused;
    int recording;
    /* Textures uploaded this frame keep their staging buffers alive until
     * the frame's fence. */
    VkBuffer stage[64];
    VkDeviceMemory stage_mem[64];
    int nstage;
} Frame;

static struct {
    const VkwContext *c;
    VkDevice dev;
    VkCommandPool pool;
    Frame f[VKB_FRAMES];
    int fi;
    uint32_t frame_no;

    Surface surf[VKB_MAX_SURFACES];
    Tex tex[VKB_MAX_TEXTURES];
    int ntex;
    Pipe pipes[VKB_MAX_PIPELINES];
    int npipes;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule vs, fs;
    VkSampler samplers[64];
    uint32_t sampler_keys[64];
    int nsamplers;
    VkImage white;
    VkDeviceMemory white_mem;
    VkImageView white_view;

    /* What is being rendered into right now. */
    Surface *rt_color, *rt_depth;
    int rendering;

    /* The window's frame images (copied from the flipped surface). */
    VkImage out[2];
    VkDeviceMemory out_mem[2];
    uint32_t out_w[2], out_h[2];
    int out_i;

    int failed;
} B;

static PFN_vkCmdPushDescriptorSetKHR pfnPushDescriptor;

/* ---------------------------------------------------------------------- */

static int fail(const char *what, VkResult r)
{
    fprintf(stderr, "[VKB] %s failed (VkResult %d); renderer off\n", what, (int)r);
    B.failed = 1;
    return 0;
}

static uint32_t va_key(uint32_t va) { return va & 0x0FFFFFFFu; }

static void img_barrier(VkCommandBuffer cmd, VkImage img, int depth,
                        VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = depth
        ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
        : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      int depth, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkResult r;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((r = vkCreateImage(B.dev, &ici, NULL, img)) != VK_SUCCESS)
        return fail("vkCreateImage", r);
    vkGetImageMemoryRequirements(B.dev, *img, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vkw_find_memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if ((r = vkAllocateMemory(B.dev, &mai, NULL, mem)) != VK_SUCCESS)
        return fail("vkAllocateMemory (image)", r);
    vkBindImageMemory(B.dev, *img, *mem, 0);
    if (view) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = *img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                : VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if ((r = vkCreateImageView(B.dev, &vci, NULL, view)) != VK_SUCCESS)
            return fail("vkCreateImageView", r);
    }
    return 1;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkResult r;

    bci.size = size;
    bci.usage = usage;
    if ((r = vkCreateBuffer(B.dev, &bci, NULL, buf)) != VK_SUCCESS)
        return fail("vkCreateBuffer", r);
    vkGetBufferMemoryRequirements(B.dev, *buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vkw_find_memory(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if ((r = vkAllocateMemory(B.dev, &mai, NULL, mem)) != VK_SUCCESS)
        return fail("vkAllocateMemory (buffer)", r);
    vkBindBufferMemory(B.dev, *buf, *mem, 0);
    return vkMapMemory(B.dev, *mem, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
}

/* ---- frame / command buffer ------------------------------------------- */

static Frame *cur(void) { return &B.f[B.fi]; }

static void begin_frame(void)
{
    Frame *f = cur();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    int i;

    if (f->recording)
        return;
    vkWaitForFences(B.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(B.dev, 1, &f->fence);
    for (i = 0; i < f->nstage; i++) {
        vkDestroyBuffer(B.dev, f->stage[i], NULL);
        vkFreeMemory(B.dev, f->stage_mem[i], NULL);
    }
    f->nstage = 0;
    f->vused = 0;
    f->uused = 0;
    vkResetCommandBuffer(f->cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f->cmd, &bi);
    f->recording = 1;
}

static void end_rendering(void)
{
    if (B.rendering) {
        vkCmdEndRendering(cur()->cmd);
        B.rendering = 0;
    }
}

/* Submit what has been recorded and move to the next frame's resources. */
static void submit_frame(void)
{
    Frame *f = cur();
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

    if (!f->recording)
        return;
    end_rendering();
    vkEndCommandBuffer(f->cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cmd;
    vkw_queue_lock();
    vkQueueSubmit(B.c->queue, 1, &si, f->fence);
    vkw_queue_unlock();
    f->recording = 0;
    B.fi = (B.fi + 1) % VKB_FRAMES;
    B.frame_no++;
}

/* ---- surfaces --------------------------------------------------------- */

static Surface *find_surface(uint32_t va, int depth)
{
    int i;
    for (i = 0; i < VKB_MAX_SURFACES; i++)
        if (B.surf[i].img && B.surf[i].depth == depth && B.surf[i].va == va_key(va))
            return &B.surf[i];
    return NULL;
}

static Surface *get_surface(uint32_t va, int depth, uint32_t w, uint32_t h)
{
    Surface *s = find_surface(va, depth);
    int i, slot = -1;

    if (s && s->w >= w && s->h >= h) {
        s->last_used = B.frame_no;
        return s;
    }
    if (s) {                                   /* grew: make a bigger one */
        if (w < s->w) w = s->w;
        if (h < s->h) h = s->h;
        end_rendering();
        submit_frame();
        vkDeviceWaitIdle(B.dev);
        vkDestroyImageView(B.dev, s->view, NULL);
        vkDestroyImage(B.dev, s->img, NULL);
        vkFreeMemory(B.dev, s->mem, NULL);
        memset(s, 0, sizeof *s);
        slot = (int)(s - B.surf);
        begin_frame();
    } else {
        for (i = 0; i < VKB_MAX_SURFACES && slot < 0; i++)
            if (!B.surf[i].img)
                slot = i;
        if (slot < 0) {
            fprintf(stderr, "[VKB] out of surface slots\n");
            return NULL;
        }
    }
    s = &B.surf[slot];
    if (!make_image(w, h, depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_B8G8R8A8_UNORM,
                    depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                          : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                    depth, &s->img, &s->mem, &s->view))
        return NULL;
    s->va = va_key(va);
    s->depth = depth;
    s->w = w;
    s->h = h;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s->last_used = B.frame_no;
    {
        static unsigned said;
        if (said++ < 40)
            fprintf(stderr, "[VKB] new %s surface %08X %ux%u\n",
                    depth ? "depth" : "colour", va, w, h);
    }
    /* Start from black / far, as a fresh surface reads in guest memory. */
    {
        VkCommandBuffer cmd = cur()->cmd;
        VkImageSubresourceRange rng = { 0, 0, 1, 0, 1 };
        end_rendering();
        img_barrier(cmd, s->img, depth, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (depth) {
            VkClearDepthStencilValue v = { 1.0f, 0 };
            rng.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCmdClearDepthStencilImage(cmd, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
        } else {
            VkClearColorValue v = { { 0, 0, 0, 0 } };
            rng.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vkCmdClearColorImage(cmd, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
        }
        s->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    return s;
}

static void to_layout(Surface *s, VkImageLayout l)
{
    if (s->layout == l)
        return;
    end_rendering();
    img_barrier(cur()->cmd, s->img, s->depth, s->layout, l);
    s->layout = l;
}

/* Make (color, depth) the current target, beginning dynamic rendering. */
static int bind_target(const Nv2aSurface *sf, uint32_t zeta_va)
{
    uint32_t w = sf->pitch / (sf->bytes_per_pixel ? sf->bytes_per_pixel : 4);
    uint32_t h = sf->height;
    Surface *c, *d = NULL;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };

    if (w < sf->width) w = sf->width;
    if (!w || !h)
        return 0;
    c = get_surface(sf->color_va, 0, w, h);
    if (zeta_va)
        d = get_surface(zeta_va, 1, w, h);
    if (!c)
        return 0;
    if (B.rendering && B.rt_color == c && B.rt_depth == d)
        return 1;
    end_rendering();
    to_layout(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (d)
        to_layout(d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    ca.imageView = c->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = c->w;
    ri.renderArea.extent.height = c->h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (d) {
        da.imageView = d->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (d->w < c->w || d->h < c->h) {
            ri.renderArea.extent.width = d->w < c->w ? d->w : c->w;
            ri.renderArea.extent.height = d->h < c->h ? d->h : c->h;
        }
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(cur()->cmd, &ri);
    B.rendering = 1;
    B.rt_color = c;
    B.rt_depth = d;
    return 1;
}

/* ---- textures --------------------------------------------------------- */

static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t px = t->width * t->height;
    switch (t->color) {
    case 0x0C: return px / 2;                         /* DXT1 */
    case 0x0E: case 0x0F: return px;                  /* DXT3, DXT5 */
    case 0x00: case 0x01: case 0x0B: case 0x19: case 0x1B: case 0x13: case 0x1F:
        return t->pitch ? t->pitch * t->height : px;
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3F:
        return t->pitch ? t->pitch * t->height : px * 4;
    default:
        return t->pitch ? t->pitch * t->height : px * 2;
    }
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    /* Every 8th dword plus the length: a texture streamed or rewritten in
     * place changes far more than one word in 32. */
    /* Small textures are hashed whole: a colour ramp the title rewrites
     * every frame (Ghost Recon 2's grade strip, 8 KB) can change in words
     * a sparse sample never reads, and a stale strip turned the graded
     * scene green. Large ones are sampled every 32 bytes. */
    uint32_t step = n <= 65536u ? 4u : 32u;
    for (i = 0; i + 4 <= n; i += step) {
        h ^= *(const uint32_t *)(p + i);
        h *= 1099511628211ull;
    }
    return h ^ n;
}

extern ptrdiff_t xbox_GetMemoryOffset(void);

static void write_bmp(const char *path, const uint8_t *map, uint32_t w, uint32_t h);
static void dump_surfaces(const char *prefix, unsigned shot);

static int upload_texture(Tex *t, const Nv2aTexture *src)
{
    uint32_t *argb;
    VkBuffer sb;
    VkDeviceMemory sm;
    void *map;
    Frame *f = cur();
    VkBufferImageCopy c;
    size_t bytes = (size_t)src->width * src->height * 4;

    if (f->nstage >= 64) {                     /* staging full: flush */
        submit_frame();
        begin_frame();
        f = cur();
    }
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &map))
        return 0;
    argb = (uint32_t *)map;
    if (!nv2a_backend_decode_texture(src, argb))
        memset(argb, 0xFF, bytes);             /* unsupported: white, as the CPU path */
    {
        /* RECOMP_VK_DUMP_TEX=<va>: write that texture as uploaded. */
        static uint32_t want = 1;
        static unsigned n;
        if (want == 1) {
            const char *e = getenv("RECOMP_VK_DUMP_TEX");
            want = e ? (uint32_t)strtoul(e, NULL, 16) & 0x0FFFFFFFu : 0;
        }
        if (want && (src->offset & 0x0FFFFFFFu) == want && n < 40) {
            char path[64];
            snprintf(path, sizeof path, "tex_%08X_%u.bmp", src->offset, n++);
            write_bmp(path, (const uint8_t *)argb, src->width, src->height);
            fprintf(stderr, "[VKB] texture %08X %ux%u fmt %02X pitch %u -> %s (first %08X %08X)\n",
                    src->offset, src->width, src->height, src->color, src->pitch, path,
                    argb[0], argb[src->width * src->height / 2]);
        }
    }
    f->stage[f->nstage] = sb;
    f->stage_mem[f->nstage++] = sm;

    if (!t->img && !make_image(src->width, src->height, VK_FORMAT_B8G8R8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               0, &t->img, &t->mem, &t->view))
        return 0;
    end_rendering();
    img_barrier(f->cmd, t->img, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = src->width;
    c.imageExtent.height = src->height;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(f->cmd, sb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_barrier(f->cmd, t->img, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    /* Submitted at once. Batched into the frame's command buffer, some
     * uploads never reached the GPU (Ghost Recon 2's colour-grade strip
     * came back empty and the graded scene turned to flat grey or red);
     * uploads happen while loading, so the extra submits cost nothing in
     * play. ponytail: find out why rather than living with it. */
    submit_frame();
    begin_frame();
    return 1;
}

/* The view a stage samples, or NULL for "use white". */
/* Size of the image behind the last view texture_view returned: a surface
 * can be larger than the texture the title samples from it (Ghost Recon 2
 * draws its 256x256 grade table into an address it also uses as a 640x480
 * buffer), so coordinates must be scaled to the part that holds the data. */
static uint32_t s_view_w, s_view_h;

static VkImageView texture_view(const Nv2aTexStage *st)
{
    const Nv2aTexture *src = &st->t;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    Surface *s;
    Tex *t = NULL;
    uint64_t h;
    int i;

    if (!st->enabled || !src->width || !src->height
        || src->width > 4096 || src->height > 4096)
        return NULL;
    /* Rendered by us: sample the surface itself. */
    s = find_surface(src->offset, 0);
    if (s) {
        if (s == B.rt_color) {
            /* Sampling the target being drawn: the title reads the previous
             * contents (a full-screen pass over its own scene). Vulkan
             * cannot sample an attachment it writes, so sample a copy. */
            static Surface copy;
            VkImageCopy rgn;
            if (!copy.img || copy.w != s->w || copy.h != s->h) {
                if (copy.img) {
                    vkDeviceWaitIdle(B.dev);
                    vkDestroyImageView(B.dev, copy.view, NULL);
                    vkDestroyImage(B.dev, copy.img, NULL);
                    vkFreeMemory(B.dev, copy.mem, NULL);
                }
                memset(&copy, 0, sizeof copy);
                if (!make_image(s->w, s->h, VK_FORMAT_B8G8R8A8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                0, &copy.img, &copy.mem, &copy.view))
                    return NULL;
                copy.w = s->w; copy.h = s->h;
                copy.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            to_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            img_barrier(cur()->cmd, copy.img, 0, copy.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&rgn, 0, sizeof rgn);
            rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rgn.srcSubresource.layerCount = 1;
            rgn.dstSubresource = rgn.srcSubresource;
            rgn.extent.width = s->w;
            rgn.extent.height = s->h;
            rgn.extent.depth = 1;
            vkCmdCopyImage(cur()->cmd, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           copy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            img_barrier(cur()->cmd, copy.img, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            copy.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            B.rt_color = NULL;                 /* rendering must restart */
            s_view_w = copy.w; s_view_h = copy.h;
            return copy.view;
        }
        to_layout(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_view_w = s->w; s_view_h = s->h;
        return s->view;
    }

    for (i = 0; i < B.ntex; i++)
        if (B.tex[i].va == src->offset && B.tex[i].color == src->color
            && B.tex[i].w == src->width && B.tex[i].h == src->height
            && (st->normalised || B.tex[i].pitch == src->pitch)) {
            t = &B.tex[i];
            break;
        }
    if (t && t->checked_frame == B.frame_no) {
        t->last_used = B.frame_no;
        return t->view;
    }
    h = hash_bytes(mem + src->offset, tex_bytes(src));
    if (t && t->hash == h) {
        t->checked_frame = B.frame_no;
        t->last_used = B.frame_no;
        return t->view;
    }
    if (!t) {
        if (B.ntex >= VKB_MAX_TEXTURES) {
            /* Evict the least recently used. */
            int lru = 0;
            for (i = 1; i < B.ntex; i++)
                if (B.tex[i].last_used < B.tex[lru].last_used)
                    lru = i;
            t = &B.tex[lru];
            submit_frame();
            vkDeviceWaitIdle(B.dev);
            begin_frame();
            vkDestroyImageView(B.dev, t->view, NULL);
            vkDestroyImage(B.dev, t->img, NULL);
            vkFreeMemory(B.dev, t->mem, NULL);
            memset(t, 0, sizeof *t);
        } else {
            t = &B.tex[B.ntex++];
            memset(t, 0, sizeof *t);
        }
        t->va = src->offset;
        t->color = src->color;
        t->w = src->width;
        t->h = src->height;
        t->pitch = src->pitch;
    }
    if (!upload_texture(t, src))
        return NULL;
    t->hash = h;
    t->checked_frame = B.frame_no;
    t->last_used = B.frame_no;
    return t->view;
}

/* NV097 filter / address words to a sampler, cached. */
static VkSampler get_sampler(const Nv2aTexStage *st)
{
    uint32_t mag = (st->t.filter >> 24) & 0xF, min = (st->t.filter >> 16) & 0xFF;
    uint32_t key = (mag << 24) | (min << 16) | (st->t.addr_u << 8) | st->t.addr_v;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSamplerAddressMode am[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,                 /* 0: treat as wrap */
        VK_SAMPLER_ADDRESS_MODE_REPEAT,                 /* 1 WRAP */
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,        /* 2 MIRROR */
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,          /* 3 CLAMP_TO_EDGE */
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,        /* 4 BORDER */
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,          /* 5 CLAMP_OGL */
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
    int i;

    for (i = 0; i < B.nsamplers; i++)
        if (B.sampler_keys[i] == key)
            return B.samplers[i];
    if (B.nsamplers >= 64)
        return B.samplers[0];
    sci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = min == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = am[st->t.addr_u & 7];
    sci.addressModeV = am[st->t.addr_v & 7];
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = 0.0f;
    vkCreateSampler(B.dev, &sci, NULL, &B.samplers[B.nsamplers]);
    B.sampler_keys[B.nsamplers] = key;
    return B.samplers[B.nsamplers++];
}

/* ---- shaders and pipelines -------------------------------------------- */

typedef struct {
    float surf[4];          /* W, H, 1/zmax, 0 */
    float texscale[4][4];   /* 1/w, 1/h (texel coordinates) or 1,1; z: enabled */
    int32_t alpha[4];       /* enable, func, ref, 0 */
} PushConsts;

/* The pixel stage, std140, one copy per draw (Nv2aPixelState). */
typedef struct {
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8], f0[8], f1[8];
    uint32_t fin[4];        /* final0, final1, final_c0, final_c1 */
    uint32_t ctl[4];        /* control, stage_program, other_stage_input, rc_seen */
    uint32_t misc[4];       /* clip_plane_mode, fog_enable, fog_color, fog_mode */
    float    fogp[4];       /* fog bias, fog scale */
} RcUbo;

static const char *VS_SRC =
    "#version 450\n"
    "layout(location=0) in vec4 inPos;\n"
    "layout(location=1) in vec4 inD0;\n"
    "layout(location=2) in vec4 inD1;\n"
    "layout(location=3) in vec4 inFog;\n"
    "layout(location=4) in vec4 inT0;\n"
    "layout(location=5) in vec4 inT1;\n"
    "layout(location=6) in vec4 inT2;\n"
    "layout(location=7) in vec4 inT3;\n"
    "layout(push_constant) uniform PC { vec4 surf; vec4 texscale[4]; ivec4 alpha; } pc;\n"
    "layout(location=0) out vec4 vD0;\n"
    "layout(location=1) out vec4 vD1;\n"
    "layout(location=2) out vec4 vFog;\n"
    "layout(location=3) out vec4 vT[4];\n"
    "void main() {\n"
    /* pos is surface pixels and depth units, divided by w; rebuild clip
     * space so the GPU clips against the near plane itself. */
    "  float w = inPos.w;\n"
    "  vec3 ndc = vec3(inPos.x / pc.surf.x * 2.0 - 1.0,\n"
    "                  inPos.y / pc.surf.y * 2.0 - 1.0,\n"
    "                  inPos.z * pc.surf.z);\n"
    "  gl_Position = vec4(ndc * w, w);\n"
    "  vD0 = inD0; vD1 = inD1; vFog = inFog;\n"
    "  vT[0] = inT0; vT[1] = inT1; vT[2] = inT2; vT[3] = inT3;\n"
    "}\n";

static const char *FS_SRC =
    "#version 450\n"
    "layout(location=0) in vec4 vD0;\n"
    "layout(location=1) in vec4 vD1;\n"
    "layout(location=2) in vec4 vFog;\n"
    "layout(location=3) in vec4 vT[4];\n"
    "layout(set=0, binding=0) uniform sampler2D tex[4];\n"
    "layout(std140, set=0, binding=1) uniform RC {\n"
    "  uvec4 cicw[2]; uvec4 aicw[2]; uvec4 cocw[2]; uvec4 aocw[2]; uvec4 f0[2]; uvec4 f1[2];\n"
    "  uvec4 fin; uvec4 ctl; uvec4 misc; vec4 fogp; } rc;\n"
    "layout(push_constant) uniform PC { vec4 surf; vec4 texscale[4]; ivec4 alpha; } pc;\n"
    "layout(location=0) out vec4 outColor;\n"
    /* A port of the executor's own CPU combiner (nv2a_combiner.c) and
     * texture-stage fetch (rc_stage_fetch), so both renderers agree. */
    "uint w8(uvec4 a[2], uint i) { return a[i >> 2][i & 3u]; }\n"
    "vec4 unpk(uint c) { return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u),\n"
    "                                float(c & 255u), float(c >> 24)) / 255.0; }\n"
    "float map_in(float x, uint b) {\n"
    "  float p = max(x, 0.0);\n"
    "  switch ((b >> 5) & 7u) {\n"
    "  case 0u: return p; case 1u: return 1.0 - clamp(x, 0.0, 1.0);\n"
    "  case 2u: return 2.0 * p - 1.0; case 3u: return -2.0 * p + 1.0;\n"
    "  case 4u: return p - 0.5; case 5u: return -p + 0.5;\n"
    "  case 6u: return x; default: return -x; }\n"
    "}\n"
    "float map_out(float x, uint m) {\n"
    "  switch (m) { case 0x08u: return x - 0.5; case 0x10u: return x * 2.0;\n"
    "  case 0x18u: return (x - 0.5) * 2.0; case 0x20u: return x * 4.0;\n"
    "  case 0x30u: return x * 0.5; default: return x; }\n"
    "}\n"
    "vec4 R[16];\n"
    "vec3 rd_rgb(uint b) {\n"
    "  vec4 r = R[b & 15u]; vec3 v = (b & 16u) != 0u ? vec3(r.a) : r.rgb;\n"
    "  return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
    "}\n"
    "float rd_a(uint b) { vec4 r = R[b & 15u]; return map_in((b & 16u) != 0u ? r.a : r.b, b); }\n"
    "vec4 samp(int st, vec2 uv) {\n"
    "  if (st == 0) return texture(tex[0], uv);\n"
    "  if (st == 1) return texture(tex[1], uv);\n"
    "  if (st == 2) return texture(tex[2], uv);\n"
    "  return texture(tex[3], uv);\n"
    "}\n"
    "float fog_factor(float d) {\n"
    "  float f, px = rc.fogp.x, py = rc.fogp.y; uint m = rc.misc.w;\n"
    "  if (rc.misc.y == 0u) return 1.0;\n"
    "  if (m == 0x800u || m == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (m == 0x801u || m == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (m == 0x802u || m == 0x803u || m == 0x804u) f = abs(f);\n"
    "  if (isnan(f)) f = 1.0;\n"
    "  return clamp(f, 0.0, 1.0);\n"
    "}\n"
    "bool alpha_pass(float a) {\n"
    "  int v = int(a * 255.0 + 0.5), r = pc.alpha.z;\n"
    "  switch (pc.alpha.y) {\n"
    "  case 0x200: return false; case 0x201: return v < r; case 0x202: return v == r;\n"
    "  case 0x203: return v <= r; case 0x204: return v > r; case 0x205: return v != r;\n"
    "  case 0x206: return v >= r; default: return true; }\n"
    "}\n"
    "void main() {\n"
    "  vec4 c;\n"
    "  if (rc.ctl.w == 0u) {\n"
    /* No combiner state yet: texture 0 times diffuse, as the CPU path. */
    "    c = vD0;\n"
    "    if (pc.texscale[0].z != 0.0) {\n"
    "      vec4 t = vT[0]; vec2 uv = (t.w != 0.0 ? t.xy / t.w : t.xy) * pc.texscale[0].xy;\n"
    "      c *= samp(0, uv);\n"
    "    }\n"
    "  } else {\n"
    "    vec4 t[4];\n"
    "    uint sp = rc.ctl.y;\n"
    "    for (int st = 0; st < 4; st++) {\n"
    "      uint mode = (sp >> uint(st * 5)) & 31u;\n"
    "      vec4 tc = vT[st]; float q = tc.w != 0.0 ? tc.w : 1.0;\n"
    "      if (mode == 0u) t[st] = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "      else if (mode == 1u || mode == 2u || mode == 3u)\n"
    "        t[st] = samp(st, tc.xy / q * pc.texscale[st].xy);\n"
    "      else if (mode == 4u) t[st] = clamp(tc, 0.0, 1.0);\n"
    "      else if (mode == 5u) {\n"
    "        for (int j = 0; j < 4; j++) {\n"
    "          bool ge = ((rc.misc.x >> uint(st * 4 + j)) & 1u) != 0u;\n"
    "          if (ge ? tc[j] >= 0.0 : tc[j] < 0.0) discard;\n"
    "        }\n"
    "        t[st] = vec4(0.0);\n"
    "      } else if (mode == 15u || mode == 16u) {\n"
    "        int src = st <= 1 ? 0 : int((rc.ctl.z >> (st == 2 ? 16u : 20u)) & 15u) % st;\n"
    "        vec4 i = t[src];\n"
    "        vec2 dc = mode == 15u ? vec2(pc.alpha.w == 4 ? 0.5 : i.a, i.r) : vec2(i.g, i.b);\n"
    "        t[st] = samp(st, dc * pc.texscale[st].xy);\n"
    "      } else t[st] = vec4(0.0);\n"
    "    }\n"
    "    for (int k = 0; k < 16; k++) R[k] = vec4(0.0);\n"
    "    R[3] = vec4(unpk(rc.misc.z).rgb, fog_factor(vFog.x));\n"
    "    R[4] = vD0; R[5] = vD1;\n"
    "    R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
    "    R[12].a = (sp & 31u) != 0u ? t[0].a : 1.0;\n"
    "    uint n = min(rc.ctl.x & 255u, 8u), flags = rc.ctl.x >> 8;\n"
    "    for (uint s = 0u; s < n; s++) {\n"
    "      uint icw = w8(rc.cicw, s), ocw = w8(rc.cocw, s);\n"
    "      uint aicw = w8(rc.aicw, s), aocw = w8(rc.aocw, s);\n"
    "      uint fl = ocw >> 12, afl = aocw >> 12;\n"
    "      R[1] = unpk(w8(rc.f0, (flags & 0x010u) != 0u ? s : 0u));\n"
    "      R[2] = unpk(w8(rc.f1, (flags & 0x100u) != 0u ? s : 0u));\n"
    "      bool mux = (flags & 1u) != 0u ? R[12].a >= 0.5 : ((int(R[12].a * 255.0) & 1) != 0);\n"
    "      vec3 A = rd_rgb(icw >> 24), Bv = rd_rgb(icw >> 16), C = rd_rgb(icw >> 8), D = rd_rgb(icw);\n"
    "      float aA = rd_a(aicw >> 24), aB = rd_a(aicw >> 16), aC = rd_a(aicw >> 8), aD = rd_a(aicw);\n"
    "      float dab = dot(A, Bv), dcd = dot(C, D);\n"
    "      vec3 pab = (fl & 2u) != 0u ? vec3(dab) : A * Bv;\n"
    "      vec3 pcd = (fl & 1u) != 0u ? vec3(dcd) : C * D;\n"
    "      vec3 sum = (fl & 4u) != 0u ? (mux ? pcd : pab) : pab + pcd;\n"
    "      uint mo = fl & 0x38u;\n"
    "      vec3 ab = clamp(vec3(map_out(pab.x, mo), map_out(pab.y, mo), map_out(pab.z, mo)), -1.0, 1.0);\n"
    "      vec3 cd = clamp(vec3(map_out(pcd.x, mo), map_out(pcd.y, mo), map_out(pcd.z, mo)), -1.0, 1.0);\n"
    "      vec3 ms = clamp(vec3(map_out(sum.x, mo), map_out(sum.y, mo), map_out(sum.z, mo)), -1.0, 1.0);\n"
    "      float apab = aA * aB, apcd = aC * aD;\n"
    "      float asum = (afl & 4u) != 0u ? (mux ? apcd : apab) : apab + apcd;\n"
    "      uint amo = afl & 0x38u;\n"
    "      float aab = clamp(map_out(apab, amo), -1.0, 1.0);\n"
    "      float acd = clamp(map_out(apcd, amo), -1.0, 1.0);\n"
    "      float ams = clamp(map_out(asum, amo), -1.0, 1.0);\n"
    "      uint d;\n"
    "      d = (ocw >> 4) & 15u; if (d != 0u) { R[d].rgb = ab; if ((fl & 0x80u) != 0u) R[d].a = ab.z; }\n"
    "      d = ocw & 15u;        if (d != 0u) { R[d].rgb = cd; if ((fl & 0x40u) != 0u) R[d].a = cd.z; }\n"
    "      d = (ocw >> 8) & 15u; if (d != 0u) R[d].rgb = ms;\n"
    "      d = (aocw >> 4) & 15u; if (d != 0u) R[d].a = aab;\n"
    "      d = aocw & 15u;        if (d != 0u) R[d].a = acd;\n"
    "      d = (aocw >> 8) & 15u; if (d != 0u) R[d].a = ams;\n"
    "      R[0] = vec4(0.0);\n"
    "    }\n"
    "    if (rc.fin.x != 0u || rc.fin.y != 0u) {\n"
    "      uint f0 = rc.fin.x, f1 = rc.fin.y, ff = f1 & 255u;\n"
    "      R[1] = unpk(rc.fin.z); R[2] = unpk(rc.fin.w);\n"
    "      vec3 a = (ff & 0x40u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;\n"
    "      vec3 b = (ff & 0x20u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;\n"
    "      R[14] = vec4((ff & 0x80u) != 0u ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);\n"
    "      vec3 E = rd_rgb(f1 >> 24), F = rd_rgb(f1 >> 16);\n"
    "      R[15] = vec4(E * F, 0.0);\n"
    "      vec3 A = rd_rgb(f0 >> 24), Bv = rd_rgb(f0 >> 16), C = rd_rgb(f0 >> 8), D = rd_rgb(f0);\n"
    "      c = vec4(D + C * (1.0 - A) + Bv * A, rd_a(f1 >> 8));\n"
    "    } else c = R[12];\n"
    "  }\n"
    "  c = clamp(c, 0.0, 1.0);\n"
    "  if (pc.alpha.w == 1) c = vec4(1.0, 0.0, 1.0, 1.0);\n"
    "  else if (pc.alpha.w == 2) c = vec4(vD0.rgb, 1.0);\n"
    "  else if (pc.alpha.w == 3) c = vec4(samp(0, vT[0].xy * pc.texscale[0].xy).rgb, 1.0);\n"
    "  if (pc.alpha.x != 0 && !alpha_pass(c.a)) discard;\n"
    "  outColor = c;\n"
    "}\n";

static VkShaderModule make_module(const char *src, int frag)
{
    size_t bytes = 0;
    uint32_t *spv = vkg_compile(src, frag, &bytes);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;

    if (!spv)
        return VK_NULL_HANDLE;
    ci.codeSize = bytes;
    ci.pCode = spv;
    vkCreateShaderModule(B.dev, &ci, NULL, &m);
    free(spv);
    return m;
}

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_op(uint32_t f)
{
    return (VkCompareOp)((f >= 0x200 && f <= 0x207) ? f - 0x200 : 7);
}

static VkStencilOp stencil_op(uint32_t o)
{
    switch (o) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

static VkPipeline get_pipeline(const Nv2aRenderState *rs, int has_depth)
{
    uint64_t k0, k1, k2;
    int i;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vb = { 0, sizeof(Nv2aVertexXf), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[8];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rz = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipeline p;
    VkResult r;

    k0 = (uint64_t)(rs->blend_enable & 1) | ((uint64_t)(rs->blend_src & 0xFFFF) << 1)
       | ((uint64_t)(rs->blend_dst & 0xFFFF) << 17) | ((uint64_t)(rs->blend_eq & 0xFFFF) << 33)
       | ((uint64_t)(rs->depth_test_enable & 1) << 49) | ((uint64_t)(rs->depth_func & 0xF) << 50)
       | ((uint64_t)(rs->depth_write & 1) << 54) | ((uint64_t)(has_depth & 1) << 55)
       | ((uint64_t)(rs->cull_enable & 1) << 56) | ((uint64_t)(rs->stencil_enable & 1) << 57);
    k1 = (uint64_t)rs->color_mask | ((uint64_t)(rs->cull_face & 0xFFFF) << 32)
       | ((uint64_t)(rs->front_face & 0xFFFF) << 48);
    k2 = (uint64_t)(rs->stencil_func & 0xF) | ((uint64_t)(rs->stencil_func_mask & 0xFF) << 4)
       | ((uint64_t)(rs->stencil_write_mask & 0xFF) << 12)
       | ((uint64_t)(rs->stencil_fail & 0xFFFF) << 20) | ((uint64_t)(rs->stencil_zfail & 0xFFFF) << 36)
       | ((uint64_t)(rs->stencil_zpass & 0xFFF) << 52);
    for (i = 0; i < B.npipes; i++)
        if (B.pipes[i].key[0] == k0 && B.pipes[i].key[1] == k1 && B.pipes[i].key[2] == k2)
            return B.pipes[i].pipe;
    if (B.npipes >= VKB_MAX_PIPELINES)
        return B.pipes[0].pipe;

    memset(stages, 0, sizeof stages);
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = B.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = B.fs;
    stages[1].pName = "main";
    for (i = 0; i < 8; i++) {
        va[i].location = (uint32_t)i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = (uint32_t)(i * 16);
    }
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 8;
    vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rz.polygonMode = VK_POLYGON_MODE_FILL;
    rz.lineWidth = 1.0f;
    rz.depthClampEnable = VK_TRUE;
    rz.cullMode = VK_CULL_MODE_NONE;
    {
        static int nocull = -1;               /* RECOMP_VK_NOCULL=1: debug */
        if (nocull < 0) nocull = getenv("RECOMP_VK_NOCULL") != NULL;
        if (nocull) goto cull_done;
    }
    if (rs->cull_enable)
        rz.cullMode = rs->cull_face == 0x0404 ? VK_CULL_MODE_FRONT_BIT
                    : rs->cull_face == 0x0408 ? VK_CULL_MODE_FRONT_AND_BACK
                    : VK_CULL_MODE_BACK_BIT;
    /* SET_FRONT_FACE in the surface's own (y-down) coordinates, as xemu
     * maps it: CW 0x0900 is Vulkan clockwise, CCW 0x0901 counter-clockwise.
     * Inverted, every quad the menus and the sky draw was culled. */
cull_done:
    rz.frontFace = rs->front_face == 0x0901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                            : VK_FRONT_FACE_CLOCKWISE;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    if (has_depth) {
        ds.depthTestEnable = rs->depth_test_enable ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = rs->depth_write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = compare_op(rs->depth_func);
        ds.stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE;
        ds.front.failOp = stencil_op(rs->stencil_fail);
        ds.front.depthFailOp = stencil_op(rs->stencil_zfail);
        ds.front.passOp = stencil_op(rs->stencil_zpass);
        ds.front.compareOp = compare_op(rs->stencil_func);
        ds.front.compareMask = rs->stencil_func_mask & 0xFF;
        ds.front.writeMask = rs->stencil_write_mask & 0xFF;
        ds.back = ds.front;
    }
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = rs->blend_enable ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = blend_factor(rs->blend_src);
    cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = blend_factor(rs->blend_dst);
    cba.colorBlendOp = cba.alphaBlendOp = blend_op(rs->blend_eq);
    cba.colorWriteMask = ((rs->color_mask & 0x00010000u) ? VK_COLOR_COMPONENT_R_BIT : 0)
                       | ((rs->color_mask & 0x00000100u) ? VK_COLOR_COMPONENT_G_BIT : 0)
                       | ((rs->color_mask & 0x00000001u) ? VK_COLOR_COMPONENT_B_BIT : 0)
                       | ((rs->color_mask & 0x01000000u) ? VK_COLOR_COMPONENT_A_BIT : 0);
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (has_depth) {
        rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
        rci.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rz;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = B.layout;
    if ((r = vkCreateGraphicsPipelines(B.dev, VK_NULL_HANDLE, 1, &gp, NULL, &p)) != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    B.pipes[B.npipes].key[0] = k0;
    B.pipes[B.npipes].key[1] = k1;
    B.pipes[B.npipes].key[2] = k2;
    B.pipes[B.npipes++].pipe = p;
    return p;
}

/* ---- init ------------------------------------------------------------- */

static int init(void)
{
    static int tried;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkDescriptorSetLayoutBinding bind[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(PushConsts) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    int i;

    if (B.dev)
        return !B.failed;
    if (tried)
        return 0;
    tried = 1;
    B.c = vkw_wait_ready(10000);
    if (!B.c) {
        fprintf(stderr, "[VKB] no Vulkan window; renderer off\n");
        B.failed = 1;
        return 0;
    }
    B.dev = B.c->dev;
    pfnPushDescriptor = (PFN_vkCmdPushDescriptorSetKHR)
        vkGetDeviceProcAddr(B.dev, "vkCmdPushDescriptorSetKHR");

    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = B.c->qfam;
    vkCreateCommandPool(B.dev, &pci, NULL, &B.pool);
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (i = 0; i < VKB_FRAMES; i++) {
        cai.commandPool = B.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(B.dev, &cai, &B.f[i].cmd);
        vkCreateFence(B.dev, &fci, NULL, &B.f[i].fence);
        if (!make_buffer(VKB_VBUF_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         &B.f[i].vbuf, &B.f[i].vmem, (void **)&B.f[i].vmap))
            return 0;
        if (!make_buffer(VKB_UBUF_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         &B.f[i].ubuf, &B.f[i].umem, (void **)&B.f[i].umap))
            return 0;
    }

    dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dli.bindingCount = 2;
    dli.pBindings = bind;
    vkCreateDescriptorSetLayout(B.dev, &dli, NULL, &B.dsl);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &B.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(B.dev, &pli, NULL, &B.layout);

    B.vs = make_module(VS_SRC, 0);
    B.fs = make_module(FS_SRC, 1);
    if (!B.vs || !B.fs) {
        B.failed = 1;
        return 0;
    }

    /* A 1x1 white texture for stages with nothing bound. */
    if (!make_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    0, &B.white, &B.white_mem, &B.white_view))
        return 0;
    begin_frame();
    {
        VkClearColorValue v = { { 1, 1, 1, 1 } };
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        img_barrier(cur()->cmd, B.white, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cur()->cmd, B.white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
        img_barrier(cur()->cmd, B.white, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    fprintf(stderr, "[VKB] renderer up on %s\n", B.c->props.deviceName);
    return 1;
}

/* ---- Nv2aBackend ------------------------------------------------------ */

static void draw_vertices(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                          const Nv2aVertexXf *v, uint32_t count,
                          const Nv2aTexStage *stages, float zmax,
                          const Nv2aPixelState *px)
{
    RcUbo ubo;
    VkDescriptorBufferInfo ubi;
    VkWriteDescriptorSet w2[2];
    Frame *f;
    VkCommandBuffer cmd;
    VkPipeline pipe;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    PushConsts pc;
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off;
    uint32_t bytes = count * (uint32_t)sizeof(Nv2aVertexXf);
    int st;

    if (!count || !init())
        return;
    begin_frame();
    if (bytes > VKB_VBUF_BYTES)
        return;
    if (cur()->vused + bytes > VKB_VBUF_BYTES
        || cur()->uused + 256u > VKB_UBUF_BYTES) {
        submit_frame();
        begin_frame();
    }
    memset(&pc, 0, sizeof pc);
    /* Textures first: sampling a surface may end the current rendering. */
    for (st = 0; st < 4; st++) {
        VkImageView view;
        s_view_w = s_view_h = 0;
        view = stages ? texture_view(&stages[st]) : NULL;
        ii[st].sampler = stages ? get_sampler(&stages[st]) : B.samplers[0];
        ii[st].imageView = view ? view : B.white_view;
        ii[st].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (view) {
            pc.texscale[st][2] = 1.0f;
            {
                /* Image size: the texture's own, or the surface's when the
                 * texture is a rendered surface (possibly larger). */
                float iw = s_view_w ? (float)s_view_w : (float)stages[st].t.width;
                float ih = s_view_h ? (float)s_view_h : (float)stages[st].t.height;
                if (!stages[st].normalised) {          /* texel coordinates */
                    pc.texscale[st][0] = 1.0f / iw;
                    pc.texscale[st][1] = 1.0f / ih;
                } else {                               /* [0,1] of the texture */
                    pc.texscale[st][0] = (float)stages[st].t.width / iw;
                    pc.texscale[st][1] = (float)stages[st].t.height / ih;
                }
            }
        }
    }
    if (!B.nsamplers) {
        Nv2aTexStage dummy;
        memset(&dummy, 0, sizeof dummy);
        get_sampler(&dummy);
    }
    for (st = 0; st < 4; st++)
        if (!ii[st].sampler)
            ii[st].sampler = B.samplers[0];
    if (!bind_target(sf, rs->zeta_va))
        return;
    {
        /* RECOMP_VK_WATCH=<va>: log the draws that target one surface. */
        static uint32_t watch = 1;
        static unsigned said;
        if (watch == 1) {
            const char *e = getenv("RECOMP_VK_WATCH");
            watch = e ? (uint32_t)strtoul(e, NULL, 16) & 0x0FFFFFFFu : 0;
        }
        if (watch && (sf->color_va & 0x0FFFFFFFu) == watch && said++ < 12)
            fprintf(stderr, "[VKB] draw -> %08X: %u verts, rt %ux%u, sf %ux%u pitch %u,"
                    " v0 %.2f %.2f %.3g w %.3g, cull %u/%X front %X, cmask %08X, blend %u,"
                    " tex0 %08X en %u\n", sf->color_va, count, B.rt_color->w,
                    B.rt_color->h, sf->width, sf->height, sf->pitch, v[0].pos[0], v[0].pos[1],
                    v[0].pos[2], v[0].pos[3], rs->cull_enable, rs->cull_face,
                    rs->front_face, rs->color_mask, rs->blend_enable,
                    stages ? stages[0].t.offset : 0, stages ? stages[0].enabled : 0);
        if (watch && (sf->color_va & 0x0FFFFFFFu) == watch && said <= 3) {
            uint32_t k;
            for (k = 0; k < count && k < 6; k++)
                fprintf(stderr, "[VKB]   v%u pos %.2f %.2f  t0 %.3f %.3f %.3f %.3f  t1 %.3f %.3f %.3f %.3f"
                        "  t2 %.3f %.3f  t3 %.3f %.3f\n", k, v[k].pos[0], v[k].pos[1],
                        v[k].tex[0][0], v[k].tex[0][1], v[k].tex[0][2], v[k].tex[0][3],
                        v[k].tex[1][0], v[k].tex[1][1], v[k].tex[1][2], v[k].tex[1][3],
                        v[k].tex[2][0], v[k].tex[2][1], v[k].tex[3][0], v[k].tex[3][1]);
        }
    }
    f = cur();
    cmd = f->cmd;
    pipe = get_pipeline(rs, B.rt_depth != NULL);
    if (!pipe)
        return;

    memcpy(f->vmap + f->vused, v, bytes);
    off = f->vused;
    f->vused += (bytes + 255u) & ~255u;

    memset(&ubo, 0, sizeof ubo);
    if (px) {
        memcpy(ubo.cicw, px->color_icw, 32);
        memcpy(ubo.aicw, px->alpha_icw, 32);
        memcpy(ubo.cocw, px->color_ocw, 32);
        memcpy(ubo.aocw, px->alpha_ocw, 32);
        memcpy(ubo.f0, px->factor0, 32);
        memcpy(ubo.f1, px->factor1, 32);
        ubo.fin[0] = px->final0; ubo.fin[1] = px->final1;
        ubo.fin[2] = px->final_c0; ubo.fin[3] = px->final_c1;
        ubo.ctl[0] = px->control; ubo.ctl[1] = px->stage_program;
        ubo.ctl[2] = px->other_stage_input; ubo.ctl[3] = px->rc_seen;
        ubo.misc[0] = px->clip_plane_mode; ubo.misc[1] = px->fog_enable;
        ubo.misc[2] = px->fog_color; ubo.misc[3] = px->fog_mode;
        ubo.fogp[0] = px->fog_param[0]; ubo.fogp[1] = px->fog_param[1];
    }
    memcpy(f->umap + f->uused, &ubo, sizeof ubo);
    ubi.buffer = f->ubuf;
    ubi.offset = f->uused;
    ubi.range = sizeof ubo;
    f->uused += 256u;

    pc.surf[0] = (float)sf->pitch / (float)(sf->bytes_per_pixel ? sf->bytes_per_pixel : 4);
    if (pc.surf[0] < (float)sf->width) pc.surf[0] = (float)sf->width;
    pc.surf[0] = (float)B.rt_color->w;
    pc.surf[1] = (float)B.rt_color->h;
    pc.surf[2] = zmax > 0.0f ? 1.0f / zmax : 1.0f;
    pc.alpha[0] = (int32_t)rs->alpha_test_enable;
    pc.alpha[1] = (int32_t)rs->alpha_func;
    pc.alpha[2] = (int32_t)(rs->alpha_ref & 0xFF);
    {
        /* RECOMP_VK_FS=magenta|d0|t0: what each draw outputs, for telling
         * "not drawn" from "drawn black". */
        static int dbg = -1;
        if (dbg < 0) {
            const char *e = getenv("RECOMP_VK_FS");
            dbg = !e ? 0 : !strcmp(e, "magenta") ? 1 : !strcmp(e, "d0") ? 2
                : !strcmp(e, "t0") ? 3 : !strcmp(e, "noalpha") ? 4 : 0;
        }
        pc.alpha[3] = dbg;
        /* WORKAROUND (not a fix): the grade pass's AR lookup reads the
         * scene's alpha as u; some draws (characters, trunks, walls) leave
         * an alpha the hardware would not, and those turned cyan. Until
         * that draw is found, the lookup uses u = 0.5 (red does not depend
         * on u in GR2's table). RECOMP_VK_GRADE_ALPHA=1 restores the real
         * lookup for debugging. See docs/VULKAN_PLAN.md. */
        {
            static int real = -1;
            if (real < 0) real = getenv("RECOMP_VK_GRADE_ALPHA") != NULL;
            if (!dbg && !real) pc.alpha[3] = 4;
        }
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    w.dstBinding = 0;
    w.descriptorCount = 4;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    w2[0] = w;
    memset(&w2[1], 0, sizeof w2[1]);
    w2[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w2[1].dstBinding = 1;
    w2[1].descriptorCount = 1;
    w2[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w2[1].pBufferInfo = &ubi;
    pfnPushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.layout, 0, 2, w2);
    vkCmdPushConstants(cmd, B.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof pc, &pc);
    vp.x = 0;
    vp.y = 0;
    vp.width = (float)B.rt_color->w;
    vp.height = (float)B.rt_color->h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    sc.offset.x = 0;
    sc.offset.y = 0;
    sc.extent.width = sf->width < B.rt_color->w ? sf->width : B.rt_color->w;
    sc.extent.height = sf->height < B.rt_color->h ? sf->height : B.rt_color->h;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    {
        float bc[4] = { ((rs->blend_color >> 16) & 0xFF) / 255.0f,
                        ((rs->blend_color >> 8) & 0xFF) / 255.0f,
                        (rs->blend_color & 0xFF) / 255.0f,
                        ((rs->blend_color >> 24) & 0xFF) / 255.0f };
        vkCmdSetBlendConstants(cmd, bc);
    }
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_ref & 0xFF);
    vkCmdBindVertexBuffers(cmd, 0, 1, &f->vbuf, &off);
    vkCmdDraw(cmd, count, 1, 0, 0);
    {
        /* RECOMP_VK_WATCH_DUMP=<n>: read every surface back right after the
         * n-th draw into the watched surface (once). */
        static int at = -2;
        static uint32_t watch;
        static int seen;
        if (at == -2) {
            const char *e = getenv("RECOMP_VK_WATCH_DUMP");
            const char *w = getenv("RECOMP_VK_WATCH");
            at = e ? atoi(e) : -1;
            watch = w ? (uint32_t)strtoul(w, NULL, 16) & 0x0FFFFFFFu : 0;
        }
        /* RECOMP_VK_WATCH_TEX=<va>: count only draws sampling that texture. */
        static uint32_t wtex = 1;
        if (wtex == 1) {
            const char *t = getenv("RECOMP_VK_WATCH_TEX");
            wtex = t ? (uint32_t)strtoul(t, NULL, 16) & 0x0FFFFFFFu : 0;
        }
        if (at >= 0 && watch && (sf->color_va & 0x0FFFFFFFu) == watch
            && (!wtex || (stages && (stages[0].t.offset & 0x0FFFFFFFu) == wtex))
            && ++seen == at) {
            end_rendering();
            dump_surfaces("after_", 0);
            fprintf(stderr, "[VKB] dumped surfaces after draw %d into %08X\n", at, watch);
        }
    }
}

static void vkb_draw_xf(const Nv2aSurface *sf, const Nv2aBatchXf *b)
{
    if (B.failed)
        return;
    {
        /* Once: the inputs of the colour-grade pass (dependent stages). */
        static int said;
        uint32_t sp = b->pixel->stage_program;
        if (said < 3 && (((sp >> 5) & 31) == 15 || ((sp >> 10) & 31) == 16)) {
            said++;
            fprintf(stderr, "[VKB] dependent pass: prog %05X other_stage_input %08X"
                    " dot_map %08X stages %08X %08X %08X\n", sp,
                    b->pixel->other_stage_input, b->pixel->dot_rgb_mapping,
                    b->pixel->stage[0].t.offset, b->pixel->stage[1].t.offset,
                    b->pixel->stage[2].t.offset);
        }
    }
    draw_vertices(sf, b->state, b->vertices, b->count, b->pixel->stage, b->pixel->zmax,
                  b->pixel);
}

/* The screen-space path: pre-transformed vertices, stage 0 only. */
static void vkb_draw(const Nv2aSurface *sf, const Nv2aBatch *b)
{
    static Nv2aVertexXf *buf;
    static uint32_t cap;
    Nv2aTexStage st[4];
    uint32_t i;

    if (B.failed || !b->count)
        return;
    if (b->count > cap) {
        free(buf);
        cap = b->count + 1024;
        buf = (Nv2aVertexXf *)malloc(cap * sizeof *buf);
        if (!buf) { cap = 0; return; }
    }
    for (i = 0; i < b->count; i++) {
        const Nv2aVertex *s = &b->vertices[i];
        Nv2aVertexXf *o = &buf[i];
        float w = s->rhw != 0.0f ? 1.0f / s->rhw : 1.0f;
        memset(o, 0, sizeof *o);
        o->pos[0] = s->x; o->pos[1] = s->y; o->pos[2] = s->z; o->pos[3] = w;
        o->d0[0] = ((s->diffuse >> 16) & 0xFF) / 255.0f;
        o->d0[1] = ((s->diffuse >> 8) & 0xFF) / 255.0f;
        o->d0[2] = (s->diffuse & 0xFF) / 255.0f;
        o->d0[3] = ((s->diffuse >> 24) & 0xFF) / 255.0f;
        o->tex[0][0] = s->u; o->tex[0][1] = s->v; o->tex[0][3] = 1.0f;
    }
    memset(st, 0, sizeof st);
    if (b->texture) {
        st[0].t = *b->texture;
        st[0].enabled = 1;
        st[0].normalised = 0;              /* uv arrive in texels */
    }
    /* Pre-transformed z: 0..1 for a title that uses D3D's convention. */
    draw_vertices(sf, b->state, buf, b->count, st, 1.0f, NULL);
}

static void vkb_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zs)
{
    VkClearAttachment ca[2];
    VkClearRect rect;
    uint32_t n = 0;

    if (B.failed || !init())
        return;
    begin_frame();
    if (!bind_target(sf, (flags & 3) ? rs->zeta_va : 0))
        return;
    if (flags & 0xF0) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[n].colorAttachment = 0;
        ca[n].clearValue.color.float32[0] = ((argb >> 16) & 0xFF) / 255.0f;
        ca[n].clearValue.color.float32[1] = ((argb >> 8) & 0xFF) / 255.0f;
        ca[n].clearValue.color.float32[2] = (argb & 0xFF) / 255.0f;
        ca[n].clearValue.color.float32[3] = ((argb >> 24) & 0xFF) / 255.0f;
        n++;
    }
    if ((flags & 3) && B.rt_depth) {
        ca[n].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                         | ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[n].colorAttachment = 0;
        /* Z24S8: depth in the top 24 bits. Z16 titles put it in the low 16
         * (the executor tells us nothing better here; 24-bit is GR2's). */
        ca[n].clearValue.depthStencil.depth = (float)(zs >> 8) / 16777215.0f;
        ca[n].clearValue.depthStencil.stencil = zs & 0xFF;
        n++;
    }
    if (!n)
        return;
    rect.rect.offset.x = 0;
    rect.rect.offset.y = 0;
    rect.rect.extent.width = sf->width < B.rt_color->w ? sf->width : B.rt_color->w;
    rect.rect.extent.height = sf->height < B.rt_color->h ? sf->height : B.rt_color->h;
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    vkCmdClearAttachments(cur()->cmd, n, ca, 1, &rect);
}

static int vkb_blit(uint32_t src_va, uint32_t src_pitch, uint32_t dst_va, uint32_t dst_pitch,
                    uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                    uint32_t bpp)
{
    Surface *s, *d;
    VkImageCopy rgn;

    (void)src_pitch;
    if (B.failed || !init() || bpp != 4)
        return 0;
    s = find_surface(src_va, 0);
    if (!s)
        return 0;                              /* not ours: let the CPU copy */
    begin_frame();
    d = get_surface(dst_va, 0, dst_pitch / 4 > dx + w ? dst_pitch / 4 : dx + w, dy + h);
    if (!d || d == s)
        return 0;
    to_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&rgn, 0, sizeof rgn);
    rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rgn.srcSubresource.layerCount = 1;
    rgn.dstSubresource = rgn.srcSubresource;
    rgn.srcOffset.x = (int32_t)sx; rgn.srcOffset.y = (int32_t)sy;
    rgn.dstOffset.x = (int32_t)dx; rgn.dstOffset.y = (int32_t)dy;
    rgn.extent.width = w < s->w - sx ? w : s->w - sx;
    rgn.extent.height = h < s->h - sy ? h : s->h - sy;
    rgn.extent.depth = 1;
    vkCmdCopyImage(cur()->cmd, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    return 1;
}

static void write_bmp(const char *path, const uint8_t *map, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54];
    uint32_t rowpad = (4 - (w * 3) % 4) % 4, size = 54 + (w * 3 + rowpad) * h, y, x;
    static const uint8_t pad[3];

    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0;) {
        for (x = 0; x < w; x++)
            fwrite(map + ((size_t)y * w + x) * 4, 1, 3, f);
        fwrite(pad, 1, rowpad, f);
    }
    fclose(f);
}

/* Read back every colour surface (RECOMP_VK_DUMP_SURFACES=1, on the dumped
 * presents): <prefix>NNNN_<va>.bmp, plus <prefix>NNNN_<va>_a.bmp with the
 * alpha channel as grey -- the post-process chain reads alpha. */
static void dump_surfaces(const char *prefix, unsigned shot)
{
    int i;
    {
        /* And the cached textures of RECOMP_VK_DUMP_TEX, read back from the
         * GPU: what the shaders actually sample. */
        const char *e = getenv("RECOMP_VK_DUMP_TEX");
        uint32_t want = e ? (uint32_t)strtoul(e, NULL, 16) & 0x0FFFFFFFu : 0;
        for (i = 0; want && i < B.ntex; i++) {
            Tex *t = &B.tex[i];
            VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkBufferImageCopy c;
            char path[128];
            if ((t->va & 0x0FFFFFFFu) != want || !t->img)
                continue;
            if (!make_buffer((VkDeviceSize)t->w * t->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             &buf, &mem, (void **)&map))
                return;
            begin_frame();
            end_rendering();
            img_barrier(cur()->cmd, t->img, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            memset(&c, 0, sizeof c);
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = t->w; c.imageExtent.height = t->h; c.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cur()->cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            img_barrier(cur()->cmd, t->img, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            submit_frame();
            vkDeviceWaitIdle(B.dev);
            snprintf(path, sizeof path, "%sgputex_%d_p%u.bmp", prefix, i, t->pitch);
            write_bmp(path, map, t->w, t->h);
            fprintf(stderr, "[VKB] gpu texture %d (%08X pitch %u view %p) first %08X mid %08X\n",
                    i, t->va, t->pitch, (void *)t->view, ((uint32_t *)map)[0],
                    ((uint32_t *)map)[t->w * t->h / 2 + t->w / 2]);
            vkDestroyBuffer(B.dev, buf, NULL);
            vkFreeMemory(B.dev, mem, NULL);
        }
    }
    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        Surface *sf = &B.surf[i];
        VkBuffer buf;
        VkDeviceMemory mem;
        uint8_t *map;
        VkBufferImageCopy c;
        char path[512];
        size_t k, n;

        if (!sf->img || sf->depth)
            continue;
        if (!make_buffer((VkDeviceSize)sf->w * sf->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &buf, &mem, (void **)&map))
            return;
        begin_frame();
        to_layout(sf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = sf->w;
        c.imageExtent.height = sf->h;
        c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cur()->cmd, sf->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buf, 1, &c);
        submit_frame();
        vkDeviceWaitIdle(B.dev);
        snprintf(path, sizeof path, "%s%04u_%08X.bmp", prefix, shot, sf->va);
        write_bmp(path, map, sf->w, sf->h);
        n = (size_t)sf->w * sf->h;
        for (k = 0; k < n; k++)
            map[k * 4] = map[k * 4 + 1] = map[k * 4 + 2] = map[k * 4 + 3];
        snprintf(path, sizeof path, "%s%04u_%08X_a.bmp", prefix, shot, sf->va);
        write_bmp(path, map, sf->w, sf->h);
        vkDestroyBuffer(B.dev, buf, NULL);
        vkFreeMemory(B.dev, mem, NULL);
    }
}

/* RECOMP_VK_DUMP=<prefix>: every RECOMP_VK_DUMP_EVERY presents (default
 * 120), read the presented frame back and write <prefix>NNNN.bmp -- the
 * guest-memory dumps (RECOMP_FB_DUMP) see nothing the GPU drew. */
static void dump_frame(VkImage img, uint32_t w, uint32_t h)
{
    static const char *prefix = (const char *)-1;
    static unsigned every, n, shot;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkBufferImageCopy c;
    char path[512];
    FILE *f;
    uint32_t y, rowpad, size;

    if (prefix == (const char *)-1) {
        const char *e = getenv("RECOMP_VK_DUMP_EVERY");
        prefix = getenv("RECOMP_VK_DUMP");
        every = e ? (unsigned)atoi(e) : 120;
        if (!every) every = 120;
    }
    if (!prefix || (n++ % every))
        return;
    if (!make_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem,
                     (void **)&map))
        return;
    begin_frame();
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cur()->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    submit_frame();
    vkDeviceWaitIdle(B.dev);
    snprintf(path, sizeof path, "%s%04u.bmp", prefix, shot);
    if (getenv("RECOMP_VK_DUMP_SURFACES"))
        dump_surfaces(prefix, shot);
    shot++;
    f = fopen(path, "wb");
    if (f) {
        uint8_t hdr[54];
        rowpad = (4 - (w * 3) % 4) % 4;
        size = 54 + (w * 3 + rowpad) * h;
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &size, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        for (y = h; y-- > 0;) {
            uint32_t x;
            static const uint8_t pad[3];
            for (x = 0; x < w; x++)
                fwrite(map + ((size_t)y * w + x) * 4, 1, 3, f);   /* B,G,R */
            fwrite(pad, 1, rowpad, f);
        }
        fclose(f);
    }
    vkDestroyBuffer(B.dev, buf, NULL);
    vkFreeMemory(B.dev, mem, NULL);
}

static void vkb_present(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h,
                        uint32_t aa_sx, uint32_t aa_sy)
{
    Surface *s;
    VkCommandBuffer cmd;
    VkImageBlit b;
    int o;

    (void)pitch; (void)aa_sx; (void)aa_sy;
    if (B.failed || !init())
        return;
    s = find_surface(va, 0);
    begin_frame();
    if (s && w && h) {
        o = B.out_i ^ 1;
        if (!B.out[o] || B.out_w[o] != w || B.out_h[o] != h) {
            if (B.out[o]) {
                submit_frame();
                vkDeviceWaitIdle(B.dev);
                begin_frame();
                vkDestroyImage(B.dev, B.out[o], NULL);
                vkFreeMemory(B.dev, B.out_mem[o], NULL);
            }
            if (!make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            0, &B.out[o], &B.out_mem[o], NULL))
                return;
            B.out_w[o] = w;
            B.out_h[o] = h;
        }
        cmd = cur()->cmd;
        to_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        img_barrier(cmd, B.out[o], 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1].x = (int32_t)(w < s->w ? w : s->w);
        b.srcOffsets[1].y = (int32_t)(h < s->h ? h : s->h);
        b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)w;
        b.dstOffsets[1].y = (int32_t)h;
        b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cmd, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       B.out[o], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        img_barrier(cmd, B.out[o], 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        submit_frame();
        B.out_i = o;
        vkw_set_gpu_frame(B.out[o], w, h);
        dump_frame(B.out[o], w, h);
        {
            /* Frames presented, once a second: the window title says it too,
             * but a log outlives the window. */
            static unsigned frames;
            static uint32_t t0;
            uint32_t now = GetTickCount();
            frames++;
            if (!t0) t0 = now;
            if (now - t0 >= 5000) {
                fprintf(stderr, "[VKB] %.1f fps (%u pipelines, %d textures)\n",
                        frames * 1000.0 / (now - t0), B.npipes, B.ntex);
                frames = 0;
                t0 = now;
            }
        }
    } else {
        submit_frame();
    }
}

static void vkb_flip(void)
{
}

static const Nv2aBackend s_vk_backend = {
    vkb_clear, vkb_draw, vkb_flip, vkb_draw_xf, vkb_blit, vkb_present
};

/* Called from main() after the window has been started. */
int gr2_renderer_start(void)
{
    const char *r = getenv("RECOMP_RENDERER");

    if (!r || strcmp(r, "vk") != 0)
        return 0;
    nv2a_backend_register(&s_vk_backend);
    fprintf(stderr, "[VKB] Vulkan renderer registered (RECOMP_RENDERER=vk)\n");
    return 1;
}
