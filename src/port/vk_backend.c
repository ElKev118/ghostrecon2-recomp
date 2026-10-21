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
#include "nv2a_vsh_glsl.h"

#define VKB_FRAMES        2
#define VKB_VBUF_BYTES    (48u << 20)          /* vertex ring per frame */
#define VKB_UBUF_BYTES    (8u << 20)           /* combiner-state ring per frame */
#define VKB_MAX_SURFACES  48
#define VKB_MAX_TEXTURES  1024
#define VKB_MAX_PIPELINES 8192

typedef struct {
    uint32_t va;                /* guest address (low 28 bits compared) */
    int      depth;             /* depth/stencil target */
    uint32_t w, h;
    VkImage  img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageLayout layout;
    uint32_t last_used;
    /* Guest memory holds newer contents than the image: something wrote the
     * surface without us (a 2D blit from a source we never rendered). Sampled
     * as a texture it then comes from memory; drawing into it again clears
     * the flag. */
    int cpu_newer;
    uint32_t written;       /* B.write_seq when last drawn/cleared/blitted into */
} Surface;

typedef struct {
    uint32_t va, color, w, h, pitch;
    uint64_t hash;
    uint32_t checked_frame;
    int      changes;           /* contents seen to change since the upload */
    VkImage  img;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t last_used;
} Tex;

/* Fragment-shader specialization: the combiner set-up of a draw (FS_SRC's
 * S0..S43). One pipeline per distinct set-up and raster state. */
#define SPEC_N 47
typedef struct {
    uint64_t key[4];            /* key[3]: GPU vertex program (0: transformed) */
    uint32_t spec[SPEC_N];
    uint32_t hash;              /* of key and spec, for the lookup */
    VkPipeline pipe;
} Pipe;
static const uint32_t *s_spec;  /* the draw being set up (NULL: all zero) */

/* Generated fragment shaders (vk_psh_gen.c), by set-up. */
size_t vk_psh_gen(const uint32_t *spec, int zofs, char *buf, size_t cap);
typedef struct {
    uint32_t spec[SPEC_N];
    uint32_t hash;
    int zofs;
    VkShaderModule mod;
} FsProg;
#define VKB_MAX_FS 2048

/* A vertex program compiled for the GPU (draw_vsh), by content. */
typedef struct {
    uint64_t hash;
    uint32_t inputs;            /* v registers it reads */
    uint32_t packed;            /* of those, 11:11:10 normals (uint input) */
    uint32_t imm;               /* of those, constants (no vertex input) */
    VkShaderModule mod;
} VshProg;
#define VKB_MAX_VSH 256
#define VSH_UBO_BYTES 3584u     /* 192 vec4 + aa + 16 imm, rounded to 256 */

/* Set while draw_vertices draws a draw_vsh batch: untransformed inputs and an
 * index list, the program's module and constants. */
typedef struct {
    const VshProg *prog;
    uint32_t id;                /* index in B.vsh, +1 */
    const float *inputs;
    uint32_t nverts, ninputs;
    uint8_t regs[16];
    const uint32_t *indices;
    uint32_t nindices;
    const float (*constants)[4];
    float aa[2];
    const Nv2aBatchVsh *b;      /* raw streams when b->raw */
    uint64_t layout;            /* hash of the raw vertex layout */
} VshDraw;
static const VshDraw *s_vsh;

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
    int ts_valid;                   /* timestamps written in this submission */
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
    VshProg vsh[VKB_MAX_VSH];
    int nvsh;
    FsProg fsp[VKB_MAX_FS];
    int nfsp;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule vs, fs, fs_zofs;
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
    uint32_t write_seq;
    uint32_t next_target, next_target_w;   /* colour target of the draw being set up */
    uint32_t next_zeta;                    /* and its depth surface (va key, 0 none) */
} B;

static PFN_vkCmdPushDescriptorSetKHR pfnPushDescriptor;

/* Per-present statistics: fence waits in begin_frame, submits, uploads. */
static long long s_stat_wait;
static unsigned s_stat_begins, s_stat_uploads;
static unsigned s_stat_passes, s_stat_barriers, s_stat_copies, s_stat_draws;
static double s_stat_gpu_ms;            /* GPU time of the submissions, by timestamps */
static VkQueryPool s_qpool;

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
    s_stat_barriers++;
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
    {
        /* Time the CPU spends waiting for the GPU here (printed by present). */
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        vkWaitForFences(B.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        QueryPerformanceCounter(&t1);
        s_stat_wait += t1.QuadPart - t0.QuadPart;
        s_stat_begins++;
        if (s_qpool && f->ts_valid) {
            uint64_t ts[2];
            if (vkGetQueryPoolResults(B.dev, s_qpool, (uint32_t)(B.fi * 2), 2, sizeof ts, ts,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                s_stat_gpu_ms += (double)(ts[1] - ts[0])
                               * B.c->props.limits.timestampPeriod / 1e6;
            f->ts_valid = 0;
        }
    }
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
    if (s_qpool) {
        vkCmdResetQueryPool(f->cmd, s_qpool, (uint32_t)(B.fi * 2), 2);
        vkCmdWriteTimestamp(f->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_qpool,
                            (uint32_t)(B.fi * 2));
    }
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
    if (s_qpool) {
        vkCmdWriteTimestamp(f->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_qpool,
                            (uint32_t)(B.fi * 2 + 1));
        f->ts_valid = 1;
    }
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

/* Surfaces are keyed by address AND width (from the pitch): Ghost Recon 2
 * uses one address as a 256-wide colour table, a 640-wide buffer and a
 * blit target within one frame. One image per address made them overwrite
 * each other (red flash, washed-out glow, wrong HUD) or read past the
 * table into the buffer.
 *
 * find_surface: want_w = 0 means "any width": the one written last. */
static Surface *find_surface_w(uint32_t va, int depth, uint32_t want_w)
{
    Surface *best = NULL;
    int i;
    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        Surface *c = &B.surf[i];
        if (!c->img || c->depth != depth || c->va != va_key(va))
            continue;
        if (want_w && c->w != want_w)
            continue;
        if (!best || c->written > best->written)
            best = c;
    }
    return best;
}

static Surface *find_surface(uint32_t va, int depth)
{
    return find_surface_w(va, depth, 0);
}

/* The surface a texture of this size reads: same width if there is one,
 * else the most recently written. */
static Surface *find_surface_for_tex(uint32_t va, uint32_t tw)
{
    Surface *s = find_surface_w(va, 0, tw);
    return s ? s : find_surface(va, 0);
}

static Surface *get_surface(uint32_t va, int depth, uint32_t w, uint32_t h)
{
    Surface *s = find_surface_w(va, depth, depth ? 0 : w);
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
                    depth ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
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
    uint32_t h = sf->clip_y + sf->height;      /* the clip box ends there */
    Surface *c, *d = NULL;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };

    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h)
        return 0;
    c = get_surface(sf->color_va, 0, w, h);
    if (zeta_va)
        d = get_surface(zeta_va, 1, w, h);
    if (!c)
        return 0;
    c->cpu_newer = 0;
    c->written = ++B.write_seq;
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
    s_stat_passes++;
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
    s_stat_uploads++;
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
    /* Recorded in the frame's command buffer, ahead of the draws that
     * sample it. (Uploads were once submitted one by one, each submit then
     * waiting for the GPU -- ~1.7 full CPU/GPU syncs a frame in play. The
     * strip that "never reached the GPU" when batched was the texture cache
     * re-checking contents once per submit-numbered frame; small textures
     * are now re-checked on every use, see texture_view.) */
    return 1;
}

/* The view a stage samples, or NULL for "use white". */
/* Size of the image behind the last view texture_view returned: a surface
 * can be larger than the texture the title samples from it (Ghost Recon 2
 * draws its 256x256 grade table into an address it also uses as a 640x480
 * buffer), so coordinates must be scaled to the part that holds the data. */
static uint32_t s_view_w, s_view_h;
/* RECOMP_VK_PROBE: 1 = flag file seen, 2 = a present happened since,
 * 3 = probing, 4 = the probed frame was presented. */
static int s_probe_armed;
#define PROBE_MAX 3000
#define PROBE_R   32
#define PROBE_PTS 6
typedef struct {
    uint32_t ok, surf, count, blend, bsrc, bdst, cmask, sp, ctl, f0, f1, t0, t0f, t1, t1f;
    /* Enough to evaluate the draw's combiners by hand. */
    uint32_t seen, cicw[8], aicw[8], cocw[8], aocw[8], k0[8], k1[8];
    float d0[4], tc0[4];
} ProbeRec;
static ProbeRec s_probe_rec[PROBE_MAX];
static VkBuffer s_probe_buf;
static VkDeviceMemory s_probe_mem;
static uint8_t *s_probe_map;
/* texture_view found a depth texture: a shadow-map stage (compare, not colour). */
static int s_view_depth;

