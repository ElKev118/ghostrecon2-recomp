/**
 * Ghost Recon 2 - mouse look, written straight into the game's camera.
 *
 * The right stick sets a turn *rate* on the soldier (+0x3CC yaw, +0x3D4
 * pitch). Once a frame sub_00219260 hands them to sub_001F4C80 (yaw) and
 * sub_001F1C20 (pitch), which ramp the rate (an inertia that suits a stick
 * and makes a mouse feel sticky), turn it into an angle with the frame time
 * and add it to the view: yaw at +0x238 (radians, 0..2pi, right is down),
 * pitch at +0x23C (radians, up is negative, clamped there by stance).
 * sub_001F4C80 ends by wrapping the yaw with sub_001EE3E0 -- replaced here:
 * for the player's soldier, called from that spot, the mouse's travel is
 * added to the yaw and to the pitch (which sub_001F1C20, next, clamps) as
 * angles, straight, before the wrap. The pad is untouched; the mouse stops
 * driving the right stick while this runs (mkb_take_look).
 *
 * The player's soldier is the one marked as a player's (is_player_soldier):
 * every soldier's yaw goes through here, the player's alone takes the mouse.
 *
 * Bring-up tool for finding the camera's angles: RECOMP_RAM_SNAP=<prefix>
 * starts a thread that, whenever the file <prefix>.flag appears, writes the
 * guest's 64 MB of RAM to <prefix>_<n>.bin and deletes the flag. With
 * RECOMP_PAD_LIVE driving the pad, snapshots before and after a turn of the
 * right stick show which floats are the view's yaw and pitch.
 */

#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "mkb_input.h"

#define GR2_PLAYER_OBJ       0x004CA968u   /* -> the local player's object */
#define GR2_PLAYER_SOLDIER   0x230u        /* its soldier */
#define GR2_SOLDIER_YAW      0x238u
#define GR2_SOLDIER_PITCH    0x23Cu
#define GR2_YAW_WRAP_RET     0x001F4F60u   /* sub_001F4C80's call of the wrap */
/* Radians per mouse count at sensitivity 1, unzoomed: 0.052 degrees (at
 * the default 1.5, 800 counts -- a quick flick -- turn about 60 degrees). */
#define GR2_MOUSE_RAD        0.0009
#define GR2_VIEW_TAN_45      0.41421356   /* the title's usual view, 45 degrees */

/* sub_001EE3E0(float *angle): wrap an angle into [0, 2pi) -- the title's
 * loop, its own constants (2pi at 0x00511914, 0.0 at 0x0050F3E8). From the
 * player's yaw update, the mouse's travel goes in first. */
static long s_last_dx, s_last_dy;      /* the last travel taken (trace) */

/* Is soldier s the one a player controls? The soldier says so itself:
 * +0x3FC is the controlling player's number (255 for none, every AI soldier)
 * and +0x3BC is 1 when a player has it -- the same in the campaign and in
 * training, where the local player's object ([0x004CA968]) has no soldier. */
static int is_player_soldier(uint32_t s)
{
    {
        /* RECOMP_GR2_MOUSE_TRACE=3: the soldiers' marks, now and then. */
        static int tr = -1;
        static unsigned n;
        if (tr < 0) { const char *e = getenv("RECOMP_GR2_MOUSE_TRACE"); tr = e && e[0] == '3'; }
        if (tr && (n++ % 400) < 16)
            fprintf(stderr, "[MPICK] soldier %08X yaw %.4f  +0x3BC %u +0x3FC %u\n",
                    s, MEMF(s + GR2_SOLDIER_YAW), MEM8(s + 0x3BCu), MEM8(s + 0x3FCu));
    }
    return MEM8(s + 0x3FCu) != 0xFFu && MEM8(s + 0x3BCu) == 1u;
}

/* The player's soldier, from each soldier's yaw update in turn (s): s
 * itself if it is the player's, else 0. */
static uint32_t player_soldier(uint32_t s)
{
    static uint32_t said;
    if (!is_player_soldier(s))
        return 0;
    if (s != said) {
        said = s;
        fprintf(stderr, "[MOUSE] the player's soldier: %08X\n", s);
    }
    return s;
}

