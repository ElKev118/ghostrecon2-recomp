#ifndef GR2_VK_GLSL_H
#define GR2_VK_GLSL_H

#include <stddef.h>
#include <stdint.h>

/* GLSL 450 source to SPIR-V. Returns malloc'd words (free() them) and the
 * size in bytes, or NULL with the log on stderr. */
uint32_t *vkg_compile(const char *src, int fragment, size_t *out_bytes);

#endif
