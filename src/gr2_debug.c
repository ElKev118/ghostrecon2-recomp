/**
 * Ghost Recon 2 - temporary bring-up instrumentation.
 *
 * Wrappers here call the generated body as sub_X_gen, so they observe
 * without changing behaviour. The recompiler finds them with
 *   tools.recomp ... --exclude-manual ../ghostrecon2/src/gr2_debug.c
 * which needs each wrapped body declared exactly as
 *   extern void sub_XXXXXXXX_gen(void);
 * Remove a wrapper once its question is answered.
 *
 * History:
 *   0x0010A4C0 RSMod::FindFile -- dumped the mod file index; showed it was
 *   empty because NtQueryDirectoryFile rejected an empty mask (fixed in
 *   xboxrecomp src/kernel/kernel_file.c).
 */

#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"
