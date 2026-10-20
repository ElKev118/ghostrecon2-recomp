/**
 * Ghost Recon 2 port - GLSL to SPIR-V at runtime (glslang), for the shaders
 * the renderer generates from the title's pixel-stage state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include "vk_glsl.h"

static int s_inited;

uint32_t *vkg_compile(const char *src, int fragment, size_t *out_bytes)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *prog;
    uint32_t *spv = NULL;
    size_t words;

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
    }
    glslang_program_delete(prog);
    glslang_shader_delete(sh);
    return spv;
}
