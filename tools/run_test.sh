#!/usr/bin/env bash
# Ghost Recon 2 port - automated test run (Git Bash on Windows).
#
#   tools/run_test.sh [seconds] [renderer]        e.g.  tools/run_test.sh 150 vk
#
# renderer: vk (Vulkan, default) | cpu (software rasteriser, slow, reference)
# Log: build/Release/run_test.log   Frames: build/Release/tst_*.bmp (Vulkan only)
#
# Extra env vars pass through (export them before calling), e.g.
#   RECOMP_VK_DUMP_SURFACES=1  RECOMP_VK_WATCH=A17000  RECOMP_FRAME_TRACE=ftrace.flag
#
# Rules: sound is always muted here (RECOMP_AUDIO_VOLUME=0). The game window
# opens on the user's screen; tell them before a run so they do not press keys.
set -u
SECS=${1:-150}
REND=${2:-vk}
cd "$(dirname "$0")/../build/Release" || exit 1

if tasklist | grep -qi ghostrecon2.exe; then
    echo "ghostrecon2.exe is already running: close it first (it also blocks linking)"; exit 1
fi
rm -f tst_*.bmp

export RECOMP_AUDIO_VOLUME=0
export RECOMP_PB_EXEC=1 RECOMP_AC97_READY=1 RECOMP_USB=1 RECOMP_VBLANK=1
export RECOMP_TRACE_BUDGET=${RECOMP_TRACE_BUDGET:-0}
export RECOMP_WINDOW=sdl
if [ "$REND" = vk ]; then
    export RECOMP_RENDERER=vk
    export RECOMP_VK_DUMP=${RECOMP_VK_DUMP:-tst_} RECOMP_VK_DUMP_EVERY=${RECOMP_VK_DUMP_EVERY:-120}
else
    unset RECOMP_RENDERER
    export RECOMP_FB_DUMP=${RECOMP_FB_DUMP:-tst_.bmp} RECOMP_PB_REPORT_MS=${RECOMP_PB_REPORT_MS:-10000}
fi
# Controller script: A presses only (title -> menus -> campaign -> briefing ->
# equipment -> mission). 8 presses over ~15 s, then a few late ones in case a
# press was swallowed by a loading screen (A in game is harmless).
export RECOMP_PAD_SCRIPT=${RECOMP_PAD_SCRIPT:-8000:a,10000:a,12000:a,14000:a,16000:a,18000:a,20000:a,23000:a,60000:a,75000:a,90000:a,110000:a}

./ghostrecon2.exe > run_test.log 2>&1 &
sleep "$SECS"
taskkill //F //IM ghostrecon2.exe > /dev/null 2>&1
sleep 1

echo "== summary ($REND, ${SECS}s)"
echo "crash reports : $(grep -c '\[CRASH\]' run_test.log)"
echo "failed icalls : $(grep -c 'Failed to resolve VA' run_test.log)"
echo "fence timeouts: $(grep -c 'not reached after' run_test.log)"
echo "pb no-path    : $(grep -c 'no path' run_test.log)"
grep -E '\[VKB\] .*fps' run_test.log | tail -3
echo "-- game log (build/Release/game/memory.log), last lines:"
tail -3 game/memory.log
ls tst_*.bmp 2>/dev/null | wc -l | xargs echo "frames dumped:"
echo "If the process vanished with no [CRASH] line: Windows Event Viewer (Application, id 1000)"
echo "and resolve the offset with build/Release/ghostrecon2.map (see RECOMP_CONTEXT.md)."
