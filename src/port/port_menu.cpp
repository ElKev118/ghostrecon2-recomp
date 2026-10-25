/**
 * Ghost Recon 2 port - the options overlay (port_menu.h).
 *
 * Dear ImGui with its SDL2 platform and Vulkan renderer backends, on the
 * window thread. The overlay renders straight onto the swapchain image after
 * the game's frame is blitted there (dynamic rendering, no render pass).
 * Every change applies at once and is saved to gr2.ini when the menu
 * closes; the language is read
 * by the title at boot, so it takes a restart.
 */

#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "port_menu.h"
#include "port_settings.h"
#include "vk_glsl.h"
#include "port_version.h"
extern void vkb_pipeline_cache_forget(void);
extern void xbox_InputSetBlocked(int blocked);
extern void xbox_InputKeysClear(void);
}

static PortMenuInit s_init;
static bool s_ready;
static bool s_open;
static bool s_dirty;            /* applied, not yet written to gr2.ini */

static void set_open(bool open);

static void check_vk(VkResult r)
{
    if (r != VK_SUCCESS)
        fprintf(stderr, "[MENU] Vulkan error %d\n", (int)r);
}

extern "C" int pmenu_init(const PortMenuInit *init)
{
    s_init = *init;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;               /* our settings live in gr2.ini */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                    | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;

    if (!ImGui_ImplSDL2_InitForVulkan(init->win))
        return 0;

    ImGui_ImplVulkan_InitInfo vi = {};
    vi.ApiVersion = VK_API_VERSION_1_3;
    vi.Instance = init->inst;
    vi.PhysicalDevice = init->phys;
    vi.Device = init->dev;
    vi.QueueFamily = init->qfam;
    vi.Queue = init->queue;
    vi.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 8;
    vi.MinImageCount = init->image_count < 2 ? 2 : init->image_count;
    vi.ImageCount = vi.MinImageCount;
    vi.UseDynamicRendering = true;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_init.format;
    vi.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&vi))
        return 0;
    s_ready = true;
    fprintf(stderr, "[MENU] ready (Delete / Home opens the port options)\n");
    if (getenv("RECOMP_MENU_OPEN"))         /* tests: start with it open */
        set_open(true);
    return 1;
}

extern "C" void pmenu_set_image_count(uint32_t n)
{
    if (s_ready)
        ImGui_ImplVulkan_SetMinImageCount(n < 2 ? 2 : n);
}

extern "C" int pmenu_is_open(void)
{
    return s_open;
}

static void set_open(bool open)
{
    s_open = open;
    if (!open && s_dirty) {
        port_settings_save();
        s_dirty = false;
    }
    /* The game must not act on what the menu is given -- nor keep a key
     * that was held when it opened. */
    xbox_InputSetBlocked(open);
    xbox_InputKeysClear();
}

extern "C" int pmenu_event(const SDL_Event *ev)
{
    if (!s_ready)
        return 0;
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat
        && (ev->key.keysym.sym == SDLK_DELETE || ev->key.keysym.sym == SDLK_HOME)) {
        set_open(!s_open);
        return 1;
    }
    if (!s_open)
        return 0;
    ImGui_ImplSDL2_ProcessEvent(ev);
    /* Window events still belong to the window (resize, close). */
    return ev->type != SDL_WINDOWEVENT && ev->type != SDL_QUIT;
}

/* ---- the panel -------------------------------------------------------- */

struct FrameChoice { const char *name; float ms; };
static const FrameChoice k_frames[] = {
    { "30 FPS (original)", -1.0f },
    { "60 FPS",            1000.0f / 60.0f },
    { "120 FPS",           1000.0f / 120.0f },
    { "144 FPS",           1000.0f / 144.0f },
    { "Unlimited",          0.0f },
};

struct LangChoice { const char *name; int code; };
static const LangChoice k_langs[] = {
    { "English", 1 }, { "French", 4 }, { "German", 3 },
    { "Italian", 6 }, { "Spanish", 5 },
};

