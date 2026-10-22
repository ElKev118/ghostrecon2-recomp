#!/usr/bin/env bash
# Full regeneration: XBE -> C -> build. ~30 min. Only needed after changing
# seed_functions.json, manual_functions.json, trace_functions.json or the
# recompiler (xboxrecomp/tools/*). Close ghostrecon2.exe first.
#   tools/regen.sh            all steps
#   tools/regen.sh recomp     skip disassembly (only trace/manual list or recompiler changed)
set -o pipefail
cd "$(dirname "$0")/../../xboxrecomp" || exit 1
G=game_files
if [ "${1:-all}" = all ]; then
  echo "[1/4] disasm (~12 min)"; python -m tools.disasm $G/default.xbe --force --seed-functions ../ghostrecon2/seed_functions.json > $G/regen_disasm.log 2>&1 || exit 1
  echo "[2/4] func_id";  python -m tools.func_id $G/default.xbe > $G/regen_funcid.log 2>&1 || exit 1
  echo "[3/4] abi";      python -m tools.abi_analysis $G/default.xbe > $G/regen_abi.log 2>&1 || exit 1
fi
echo "[4/4] recomp (~4 min)"
python -m tools.recomp $G/default.xbe --all --split 250 --gen-dir ../ghostrecon2/src/recomp/gen \
    --manual-functions ../ghostrecon2/manual_functions.json \
    --trace-functions ../ghostrecon2/trace_functions.json \
    --mmio-functions ../ghostrecon2/mmio_functions.json > $G/regen_recomp.log 2>&1 || exit 1
tail -2 $G/regen_recomp.log
echo "build (~15 min, all generated chunks)"
cmake --build ../ghostrecon2/build --config Release -- -m:8 -v:m > $G/regen_build.log 2>&1
echo "build rc=$?"; grep -E " error " $G/regen_build.log | head