static int is_depth_format(uint32_t c)
{
    return c == 0x2A || c == 0x2B || c == 0x2C || c == 0x2D
        || c == 0x2E || c == 0x2F || c == 0x30 || c == 0x31;
}

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
    if (is_depth_format(src->color)) {
        /* A depth texture is a depth surface we rendered: sample its depth
         * aspect and let the shader compare (shadow map). */
        Surface *dz = find_surface_w(src->offset, 1, 0);
        /* Refused only when it is this draw's own depth attachment. The
         * attachment of the *previous* draw (B.rt_depth) is no reason: GR2
         * draws each shadow receiver pass right after rendering the map,
         * so that test threw away every shadow map. */
        if (!dz || (B.next_zeta && dz->va == B.next_zeta))
            return NULL;
        to_layout(dz, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_view_w = dz->w; s_view_h = dz->h;
        s_view_depth = 1;
        return dz->view;
    }
    /* Rendered by us: sample the surface itself. */
    s = find_surface_for_tex(src->offset, src->width);
    if (s && src->width == 256 && (src->offset & 0x0FFFFFFFu) == 0x00A17000u) {
        Surface *big = find_surface_w(src->offset, 0, 640);
        static unsigned n;
        if (big && big->written > s->written && n++ < 30)
            fprintf(stderr, "[VKB] grade lookup: table written %u but 640 buffer at the same address written later (%u)\n",
                    s->written, big->written);
    }
    if (0) {
        static unsigned said;
        if (said++ < 20)
            fprintf(stderr, "[VKB] tex %08X %ux%u: no surface that wide, using %ux%u (written %u, seq %u)\n",
                    src->offset, src->width, src->height, s->w, s->h, s->written, B.write_seq);
    }
    if (s && s->cpu_newer)
        s = NULL;                              /* read guest memory instead */
    if (s) {
        /* The target of the draw being prepared, not of the last one: a
         * post-process pass that draws onto the scene while reading it,
         * right after drawing elsewhere, sampled its own attachment -- a
         * feedback loop with undefined results (the diagonal seam between
         * the quad's triangles, half-screen tints, flickering veils). */
        if (s->va == B.next_target && s->w == B.next_target_w) {
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
            s_stat_copies++;
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
    /* Checked once a frame -- or on every use for a small texture that has
     * been seen to change: the title rewrites some within a frame (GR2's
     * colour-grade strip). Hashing every small texture on every use cost a
     * quarter of the pushbuffer thread. */
    if (t && t->checked_frame == B.frame_no
        && !(t->changes && tex_bytes(src) <= 65536u)) {
        t->last_used = B.frame_no;
        return t->view;
    }
    h = hash_bytes(mem + src->offset, tex_bytes(src));
    if (t && t->img && t->hash != h)
        t->changes++;
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
    /* Largest normalised coordinate inside each stage's texture when the
     * image behind it is larger (a surface): u,v for stages 0/1 in [0],
     * 2/3 in [1]. The hardware clamps at the texture's own edge; sampling
     * on into the rest of the surface read whatever else lives there. */
    float clampmax[2][4];
} PushConsts;

/* The pixel stage, std140, one copy per draw (Nv2aPixelState). */
typedef struct {
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8], f0[8], f1[8];
    uint32_t fin[4];        /* final0, final1, final_c0, final_c1 */
    uint32_t ctl[4];        /* control, stage_program, other_stage_input, rc_seen */
    uint32_t misc[4];       /* clip_plane_mode, fog_enable, fog_color, fog_mode */
    float    fogp[4];       /* fog bias, fog scale */
    float    clampmin[2][4];/* half a texel of the image, per stage (see clampmax) */
    uint32_t shadow[4];     /* x: stage mask of shadow-map stages, y: compare func */
    float    shadowmax[4];  /* per stage: largest depth value of its format */
    uint32_t border[4];     /* per stage: SET_TEXTURE_BORDER_COLOR (ARGB) */
    uint32_t bmask[4];      /* x: bit 2*st u, 2*st+1 v use the border colour */
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
    "layout(push_constant) uniform PC { vec4 surf; vec4 texscale[4]; ivec4 alpha; vec4 clampmax[2]; } pc;\n"
    "layout(location=0) out vec4 vD0;\n"
    "layout(location=1) out vec4 vD1;\n"
    "layout(location=2) out vec4 vFog;\n"
    "layout(location=3) out vec4 vT[4];\n"
    "void main() {\n"
    /* pos is surface pixels and depth units, divided by w; rebuild clip
     * space so the GPU clips against the near plane itself. */
    "  float w = inPos.w;\n"
    /* The NV2A keeps 4 bits of sub-pixel precision: truncate to 1/16 as
     * xemu's roundScreenCoords does. Titles add D3D's 0.53125 screen offset;
     * truncated to 0.5 it lands on the first pixel centre (covered, top-left
     * rule), unrounded it misses row and column 0 -- Ghost Recon 2's colour
     * grade table then had a black column 0, and every scene pixel with
     * alpha 0 lost its red (the cyan ground). */
    "  vec2 sp = trunc(inPos.xy * 16.0) / 16.0;\n"
    "  vec3 ndc = vec3(sp.x / pc.surf.x * 2.0 - 1.0,\n"
    "                  sp.y / pc.surf.y * 2.0 - 1.0,\n"
    "                  inPos.z * pc.surf.z);\n"
    "  gl_Position = vec4(ndc * w, w);\n"
    /* The NV2A clamps the vertex colours to [0,1] before interpolation
     * (xemu: vtxD0 = clamp(NaNToOne(oD0), 0, 1)). Vertex programs write
     * more (GR2's ground detail passes: oD0.a = 1.46), and unclamped every
     * detail pass brightened instead of staying neutral -- 190 stacked passes
     * burnt the base entrance white. */
    "  vD0 = clamp(mix(inD0, vec4(1.0), isnan(inD0)), 0.0, 1.0);\n"
    "  vD1 = clamp(mix(inD1, vec4(1.0), isnan(inD1)), 0.0, 1.0);\n"
    "  vFog = inFog;\n"
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
    "  uvec4 fin; uvec4 ctl; uvec4 misc; vec4 fogp; vec4 clampmin[2]; uvec4 shadow; vec4 shadowmax;\n"
    "  uvec4 border; uvec4 bmask; } rc;\n"
    /* The per-draw configuration as specialization constants: the driver
     * compiles one shader per combiner set-up, loops unrolled and the
     * register file in registers (see SPEC_N). */
    "layout(constant_id=0) const uint S0 = 0u;\n"
    "layout(constant_id=1) const uint S1 = 0u;\n"
    "layout(constant_id=2) const uint S2 = 0u;\n"
    "layout(constant_id=3) const uint S3 = 0u;\n"
    "layout(constant_id=4) const uint S4 = 0u;\n"
    "layout(constant_id=5) const uint S5 = 0u;\n"
    "layout(constant_id=6) const uint S6 = 0u;\n"
    "layout(constant_id=7) const uint S7 = 0u;\n"
    "layout(constant_id=8) const uint S8 = 0u;\n"
    "layout(constant_id=9) const uint S9 = 0u;\n"
    "layout(constant_id=10) const uint S10 = 0u;\n"
    "layout(constant_id=11) const uint S11 = 0u;\n"
    "layout(constant_id=12) const uint S12 = 0u;\n"
    "layout(constant_id=13) const uint S13 = 0u;\n"
    "layout(constant_id=14) const uint S14 = 0u;\n"
    "layout(constant_id=15) const uint S15 = 0u;\n"
    "layout(constant_id=16) const uint S16 = 0u;\n"
    "layout(constant_id=17) const uint S17 = 0u;\n"
    "layout(constant_id=18) const uint S18 = 0u;\n"
    "layout(constant_id=19) const uint S19 = 0u;\n"
    "layout(constant_id=20) const uint S20 = 0u;\n"
    "layout(constant_id=21) const uint S21 = 0u;\n"
    "layout(constant_id=22) const uint S22 = 0u;\n"
    "layout(constant_id=23) const uint S23 = 0u;\n"
    "layout(constant_id=24) const uint S24 = 0u;\n"
    "layout(constant_id=25) const uint S25 = 0u;\n"
    "layout(constant_id=26) const uint S26 = 0u;\n"
    "layout(constant_id=27) const uint S27 = 0u;\n"
    "layout(constant_id=28) const uint S28 = 0u;\n"
    "layout(constant_id=29) const uint S29 = 0u;\n"
    "layout(constant_id=30) const uint S30 = 0u;\n"
    "layout(constant_id=31) const uint S31 = 0u;\n"
    "layout(constant_id=32) const uint S32 = 0u;\n"
    "layout(constant_id=33) const uint S33 = 0u;\n"
    "layout(constant_id=34) const uint S34 = 0u;\n"
    "layout(constant_id=35) const uint S35 = 0u;\n"
    "layout(constant_id=36) const uint S36 = 0u;\n"
    "layout(constant_id=37) const uint S37 = 0u;\n"
    "layout(constant_id=38) const uint S38 = 0u;\n"
    "layout(constant_id=39) const uint S39 = 0u;\n"
    "layout(constant_id=40) const uint S40 = 0u;\n"
    "layout(constant_id=41) const uint S41 = 0u;\n"
    "layout(constant_id=42) const uint S42 = 0u;\n"
    "layout(constant_id=43) const uint S43 = 0u;\n"
    /* Alpha test (enable, func, ref): a shader that can discard loses the
     * early depth test, so only the pipelines that alpha-test may. */
    "layout(constant_id=44) const uint S44 = 0u;\n"
    "layout(constant_id=45) const uint S45 = 0u;\n"
    "layout(constant_id=46) const uint S46 = 0u;\n"
    "uint sc_cicw(uint i) { return i == 0u ? S0 : i == 1u ? S1 : i == 2u ? S2 : i == 3u ? S3 : i == 4u ? S4 : i == 5u ? S5 : i == 6u ? S6 : S7; }\n"
    "uint sc_aicw(uint i) { return i == 0u ? S8 : i == 1u ? S9 : i == 2u ? S10 : i == 3u ? S11 : i == 4u ? S12 : i == 5u ? S13 : i == 6u ? S14 : S15; }\n"
    "uint sc_cocw(uint i) { return i == 0u ? S16 : i == 1u ? S17 : i == 2u ? S18 : i == 3u ? S19 : i == 4u ? S20 : i == 5u ? S21 : i == 6u ? S22 : S23; }\n"
    "uint sc_aocw(uint i) { return i == 0u ? S24 : i == 1u ? S25 : i == 2u ? S26 : i == 3u ? S27 : i == 4u ? S28 : i == 5u ? S29 : i == 6u ? S30 : S31; }\n"
    "const uvec4 SC_CTL = uvec4(S32, S33, S34, S35);\n"
    "const uvec4 SC_FIN = uvec4(S36, S37, 0u, 0u);\n"
    "const uvec4 SC_SH = uvec4(S38, S39, S40, 0u);\n"
    "const uvec4 SC_MISC = uvec4(S41, S42, 0u, S43);\n"
    "layout(push_constant) uniform PC { vec4 surf; vec4 texscale[4]; ivec4 alpha; vec4 clampmax[2]; } pc;\n"
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
    "  vec4 cm = pc.clampmax[st >> 1]; vec2 mx = (st & 1) == 0 ? cm.xy : cm.zw;\n"
    "  vec4 cn = rc.clampmin[st >> 1]; vec2 mn = (st & 1) == 0 ? cn.xy : cn.zw;\n"
    /* BORDER addressing with the border colour register: outside [0,1] the
     * stage reads that colour (Ghost Recon 2's ground detail decals use a
     * neutral grey there; a black border blackened the ground). */
    /* The texture's own extent: the whole image, or for a texture inside a
     * larger surface (GR2's 300x300 shadow maps in a 320x300 one) the part
     * clampmin/clampmax bracket by half a texel. Clamping instead stretched
     * the edge texels outward -- shadows of whatever sat on a cascade's edge
     * spread over the ground, changing as the cascades moved with the view. */
    "  uint bm = (SC_SH.z >> uint(st * 2)) & 3u;\n"
    "  vec2 lim = mx.x > 0.0 ? mx + mn : vec2(1.0);\n"
    "  if (bm != 0u\n"
    "      && (((bm & 1u) != 0u && (uv.x < 0.0 || uv.x > lim.x))\n"
    "          || ((bm & 2u) != 0u && (uv.y < 0.0 || uv.y > lim.y))))\n"
    "    return unpk(rc.border[st]);\n"
    "  if (mx.x > 0.0) uv = clamp(uv, mn, mx);\n"
    "  if (st == 0) return texture(tex[0], uv);\n"
    "  if (st == 1) return texture(tex[1], uv);\n"
    "  if (st == 2) return texture(tex[2], uv);\n"
    "  return texture(tex[3], uv);\n"
    "}\n"
    "float fog_factor(float d) {\n"
    "  float f, px = rc.fogp.x, py = rc.fogp.y; uint m = SC_MISC.w;\n"
    "  if (SC_MISC.y == 0u || pc.alpha.w == 8) return 1.0;\n"
    "  if (m == 0x800u || m == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (m == 0x801u || m == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (m == 0x802u || m == 0x803u || m == 0x804u) f = abs(f);\n"
    "  if (isnan(f)) f = 1.0;\n"
    "  return clamp(f, 0.0, 1.0);\n"
    "}\n"
    "bool alpha_pass(float a) {\n"
    "  int v = int(a * 255.0 + 0.5), r = int(S46);\n"
    "  switch (int(S45)) {\n"
    "  case 0x200: return false; case 0x201: return v < r; case 0x202: return v == r;\n"
    "  case 0x203: return v <= r; case 0x204: return v > r; case 0x205: return v != r;\n"
    "  case 0x206: return v >= r; default: return true; }\n"
    "}\n"
    "void main() {\n"
    /* Polygon offset (xemu: zvalue += depthOffset + depthFactor * slope), in
     * depth units; only the ZOFS variant writes depth, so every other draw
     * keeps early depth testing. */
    "#ifdef ZOFS\n"
    "  float zu = gl_FragCoord.z / pc.surf.z;\n"
    "  float zsl = max(abs(dFdx(zu)), abs(dFdy(zu)));\n"
    "#endif\n"
    "  vec4 c;\n"
    "  if (SC_CTL.w == 0u) {\n"
    /* No combiner state yet: texture 0 times diffuse, as the CPU path. */
    "    c = vD0;\n"
    "    if (pc.texscale[0].z != 0.0) {\n"
    "      vec4 t = vT[0]; vec2 uv = (t.w != 0.0 ? t.xy / t.w : t.xy) * pc.texscale[0].xy;\n"
    "      c *= samp(0, uv);\n"
    "    }\n"
    "  } else {\n"
    "    vec4 t[4];\n"
    "    uint sp = SC_CTL.y;\n"
    "    for (int st = 0; st < 4; st++) {\n"
    "      uint mode = (sp >> uint(st * 5)) & 31u;\n"
    "      vec4 tc = vT[st]; float q = tc.w != 0.0 ? tc.w : 1.0;\n"
    "      if (mode == 0u) t[st] = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "      else if ((mode == 1u || mode == 2u) && ((SC_SH.x >> uint(st)) & 1u) != 0u) {\n"
    /* Shadow map (xemu psh_append_shadowmap): stored depth <func> z/w. */
    "        float d = samp(st, tc.xy / q * pc.texscale[st].xy).r * rc.shadowmax[st];\n"
    "        float z = clamp(tc.z / q, 0.0, rc.shadowmax[st]);\n"
    "        bool r; uint f = SC_SH.y;\n"
    "        if (f == 0u) r = false; else if (f == 1u) r = d < z; else if (f == 2u) r = d == z;\n"
    "        else if (f == 3u) r = d <= z; else if (f == 4u) r = d > z; else if (f == 5u) r = d != z;\n"
    "        else if (f == 6u) r = d >= z; else r = true;\n"
    "        if (rc.shadow.z == 1u) r = false; else if (rc.shadow.z == 2u) r = true;\n"
    "        t[st] = vec4(r ? 1.0 : 0.0);\n"
    /* RECOMP_VK_FS=shd: stage 0's decision as the pass colour (on the
     * DST*(1-SRC) shadow passes the probe turns it back: src = 1 - after /
     * before). r 0.5 = inside the map, g 0.5 = compared shadowed, b 0.5 =
     * stored depth is the far clear. */
    "        if (pc.alpha.w == 30 && st == 0) {\n"
    "          vec4 cm = pc.clampmax[0]; vec4 cn = rc.clampmin[0];\n"
    "          vec2 lim = cm.x > 0.0 ? cm.xy + cn.xy : vec2(1.0);\n"
    "          vec2 uv = tc.xy / q * pc.texscale[0].xy;\n"
    "          bool ins = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, lim));\n"
    "          outColor = vec4(ins ? 0.5 : 0.0, r ? 0.0 : 0.5, d >= rc.shadowmax[0] * 0.999 ? 0.5 : 0.0, 0.0);\n"
    "          return;\n"
    "        }\n"
    "      }\n"
    "      else if (mode == 1u || mode == 2u || mode == 3u)\n"
    "        t[st] = samp(st, tc.xy / q * pc.texscale[st].xy);\n"
    "      else if (mode == 4u) t[st] = clamp(tc, 0.0, 1.0);\n"
    "      else if (mode == 5u) {\n"
    "        for (int j = 0; j < 4; j++) {\n"
    "          bool ge = ((SC_MISC.x >> uint(st * 4 + j)) & 1u) != 0u;\n"
    "          if (ge ? tc[j] >= 0.0 : tc[j] < 0.0) discard;\n"
    "        }\n"
    "        t[st] = vec4(0.0);\n"
    "      } else if (mode == 15u || mode == 16u) {\n"
    "        int src = st <= 1 ? 0 : int((SC_CTL.z >> (st == 2 ? 16u : 20u)) & 15u) % st;\n"
    "        vec4 i = t[src];\n"
    "        vec2 dc = mode == 15u ? vec2(pc.alpha.w == 4 ? 0.5 : i.a, i.r) : vec2(i.g, i.b);\n"
    "        t[st] = samp(st, dc * pc.texscale[st].xy);\n"
    "      } else t[st] = vec4(0.0);\n"
    "    }\n"
    "    for (int k = 0; k < 16; k++) R[k] = vec4(0.0);\n"
    "    R[3] = vec4(unpk(rc.misc.z).rgb, fog_factor(vFog.x));\n"
    "    R[4] = vD0; R[5] = vD1;\n"
    "    R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
    "    if (pc.alpha.w == 20) { outColor = vec4(t[0].rgb, 1.0); return; }\n"
    "    if (pc.alpha.w == 21) { outColor = vec4(clamp(vD0.rgb, 0.0, 1.0), clamp(vD0.a, 0.0, 1.0)); return; }\n"
    "    R[12].a = (sp & 31u) != 0u ? t[0].a : 1.0;\n"
    "    uint n = min(SC_CTL.x & 255u, 8u), flags = SC_CTL.x >> 8;\n"
    "    for (uint s = 0u; s < n; s++) {\n"
    "      uint icw = sc_cicw(s), ocw = sc_cocw(s);\n"
    "      uint aicw = sc_aicw(s), aocw = sc_aocw(s);\n"
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
    /* RECOMP_VK_FS=stN: r0 after stage N (on the passes C chose). */
    "      if (pc.alpha.w >= 10 && pc.alpha.w < 20 && int(s) == pc.alpha.w - 10) {\n"
    "        outColor = vec4(clamp(R[12].rgb, 0.0, 1.0), clamp(R[12].a, 0.0, 1.0)); return; }\n"
    "    }\n"
    "    if (SC_FIN.x != 0u || SC_FIN.y != 0u) {\n"
    "      uint f0 = SC_FIN.x, f1 = SC_FIN.y, ff = f1 & 255u;\n"
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
    "  if (pc.alpha.w == 5 && ((SC_CTL.y >> 5) & 31u) == 15u) c = vec4(R[9].rgb, 1.0);\n"
    "  if (pc.alpha.w == 6 && ((SC_CTL.y >> 5) & 31u) == 15u) c = vec4(R[8].rgb, 1.0);\n"
    "  if (pc.alpha.w == 7 && SC_MISC.y != 0u) c = vec4(vec3(fog_factor(vFog.x)), c.a);\n"
    "  if (pc.alpha.w == 1) c = vec4(1.0, 0.0, 1.0, 1.0);\n"
    "  else if (pc.alpha.w == 2) c = vec4(vD0.rgb, 1.0);\n"
    "  else if (pc.alpha.w == 3) c = vec4(samp(0, vT[0].xy * pc.texscale[0].xy).rgb, 1.0);\n"
    "  if (S44 != 0u && !alpha_pass(c.a)) discard;\n"
    "  outColor = c;\n"
    "#ifdef ZOFS\n"
    "  gl_FragDepth = clamp((zu + rc.fogp.w + rc.fogp.z * zsl) * pc.surf.z, 0.0, 1.0);\n"
    "#endif\n"
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

/* SET_POLY_OFFSET_FILL_ENABLE with a non-zero offset, onto a depth target. */
static int poly_offset_on(const Nv2aRenderState *rs, int has_depth)
{
    return has_depth && rs->poly_offset_fill
        && (rs->poly_offset_factor != 0.0f || rs->poly_offset_bias != 0.0f);
}

/* The Vulkan vertex format reading an NV097 attribute as the CPU's fetch_attr
 * does, or VK_FORMAT_UNDEFINED when there is none this device can fetch. */
static VkFormat raw_format(uint32_t type, uint32_t size)
{
    static const VkFormat f2[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                    VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat f1[4] = { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
                                    VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
    static const VkFormat f5[4] = { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                    VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    static const VkFormat f4[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                    VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static int8_t ok[8][5];                     /* 0 unknown, 1 yes, -1 no */
    VkFormat f;

    if (size < 1 || size > 4)
        return VK_FORMAT_UNDEFINED;
    switch (type) {
    case 0: f = size == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED; break;
    case 1: f = f1[size - 1]; break;
    case 2: f = f2[size - 1]; break;
    case 4: f = f4[size - 1]; break;
    case 5: f = f5[size - 1]; break;
    case 6: f = VK_FORMAT_R32_UINT; break;          /* decoded by the shader */
    default: f = VK_FORMAT_UNDEFINED; break;
    }
    if (f == VK_FORMAT_UNDEFINED)
        return f;
    if (!ok[type][size]) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(B.c->phys, f, &fp);
        ok[type][size] = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : -1;
    }
    return ok[type][size] > 0 ? f : VK_FORMAT_UNDEFINED;
}

/* The fragment shader for a set-up: generated once, then cached. NULL
 * (use the uber-shader) when generation or compilation fails. */
static VkShaderModule fs_get(const uint32_t *spec, int zofs)
{
    static char *buf;
    uint32_t h = 2166136261u;
    FsProg *p;
    int i;

    for (i = 0; i < SPEC_N; i++)
        h = (h ^ spec[i]) * 16777619u;
    h ^= (uint32_t)zofs;
    for (i = 0; i < B.nfsp; i++)
        if (B.fsp[i].hash == h && B.fsp[i].zofs == zofs
            && !memcmp(B.fsp[i].spec, spec, sizeof B.fsp[i].spec))
            return B.fsp[i].mod;
    if (B.nfsp >= VKB_MAX_FS)
        return VK_NULL_HANDLE;
    if (!buf && !(buf = (char *)malloc(256u << 10)))
        return VK_NULL_HANDLE;
    p = &B.fsp[B.nfsp++];
    memcpy(p->spec, spec, sizeof p->spec);
    p->hash = h;
    p->zofs = zofs;
    p->mod = VK_NULL_HANDLE;
    if (vk_psh_gen(spec, zofs, buf, 256u << 10))
        p->mod = make_module(buf, 1);
    if (!p->mod) {
        static unsigned said;
        if (said++ < 16)
            fprintf(stderr, "[VKB] fragment shader generation failed (prog %05X ctl %08X)\n",
                    spec[33], spec[32]);
        if (getenv("RECOMP_VK_PSH_DUMP")) {
            FILE *fp = fopen("psh_failed.glsl", "wb");
            if (fp) { fputs(buf, fp); fclose(fp); }
        }
    }
    return p->mod;
}

static VkPipeline get_pipeline(const Nv2aRenderState *rs, int has_depth)
{
    uint64_t k0, k1, k2, k3 = s_vsh ? (s_vsh->id | (s_vsh->layout << 16)) : 0;
    uint32_t hsh, specv[SPEC_N];
    VkSpecializationMapEntry sme[SPEC_N];
    VkSpecializationInfo spi;
    int i;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vb = { 0, sizeof(Nv2aVertexXf), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputBindingDescription vbs[16];
    VkVertexInputAttributeDescription va[16];
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
       | ((uint64_t)(rs->cull_enable & 1) << 56) | ((uint64_t)(rs->stencil_enable & 1) << 57)
       | ((uint64_t)poly_offset_on(rs, has_depth) << 58);
    k1 = (uint64_t)rs->color_mask | ((uint64_t)(rs->cull_face & 0xFFFF) << 32)
       | ((uint64_t)(rs->front_face & 0xFFFF) << 48);
    k2 = (uint64_t)(rs->stencil_func & 0xF) | ((uint64_t)(rs->stencil_func_mask & 0xFF) << 4)
       | ((uint64_t)(rs->stencil_write_mask & 0xFF) << 12)
       | ((uint64_t)(rs->stencil_fail & 0xFFFF) << 20) | ((uint64_t)(rs->stencil_zfail & 0xFFFF) << 36)
       | ((uint64_t)(rs->stencil_zpass & 0xFFF) << 52);
    {
        static const uint32_t zero[SPEC_N];
        const uint32_t *sp = s_spec ? s_spec : zero;
        uint32_t h = (uint32_t)(k0 ^ (k0 >> 32) ^ k1 * 31 ^ (k1 >> 32) ^ k2 * 131 ^ (k2 >> 32)
                                ^ k3 * 7919);
        for (i = 0; i < SPEC_N; i++)
            h = (h ^ sp[i]) * 16777619u;
        hsh = h;
        for (i = 0; i < B.npipes; i++)
            if (B.pipes[i].hash == h && B.pipes[i].key[0] == k0 && B.pipes[i].key[1] == k1
                && B.pipes[i].key[2] == k2 && B.pipes[i].key[3] == k3
                && !memcmp(B.pipes[i].spec, sp, sizeof B.pipes[i].spec))
                return B.pipes[i].pipe;
        memcpy(specv, sp, sizeof specv);
    }
    if (B.npipes >= VKB_MAX_PIPELINES)
        return B.pipes[0].pipe;

    memset(stages, 0, sizeof stages);
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s_vsh ? s_vsh->prog->mod : B.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ((k0 >> 58) & 1) ? B.fs_zofs : B.fs;
    {
        /* A shader generated for this set-up, unless a debug mode needs
         * the uber-shader (RECOMP_VK_FS, RECOMP_VK_UBER=1). */
        static int uber = -1;
        if (uber < 0)
            uber = getenv("RECOMP_VK_UBER") || getenv("RECOMP_VK_FS") || getenv("RECOMP_VK_FS_TRIVIAL");
        if (!uber) {
            VkShaderModule m = fs_get(specv, (int)((k0 >> 58) & 1));
            if (m)
                stages[1].module = m;
        }
    }
    stages[1].pName = "main";
    for (i = 0; i < SPEC_N; i++) {
        sme[i].constantID = (uint32_t)i;
        sme[i].offset = (uint32_t)(i * 4);
        sme[i].size = 4;
    }
    spi.mapEntryCount = SPEC_N;
    spi.pMapEntries = sme;
    spi.dataSize = sizeof specv;
    spi.pData = specv;
    stages[1].pSpecializationInfo = &spi;
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
    if (s_vsh && s_vsh->b && s_vsh->b->raw) {
        /* The guest's own streams: one binding each, attributes decoded
         * by the vertex input. */
        const Nv2aBatchVsh *rb = s_vsh->b;
        for (i = 0; i < (int)rb->nstreams; i++) {
            vbs[i].binding = (uint32_t)i;
            vbs[i].stride = rb->stream[i].stride;
            vbs[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        }
        for (i = 0; i < (int)rb->nattrs; i++) {
            va[i].location = rb->attr[i].reg;
            va[i].binding = rb->attr[i].stream;
            va[i].format = raw_format(rb->attr[i].type, rb->attr[i].size);
            va[i].offset = rb->attr[i].offset;
        }
        vi.vertexBindingDescriptionCount = rb->nstreams;
        vi.pVertexBindingDescriptions = vbs;
        vi.vertexAttributeDescriptionCount = rb->nattrs;
    } else if (s_vsh) {
        /* The program's inputs, packed in register order. */
        vb.stride = s_vsh->ninputs * 16;
        for (i = 0; i < (int)s_vsh->ninputs; i++) {
            va[i].location = s_vsh->regs[i];
            va[i].binding = 0;
            va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            va[i].offset = (uint32_t)(i * 16);
        }
        vi.vertexAttributeDescriptionCount = s_vsh->ninputs;
    }
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
    B.pipes[B.npipes].key[3] = k3;
    memcpy(B.pipes[B.npipes].spec, specv, sizeof specv);
    B.pipes[B.npipes].hash = hsh;
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
    VkDescriptorSetLayoutBinding bind[3] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        /* draw_vsh: the vertex program's constants (and the AA scale) */
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL } };
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
    {
        /* Two timestamps per frame slot: GPU time per submission. */
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = VKB_FRAMES * 2;
        if (B.c->props.limits.timestampComputeAndGraphics
            && vkCreateQueryPool(B.dev, &qi, NULL, &s_qpool) != VK_SUCCESS)
            s_qpool = VK_NULL_HANDLE;
    }

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
        if (!make_buffer(VKB_VBUF_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         &B.f[i].vbuf, &B.f[i].vmem, (void **)&B.f[i].vmap))
            return 0;
        if (!make_buffer(VKB_UBUF_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         &B.f[i].ubuf, &B.f[i].umem, (void **)&B.f[i].umap))
            return 0;
    }

    dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dli.bindingCount = 3;
    dli.pBindings = bind;
    vkCreateDescriptorSetLayout(B.dev, &dli, NULL, &B.dsl);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &B.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(B.dev, &pli, NULL, &B.layout);

    B.vs = make_module(VS_SRC, 0);
    B.fs = make_module(FS_SRC, 1);
    if (getenv("RECOMP_VK_FS_TRIVIAL")) {
        /* Diagnostic: the vertex colour only -- fill cost without the
         * pixel shader's work. */
        B.fs = make_module("#version 450\n"
                           "layout(location=0) in vec4 vD0;\n"
                           "layout(location=0) out vec4 outColor;\n"
                           "void main() { outColor = vD0; }\n", 1);
    }
    {
        /* The same shader with polygon offset (writes gl_FragDepth). */
        const char *ver = "#version 450\n";
        size_t n = strlen(FS_SRC) + 32;
        char *src = (char *)malloc(n);
        if (src && !strncmp(FS_SRC, ver, strlen(ver))) {
            snprintf(src, n, "%s#define ZOFS\n%s", ver, FS_SRC + strlen(ver));
            B.fs_zofs = make_module(src, 1);
        }
        free(src);
    }
    if (!B.vs || !B.fs || !B.fs_zofs) {
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
    float cmin[4][2];
    uint32_t shadow_mask = 0;
    float shadow_max[4] = { 1, 1, 1, 1 };
    VkDescriptorBufferInfo ubi, vbi;
    VkWriteDescriptorSet w2[3];
    Frame *f;
    VkCommandBuffer cmd;
    VkPipeline pipe;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    PushConsts pc;
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off;
    /* draw_vsh: program inputs then the index list; else transformed
     * vertices. Debug output below reads v[0]: a zero vertex then. */
    static const Nv2aVertexXf zero_v;
    uint32_t vbytes = s_vsh ? s_vsh->nverts * s_vsh->ninputs * 16u
                            : count * (uint32_t)sizeof(Nv2aVertexXf);
    const Nv2aBatchVsh *rawb = (s_vsh && s_vsh->b && s_vsh->b->raw) ? s_vsh->b : NULL;
    uint32_t ibytes = s_vsh ? s_vsh->nindices * 4u : 0;
    if (rawb) {
        uint32_t k;
        vbytes = 0;
        for (k = 0; k < rawb->nstreams; k++)
            vbytes += (rawb->stream[k].bytes + 255u) & ~255u;
    }
    uint32_t bytes = ((vbytes + 255u) & ~255u) + ibytes;
    uint32_t ubytes = 512u + (s_vsh ? VSH_UBO_BYTES : 0u);
    int st;

    if (!count || !init())
        return;
    if (!v)
        v = &zero_v;
    begin_frame();
    if (bytes > VKB_VBUF_BYTES)
        return;
    if (cur()->vused + bytes > VKB_VBUF_BYTES
        || cur()->uused + ubytes > VKB_UBUF_BYTES) {
        submit_frame();
        begin_frame();
    }
    memset(&pc, 0, sizeof pc);
    memset(cmin, 0, sizeof cmin);
    /* Textures first: sampling a surface may end the current rendering. */
    {
        /* RECOMP_VK_PREGRADE=n: dump every surface right before the n-th
         * and (n+1)-th colour-grade pass (stage 1 DEPENDENT_AR). */
        static int want = -2, seen;
        if (want == -2) {
            const char *e = getenv("RECOMP_VK_PREGRADE");
            want = e ? atoi(e) : -1;
        }
        if (want > 0 && px && ((px->stage_program >> 5) & 31) == 15) {
            seen++;
            if (seen == want || seen == want + 1) {
                end_rendering();
                dump_surfaces(seen == want ? "pg0_" : "pg1_", 0);
                begin_frame();                 /* the dump submitted ours */
                B.rt_color = NULL;
                fprintf(stderr, "[VKB] pre-grade dump %d, grade target %08X reads %08X\n",
                        seen, sf->color_va, stages ? stages[0].t.offset : 0);
            }
        }
    }
    B.next_target = va_key(sf->color_va);
    B.next_zeta = rs->zeta_va ? va_key(rs->zeta_va) : 0;
    B.next_target_w = sf->pitch / (sf->bytes_per_pixel ? sf->bytes_per_pixel : 4);
    if (B.next_target_w < sf->width) B.next_target_w = sf->width;
    for (st = 0; st < 4; st++) {
        VkImageView view;
        s_view_w = s_view_h = 0;
        s_view_depth = 0;
        view = stages ? texture_view(&stages[st]) : NULL;
        if (stages && stages[st].enabled && is_depth_format(stages[st].t.color)) {
            static unsigned said;
            if (said++ < 40)
                fprintf(stderr, "[VKB] shadow stage %d: tex %08X %ux%u fmt %02X norm %u func %u "
                        "-> %s %ux%u, v0 tc %.3f %.3f %.3f %.3f\n",
                        st, stages[st].t.offset, stages[st].t.width, stages[st].t.height,
                        stages[st].t.color, stages[st].normalised, px ? px->shadow_func : 99,
                        view ? (s_view_depth ? "depth" : "COLOUR?") : "NONE(white)",
                        s_view_w, s_view_h,
                        v[0].tex[st][0], v[0].tex[st][1],
                        v[0].tex[st][2], v[0].tex[st][3]);
        }
        if (view && s_view_depth) {
            shadow_mask |= 1u << st;
            shadow_max[st] = (stages[st].t.color == 0x2C || stages[st].t.color == 0x2D
                              || stages[st].t.color == 0x30 || stages[st].t.color == 0x31)
                           ? 65535.0f : 16777215.0f;
        }
        ii[st].sampler = stages ? get_sampler(&stages[st]) : B.samplers[0];
        ii[st].imageView = view ? view : B.white_view;
        ii[st].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (view) {
            pc.texscale[st][2] = 1.0f;
            if (stages && ((stages[0].t.offset ^ 0) , ((px ? px->stage_program : 0) >> 5 & 31) == 15)) {
                static unsigned said;
                if (said++ < 12)
                    fprintf(stderr, "[VKB] grade stage %d: tex %08X %ux%u fmt %02X norm %u -> image %ux%u\n",
                            st, stages[st].t.offset, stages[st].t.width, stages[st].t.height,
                            stages[st].t.color, stages[st].normalised, s_view_w, s_view_h);
            }
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
                if (s_view_w && (stages[st].t.width < s_view_w
                                 || stages[st].t.height < s_view_h)) {
                    cmin[st][0] = 0.5f / iw;
                    cmin[st][1] = 0.5f / ih;
                    /* Half a texel inside the texture's own edge. */
                    pc.clampmax[st >> 1][(st & 1) * 2 + 0] =
                        ((float)stages[st].t.width - 0.5f) / iw;
                    pc.clampmax[st >> 1][(st & 1) * 2 + 1] =
                        ((float)stages[st].t.height - 0.5f) / ih;
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
    if (rs->zeta_va && (rs->zeta_va & 0x0FFFFFFFu) == 0x00A17000u) {
        /* Shadow-map renders: the depth format they write decides the
         * scale the compare must use. */
        static unsigned said;
        if (said++ < 12)
            fprintf(stderr, "[VKB] shadow-map render: colour %08X zeta %08X fmt %u zmax %.0f "
                    "%ux%u, v0 z %.3f w %.3f, poly offset %u factor %g bias %g\n",
                    sf->color_va, rs->zeta_va,
                    px ? px->zeta_format : 99, zmax, sf->width, sf->height,
                    v[0].pos[2], v[0].pos[3], rs->poly_offset_fill,
                    rs->poly_offset_factor, rs->poly_offset_bias);
    }
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
            for (k = 0; !s_vsh && k < count && k < 6; k++)
                fprintf(stderr, "[VKB]   v%u pos %.2f %.2f  t0 %.3f %.3f %.3f %.3f  t1 %.3f %.3f %.3f %.3f"
                        "  t2 %.3f %.3f  t3 %.3f %.3f\n", k, v[k].pos[0], v[k].pos[1],
                        v[k].tex[0][0], v[k].tex[0][1], v[k].tex[0][2], v[k].tex[0][3],
                        v[k].tex[1][0], v[k].tex[1][1], v[k].tex[1][2], v[k].tex[1][3],
                        v[k].tex[2][0], v[k].tex[2][1], v[k].tex[3][0], v[k].tex[3][1]);
        }
    }
    f = cur();
    cmd = f->cmd;
    {
        /* The specialization words (FS_SRC S0..S43), from the pixel state. */
        static uint32_t spec[SPEC_N];
        int k;
        memset(spec, 0, sizeof spec);
        if (px) {
            memcpy(spec + 0, px->color_icw, 32);
            memcpy(spec + 8, px->alpha_icw, 32);
            memcpy(spec + 16, px->color_ocw, 32);
            memcpy(spec + 24, px->alpha_ocw, 32);
            spec[32] = px->control;
            spec[33] = px->stage_program;
            spec[34] = px->other_stage_input;
            spec[35] = px->rc_seen;
            spec[36] = px->final0;
            spec[37] = px->final1;
            spec[39] = px->shadow_func;
            spec[41] = px->clip_plane_mode;
            spec[42] = px->fog_enable;
            spec[43] = px->fog_mode;
        }
        spec[38] = shadow_mask;
        spec[44] = rs->alpha_test_enable ? 1u : 0u;
        spec[45] = rs->alpha_test_enable ? rs->alpha_func : 0u;
        spec[46] = rs->alpha_test_enable ? (rs->alpha_ref & 0xFF) : 0u;
        for (k = 0; stages && k < 4; k++) {
            if (!stages[k].enabled || !(stages[k].format_word & 0x8u))
                continue;
            if ((stages[k].t.addr_u & 7) == 4) spec[40] |= 1u << (k * 2);
            if ((stages[k].t.addr_v & 7) == 4) spec[40] |= 2u << (k * 2);
        }
        s_spec = spec;
        pipe = get_pipeline(rs, B.rt_depth != NULL);
        s_spec = NULL;
    }
    if (!pipe)
        return;

    off = f->vused;
    if (rawb) {
        uint32_t k, at = 0;
        for (k = 0; k < rawb->nstreams; k++) {
            memcpy(f->vmap + f->vused + at, rawb->stream[k].data, rawb->stream[k].bytes);
            at += (rawb->stream[k].bytes + 255u) & ~255u;
        }
        memcpy(f->vmap + f->vused + ((vbytes + 255u) & ~255u), s_vsh->indices, ibytes);
    } else if (s_vsh) {
        memcpy(f->vmap + f->vused, s_vsh->inputs, vbytes);
        memcpy(f->vmap + f->vused + ((vbytes + 255u) & ~255u), s_vsh->indices, ibytes);
    } else {
        memcpy(f->vmap + f->vused, v, vbytes);
    }
    f->vused += (bytes + 255u) & ~255u;

    memset(&ubo, 0, sizeof ubo);
    uint32_t px_state_sp = px ? px->stage_program : 0, px_state_ctl = px ? px->control : 0;
    uint32_t px_state_f0 = px ? px->final0 : 0, px_state_f1 = px ? px->final1 : 0;
    const Nv2aPixelState *px_state = px;     /* the probe block shadows px */
    uint32_t px_state_seen = px ? px->rc_seen : 0;
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
        ubo.fogp[2] = rs->poly_offset_factor;   /* read only by the ZOFS shader */
        ubo.fogp[3] = rs->poly_offset_bias;
    }
    {
        int k;
        for (k = 0; k < 4; k++) {
            ubo.clampmin[k >> 1][(k & 1) * 2 + 0] = cmin[k][0];
            ubo.clampmin[k >> 1][(k & 1) * 2 + 1] = cmin[k][1];
        }
        /* Border colour: address mode 4 with the format word's BORDER_SOURCE
         * bit (3) set to "colour" (else the texels carry the border). */
        for (k = 0; stages && k < 4; k++) {
            if (!stages[k].enabled || !(stages[k].format_word & 0x8u))
                continue;
            if ((stages[k].t.addr_u & 7) == 4) ubo.bmask[0] |= 1u << (k * 2);
            if ((stages[k].t.addr_v & 7) == 4) ubo.bmask[0] |= 2u << (k * 2);
            ubo.border[k] = stages[k].border_color;
        }
    }
    ubo.shadow[0] = shadow_mask;
    ubo.shadow[1] = px ? px->shadow_func : 0;
    {
        /* RECOMP_VK_SHADOW=0|1: force every shadow-map compare to fail or
         * pass (diagnostic: is the compare what darkens the ground?). */
        static int force = -2;
        if (force == -2) {
            const char *e = getenv("RECOMP_VK_SHADOW");
            force = e ? atoi(e) + 1 : 0;
        }
        ubo.shadow[2] = (uint32_t)force;
    }
    memcpy(ubo.shadowmax, shadow_max, sizeof shadow_max);
    memcpy(f->umap + f->uused, &ubo, sizeof ubo);
    ubi.buffer = f->ubuf;
    ubi.offset = f->uused;
    ubi.range = sizeof ubo;
    f->uused += 512u;    /* sizeof(RcUbo) <= 512, offset alignment */
    if (s_vsh) {
        /* c[0..191] then the anti-aliasing scale (std140: vec4s). */
        float aa[4] = { s_vsh->aa[0], s_vsh->aa[1], 0.0f, 0.0f };
        memcpy(f->umap + f->uused, s_vsh->constants, 192 * 16);
        memcpy(f->umap + f->uused + 192 * 16, aa, sizeof aa);
        if (rawb)
            memcpy(f->umap + f->uused + 192 * 16 + 16, rawb->imm, sizeof rawb->imm);
        vbi.buffer = f->ubuf;
        vbi.offset = f->uused;
        vbi.range = 192 * 16 + 16 + 256;
        f->uused += VSH_UBO_BYTES;
    }

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
                : !strcmp(e, "t0") ? 3 : !strcmp(e, "noalpha") ? 4
                : !strcmp(e, "gradeAR") ? 5 : !strcmp(e, "gradeT0") ? 6
                : !strcmp(e, "fog") ? 7 : !strcmp(e, "nofog") ? 8
                : !strncmp(e, "st", 2) ? 10 + atoi(e + 2)
                : !strcmp(e, "dtex0") ? 20 : !strcmp(e, "dd0") ? 21
                : !strcmp(e, "shd") ? 30 : 0;
        }
        /* Not on full-screen quads: the post-process passes would paint
         * the debug output over the whole scene. */
        pc.alpha[3] = (dbg >= 2 && dbg != 5 && dbg != 6 && count <= 6) ? 0 : dbg;
        /* st*, dtex0, dd0: only on the modulate (DST_COLOR/SRC_COLOR) passes,
         * whose output the probe can turn back into the shader's: out =
         * 2 * src * dst. */
        if (dbg >= 10 && dbg < 30 && !(rs->blend_enable && rs->blend_src == 0x306))
            pc.alpha[3] = 0;
        if (dbg == 30 && !(rs->blend_enable && rs->blend_dst == 0x301))
            pc.alpha[3] = 0;
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
    if (s_vsh) {
        w2[2] = w2[1];
        w2[2].dstBinding = 2;
        w2[2].pBufferInfo = &vbi;
    }
    pfnPushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.layout, 0, s_vsh ? 3 : 2, w2);
    vkCmdPushConstants(cmd, B.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof pc, &pc);
    vp.x = 0;
    vp.y = 0;
    vp.width = (float)B.rt_color->w;
    vp.height = (float)B.rt_color->h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    /* Scissor = the surface clip box, origin included (xemu draw.c). */
    {
        uint32_t x0 = sf->clip_x < B.rt_color->w ? sf->clip_x : B.rt_color->w;
        uint32_t y0 = sf->clip_y < B.rt_color->h ? sf->clip_y : B.rt_color->h;
        uint32_t x1 = sf->clip_x + sf->width, y1 = sf->clip_y + sf->height;
        if (x1 > B.rt_color->w) x1 = B.rt_color->w;
        if (y1 > B.rt_color->h) y1 = B.rt_color->h;
        sc.offset.x = (int32_t)x0;
        sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0;
        sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    {
        /* RECOMP_VK_TINY=1: a 1x1 scissor -- the frame without its pixel
         * work, to tell vertex/draw cost from fill cost (diagnostic). */
        static int tiny = -1;
        if (tiny < 0) tiny = getenv("RECOMP_VK_TINY") != NULL;
        if (tiny) { sc.extent.width = sc.extent.width ? 1 : 0; sc.extent.height = sc.extent.height ? 1 : 0; }
    }
    vkCmdSetScissor(cmd, 0, 1, &sc);
    {
        float bc[4] = { ((rs->blend_color >> 16) & 0xFF) / 255.0f,
                        ((rs->blend_color >> 8) & 0xFF) / 255.0f,
                        (rs->blend_color & 0xFF) / 255.0f,
                        ((rs->blend_color >> 24) & 0xFF) / 255.0f };
        vkCmdSetBlendConstants(cmd, bc);
    }
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_ref & 0xFF);
    if (rawb) {
        VkBuffer bufs[16];
        VkDeviceSize offs[16];
        uint32_t k, at = 0;
        for (k = 0; k < rawb->nstreams; k++) {
            bufs[k] = f->vbuf;
            offs[k] = off + at;
            at += (rawb->stream[k].bytes + 255u) & ~255u;
        }
        if (rawb->nstreams)
            vkCmdBindVertexBuffers(cmd, 0, rawb->nstreams, bufs, offs);
    } else {
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->vbuf, &off);
    }
    s_stat_draws++;
    if (s_vsh) {
        vkCmdBindIndexBuffer(cmd, f->vbuf, off + ((vbytes + 255u) & ~255u), VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, s_vsh->nindices, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd, count, 1, 0, 0);
    }
    {
        /* RECOMP_VK_SYNC_EACH=1: end rendering + full barrier after every
         * draw; =2: also submit and wait. Hazard hunting. */
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("RECOMP_VK_SYNC_EACH"); mode = e ? atoi(e) : 0; }
        if (mode >= 1) {
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            end_rendering();
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            B.rt_color = NULL;
        }
        if (mode >= 2) {
            submit_frame();
            vkDeviceWaitIdle(B.dev);
            begin_frame();
        }
    }
    {
        /* RECOMP_VK_PROBE=x,y (+ flag file probe.flag): for one frame, read
         * pixel (x,y) of the colour target after every draw and log it with
         * the draw's state -- which draw turns a pixel wrong. */
        /* RECOMP_VK_PROBE=x,y[;x,y...]: up to PROBE_PTS points at once. */
        static int px = -2, py, active, done, idx, npts;
        static int ptx[PROBE_PTS], pty[PROBE_PTS];
        if (px == -2) {
            const char *e = getenv("RECOMP_VK_PROBE");
            px = -1;
            while (e && npts < PROBE_PTS && sscanf(e, "%d,%d", &ptx[npts], &pty[npts]) == 2) {
                npts++;
                e = strchr(e, ';');
                if (e) e++;
            }
            if (npts) { px = ptx[0]; py = pty[0]; }
        }
        if (px >= 0 && !done) {
            /* Start at a frame boundary (the next present), so the log
             * covers one whole frame from its first draw. */
            if (!active && !s_probe_armed
                && GetFileAttributesA("probe.flag") != INVALID_FILE_ATTRIBUTES)
                s_probe_armed = 1;
            if (!active && s_probe_armed == 2) {
                active = 1;
                s_probe_armed = 3;
                fprintf(stderr, "[PROBE] frame start\n");
            }
            if (active && B.rt_color && idx < PROBE_MAX) {
                /* No CPU wait per draw (a stalled frame makes the title
                 * switch to its loading logo): copy a PROBE_R square around
                 * (x,y) into this draw's slot and read them all at the end. */
                Surface *t = B.rt_color;
                ProbeRec *r = &s_probe_rec[idx];
                VkBufferImageCopy c;
                int p;
                if (!s_probe_buf && !make_buffer((VkDeviceSize)PROBE_MAX * PROBE_PTS * PROBE_R * PROBE_R * 4,
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 &s_probe_buf, &s_probe_mem, (void **)&s_probe_map))
                    done = 1;
                if (!done) {
                    r->ok = t->w >= PROBE_R && t->h >= PROBE_R;
                    r->surf = t->va; r->count = count;
                    r->blend = rs->blend_enable; r->bsrc = rs->blend_src; r->bdst = rs->blend_dst;
                    r->cmask = rs->color_mask;
                    r->sp = px_state_sp; r->ctl = px_state_ctl; r->f0 = px_state_f0; r->f1 = px_state_f1;
                    r->seen = px_state_seen;
                    if (px_state) {
                        memcpy(r->cicw, px_state->color_icw, 32); memcpy(r->aicw, px_state->alpha_icw, 32);
                        memcpy(r->cocw, px_state->color_ocw, 32); memcpy(r->aocw, px_state->alpha_ocw, 32);
                        memcpy(r->k0, px_state->factor0, 32); memcpy(r->k1, px_state->factor1, 32);
                    }
                    memcpy(r->d0, v[0].d0, sizeof r->d0);
                    memcpy(r->tc0, v[0].tex[0], sizeof r->tc0);
                    r->t0 = stages ? stages[0].t.offset : 0; r->t0f = stages ? stages[0].t.color : 0;
                    r->t1 = stages ? stages[1].t.offset : 0; r->t1f = stages ? stages[1].t.color : 0;
                    if (r->ok) {
                        to_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        for (p = 0; p < npts; p++) {
                            int x0 = ptx[p] - PROBE_R / 2, y0 = pty[p] - PROBE_R / 2;
                            if (x0 < 0) x0 = 0;
                            if (y0 < 0) y0 = 0;
                            if (x0 + PROBE_R > (int)t->w) x0 = (int)t->w - PROBE_R;
                            if (y0 + PROBE_R > (int)t->h) y0 = (int)t->h - PROBE_R;
                            memset(&c, 0, sizeof c);
                            c.bufferOffset = ((VkDeviceSize)idx * PROBE_PTS + p) * PROBE_R * PROBE_R * 4;
                            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            c.imageSubresource.layerCount = 1;
                            c.imageOffset.x = x0; c.imageOffset.y = y0;
                            c.imageExtent.width = PROBE_R; c.imageExtent.height = PROBE_R;
                            c.imageExtent.depth = 1;
                            vkCmdCopyImageToBuffer(cur()->cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   s_probe_buf, 1, &c);
                        }
                        B.rt_color = NULL;
                    }
                    idx++;
                }
            }
        }
        /* One whole frame, from the first draw after a present to the next
         * present. */
        if (active && !done && (idx >= PROBE_MAX || s_probe_armed == 4)) {
            int i;
            done = 1;
            end_rendering();
            submit_frame();
            vkDeviceWaitIdle(B.dev);
            begin_frame();
            B.rt_color = NULL;
            fprintf(stderr, "[PROBE] done after %d draws\n", idx);
            for (i = 0; i < idx * npts; i++) {
                const ProbeRec *r = &s_probe_rec[i / npts];
                const uint8_t *m = s_probe_map + ((size_t)(i / npts) * PROBE_PTS + i % npts) * PROBE_R * PROBE_R * 4;
                const uint8_t *ce = m + ((PROBE_R / 2) * PROBE_R + PROBE_R / 2) * 4;
                unsigned cyan = 0, dark = 0, k, sum = 0;
                if (!r->ok) continue;
                for (k = 0; k < PROBE_R * PROBE_R; k++) {
                    int bb = m[k*4], gg = m[k*4+1], rr = m[k*4+2];
                    sum += bb + gg + rr;
                    if (rr + gg + bb < 24) dark++;
                    else if (gg + bb > 60 && 2 * rr < gg && 2 * rr < bb) cyan++;
                }
                fprintf(stderr, "[PROBE] #%d p%d (%d,%d) surf %08X n %u | RGBA %3u %3u %3u %3u | sq dark %u cyan %u avg %u"
                        " | blend %u %X/%X cmask %08X | prog %05X ctl %08X fin %08X %08X | t0 %08X fmt %02X t1 %08X fmt %02X\n",
                        i / npts, i % npts, ptx[i % npts], pty[i % npts], r->surf, r->count, ce[2], ce[1], ce[0], ce[3], dark, cyan,
                        sum / (PROBE_R * PROBE_R * 3), r->blend, r->bsrc, r->bdst, r->cmask,
                        r->sp, r->ctl, r->f0, r->f1, r->t0, r->t0f, r->t1, r->t1f);
            }
            /* Full combiner state of the modulate passes (DST_COLOR/SRC_COLOR),
             * with vertex 0's colour and texture coordinate. */
            for (i = 0; i < idx; i++) {
                const ProbeRec *r = &s_probe_rec[i];
                int s;
                fprintf(stderr, "[PROBE] rc #%d seen %u ctl %08X d0 %.3f %.3f %.3f %.3f tc0 %.3f %.3f %.3f %.3f\n",
                        i, r->seen, r->ctl, r->d0[0], r->d0[1], r->d0[2], r->d0[3],
                        r->tc0[0], r->tc0[1], r->tc0[2], r->tc0[3]);
                for (s = 0; s < (int)(r->ctl & 7); s++)
                    fprintf(stderr, "[PROBE] rc #%d  %d: cI %08X aI %08X cO %08X aO %08X f0 %08X f1 %08X\n",
                            i, s, r->cicw[s], r->aicw[s], r->cocw[s], r->aocw[s], r->k0[s], r->k1[s]);
            }
            fflush(stderr);
            {
                /* Re-arm: each new probe.flag takes another frame, so two
                 * captures of one view (before / after something changes)
                 * can be compared. Surfaces go to pr<N>_. */
                static unsigned shot;
                char pre[16];
                snprintf(pre, sizeof pre, "pr%u_", shot);
                dump_surfaces(pre, 0);
                fprintf(stderr, "[PROBE] capture %u written (%s*)\n", shot, pre);
                shot++;
                DeleteFileA("probe.flag");
                active = 0; done = 0; idx = 0; s_probe_armed = 0;
            }
            begin_frame();
            B.rt_color = NULL;
        }
    }
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
        /* RECOMP_VK_SKIP_TEX=<va>: drop every draw that samples that
         * address on any stage (diagnostic: what a pass contributes). */
        static uint32_t skip = 1;
        int k;
        if (skip == 1) {
            const char *e = getenv("RECOMP_VK_SKIP_TEX");
            skip = e ? (uint32_t)strtoul(e, NULL, 16) & 0x0FFFFFFFu : 0;
        }
        for (k = 0; skip && k < 4; k++)
            if (b->pixel->stage[k].enabled
                && (b->pixel->stage[k].t.offset & 0x0FFFFFFFu) == skip
                && ((b->pixel->stage_program >> (k * 5)) & 31) != 0)
                return;
    }
    {
        /* RECOMP_VK_SKIP_UNSUP=1: drop draws whose texture shader uses a
         * mode the uber-shader does not implement (cube maps, dot products,
         * bump env) -- diagnostic until those exist. */
        static int on = -1;
        uint32_t sp = b->pixel->stage_program, k;
        if (on < 0) on = getenv("RECOMP_VK_SKIP_UNSUP") != NULL;
        for (k = 0; on && k < 4; k++) {
            uint32_t m = (sp >> (k * 5)) & 31;
            if (m && m != 1 && m != 2 && m != 4 && m != 5 && m != 15 && m != 16)
                return;
        }
    }
    {
        /* RECOMP_VK_RC_DUMP=<prog hex>: the raw combiner registers of the
         * first few draws with that stage program. */
        static int want = -2, said;
        if (want == -2) {
            const char *e = getenv("RECOMP_VK_RC_DUMP");
            want = e ? (int)strtoul(e, NULL, 16) : -1;
        }
        if (want >= 0 && b->pixel->stage_program == (uint32_t)want && said < 8
            && (!getenv("RECOMP_VK_RC_W") || sf->width == (uint32_t)atoi(getenv("RECOMP_VK_RC_W")))
            && (!getenv("RECOMP_VK_RC_T0")
                || ((b->pixel->stage[0].t.offset ^ (uint32_t)strtoul(getenv("RECOMP_VK_RC_T0"), NULL, 16))
                    & 0x0FFFFFFFu) == 0)) {
            const Nv2aPixelState *p = b->pixel;
            int i;
            said++;
            fprintf(stderr, "[RC] prog %05X control %08X final %08X %08X fc %08X %08X blend %u %X/%X\n",
                    p->stage_program, p->control, p->final0, p->final1, p->final_c0, p->final_c1,
                    b->state->blend_enable, b->state->blend_src, b->state->blend_dst);
            for (i = 0; i < 8; i++)
                fprintf(stderr, "[RC]  %d: cI %08X aI %08X cO %08X aO %08X f0 %08X f1 %08X\n", i,
                        p->color_icw[i], p->alpha_icw[i], p->color_ocw[i], p->alpha_ocw[i],
                        p->factor0[i], p->factor1[i]);
            for (i = 0; i < 4; i++)
                fprintf(stderr, "[RC]  stage%d tex %08X %ux%u fmt %02X levels %u addr %u/%u filter %08X norm %u en %u fmtword %08X ctl0 %08X border %08X\n",
                        i, p->stage[i].t.offset, p->stage[i].t.width, p->stage[i].t.height, p->stage[i].t.color,
                        p->stage[i].t.levels, p->stage[i].t.addr_u, p->stage[i].t.addr_v, p->stage[i].t.filter,
                        p->stage[i].normalised, p->stage[i].enabled, p->stage[i].format_word, p->stage[i].control0, p->stage[i].border_color);
            fprintf(stderr, "[RC]  target %08X %ux%u pitch %u, %u verts, zeta %08X fmt %u\n",
                    sf->color_va, sf->width, sf->height, sf->pitch, b->count,
                    b->state->zeta_va, p->zeta_format);
            fprintf(stderr, "[RC]  depth test %u func %X write %u | stencil %u func %X ref %u mask %X"
                    " wmask %X ops %X/%X/%X | alpha test %u %X %u | cmask %08X\n",
                    b->state->depth_test_enable, b->state->depth_func, b->state->depth_write,
                    b->state->stencil_enable, b->state->stencil_func, b->state->stencil_ref,
                    b->state->stencil_func_mask, b->state->stencil_write_mask,
                    b->state->stencil_fail, b->state->stencil_zfail, b->state->stencil_zpass,
                    b->state->alpha_test_enable, b->state->alpha_func, b->state->alpha_ref,
                    b->state->color_mask);
            for (i = 0; b->vertices && i < (int)b->count && i < 6; i++)
                fprintf(stderr, "[RC]  v%d pos %.3f %.3f %.3f %.3f t0 %.3f %.3f %.3f %.3f\n", i,
                        b->vertices[i].pos[0], b->vertices[i].pos[1], b->vertices[i].pos[2],
                        b->vertices[i].pos[3], b->vertices[i].tex[0][0], b->vertices[i].tex[0][1],
                        b->vertices[i].tex[0][2], b->vertices[i].tex[0][3]);
        }
    }
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
    {
        /* Texture-stage modes the uber-shader does not implement, once per
         * stage program. */
        static uint32_t seenp[64]; static int ns;
        uint32_t sp = b->pixel->stage_program, k, bad = 0;
        for (k = 0; k < 4; k++) {
            uint32_t m = (sp >> (k * 5)) & 31;
            if (m && m != 1 && m != 2 && m != 4 && m != 5 && m != 15 && m != 16) bad = 1;
        }
        if (bad) {
            int j;
            for (j = 0; j < ns && seenp[j] != sp; j++) ;
            if (j == ns && ns < 64) {
                seenp[ns++] = sp;
                fprintf(stderr, "[VKB] unsupported stage modes: prog %05X = %u %u %u %u, blend %u %X/%X, surf %08X\n",
                        sp, sp & 31, (sp >> 5) & 31, (sp >> 10) & 31, (sp >> 15) & 31,
                        b->state->blend_enable, b->state->blend_src, b->state->blend_dst, sf->color_va);
            }
        }
    }
    {
        /* Colour formats seen per surface, once each. */
        static uint32_t seen[64]; static int nseen;
        uint32_t key = (sf->color_va & 0x0FFFFFFFu) | (b->pixel->color_format << 28);
        int k;
        for (k = 0; k < nseen && seen[k] != key; k++) ;
        if (k == nseen && nseen < 64) {
            seen[nseen++] = key;
            fprintf(stderr, "[VKB] surface %08X colour format %u\n", sf->color_va,
                    b->pixel->color_format);
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
    {
        /* Clears that touch the shadow-map address, logged for a while. */
        static unsigned said;
        if (((sf->color_va & 0x0FFFFFFFu) == 0x00A17000u
             || (rs->zeta_va & 0x0FFFFFFFu) == 0x00A17000u) && said++ < 40)
            fprintf(stderr, "[VKB] clear flags %02X colour %08X %ux%u zeta %08X fmt %u zs %08X argb %08X\n",
                    flags, sf->color_va, sf->width, sf->height, rs->zeta_va,
                    rs->zeta_format, zs, argb);
    }
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
        /* xemu pgraph_get_clear_surface: Z16 keeps the depth in the low 16
         * bits, Z24S8 in the top 24 with stencil below. Reading a Z16 clear
         * as Z24 turned GR2's shadow maps' "far" (0xFFFF) into ~0, so every
         * texel no caster covered compared as shadowed -- the dark ground. */
        /* (Linear in both cases, as draws store z / zmax even for float Z.) */
        if (rs->zeta_format == 1) {
            ca[n].clearValue.depthStencil.depth = (float)(zs & 0xFFFF) / 65535.0f;
            ca[n].clearValue.depthStencil.stencil = 0;
        } else {
            ca[n].clearValue.depthStencil.depth = (float)(zs >> 8) / 16777215.0f;
            ca[n].clearValue.depthStencil.stencil = zs & 0xFF;
        }
        n++;
    }
    if (!n)
        return;
    {
        /* The clear rectangle registers, inclusive, clamped to the image as
         * xemu does. A title that never set them (both zero) gets the clip
         * box. Clearing from (0,0) instead left GR2's weapon slots with the
         * scene's depth, and the weapon models failed the depth test. */
        uint32_t x0 = rs->clear_x0, y0 = rs->clear_y0;
        uint32_t x1 = rs->clear_x1 + 1, y1 = rs->clear_y1 + 1;
        if (!rs->clear_x1 && !rs->clear_y1) {
            x0 = sf->clip_x; y0 = sf->clip_y;
            x1 = sf->clip_x + sf->width; y1 = sf->clip_y + sf->height;
        }
        if (x1 > B.rt_color->w) x1 = B.rt_color->w;
        if (y1 > B.rt_color->h) y1 = B.rt_color->h;
        if (x0 >= x1 || y0 >= y1)
            return;
        rect.rect.offset.x = (int32_t)x0;
        rect.rect.offset.y = (int32_t)y0;
        rect.rect.extent.width = x1 - x0;
        rect.rect.extent.height = y1 - y0;
    }
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
    if (!s) {
        /* Not ours: the executor copies guest memory, so our image of the
         * destination (if any) is now older than memory. */
        d = find_surface(dst_va, 0);
        if (d)
            d->cpu_newer = 1;
        return 0;
    }
    begin_frame();
    d = get_surface(dst_va, 0, dst_pitch / 4 > dx + w ? dst_pitch / 4 : dx + w, dy + h);
    if (!d || d == s)
        return 0;
    d->written = ++B.write_seq;
    d->cpu_newer = 0;
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

        if (!sf->img)
            continue;
        if (!make_buffer((VkDeviceSize)sf->w * sf->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &buf, &mem, (void **)&map))
            return;
        begin_frame();
        to_layout(sf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (sf->depth) {
            /* Depth aspect as grey (0 near .. 255 far), with its range. */
            float *z = (float *)map, lo = 1e30f, hi = -1e30f;
            size_t nfar = 0;
            memset(&c, 0, sizeof c);
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = sf->w; c.imageExtent.height = sf->h; c.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cur()->cmd, sf->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   buf, 1, &c);
            submit_frame();
            vkDeviceWaitIdle(B.dev);
            n = (size_t)sf->w * sf->h;
            for (k = 0; k < n; k++) {
                float v = z[k];
                uint8_t g;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (v >= 0.9999f) nfar++;
                g = (uint8_t)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f);
                map[k * 4] = map[k * 4 + 1] = map[k * 4 + 2] = g; map[k * 4 + 3] = 255;
            }
            snprintf(path, sizeof path, "%s%04u_%08X_w%u_z.bmp", prefix, shot, sf->va, sf->w);
            write_bmp(path, map, sf->w, sf->h);
            fprintf(stderr, "[VKB] depth %08X %ux%u: min %.6f (%.0f) max %.6f (%.0f), %zu of %zu far\n",
                    sf->va, sf->w, sf->h, lo, lo * 65535.0f, hi, hi * 65535.0f, nfar, n);
            vkDestroyBuffer(B.dev, buf, NULL);
            vkFreeMemory(B.dev, mem, NULL);
            continue;
        }
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
        snprintf(path, sizeof path, "%s%04u_%08X_w%u.bmp", prefix, shot, sf->va, sf->w);
        write_bmp(path, map, sf->w, sf->h);
        n = (size_t)sf->w * sf->h;
        for (k = 0; k < n; k++)
            map[k * 4] = map[k * 4 + 1] = map[k * 4 + 2] = map[k * 4 + 3];
        snprintf(path, sizeof path, "%s%04u_%08X_w%u_a.bmp", prefix, shot, sf->va, sf->w);
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
    if (s_probe_armed == 1 || s_probe_armed == 3)
        s_probe_armed++;                       /* 3 -> 4: the frame is done */
    Surface *s;
    VkCommandBuffer cmd;
    VkImageBlit b;
    int o;

    (void)pitch; (void)aa_sx; (void)aa_sy;
    if (B.failed || !init())
        return;
    s = find_surface_w(va, 0, w);
    if (!s)
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
                LARGE_INTEGER fq;
                QueryPerformanceFrequency(&fq);
                fprintf(stderr, "[VKB] %.1f fps (%u pipelines, %d textures) | per frame: "
                        "%.1f submits, %.1f uploads, %.2f ms waiting for the GPU, GPU busy %.2f ms, "
                        "%.0f draws, %.0f passes, %.0f barriers, %.1f copies\n",
                        frames * 1000.0 / (now - t0), B.npipes, B.ntex,
                        (double)s_stat_begins / frames, (double)s_stat_uploads / frames,
                        (double)s_stat_wait * 1000.0 / fq.QuadPart / frames,
                        s_stat_gpu_ms / frames, (double)s_stat_draws / frames,
                        (double)s_stat_passes / frames, (double)s_stat_barriers / frames,
                        (double)s_stat_copies / frames);
                s_stat_wait = 0; s_stat_begins = 0; s_stat_uploads = 0;
                s_stat_gpu_ms = 0; s_stat_draws = 0; s_stat_passes = 0;
                s_stat_barriers = 0; s_stat_copies = 0;
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

/* ---- draw_vsh: vertex programs on the GPU ------------------------------ */

static const char *VSH_OUTPUTS =
    "layout(push_constant) uniform PC { vec4 surf; vec4 texscale[4]; ivec4 alpha; vec4 clampmax[2]; } pc;\n"
    "layout(std140, set = 0, binding = 2) uniform VSHC { vec4 cst[192]; vec4 vaa; vec4 vimm[16]; };\n"
    "layout(location=0) out vec4 vD0;\n"
    "layout(location=1) out vec4 vD1;\n"
    "layout(location=2) out vec4 vFog;\n"
    "layout(location=3) out vec4 vT[4];\n";

/* After the program: what the CPU path does to a transformed vertex
 * (transform_vertex, then VS_SRC), so both paths draw the same thing. */
static const char *VSH_EPILOGUE =
    "  vec4 p = oPos;\n"
    "  if (floatBitsToUint(p.w) == 0u) p.w = 1.0;\n"   /* +0: no divide by w */
    "  p.xy *= vaa.xy;\n"
    "  float w = p.w;\n"
    "  vec2 sp = trunc(p.xy * 16.0) / 16.0;\n"
    "  vec3 ndc = vec3(sp.x / pc.surf.x * 2.0 - 1.0,\n"
    "                  sp.y / pc.surf.y * 2.0 - 1.0,\n"
    "                  p.z * pc.surf.z);\n"
    "  gl_Position = vec4(ndc * w, w);\n"
    "  vD0 = clamp(mix(o[3], vec4(1.0), isnan(o[3])), 0.0, 1.0);\n"
    "  vD1 = clamp(mix(o[4], vec4(1.0), isnan(o[4])), 0.0, 1.0);\n"
    "  vFog = o[5];\n"
    "  vT[0] = o[9]; vT[1] = o[10]; vT[2] = o[11]; vT[3] = o[12];\n";

/* The compiled program for this batch, from the cache or built now. */
static VshProg *vsh_get(const Nv2aBatchVsh *b, uint32_t *id)
{
    uint32_t mask = 0, k, packed = 0, imm = 0;
    size_t cap = 512u << 10, n;
    char *src, *body;
    VshProg *p;
    int i;

    for (k = 0; k < b->ninputs; k++)
        mask |= 1u << b->input_reg[k];
    if (b->raw) {
        imm = b->imm_mask & mask;
        for (k = 0; k < b->nattrs; k++)
            if (b->attr[k].type == 6)
                packed |= 1u << b->attr[k].reg;
    }
    for (i = 0; i < B.nvsh; i++)
        if (B.vsh[i].hash == b->program_hash && B.vsh[i].inputs == mask
            && B.vsh[i].packed == packed && B.vsh[i].imm == imm) {
            *id = (uint32_t)i + 1;
            return B.vsh[i].mod ? &B.vsh[i] : NULL;
        }
    if (B.nvsh >= VKB_MAX_VSH)
        return NULL;
    p = &B.vsh[B.nvsh];
    memset(p, 0, sizeof *p);
    p->hash = b->program_hash;
    p->inputs = mask;
    p->packed = packed;
    p->imm = imm;
    *id = (uint32_t)B.nvsh + 1;
    B.nvsh++;                    /* a failed compile stays cached as NULL */
    src = (char *)malloc(cap);
    body = (char *)malloc(cap);
    if (!src || !body) {
        free(src); free(body);
        return NULL;
    }
    if (!nv2a_vsh_glsl(b->program, b->nslots, b->start, body, cap)) {
        fprintf(stderr, "[VKB] vertex program %016llX: GLSL generation failed\n",
                (unsigned long long)b->program_hash);
        free(src); free(body);
        return NULL;
    }
    n = (size_t)snprintf(src, cap, "#version 450\n");
    for (k = 0; k < 16; k++)
        if ((mask & (1u << k)) && !(imm & (1u << k)))
            n += (size_t)snprintf(src + n, cap - n, "layout(location=%u) in %s in_%u;\n",
                                  k, (packed & (1u << k)) ? "uint" : "vec4", k);
    n += (size_t)snprintf(src + n, cap - n, "%s%s", VSH_OUTPUTS, nv2a_vsh_glsl_prelude());
    n += (size_t)snprintf(src + n, cap - n,
                          "void main() {\n  vec4 v[16];\n"
                          "  for (int k = 0; k < 16; k++) v[k] = vec4(0.0, 0.0, 0.0, 1.0);\n");
    for (k = 0; k < 16; k++) {
        if (!(mask & (1u << k)))
            continue;
        if (imm & (1u << k))
            n += (size_t)snprintf(src + n, cap - n, "  v[%u] = vimm[%u];\n", k, k);
        else if (packed & (1u << k))
            /* 11:11:10 signed normal, as fetch_attr decodes it. */
            n += (size_t)snprintf(src + n, cap - n,
                                  "  v[%u] = vec4(float(int(in_%u << 21u) >> 21) / 1023.0,"
                                  " float(int(in_%u << 10u) >> 21) / 1023.0,"
                                  " float(int(in_%u) >> 22) / 511.0, 1.0);\n", k, k, k, k);
        else
            n += (size_t)snprintf(src + n, cap - n, "  v[%u] = in_%u;\n", k, k);
    }
    n += (size_t)snprintf(src + n, cap - n, "%s%s}\n", body, VSH_EPILOGUE);
    if (n >= cap) {
        free(src); free(body);
        return NULL;
    }
    p->mod = make_module(src, 0);
    {
        static unsigned said;
        if (said++ < 64)
            fprintf(stderr, "[VKB] vertex program %016llX (inputs %04X): %s\n",
                    (unsigned long long)p->hash, mask, p->mod ? "compiled" : "COMPILE FAILED");
        if (!p->mod && getenv("RECOMP_VK_VSH_DUMP")) {
            FILE *fp = fopen("vsh_failed.glsl", "wb");
            if (fp) { fputs(src, fp); fclose(fp); }
        }
    }
    free(src);
    free(body);
    return p->mod ? p : NULL;
}

static int vkb_draw_vsh(const Nv2aSurface *sf, const Nv2aBatchVsh *b)
{
    VshDraw d;
    Nv2aBatchXf x;
    VshProg *p;
    uint32_t id = 0;

    if (B.failed || !init())
        return 0;
    if (b->ninputs > 16 || !b->nindices)
        return 0;
    if (b->raw) {
        uint32_t k;
        for (k = 0; k < b->nattrs; k++)
            if (raw_format(b->attr[k].type, b->attr[k].size) == VK_FORMAT_UNDEFINED)
                return 0;                      /* the CPU fetches this one */
    }
    p = vsh_get(b, &id);
    if (!p)
        return 0;                              /* the CPU path draws it */
    memset(&d, 0, sizeof d);
    d.prog = p;
    d.id = id;
    d.inputs = b->inputs;
    d.nverts = b->nverts;
    d.ninputs = b->ninputs;
    memcpy(d.regs, b->input_reg, sizeof d.regs);
    d.indices = b->indices;
    d.nindices = b->nindices;
    d.constants = b->constants;
    d.aa[0] = b->aa_sx;
    d.aa[1] = b->aa_sy;
    d.b = b;
    if (b->raw) {
        uint64_t h = 1469598103934665603ull;
        uint32_t k;
        for (k = 0; k < b->nstreams; k++)
            h = (h ^ b->stream[k].stride) * 1099511628211ull;
        for (k = 0; k < b->nattrs; k++)
            h = (h ^ ((uint64_t)b->attr[k].reg | (uint64_t)b->attr[k].stream << 8
                      | (uint64_t)b->attr[k].type << 16 | (uint64_t)b->attr[k].size << 24
                      | (uint64_t)b->attr[k].offset << 32)) * 1099511628211ull;
        d.layout = (h ^ (h >> 40)) & 0xFFFFFFFFFFull;
        if (!d.layout) d.layout = 1;
    }
    x.vertices = NULL;
    x.count = b->nindices;
    x.state = b->state;
    x.pixel = b->pixel;
    s_vsh = &d;
    vkb_draw_xf(sf, &x);
    s_vsh = NULL;
    return 1;
}

static const Nv2aBackend s_vk_backend = {
    vkb_clear, vkb_draw, vkb_flip, vkb_draw_xf, vkb_blit, vkb_present, vkb_draw_vsh
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
