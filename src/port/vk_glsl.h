#ifndef GR2_VK_GLSL_H
#define GR2_VK_GLSL_H

#include <stddef.h>
#include <stdint.h>

/* GLSL 450 source to SPIR-V. Returns malloc'd words (free() them) and the
 * size in bytes, or NULL with the log on stderr. */
uint32_t *vkg_compile(const char *src, int fragment, size_t *out_bytes);

/* %LOCALAPPDATA%\GhostRecon2\shadercache, or "" when the cache is off. */
const char *vkg_cache_dir(void);

/* On/off while running (the port menu); starts from RECOMP_SHADER_CACHE. */
int  vkg_cache_enabled(void);
void vkg_set_cache_enabled(int on);

/* Delete the cached SPIR-V and pipeline data; returns the files removed. */
int  vkg_cache_clear(void);

/* Since the last call: SPIR-V loaded from the cache, compiled, and the time
 * spent compiling. */
void vkg_stats(unsigned *hits, unsigned *compiles, double *compile_ms);

#endif
