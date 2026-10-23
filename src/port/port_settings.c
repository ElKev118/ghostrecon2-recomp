/**
 * Ghost Recon 2 port - gr2.ini, the port menu's settings (port_settings.h).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_settings.h"

/* Live setters: the toolkit's mixer and the title's frame limiter. */
extern void  xbox_AudioSetVolume(int pct);
extern int   xbox_AudioGetVolume(void);
extern void  xbox_AudioSetReverb(float gain);
extern float xbox_AudioGetReverb(void);
extern void  xbox_AudioSetAmbience(float gain);
extern float xbox_AudioGetAmbience(void);
extern void  gr2_set_frame_ms(float ms);
extern void  vkb_set_render_scale(float scale);
extern float gr2_get_frame_ms(void);

static PortSettings s_cur;
static int s_boot_language = 1;
static char s_path[MAX_PATH];

static void ini_path(void)
{
    char *slash;

    GetModuleFileNameA(NULL, s_path, MAX_PATH);
    slash = strrchr(s_path, '\\');
    if (slash)
        strcpy(slash + 1, "gr2.ini");
    else
        strcpy(s_path, "gr2.ini");
}

static int ini_int(const char *sec, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, s_path);
}

static float ini_float(const char *sec, const char *key, float def)
{
    char buf[32];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, s_path);
    return buf[0] ? (float)atof(buf) : def;
}

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return e ? atoi(e) : def;
}

void port_settings_startup(void)
{
    int use_ini;

    ini_path();
    use_ini = !getenv("RECOMP_NO_INI")
              && GetFileAttributesA(s_path) != INVALID_FILE_ATTRIBUTES;

    /* Defaults: the launcher's variables, as before the menu existed. */
    s_cur.window_mode = PS_WINDOWED;
    s_cur.vsync = !(getenv("RECOMP_PRESENT_VSYNC")
                    && getenv("RECOMP_PRESENT_VSYNC")[0] == '0');
    s_cur.frame_ms = gr2_get_frame_ms();
    s_cur.volume = xbox_AudioGetVolume();
    s_cur.reverb = xbox_AudioGetReverb();
    s_cur.ambience = xbox_AudioGetAmbience();
    s_cur.language = env_int("RECOMP_LANGUAGE", 1);
    s_cur.shader_cache = !(getenv("RECOMP_SHADER_CACHE")
                           && getenv("RECOMP_SHADER_CACHE")[0] == '0');
    s_cur.render_scale = getenv("RECOMP_RENDER_SCALE")
                       ? (float)atof(getenv("RECOMP_RENDER_SCALE")) : 1.0f;
    s_cur.aspect = env_int("RECOMP_ASPECT", PS_ASPECT_4_3);

    if (use_ini) {
        char buf[16];

        s_cur.window_mode = ini_int("Display", "WindowMode", s_cur.window_mode);
        s_cur.vsync       = ini_int("Display", "VSync", s_cur.vsync);
        s_cur.frame_ms    = ini_float("Display", "FrameLimitMs", s_cur.frame_ms);
        s_cur.volume      = ini_int("Audio", "Volume", s_cur.volume);
        s_cur.reverb      = ini_float("Audio", "Reverb", s_cur.reverb);
        s_cur.ambience    = ini_float("Audio", "Ambience", s_cur.ambience);
        s_cur.language    = ini_int("Game", "Language", s_cur.language);
        s_cur.shader_cache = ini_int("Display", "ShaderCache", s_cur.shader_cache);
        s_cur.render_scale = ini_float("Display", "RenderScale", s_cur.render_scale);
        s_cur.aspect = ini_int("Display", "Aspect", s_cur.aspect);
        _putenv(s_cur.shader_cache ? "RECOMP_SHADER_CACHE=1" : "RECOMP_SHADER_CACHE=0");

        /* Read once by the window and the kernel at their own start. */
        _putenv(s_cur.vsync ? "RECOMP_PRESENT_VSYNC=1" : "RECOMP_PRESENT_VSYNC=0");
        snprintf(buf, sizeof buf, "%d", s_cur.language);
        _putenv_s("RECOMP_LANGUAGE", buf);
        port_settings_apply_live();
        fprintf(stderr, "[PORT] settings from %s\n", s_path);
    }
    s_boot_language = s_cur.language;
    vkb_set_render_scale(s_cur.render_scale);  /* ini or RECOMP_RENDER_SCALE */
}

PortSettings *port_settings(void)
{
    return &s_cur;
}

int port_settings_boot_language(void)
{
    return s_boot_language;
}

void port_settings_apply_live(void)
{
    xbox_AudioSetVolume(s_cur.volume);
    xbox_AudioSetReverb(s_cur.reverb);
    xbox_AudioSetAmbience(s_cur.ambience);
    gr2_set_frame_ms(s_cur.frame_ms);
    vkb_set_render_scale(s_cur.render_scale);
}

static int put(const char *sec, const char *key, const char *val)
{
    return WritePrivateProfileStringA(sec, key, val, s_path) != 0;
}

int port_settings_save(void)
{
    char b[32];
    int ok = 1;

    snprintf(b, sizeof b, "%d", s_cur.window_mode); ok &= put("Display", "WindowMode", b);
    snprintf(b, sizeof b, "%d", s_cur.vsync);       ok &= put("Display", "VSync", b);
    snprintf(b, sizeof b, "%d", s_cur.shader_cache); ok &= put("Display", "ShaderCache", b);
    snprintf(b, sizeof b, "%.3f", s_cur.frame_ms);  ok &= put("Display", "FrameLimitMs", b);
    snprintf(b, sizeof b, "%.2f", s_cur.render_scale); ok &= put("Display", "RenderScale", b);
    snprintf(b, sizeof b, "%d", s_cur.aspect);      ok &= put("Display", "Aspect", b);
    snprintf(b, sizeof b, "%d", s_cur.volume);      ok &= put("Audio", "Volume", b);
    snprintf(b, sizeof b, "%.3f", s_cur.reverb);    ok &= put("Audio", "Reverb", b);
    snprintf(b, sizeof b, "%.3f", s_cur.ambience);  ok &= put("Audio", "Ambience", b);
    snprintf(b, sizeof b, "%d", s_cur.language);    ok &= put("Game", "Language", b);
    fprintf(stderr, "[PORT] settings %s %s\n", ok ? "saved to" : "NOT saved to", s_path);
    return ok;
}

void port_settings_restart(void)
{
    char exe[MAX_PATH], buf[16];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    /* The child inherits this environment: make it carry the new language,
     * not the one this process started with. */
    snprintf(buf, sizeof buf, "%d", s_cur.language);
    _putenv_s("RECOMP_LANGUAGE", buf);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fprintf(stderr, "[PORT] restarting\n");
    fflush(stderr);
    if (CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        _exit(0);
    }
    fprintf(stderr, "[PORT] restart failed (%lu)\n", GetLastError());
}
