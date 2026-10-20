/**
 * Ghost Recon 2 - XDK Direct3D (LTCG, recompiled from the XBE's D3D section)
 *
 * The title's own D3D8 writes NV2A pushbuffers, which the runtime's
 * pushbuffer executor (RECOMP_PB_EXEC=1) runs on its poll thread. D3D's fence
 * wait spins on the GPU semaphore at *(device + 0x30), which only advances if
 * the executor knows where it lives. This replaces the fence wait with the
 * "honest" version from xboxrecomp docs/technical/d3d8ltcg-device-context.md.
 *
 * Addresses (Ghost Recon 2, Title ID 0x55530005, XDK 5849):
 *   0x0043A1B8  D3D device pointer global (most-read global in the D3D section)
 *   0x0042F970  D3D_BlockOnTime(fence, flags), ret 8
 *   0x0042F8B0  KickOff(flags), ret 4 -- what BlockOnTime does first when the
 *               fence is the newest one (== device + 0x2C)
 *
 * The generated body of 0x0042F970 is excluded via manual_functions.json
 * (tools.recomp --manual-functions), so this definition is what every caller
 * links against.
 */

#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"

#include <stdio.h>
#include <windows.h>

#define GR2_D3D_DEVICE_PTR   0x0043A1B8u
#define GR2_FENCE_WAIT_MS    250

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* Has the GPU (the executor) consumed the pushbuffer up to `fence`?
 * Same test as the original: done once write - fence >= write - gpu_read. */
static int gr2_fence_passed(uint32_t dev, uint32_t fence)
{
    uint32_t write = MEM32(dev + 0x2C);
    uint32_t read  = MEM32(MEM32(dev + 0x30));
    return (uint32_t)(write - fence) >= (uint32_t)(write - read);
}

/* D3D_BlockOnTime(DWORD fence, DWORD flags) -- ret 8 */
void sub_0042F970(void)
{
    static uint32_t s_semaphore_va;
    uint32_t fence = MEM32(esp + 4);
    uint32_t dev   = MEM32(GR2_D3D_DEVICE_PTR);

    if (!dev) {
        esp += 12; return;
    }

    /* Hand the executor the semaphore once; re-check in case D3D moved it
     * (device reset rewrites the field). */
    if (MEM32(dev + 0x30) != s_semaphore_va) {
        s_semaphore_va = MEM32(dev + 0x30);
        nv2a_pb_set_semaphore_target(s_semaphore_va);
        fprintf(stderr, "  [D3D] GPU semaphore at 0x%08X (device 0x%08X)\n",
                s_semaphore_va, dev);
        fflush(stderr);
    }

    if (!gr2_fence_passed(dev, fence)) {
        /* Kick off whatever has been written before waiting: the release
         * for this fence may still sit in the unsubmitted part of the
         * pushbuffer. Kicking only when the fence was the newest one left
         * older fences waiting the whole timeout for commands nobody had
         * sent -- several 250 ms stalls a frame, 1-3 FPS in a mission. A
         * kick with nothing new to send costs one register write. */
        /* KickOff itself waits on fences (for ring space) through this very
         * function: kicking from inside that wait recursed until the native
         * stack overflowed (0xC00000FD in sub_0042F8B0 / sub_0042F970). The
         * outer call has already kicked, so the inner one only waits. */
        static __declspec(thread) int s_in_kick;
        if (!s_in_kick) {
            s_in_kick = 1;
            PUSH32(esp, 0);              /* KickOff flags */
            PUSH32(esp, 0x0042F99Fu);    /* return address, as the original */
            sub_0042F8B0();              /* ret 4: pops ret + arg */
            s_in_kick = 0;
        }

        DWORD start = GetTickCount();
        while (!gr2_fence_passed(dev, fence)) {
            if (GetTickCount() - start >= GR2_FENCE_WAIT_MS) {
                static int warned;
                static unsigned timeouts;
                if (++timeouts % 50 == 0)
                    fprintf(stderr, "  [D3D] %u fence waits timed out so far\n", timeouts);
                if (warned++ < 8) {
                    fprintf(stderr, "  [D3D] fence 0x%08X not reached after %d ms "
                            "(write=0x%08X gpu=0x%08X)\n", fence, GR2_FENCE_WAIT_MS,
                            MEM32(dev + 0x2C), MEM32(MEM32(dev + 0x30)));
                    fflush(stderr);
                }
                break;
            }
            Sleep(0);
        }
    }

    esp += 12; return; /* ret 8 */
}
