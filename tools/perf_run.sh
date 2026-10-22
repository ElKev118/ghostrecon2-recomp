#!/usr/bin/env bash
# Ghost Recon 2 - automated performance run with no frame limit.
#   tools/perf_run.sh [main|pb]   sample the game thread (default) or the
#                                 pushbuffer thread for 25 s once in play
# Log: build/Release/run_perf_<thread>.log, samples: samples_<thread>.bin
# (tools/samples.py . reads samples.bin). Sound muted; the window opens on
# screen -- do not press keys during the run.
THREAD=${1:-main}
cd "$(dirname "$0")/../build/Release" || exit 1
if tasklist | grep -qi ghostrecon2.exe; then echo "close ghostrecon2.exe first"; exit 1; fi
touch samples.stop; rm -f samples.bin
export RECOMP_NO_INI=1 RECOMP_PB_EXEC=1 RECOMP_WINDOW=sdl RECOMP_RENDERER=vk RECOMP_AC97_READY=1 RECOMP_USB=1 RECOMP_VBLANK=1 RECOMP_TRACE_BUDGET=0 RECOMP_AUDIO_VOLUME=0
export RECOMP_GR2_FRAME_MS=0 RECOMP_NV2A_KICK_SPIN=${RECOMP_NV2A_KICK_SPIN:-1}
export RECOMP_PAD_SCRIPT=8000:a,10000:a,12000:a,14000:a,16000:a,18000:a,20000:a,23000:a,60000:a,75000:a,90000:a,110000:a
export RECOMP_SAMPLE=2 RECOMP_SAMPLE_THREAD=$THREAD
./ghostrecon2.exe > run_perf_$THREAD.log 2>&1 &
sleep 115
rm -f samples.stop
sleep 25
touch samples.stop
sleep 1
taskkill //F //IM ghostrecon2.exe > /dev/null 2>&1
sleep 1
cp samples.bin samples_$THREAD.bin
echo "crashes: $(grep -c '\[CRASH\]' run_perf_$THREAD.log)"
grep -E '\[VKB\] .*fps' run_perf_$THREAD.log | tail -4
