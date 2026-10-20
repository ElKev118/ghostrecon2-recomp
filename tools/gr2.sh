#!/usr/bin/env bash
# Ghost Recon 2 port - one entry point for agents whose terminal runs each
# command separately (no `cd` carried over, no `&&` chains). Always:
#
#   bash /c/XboxRecompProject/ghostrecon2/tools/gr2.sh <action> [args]
#
# Actions:
#   kill                 close the game if it is running
#   build                compile the exe (~10 s - 2 min); prints errors only
#   test [secs] [vk|cpu] kill + build + automated muted test (default 150 vk)
#   run  [secs] [vk|cpu] automated test without building
#   log  [pattern]       last lines of the test log, or grep -c of a pattern
#   grep <pattern>       matching lines of the test log (first 40)
#   game                 last lines of the game's own log (memory.log)
#   status               git status / last commits of both repos
#   regen [recomp]       regenerate C from the XBE (20-30 min; ask the user first)
#
# Extra RECOMP_* variables: put them before the command, e.g.
#   RECOMP_VK_DUMP_SURFACES=1 bash /c/XboxRecompProject/ghostrecon2/tools/gr2.sh run 150 vk
set -u
P=/c/XboxRecompProject/ghostrecon2
R=$P/build/Release
A=${1:-help}
shift || true

kill_game() { taskkill //F //IM ghostrecon2.exe > /dev/null 2>&1; sleep 1; }

build() {
    kill_game
    cd "$P" || exit 1
    out=$(cmake --build build --config Release --target ghostrecon2 -- -m:8 -v:m 2>&1)
    errs=$(echo "$out" | grep -E " error |error LNK|fatal error")
    if [ -n "$errs" ]; then echo "BUILD FAILED"; echo "$errs" | head -20; exit 1; fi
    echo "BUILD OK: $(ls -la --time-style=+%H:%M:%S $R/ghostrecon2.exe | awk '{print $6, $7}')"
}

case "$A" in
    kill)   kill_game; echo "killed (if it was running)";;
    build)  build;;
    test)   build; bash "$P/tools/run_test.sh" "${1:-150}" "${2:-vk}";;
    run)    kill_game; bash "$P/tools/run_test.sh" "${1:-150}" "${2:-vk}";;
    log)    if [ -n "${1:-}" ]; then grep -c -- "$1" "$R/run_test.log"; else tail -40 "$R/run_test.log" | cut -c1-200; fi;;
    grep)   grep -- "${1:?pattern}" "$R/run_test.log" | head -40 | cut -c1-200;;
    game)   tail -15 "$R/game/memory.log";;
    status) git -C "$P" status --short; git -C "$P" log --oneline | head -3
            git -C /c/XboxRecompProject/xboxrecomp status --short | grep -v '^??'
            git -C /c/XboxRecompProject/xboxrecomp log --oneline | head -3;;
    regen)  kill_game; bash "$P/tools/regen.sh" "${1:-all}";;
    *)      sed -n '2,20p' "$0";;
esac
