/**
 * Ghost Recon 2 port - where things live, and running without a launcher.
 *
 * The exe runs on its own (double click, a shortcut): what the .bat
 * launchers used to set are now defaults here, and the working directory
 * is the exe's, so the game data is found in <exe dir>\game whatever
 * started it. A variable already set (by a launcher, a test script) wins.
 *
 * Everything the port writes goes to the title's own folder,
 * %LOCALAPPDATA%\GhostRecon2 -- one per title, so Summit Strike gets its
 * own:
 *   SaveData\      the console's hard disk: saves (UDATA), caches, images
 *   shadercache\   SPIR-V and pipeline caches (vk_glsl.c, vk_backend.c)
 *   gr2.ini        the options menu's settings (port_settings.c)
 *   logs\          ghostrecon2.log (when no console or redirect catches
 *                  stderr; the previous one as .old) and xbox_kernel.log
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>

static char s_data[MAX_PATH];

/* %LOCALAPPDATA%\GhostRecon2, created on first use. */
const char *gr2_data_dir(void)
{
    if (!s_data[0]) {
        char base[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
            snprintf(s_data, sizeof s_data, "%s\\GhostRecon2", base);
        else
            snprintf(s_data, sizeof s_data, "GhostRecon2");
        SHCreateDirectoryExA(NULL, s_data, NULL);
    }
    return s_data;
}

static void set_default(const char *name, const char *value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

/* stderr goes nowhere when the exe is started without a console or a
 * redirect (it is a GUI program): then to logs\ghostrecon2.log. */
static void open_log(const char *logs)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    char path[MAX_PATH], old[MAX_PATH];

    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
        return;
    snprintf(path, sizeof path, "%s\\ghostrecon2.log", logs);
    snprintf(old, sizeof old, "%s\\ghostrecon2.old.log", logs);
    MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    if (freopen(path, "a", stdout))
        setvbuf(stdout, NULL, _IONBF, 0);
}

/* The console's disk, per title. Builds before this kept it in
 * %LOCALAPPDATA%\xboxrecomp, shared by every title the toolkit runs; a
 * player's saves there move here once. */
static void save_dir(char *out, size_t n)
{
    char base[MAX_PATH], shared[MAX_PATH];

    snprintf(out, n, "%s\\SaveData", gr2_data_dir());
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
        return;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        snprintf(shared, sizeof shared, "%s\\xboxrecomp", base);
        if (GetFileAttributesA(shared) != INVALID_FILE_ATTRIBUTES) {
            if (MoveFileExA(shared, out, 0))
                fprintf(stderr, "[PORT] saves moved from %s to %s\n", shared, out);
            else
                fprintf(stderr, "[PORT] could not move %s to %s (error %lu); new saves go to the latter\n",
                        shared, out, GetLastError());
        }
    }
}

static char s_save[MAX_PATH];

/* The save dir for xbox_path_init (after gr2_startup). */
const char *gr2_save_dir(void)
{
    return s_save;
}

void gr2_startup(void)
{
    char exe[MAX_PATH], logs[MAX_PATH], klog[MAX_PATH], *slash;

    /* <exe dir> as the working directory: game\ and relative paths resolve
     * there however the exe was started. */
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) && (slash = strrchr(exe, '\\'))) {
        *slash = '\0';
        SetCurrentDirectoryA(exe);
    }

    snprintf(logs, sizeof logs, "%s\\logs", gr2_data_dir());
    SHCreateDirectoryExA(NULL, logs, NULL);
    open_log(logs);
    snprintf(klog, sizeof klog, "%s\\xbox_kernel.log", logs);
    set_default("XBOX_KERNEL_LOG", klog);

    /* What run_gr2_sinlimite.bat set: the Vulkan renderer in the SDL
     * window, the emulated APU, USB pads (and keyboard + mouse), the 60 Hz
     * vblank the movies and the frame pacing run on, two executor threads
     * and the kickoff answered at once. The frame limit, volume and the
     * rest come from gr2.ini. */
    set_default("RECOMP_PB_EXEC", "1");
    set_default("RECOMP_WINDOW", "sdl");
    set_default("RECOMP_RENDERER", "vk");
    set_default("RECOMP_AC97_READY", "1");
    set_default("RECOMP_MKB", "1");
    set_default("RECOMP_USB", "1");
    set_default("RECOMP_VBLANK", "1");
    set_default("RECOMP_TRACE_BUDGET", "0");
    set_default("RECOMP_NV2A_KICK_SPIN", "1");
    set_default("RECOMP_PB_THREADS", "2");
    set_default("RECOMP_GR2_FRAME_MS", "0");
    set_default("RECOMP_AUDIO_VOLUME", "80");

    save_dir(s_save, sizeof s_save);
}
