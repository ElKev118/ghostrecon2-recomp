#!/usr/bin/env bash
# Boot with no input, sample the game's main thread for the first SECS
# seconds and record the audio output. tools/video_run.sh [secs] [thread]
# Log: build/Release/run_video.log, audio: video.wav, samples: samples.bin
SECS=${1:-40}
THREAD=${2:-main}
cd "$(dirname "$0")/../build/Release" || exit 1
if tasklist | grep -qi ghostrecon2.exe; then echo "close ghostrecon2.exe first"; exit 1; fi
rm -f samples.stop samples.bin video.wav
export RECOMP_NO_INI=1 RECOMP_PB_EXEC=1 RECOMP_WINDOW=sdl RECOMP_RENDERER=vk RECOMP_AC97_READY=1 RECOMP_USB=1 RECOMP_VBLANK=1 RECOMP_TRACE_BUDGET=${TRACE_BUDGET-0} RECOMP_AUDIO_VOLUME=0
export RECOMP_GR2_FRAME_MS=${FRAME_MS-0} RECOMP_NV2A_KICK_SPIN=1 RECOMP_PB_THREADS=${PB_THREADS-2}
export RECOMP_AUDIO_WAV="$(cygpath -w "$PWD")\video.wav"
export RECOMP_SAMPLE=2 RECOMP_SAMPLE_THREAD=$THREAD
./ghostrecon2.exe > run_video.log 2>&1 &
sleep "$SECS"
touch samples.stop
sleep 1
taskkill //F //IM ghostrecon2.exe > /dev/null 2>&1
sleep 1