void sub_001EE3E0(void)
{
    uint32_t p = MEM32(esp + 4);
    float two_pi = MEMF(0x00511914u);
    double zero = MEMD(0x0050F3E8u);
    float a;

    if (MEM32(esp) == GR2_YAW_WRAP_RET) {
        uint32_t soldier = player_soldier(p - GR2_SOLDIER_YAW);
        {
            /* RECOMP_GR2_MOUSE_TRACE=1: once a second, how often the yaw
             * update ran, for the player, and took mouse travel. */
            static int tr = -1;
            static unsigned calls, mine, moved;
            static DWORD t0;
            if (tr < 0) tr = getenv("RECOMP_GR2_MOUSE_TRACE") != NULL;
            if (tr) {
                DWORD now = GetTickCount();
                calls++;
                if (soldier && p == soldier + GR2_SOLDIER_YAW) mine++;
                if (!t0) t0 = now;
                if (now - t0 >= 1000) {
                    fprintf(stderr, "[MOUSE] %u yaw updates/s, %u the player's, %u with mouse travel"
                            " (player %08X)\n", calls, mine, moved, soldier);
                    calls = mine = moved = 0;
                    t0 = now;
                }
                if (soldier && p == soldier + GR2_SOLDIER_YAW && (s_last_dx || s_last_dy)) moved++;
                /* RECOMP_GR2_MOUSE_TRACE=2: every player update -- the soldier's
                 * yaw against the camera's look (0x00439720), to see any lag. */
                if (tr && soldier && p == soldier + GR2_SOLDIER_YAW
                    && getenv("RECOMP_GR2_MOUSE_TRACE")[0] == '2') {
                    static unsigned said;
                    static DWORD last_move;
                    if (s_last_dx || s_last_dy)
                        last_move = now;
                    if (last_move && now - last_move < 1500 && said++ < 3000)
                        fprintf(stderr, "[MLAG] t=%lu yaw %.4f cam %.4f dx %ld\n",
                                (unsigned long)GetTickCount(), MEMF(p),
                                atan2(MEMF(0x00439724u), MEMF(0x00439720u)), s_last_dx);
                }
            }
        }
        if (soldier && p == soldier + GR2_SOLDIER_YAW) {
            long dx, dy;
            static int fake = -1;
            static long fdx, fdy;
            int got = mkb_take_look(&dx, &dy);
            if (fake < 0) {
                /* RECOMP_GR2_MOUSE_FAKE=dx,dy: that travel every frame (tests). */
                const char *e = getenv("RECOMP_GR2_MOUSE_FAKE");
                fake = e && sscanf(e, "%ld,%ld", &fdx, &fdy) == 2;
                fprintf(stderr, "[MOUSE] direct look on the player's soldier %08X\n", soldier);
            }
            if (fake) { dx = fdx; dy = fdy; got = 1; }
            s_last_dx = got ? dx : 0;
            s_last_dy = got ? dy : 0;
            if (got && (dx || dy)) {
                extern float gr2_view_tan(void);
                double k = GR2_MOUSE_RAD * mkb_sensitivity();
                double zoom = gr2_view_tan() / GR2_VIEW_TAN_45;
                /* Zoomed in (a scope, binoculars): the same hand turns the
                 * view by the same share of the screen. */
                if (zoom > 0.02 && zoom < 1.0)
                    k *= zoom;
                MEMF(p) = (float)(MEMF(p) - dx * k);
                MEMF(soldier + GR2_SOLDIER_PITCH) =
                    (float)(MEMF(soldier + GR2_SOLDIER_PITCH) + dy * k);
            }
        }
    }
    a = MEMF(p);
    while (zero > (double)a) {          /* below zero: up by 2pi */
        a = a + two_pi;
        MEMF(p) = a;
        a = MEMF(p);
    }
    while (a >= two_pi) {               /* 2pi and more: down */
        a = a - two_pi;
    }
    MEMF(p) = a;
    eax = p;                            /* as the title leaves it */
    esp += 4;                           /* ret */
}

static char s_snap_prefix[MAX_PATH];

static void ram_snapshot(int n)
{
    char path[MAX_PATH + 16];
    FILE *f;
    uint32_t va;

    snprintf(path, sizeof path, "%s_%d.bin", s_snap_prefix, n);
    f = fopen(path, "wb");
    if (!f)
        return;
    for (va = 0; va < 0x04000000u; va += 0x10000u) {
        MEMORY_BASIC_INFORMATION mi;
        const void *p = (const void *)XBOX_PTR(va);
        static const uint8_t zero[0x10000];
        if (VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT
            && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            fwrite(p, 1, 0x10000u, f);
        else
            fwrite(zero, 1, 0x10000u, f);
    }
    fclose(f);
    fprintf(stderr, "[SNAP] RAM written to %s\n", path);
}

static DWORD WINAPI snap_thread(LPVOID arg)
{
    extern int xbox_WatchArmAt(uint32_t va);
    char flag[MAX_PATH + 8], watch[MAX_PATH + 8];
    int n = 0;
    (void)arg;
    snprintf(flag, sizeof flag, "%s.flag", s_snap_prefix);
    snprintf(watch, sizeof watch, "%s.watch", s_snap_prefix);
    for (;;) {
        FILE *w;
        if (GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
            ram_snapshot(n++);
            DeleteFileA(flag);
        }
        /* <prefix>.watch holding a guest address: trap the writes to it
         * (RECOMP_WATCH, armed now rather than at boot). */
        if ((w = fopen(watch, "rb")) != NULL) {
            char line[32] = { 0 };
            uint32_t va;
            fread(line, 1, sizeof line - 1, w);
            fclose(w);
            DeleteFileA(watch);
            /* "p+0x23C": that field of the player's soldier. */
            if (line[0] == 'p' && line[1] == '+') {
                uint32_t obj = MEM32(0x004CA968u);
                va = obj ? MEM32(obj + 0x230u) + (uint32_t)strtoul(line + 2, NULL, 0) : 0;
                fprintf(stderr, "[SNAP] player soldier %08X\n", obj ? MEM32(obj + 0x230u) : 0);
            } else {
                va = (uint32_t)strtoul(line, NULL, 0);
            }
            if (va)
                xbox_WatchArmAt(va);
        }
        Sleep(50);
    }
    return 0;
}

void gr2_mouse_boot(void)
{
    const char *e = getenv("RECOMP_RAM_SNAP");
    if (e && *e) {
        snprintf(s_snap_prefix, sizeof s_snap_prefix, "%s", e);
        CreateThread(NULL, 0, snap_thread, NULL, 0, NULL);
    }
}
