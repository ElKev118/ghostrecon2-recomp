/**
 * Ghost Recon 2 port - the options the port menu changes, kept in gr2.ini
 * next to the exe (docs/PORT_MENU_PLAN.md).
 *
 * Until the menu saves, nothing changes: the launcher's RECOMP_* variables
 * decide as before. Once gr2.ini exists its keys win over those variables,
 * so what the menu shows is what the next start uses. RECOMP_NO_INI=1
 * ignores the file (the automated test scripts set it).
 */
#ifndef GR2_PORT_SETTINGS_H
#define GR2_PORT_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { PS_WINDOWED = 0, PS_BORDERLESS = 1, PS_FULLSCREEN = 2 };
enum { PS_ASPECT_4_3 = 0, PS_ASPECT_16_9 = 1, PS_ASPECT_FILL = 2 };

typedef struct {
    int   window_mode;     /* PS_* */
    int   vsync;           /* 0/1 */
    float frame_ms;        /* -1 = the title's own (30 FPS), 0 = no limit */
    int   volume;          /* 0..100 */
    float reverb;          /* reverb send, 0..0.5 */
    float ambience;        /* level of sounds many voices share (rain), 0..1 */
    int   language;        /* console code: 1 en, 3 de, 4 fr, 5 es, 6 it */
    int   shader_cache;    /* 0/1: keep compiled shaders on disk */
    float render_scale;    /* internal resolution, times 640x480 (1..5) */
    int   aspect;          /* PS_ASPECT_*: how the 4:3 frame fills the window */
    int   scale_filter;    /* VKP_*: 1 area (shrinking averages each pixel), 0 bilinear */
} PortSettings;

/* Read gr2.ini (if any) and push what must be in place before the guest
 * starts into the environment. Call first thing in main(). */
void port_settings_startup(void);

/* The current values (the file's, or the launcher's when there is none). */
PortSettings *port_settings(void);

/* The language the running title was started with (a change needs a
 * restart). */
int port_settings_boot_language(void);

/* Apply the live ones (volume, reverb, frame limit) from port_settings(). */
void port_settings_apply_live(void);

/* Write gr2.ini. Returns 1 on success. */
int port_settings_save(void);

/* Start a new copy of the game with the same command line and exit. */
void port_settings_restart(void);

#ifdef __cplusplus
}
#endif

#endif
