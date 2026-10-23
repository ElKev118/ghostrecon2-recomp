/**
 * Ghost Recon 2 port - GLSL to SPIR-V at runtime (glslang), for the shaders
 * the renderer generates from the title's pixel-stage state.
 *
 * Compiled SPIR-V is kept on disk, keyed by a hash of the source, in
 * %LOCALAPPDATA%\GhostRecon2\shadercache: glslang takes milliseconds per
 * shader, and a new effect on screen meant a new shader compiled in the
 * middle of a frame -- a hitch the first time each one appeared, in every
 * session. Now only the first session ever pays for it.
 * RECOMP_SHADER_CACHE=0 turns the cache off.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include "vk_glsl.h"

static int s_inited;

/* Bump when anything that changes the SPIR-V for the same source changes
 * (glslang version, target, options). */
#define SPV_CACHE_SALT "gr2-spv-1|vk1.3|spv1.3|glslang14.3.0"

static uint64_t fnv1a64(const void *p, size_t n, uint64_t h)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) {
        h ^= *b++;
        h *= 0x100000001B3ull;
    }
    return h;
}

const char *vkg_cache_dir(void)
{
    static char dir[MAX_PATH];
    static int done;

    if (!done) {
        const char *off = getenv("RECOMP_SHADER_CACHE");
        const char *base = getenv("LOCALAPPDATA");
        done = 1;
        if ((off && off[0] == '0') || !base)
            return dir;
        snprintf(dir, sizeof dir, "%s\\GhostRecon2", base);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof dir, "%s\\GhostRecon2\\shadercache", base);
        if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            dir[0] = 0;
    }
    return dir;
}

static void cache_path(char *out, size_t n, const char *src, int fragment)
{
    uint64_t h = fnv1a64(SPV_CACHE_SALT, sizeof SPV_CACHE_SALT - 1,
                         0xCBF29CE484222325ull);
    h = fnv1a64(src, strlen(src), h);
    snprintf(out, n, "%s\\%016llx_%c.spv", vkg_cache_dir(),
             (unsigned long long)h, fragment ? 'f' : 'v');
}

static uint32_t *cache_load(const char *path, size_t *out_bytes)
{
    FILE *f = fopen(path, "rb");
    uint32_t *spv = NULL;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 20 && n % 4 == 0 && (spv = (uint32_t *)malloc((size_t)n))) {
        if (fread(spv, 1, (size_t)n, f) != (size_t)n || spv[0] != 0x07230203u) {
            free(spv);                         /* torn or not SPIR-V: recompile */
            spv = NULL;
        } else {
            *out_bytes = (size_t)n;
        }
    }
    fclose(f);
    return spv;
}

static void cache_store(const char *path, const uint32_t *spv, size_t bytes)
{
    char tmp[MAX_PATH + 16];
    FILE *f;

    /* Written aside and renamed, so a crash mid-write leaves no half file. */
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
    if (!(f = fopen(tmp, "wb")))
        return;
    if (fwrite(spv, 1, bytes, f) == bytes) {
        fclose(f);
        if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
            DeleteFileA(tmp);
        return;
    }
    fclose(f);
    DeleteFileA(tmp);
}

/* For the renderer's once-a-second line. */
static volatile LONG s_hits, s_compiles;
static volatile LONG64 s_compile_us;

void vkg_stats(unsigned *hits, unsigned *compiles, double *compile_ms)
{
    *hits = (unsigned)InterlockedExchange(&s_hits, 0);
    *compiles = (unsigned)InterlockedExchange(&s_compiles, 0);
    *compile_ms = (double)InterlockedExchange64(&s_compile_us, 0) / 1000.0;
}

uint32_t *vkg_compile(const char *src, int fragment, size_t *out_bytes)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *prog;
    uint32_t *spv = NULL;
    size_t words;
    char path[MAX_PATH];
    LARGE_INTEGER t0, t1, fq;

    path[0] = 0;
    if (vkg_cache_dir()[0]) {
        cache_path(path, sizeof path, src, fragment);
        if ((spv = cache_load(path, out_bytes))) {
            InterlockedIncrement(&s_hits);
            return spv;
        }
    }
    QueryPerformanceCounter(&t0);

    if (!s_inited) {
        glslang_initialize_process();
        s_inited = 1;
    }
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();

    sh = glslang_shader_create(&in);
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        fprintf(stderr, "[VKG] %s shader failed:\n%s\n%s\n--- source ---\n%s\n",
                fragment ? "fragment" : "vertex",
                glslang_shader_get_info_log(sh),
                glslang_shader_get_info_debug_log(sh), src);
        glslang_shader_delete(sh);
        return NULL;
    }
    prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr, "[VKG] link failed: %s\n", glslang_program_get_info_log(prog));
        glslang_program_delete(prog);
        glslang_shader_delete(sh);
        return NULL;
    }
    glslang_program_SPIRV_generate(prog, in.stage);
    words = glslang_program_SPIRV_get_size(prog);
    spv = (uint32_t *)malloc(words * 4);
    if (spv) {
        glslang_program_SPIRV_get(prog, spv);
        *out_bytes = words * 4;
        if (path[0])
            cache_store(path, spv, words * 4);
    }
    glslang_program_delete(prog);
    glslang_shader_delete(sh);

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    InterlockedIncrement(&s_compiles);
    InterlockedAdd64(&s_compile_us, (t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
    return spv;
}
