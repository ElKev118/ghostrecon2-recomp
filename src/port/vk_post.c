/**
 * Ghost Recon 2 port - the window's scaling pass (vk_post.h).
 *
 * One full-screen triangle into the frame's rectangle, dynamic rendering,
 * the source bound with a push descriptor. Runs on the window thread, in
 * its present command buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_post.h"
#include "vk_glsl.h"

static const char *VS =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* AREA: each output pixel is the mean of the source over its footprint,
 * from n x n bilinear taps spread evenly across it (n = the shrink factor,
 * rounded up, at most 8 per axis) -- a box filter as wide as the pixel.
 * At 1x or when enlarging it is a single bilinear tap. */
static const char *FS =
    "#version 450\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform P { vec2 src_size; vec2 dst_size; int filt; } pc;\n"
    "void main() {\n"
    "  vec2 ratio = pc.src_size / pc.dst_size;\n"
    "  if (pc.filt == 0 || (ratio.x <= 1.0 && ratio.y <= 1.0)) {\n"
    "    o = vec4(texture(src, uv).rgb, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  ivec2 n = clamp(ivec2(ceil(ratio)), ivec2(1), ivec2(8));\n"
    "  vec2 foot = 1.0 / pc.dst_size;\n"
    "  vec3 acc = vec3(0.0);\n"
    "  for (int j = 0; j < n.y; j++)\n"
    "    for (int i = 0; i < n.x; i++) {\n"
    "      vec2 f = (vec2(i, j) + 0.5) / vec2(n) - 0.5;\n"
    "      acc += texture(src, uv + f * foot).rgb;\n"
    "    }\n"
    "  o = vec4(acc / float(n.x * n.y), 1.0);\n"
    "}\n";

typedef struct { float src_size[2], dst_size[2]; int32_t filt; } Push;

static struct {
    VkDevice dev;
    VkSampler samp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule vs, fs;
    VkPipeline pipe;
    VkFormat fmt;
    PFN_vkCmdPushDescriptorSetKHR push_desc;
    int ok;
} P;

static VkShaderModule module(const char *src, int frag)
{
    size_t bytes = 0;
    uint32_t *code = vkg_compile(src, frag, &bytes);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;

    if (!code)
        return VK_NULL_HANDLE;
    ci.codeSize = bytes;
    ci.pCode = code;
    vkCreateShaderModule(P.dev, &ci, NULL, &m);
    free(code);
    return m;
}

static int make_pipeline(VkFormat fmt)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };

    if (P.pipe) {
        vkDestroyPipeline(P.dev, P.pipe, NULL);
        P.pipe = VK_NULL_HANDLE;
    }
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = P.vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = P.fs;
    st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &fmt;
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = P.layout;
    if (vkCreateGraphicsPipelines(P.dev, VK_NULL_HANDLE, 1, &gp, NULL, &P.pipe) != VK_SUCCESS) {
        fprintf(stderr, "[VKP] pipeline failed; the window blits instead\n");
        return 0;
    }
    P.fmt = fmt;
    return 1;
}

int vkp_init(VkDevice dev, VkFormat target_format)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    P.dev = dev;
    P.push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dli.bindingCount = 1;
    dli.pBindings = &b;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &P.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (!P.push_desc
        || vkCreateSampler(dev, &sci, NULL, &P.samp) != VK_SUCCESS
        || vkCreateDescriptorSetLayout(dev, &dli, NULL, &P.dsl) != VK_SUCCESS
        || vkCreatePipelineLayout(dev, &pli, NULL, &P.layout) != VK_SUCCESS
        || !(P.vs = module(VS, 0)) || !(P.fs = module(FS, 1))
        || !make_pipeline(target_format)) {
        fprintf(stderr, "[VKP] scaling pass unavailable; the window blits instead\n");
        return 0;
    }
    P.ok = 1;
    return 1;
}

int vkp_draw(VkCommandBuffer cmd, VkImageView src, uint32_t src_w, uint32_t src_h,
             VkImageView dst, VkFormat dst_format, uint32_t dst_w, uint32_t dst_h,
             VkRect2D rect, int filter)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkViewport vp;
    Push pc;

    if (!P.ok || !src || !dst || !rect.extent.width || !rect.extent.height)
        return 0;
    if (dst_format != P.fmt && !make_pipeline(dst_format)) {
        P.ok = 0;
        return 0;
    }
    ca.imageView = dst;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = dst_w;
    ri.renderArea.extent.height = dst_h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe);
    ii.sampler = P.samp;
    ii.imageView = src;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    P.push_desc(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.layout, 0, 1, &w);
    vp.x = (float)rect.offset.x;
    vp.y = (float)rect.offset.y;
    vp.width = (float)rect.extent.width;
    vp.height = (float)rect.extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    pc.src_size[0] = (float)src_w;
    pc.src_size[1] = (float)src_h;
    pc.dst_size[0] = (float)rect.extent.width;
    pc.dst_size[1] = (float)rect.extent.height;
    pc.filt = filter;
    vkCmdPushConstants(cmd, P.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    return 1;
}