static int frame_index(float ms)
{
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < (int)IM_ARRAYSIZE(k_frames); i++) {
        float d = k_frames[i].ms - ms;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void panel(void)
{
    PortSettings *s = port_settings();
    bool changed = false;
    ImGuiIO &io = ImGui::GetIO();
    /* Sized for the window: 1x at 720 lines, so a 4K screen is not a
     * postage stamp. Spacing is rescaled from a copy of the unscaled style. */
    float scale = io.DisplaySize.y / 720.0f;
    static float s_scale = 0.0f;
    static ImGuiStyle s_base;

    if (scale < 1.0f) scale = 1.0f;
    if (s_scale == 0.0f)
        s_base = ImGui::GetStyle();
    if (scale != s_scale) {
        ImGui::GetStyle() = s_base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;
        s_scale = scale;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460 * scale, 0), ImGuiCond_Always);
    ImGui::Begin("Ghost Recon 2 " PORT_VERSION " - Port Options", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char *modes[] = { "Windowed", "Borderless fullscreen", "Fullscreen" };
        if (ImGui::Combo("Window mode", &s->window_mode, modes, IM_ARRAYSIZE(modes))) {
            s_init.set_window_mode(s->window_mode);
            changed = true;
        }
        bool vs = s->vsync != 0;
        if (ImGui::Checkbox("VSync", &vs)) {
            s->vsync = vs;
            s_init.set_vsync(s->vsync);
            changed = true;
        }
        int fi = frame_index(s->frame_ms);
        if (ImGui::BeginCombo("Frame limit", k_frames[fi].name)) {
            for (int i = 0; i < (int)IM_ARRAYSIZE(k_frames); i++)
                if (ImGui::Selectable(k_frames[i].name, i == fi)) {
                    s->frame_ms = k_frames[i].ms;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        {
            /* Times the console's 640x480; the window scales the result. */
            static const struct { float scale; const char *name; } k_res[] = {
                { 1.0f,  "Native (640x480)" },
                { 1.5f,  "x1.5 (960x720, 720p)" },
                { 2.0f,  "x2 (1280x960)" },
                { 2.25f, "x2.25 (1440x1080, 1080p)" },
                { 3.0f,  "x3 (1920x1440, 1440p)" },
                { 4.0f,  "x4 (2560x1920)" },
                { 4.5f,  "x4.5 (2880x2160, 4K)" },
                { 5.0f,  "x5 (3200x2400)" },
                { 6.0f,  "x6 (3840x2880, 2x2 supersampling at 1440p)" },
            };
            int ri = 0;
            for (int i = 0; i < (int)IM_ARRAYSIZE(k_res); i++)
                if (k_res[i].scale <= s->render_scale + 0.01f) ri = i;
            if (ImGui::BeginCombo("Internal resolution", k_res[ri].name)) {
                for (int i = 0; i < (int)IM_ARRAYSIZE(k_res); i++)
                    if (ImGui::Selectable(k_res[i].name, i == ri)) {
                        s->render_scale = k_res[i].scale;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The game's 3D drawn at this size, then fitted to the window.\n"
                                  "Pick the one nearest your screen's height.");
        }
        {
            static const char *aspects[] = { "4:3 (original)", "16:9 (stretched)",
                                             "Fill window (stretched)" };
            if (ImGui::Combo("Aspect ratio", &s->aspect, aspects, IM_ARRAYSIZE(aspects)))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The game draws 4:3; 16:9 stretches it sideways,\n"
                                  "as xemu's widescreen view does.");
        }
        {
            static const char *aas[] = { "Off", "SMAA" };
            if (ImGui::Combo("Anti-aliasing", &s->aa, aas, IM_ARRAYSIZE(aas)))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("SMAA smooths jagged edges on the frame, at the internal\n"
                                  "resolution. Light; most useful at native or x1.5-x2.");
        }
        {
            static const char *ups[] = { "Bilinear", "FSR 1" };
            if (ImGui::Combo("Upscaling", &s->upscaler, ups, IM_ARRAYSIZE(ups)))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("When the window is bigger than the internal resolution:\n"
                                  "FSR 1 enlarges with sharp edges; Bilinear is softer.");
        }
        {
            float sh = s->sharpen * 100.0f;
            if (ImGui::SliderFloat("Sharpen (RCAS)", &sh, 0.0f, 100.0f,
                                   sh < 0.5f ? "Off" : "%.0f%%")) {
                s->sharpen = sh / 100.0f;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("FSR's contrast-adaptive sharpening, last, at any scale.\n"
                                  "50-70%% brings back detail after supersampling;\n"
                                  "high values also bring back shimmer and halos.");
        }
        {
            static const char *filters[] = { "Bilinear", "Area (supersampling)" };
            if (ImGui::Combo("Downscale filter", &s->scale_filter, filters, IM_ARRAYSIZE(filters)))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("When the internal resolution is above the window's:\n"
                                  "Area averages every pixel it shrinks (smooth edges, no\n"
                                  "shimmer); Bilinear skips most of them.");
        }

        bool sc = s->shader_cache != 0;
        if (ImGui::Checkbox("Shader cache", &sc)) {
            s->shader_cache = sc;
            vkg_set_cache_enabled(sc);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keep compiled shaders on disk, so effects seen before\n"
                              "do not stutter the next time the game runs.");
        ImGui::SameLine();
        static int s_cleared = -1;
        if (ImGui::Button("Clear shader cache")) {
            s_cleared = vkg_cache_clear();
            vkb_pipeline_cache_forget();
        }
        if (s_cleared >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d files removed", s_cleared);
        }
    }

    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderInt("Volume", &s->volume, 0, 100, "%d%%");
        float rv = s->reverb * 100.0f;
        if (ImGui::SliderFloat("Reverb", &rv, 0.0f, 30.0f, "%.0f%%")) {
            s->reverb = rv / 100.0f;
            changed = true;
        }
        float am = s->ambience * 100.0f;
        if (ImGui::SliderFloat("Ambience (rain)", &am, 0.0f, 100.0f, "%.0f%%")) {
            s->ambience = am / 100.0f;
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
        int li = 0;
        for (int i = 0; i < (int)IM_ARRAYSIZE(k_langs); i++)
            if (k_langs[i].code == s->language) li = i;
        if (ImGui::BeginCombo("Language", k_langs[li].name)) {
            for (int i = 0; i < (int)IM_ARRAYSIZE(k_langs); i++)
                if (ImGui::Selectable(k_langs[i].name, i == li)) {
                    s->language = k_langs[i].code;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        if (s->language != port_settings_boot_language()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Restart required (texts, voices and movies).");
            if (ImGui::Button("Restart now")) {
                port_settings_save();
                port_settings_restart();
            }
        }
    }

    if (changed) {
        port_settings_apply_live();
        s_dirty = true;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Saved to gr2.ini on close. Delete / Home closes this menu.");
    if (ImGui::Button("Close"))
        set_open(false);
    ImGui::End();
}

extern "C" void pmenu_draw(VkCommandBuffer cmd, VkImageView view, uint32_t w, uint32_t h)
{
    if (!s_ready || !s_open)
        return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    panel();
    ImGui::Render();

    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    att.imageView = view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}
