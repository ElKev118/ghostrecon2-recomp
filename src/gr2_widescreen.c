/**
 * Ghost Recon 2 - native widescreen (Hor+): more of the world at the sides,
 * the same view up and down as the 4:3 game.
 *
 * The engine's camera (its update is sub_000360E0) keeps a *horizontal* field
 * of view (+0x38) and an aspect ratio (+0x3C, 4:3 from the constant at
 * 0x00510F88). It builds its projection as
 *     right = tan(fov * 0.5) * near            (+0x48; left = -right, +0x44)
 *     top   = right / aspect                   (+0x4C; bottom = -top, +0x50)
 * and hands the four to D3DXMatrixPerspectiveOffCenterLH (sub_0044609E).
 * A wider aspect alone only takes height away (the same width of world, cut
 * at the top and bottom: a zoom once the window stretches it). So the aspect
 * stays 4:3 and, when the camera is the caller, left and right are widened by
 * k = display aspect / (4/3) on their way into the matrix -- and in the
 * camera's own copies, which its frustum is built from. The frame is then a
 * wide view squeezed into 640x480 that the renderer draws wider (its images
 * are k times as wide) and the window shows at the display's shape; the 2D
 * layer is narrowed back by the renderer (vkb_set_widescreen).
 */

#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/* The return address of the camera's call, in sub_000360E0. */
#define GR2_CAMERA_PROJ_RET  0x0003628Eu
/* The camera's matrix (pOut) lives at camera + 0xAC. */
#define GR2_CAMERA_MATRIX    0xACu

static volatile float s_k = 1.0f;       /* display aspect / (4/3); 1 = 4:3 */

void gr2_set_widescreen(float aspect, float hud_pos)
{
    extern void vkb_set_widescreen(float k, float hud_pos);
    float k = aspect / (4.0f / 3.0f);

    if (!(k > 1.01f && k < 6.0f))
        k = 1.0f;
    if (k != s_k)
        fprintf(stderr, "[WIDE] %s (display aspect %.4f, k %.4f)\n",
                k > 1.0f ? "native widescreen" : "4:3, as shipped", aspect, k);
    s_k = k;
    vkb_set_widescreen(k, hud_pos);
}

float gr2_get_widescreen_k(void)
{
    return s_k;
}

/* tan(horizontal FOV / 2) of the view camera, as the title sets it (0.414
 * for its usual 45 degrees; smaller when zoomed); 0 until first seen. */
static volatile float s_view_tan;

float gr2_view_tan(void)
{
    return s_view_tan;
}

/* RECOMP_GR2_ASPECT=<display aspect> and RECOMP_HUD_POS=<0..1>, at boot (the
 * trial launchers). */
void gr2_widescreen_boot(void)
{
    const char *e = getenv("RECOMP_GR2_ASPECT");
    const char *h = getenv("RECOMP_HUD_POS");
    if (e && *e)
        gr2_set_widescreen((float)atof(e), h ? (float)atof(h) : 1.0f);
}

/* D3DXMatrixPerspectiveOffCenterLH(pOut, l, r, b, t, zn, zf) -- ret 0x1C.
 * The title's own (XDK D3DX) arithmetic, with 1.0 and 2.0 read where it reads
 * them; only the camera's call has its sides widened. */
void sub_0044609E(void)
{
    uint32_t out = MEM32(esp + 4);
    float l = MEMF(esp + 8), r = MEMF(esp + 12);
    float b = MEMF(esp + 16), t = MEMF(esp + 20);
    float zn = MEMF(esp + 24), zf = MEMF(esp + 28);
    float k = s_k, a, c, q;

    {
        /* RECOMP_GR2_PROJ_TRACE=1: each caller once, with what it asks for. */
        static int tr = -1;
        static uint32_t seen[32];
        static int nseen;
        int i;
        if (tr < 0) tr = getenv("RECOMP_GR2_PROJ_TRACE") != NULL;
        if (tr) {
            for (i = 0; i < nseen && seen[i] != (MEM32(esp) ^ out); i++) {}
            if (i == nseen && nseen < 32) {
                seen[nseen++] = MEM32(esp) ^ out;
                fprintf(stderr, "[PROJ] caller %08X  l %.4f r %.4f b %.4f t %.4f zn %.3f zf %.1f out %08X\n",
                        MEM32(esp), l, r, b, t, zn, zf, out);
            }
        }
    }
    if (MEM32(esp) == GR2_CAMERA_PROJ_RET && r > l && zn > 0.0f) {
        /* How wide the cameras see, for the mouse: a zoomed view turns
         * slower under the same hand. The title keeps several cameras, all
         * 45 degrees in play; a scope narrows one of them -- which one
         * depends on the scope -- so the narrowest of the last tenth of a
         * second counts. */
        static float cur;
        static DWORD t0;
        DWORD now = GetTickCount();
        float tn = (r - l) / (2.0f * zn);
        if (!t0 || now - t0 >= 100) {
            if (t0) {
                if (fabsf(cur - s_view_tan) > 0.05f * cur)
                    fprintf(stderr, "[ZOOM] view %.1f degrees wide (mouse x%.2f)\n",
                            2.0 * atan(cur) * 57.29578, cur / 0.41421356);
                s_view_tan = cur;
            }
            cur = tn;
            t0 = now;
        } else if (tn < cur) {
            cur = tn;
        }
    }
    if (k > 1.0f && MEM32(esp) == GR2_CAMERA_PROJ_RET) {
        uint32_t cam = out - GR2_CAMERA_MATRIX;
        l *= k;
        r *= k;
        MEMF(cam + 0x44) = l;
        MEMF(cam + 0x48) = r;
    }
    a = MEMF(0x0050FF64u) / (r - l);            /* 1 / (r - l) */
    c = MEMF(0x0050FF64u) / (t - b);            /* 1 / (t - b) */
    q = zf / (zf - zn);

    MEMF(out + 0x00) = a * zn * MEMF(0x00510180u);
    MEMF(out + 0x04) = 0.0f;
    MEMF(out + 0x08) = 0.0f;
    MEMF(out + 0x0C) = 0.0f;
    MEMF(out + 0x10) = 0.0f;
    MEMF(out + 0x14) = c * zn * MEMF(0x00510180u);
    MEMF(out + 0x18) = 0.0f;
    MEMF(out + 0x1C) = 0.0f;
    MEMF(out + 0x20) = -((l + r) * a);
    MEMF(out + 0x24) = -((b + t) * c);
    MEMF(out + 0x28) = q;
    MEMF(out + 0x2C) = 1.0f;
    MEMF(out + 0x30) = 0.0f;
    MEMF(out + 0x34) = 0.0f;
    MEMF(out + 0x38) = -(q * zn);
    MEMF(out + 0x3C) = 0.0f;

    eax = out;
    esp += 32;                                  /* ret 0x1C */
}
