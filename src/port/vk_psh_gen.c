/*
 * Fragment shader generator: one straight-line GLSL shader per pixel-stage
 * set-up, the same arithmetic as vk_backend.c's FS_SRC (the uber-shader) with
 * every decision taken here instead of per pixel.
 *
 * The uber-shader interpreted the combiner words in loops over a register
 * array; on an AMD iGPU that was ~95% of the GPU's frame time (63 ms of 66
 * in Ghost Recon 2's first mission), and specialization constants did not
 * let the driver fold it. Here the registers are plain variables, inputs
 * are expressions, unused registers and stages generate nothing.
 *
 * spec[] is the draw's set-up exactly as vk_backend.c builds it for the
 * pipeline key (SPEC_N words):
 *    0- 7 colour input words   8-15 alpha input words
 *   16-23 colour output words 24-31 alpha output words
 *   32 control  33 stage program  34 other stage input  35 rc_seen
 *   36 final0   37 final1         38 shadow stage mask  39 shadow func
 *   40 border mask (bit 2st u, 2st+1 v)  41 clip plane mode
 *   42 fog enable  43 fog mode  44 alpha test  45 alpha func  46 alpha ref
 * Dynamic values (combiner factors, fog colour and parameters, texture
 * scales, clamps, border colours, shadow depth ranges) stay in the UBO and
 * push constants, laid out as in FS_SRC.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t cap, len;
    int    over;
} Gen;

static void put(Gen *g, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (g->over)
        return;
    va_start(ap, fmt);
    n = vsnprintf(g->buf + g->len, g->cap - g->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= g->cap - g->len) {
        g->over = 1;
        return;
    }
    g->len += (size_t)n;
}

/* A combiner input mapping applied to an expression (map_in in FS_SRC). */
static void mapped(char *out, size_t n, const char *v, uint32_t b, int vec)
{
    const char *z = vec ? "vec3(0.0)" : "0.0";
    const char *o = vec ? "vec3(1.0)" : "1.0";
    switch ((b >> 5) & 7u) {
    case 0: snprintf(out, n, "max(%s, %s)", v, z); break;
    case 1: snprintf(out, n, "(%s - clamp(%s, %s, %s))", o, v, z, o); break;
    case 2: snprintf(out, n, "(2.0 * max(%s, %s) - %s)", v, z, o); break;
    case 3: snprintf(out, n, "(-2.0 * max(%s, %s) + %s)", v, z, o); break;
    case 4: snprintf(out, n, "(max(%s, %s) - 0.5)", v, z); break;
    case 5: snprintf(out, n, "(-max(%s, %s) + 0.5)", v, z); break;
    case 6: snprintf(out, n, "(%s)", v); break;
    default: snprintf(out, n, "(-(%s))", v); break;
    }
}

/* rd_rgb / rd_a: register b&15, alpha replicated when bit 4 is set. */
static void in_rgb(char *out, size_t n, uint32_t b)
{
    char v[32];
    if (b & 16u)
        snprintf(v, sizeof v, "vec3(r%u.a)", b & 15u);
    else
        snprintf(v, sizeof v, "r%u.rgb", b & 15u);
    mapped(out, n, v, b, 1);
}

static void in_a(char *out, size_t n, uint32_t b)
{
    char v[16];
    snprintf(v, sizeof v, "r%u.%c", b & 15u, (b & 16u) ? 'a' : 'b');
    mapped(out, n, v, b, 0);
}

/* map_out: the stage's output scale/bias. */
static void outmap(char *out, size_t n, const char *v, uint32_t mo)
{
    switch (mo) {
    case 0x08: snprintf(out, n, "(%s - 0.5)", v); break;
    case 0x10: snprintf(out, n, "(%s * 2.0)", v); break;
    case 0x18: snprintf(out, n, "((%s - 0.5) * 2.0)", v); break;
    case 0x20: snprintf(out, n, "(%s * 4.0)", v); break;
    case 0x30: snprintf(out, n, "(%s * 0.5)", v); break;
    default:   snprintf(out, n, "(%s)", v); break;
    }
}

static const char *HEADER =
    "#version 450\n"
    "layout(location=0) in vec4 vD0;\n"
    "layout(location=1) in vec4 vD1;\n"
    "layout(location=2) in vec4 vFog;\n"
    "layout(location=3) in vec4 vT[4];\n"
    "layout(set=0, binding=0) uniform sampler2D tex[4];\n"
    "layout(std140, set=0, binding=1) uniform RC {\n"
    "  uvec4 cicw[2]; uvec4 aicw[2]; uvec4 cocw[2]; uvec4 aocw[2]; uvec4 f0[2]; uvec4 f1[2];\n"
    "  uvec4 fin; uvec4 ctl; uvec4 misc; vec4 fogp; vec4 clampmin[2]; uvec4 shadow; vec4 shadowmax;\n"
    "  uvec4 border; uvec4 bmask; } rc;\n"
    "layout(push_constant) uniform PC { vec4 surf; vec4 texscale[4]; ivec4 alpha; vec4 clampmax[2]; } pc;\n"
    "layout(location=0) out vec4 outColor;\n"
    "vec4 unpk(uint c) { return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u),\n"
    "                                float(c & 255u), float(c >> 24)) / 255.0; }\n";

/* samp_N: FS_SRC's samp() for one stage, the border mask decided here. */
static void gen_samp(Gen *g, int st, uint32_t bm)
{
    const char *cm = (st & 1) ? "zw" : "xy";
    put(g, "vec4 samp%d(vec2 uv) {\n", st);
    put(g, "  vec2 mx = pc.clampmax[%d].%s, mn = rc.clampmin[%d].%s;\n", st >> 1, cm, st >> 1, cm);
    if (bm) {
        put(g, "  vec2 lim = mx.x > 0.0 ? mx + mn : vec2(1.0);\n");
        put(g, "  if (false");
        if (bm & 1u) put(g, " || uv.x < 0.0 || uv.x > lim.x");
        if (bm & 2u) put(g, " || uv.y < 0.0 || uv.y > lim.y");
        put(g, ") return unpk(rc.border[%d]);\n", st);
    }
    put(g, "  if (mx.x > 0.0) uv = clamp(uv, mn, mx);\n");
    put(g, "  return texture(tex[%d], uv);\n}\n", st);
}

size_t vk_psh_gen(const uint32_t *spec, int zofs, char *buf, size_t cap)
{
    Gen g = { buf, cap, 0, 0 };
    uint32_t sp = spec[33], ctl = spec[32], flags = ctl >> 8;
    uint32_t n = ctl & 255u, s;
    int st, need_mux = 0;

    if (n > 8) n = 8;
    put(&g, "%s", HEADER);
    for (st = 0; st < 4; st++)
        gen_samp(&g, st, (spec[40] >> (st * 2)) & 3u);
    /* fog_factor, the mode decided here. */
    put(&g, "float fog_factor(float d) {\n");
    if (!spec[42]) {
        put(&g, "  return 1.0;\n}\n");
    } else {
        uint32_t m = spec[43];
        put(&g, "  float f, px = rc.fogp.x, py = rc.fogp.y;\n");
        if (m == 0x800u || m == 0x802u)
            put(&g, "  f = px + exp2(d * py * 16.0) - 1.5;\n");
        else if (m == 0x801u || m == 0x803u)
            put(&g, "  f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n");
        else
            put(&g, "  f = px + d * py - 1.0;\n");
        if (m == 0x802u || m == 0x803u || m == 0x804u)
            put(&g, "  f = abs(f);\n");
        put(&g, "  if (isnan(f)) f = 1.0;\n  return clamp(f, 0.0, 1.0);\n}\n");
    }
    put(&g, "void main() {\n");
    if (zofs)
        put(&g, "  float zu = gl_FragCoord.z / pc.surf.z;\n"
                "  float zsl = max(abs(dFdx(zu)), abs(dFdy(zu))) * max(pc.surf.w, 1.0);\n");
    put(&g, "  vec4 c;\n");
    if (!spec[35]) {
        /* No combiner state yet: texture 0 times diffuse. */
        put(&g, "  c = vD0;\n"
                "  if (pc.texscale[0].z != 0.0) {\n"
                "    vec4 t = vT[0]; vec2 uv = (t.w != 0.0 ? t.xy / t.w : t.xy) * pc.texscale[0].xy;\n"
                "    c *= samp0(uv);\n  }\n");
    } else {
        /* Texture stages. */
        for (st = 0; st < 4; st++) {
            uint32_t mode = (sp >> (st * 5)) & 31u;
            put(&g, "  vec4 t%d;\n  {\n    vec4 tc = vT[%d]; float q = tc.w != 0.0 ? tc.w : 1.0;\n", st, st);
            if (mode == 0) {
                put(&g, "    t%d = vec4(0.0, 0.0, 0.0, 1.0);\n", st);
            } else if ((mode == 1 || mode == 2) && ((spec[38] >> st) & 1u)) {
                static const char *cmp[8] = { "false", "d < z", "d == z", "d <= z",
                                              "d > z", "d != z", "d >= z", "true" };
                put(&g, "    float d = samp%d(tc.xy / q * pc.texscale[%d].xy).r * rc.shadowmax[%d];\n"
                        "    float z = clamp(tc.z / q, 0.0, rc.shadowmax[%d]);\n"
                        "    bool r = %s;\n"
                        "    if (rc.shadow.z == 1u) r = false; else if (rc.shadow.z == 2u) r = true;\n"
                        "    t%d = vec4(r ? 1.0 : 0.0);\n",
                    st, st, st, st, cmp[spec[39] & 7u], st);
            } else if (mode == 1 || mode == 2 || mode == 3) {
                put(&g, "    t%d = samp%d(tc.xy / q * pc.texscale[%d].xy);\n", st, st, st);
            } else if (mode == 4) {
                put(&g, "    t%d = clamp(tc, 0.0, 1.0);\n", st);
            } else if (mode == 5) {
                int j;
                for (j = 0; j < 4; j++) {
                    int ge = (spec[41] >> (st * 4 + j)) & 1u;
                    put(&g, "    if (tc[%d] %s 0.0) discard;\n", j, ge ? ">=" : "<");
                }
                put(&g, "    t%d = vec4(0.0);\n", st);
            } else if ((mode == 15 || mode == 16) && st >= 1) {
                int src = st <= 1 ? 0 : (int)((spec[34] >> (st == 2 ? 16 : 20)) & 15u) % st;
                if (mode == 15)
                    put(&g, "    t%d = samp%d(vec2(t%d.a, t%d.r) * pc.texscale[%d].xy);\n",
                        st, st, src, src, st);
                else
                    put(&g, "    t%d = samp%d(vec2(t%d.g, t%d.b) * pc.texscale[%d].xy);\n",
                        st, st, src, src, st);
            } else {
                put(&g, "    t%d = vec4(0.0);\n", st);
            }
            put(&g, "  }\n");
        }
        /* Registers. */
        put(&g, "  vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0);\n"
                /* FOG_COLOR: red in bits 0-7, the reverse of unpk's
                 * D3DCOLOR -- as rgb, far terrain went teal. */
                "  vec4 r3 = vec4(unpk(rc.misc.z).bgr, fog_factor(vFog.x));\n"
                "  vec4 r4 = vD0, r5 = vD1, r6 = vec4(0.0), r7 = vec4(0.0);\n"
                "  vec4 r8 = t0, r9 = t1, r10 = t2, r11 = t3;\n"
                "  vec4 r12 = vec4(0.0), r13 = vec4(0.0), r14 = vec4(0.0), r15 = vec4(0.0);\n");
        put(&g, "  r12.a = %s;\n", (sp & 31u) ? "t0.a" : "1.0");
        /* Combiner stages. */
        for (s = 0; s < n; s++) {
            uint32_t icw = spec[0 + s], aicw = spec[8 + s];
            uint32_t ocw = spec[16 + s], aocw = spec[24 + s];
            uint32_t fl = ocw >> 12, afl = aocw >> 12;
            uint32_t mo = fl & 0x38u, amo = afl & 0x38u, d;
            char A[96], Bv[96], C[96], D[96], aA[96], aB[96], aC[96], aD[96], t1b[128];

            in_rgb(A, sizeof A, icw >> 24);  in_rgb(Bv, sizeof Bv, icw >> 16);
            in_rgb(C, sizeof C, icw >> 8);   in_rgb(D, sizeof D, icw);
            in_a(aA, sizeof aA, aicw >> 24); in_a(aB, sizeof aB, aicw >> 16);
            in_a(aC, sizeof aC, aicw >> 8);  in_a(aD, sizeof aD, aicw);
            put(&g, "  {\n");
            put(&g, "    r1 = unpk(rc.f0[%u][%u]);\n", ((flags & 0x010u) ? s : 0u) >> 2,
                ((flags & 0x010u) ? s : 0u) & 3u);
            put(&g, "    r2 = unpk(rc.f1[%u][%u]);\n", ((flags & 0x100u) ? s : 0u) >> 2,
                ((flags & 0x100u) ? s : 0u) & 3u);
            if ((fl & 4u) || (afl & 4u)) {
                need_mux = 1;
                if (flags & 1u)
                    put(&g, "    bool mux = r12.a >= 0.5;\n");
                else
                    put(&g, "    bool mux = (int(r12.a * 255.0) & 1) != 0;\n");
            }
            put(&g, "    vec3 A = %s, B = %s, C = %s, D = %s;\n", A, Bv, C, D);
            put(&g, "    float aA = %s, aB = %s, aC = %s, aD = %s;\n", aA, aB, aC, aD);
            put(&g, "    vec3 pab = %s;\n", (fl & 2u) ? "vec3(dot(A, B))" : "A * B");
            put(&g, "    vec3 pcd = %s;\n", (fl & 1u) ? "vec3(dot(C, D))" : "C * D");
            put(&g, "    vec3 sum = %s;\n", (fl & 4u) ? "(mux ? pcd : pab)" : "pab + pcd");
            outmap(t1b, sizeof t1b, "pab", mo);
            put(&g, "    vec3 ab = clamp(%s, -1.0, 1.0);\n", t1b);
            outmap(t1b, sizeof t1b, "pcd", mo);
            put(&g, "    vec3 cd = clamp(%s, -1.0, 1.0);\n", t1b);
            outmap(t1b, sizeof t1b, "sum", mo);
            put(&g, "    vec3 ms = clamp(%s, -1.0, 1.0);\n", t1b);
            put(&g, "    float apab = aA * aB, apcd = aC * aD;\n");
            put(&g, "    float asum = %s;\n", (afl & 4u) ? "(mux ? apcd : apab)" : "apab + apcd");
            outmap(t1b, sizeof t1b, "apab", amo);
            put(&g, "    float aab = clamp(%s, -1.0, 1.0);\n", t1b);
            outmap(t1b, sizeof t1b, "apcd", amo);
            put(&g, "    float acd = clamp(%s, -1.0, 1.0);\n", t1b);
            outmap(t1b, sizeof t1b, "asum", amo);
            put(&g, "    float ams = clamp(%s, -1.0, 1.0);\n", t1b);
            d = (ocw >> 4) & 15u;
            if (d) {
                put(&g, "    r%u.rgb = ab;\n", d);
                if (fl & 0x80u) put(&g, "    r%u.a = ab.z;\n", d);
            }
            d = ocw & 15u;
            if (d) {
                put(&g, "    r%u.rgb = cd;\n", d);
                if (fl & 0x40u) put(&g, "    r%u.a = cd.z;\n", d);
            }
            d = (ocw >> 8) & 15u;
            if (d) put(&g, "    r%u.rgb = ms;\n", d);
            d = (aocw >> 4) & 15u;
            if (d) put(&g, "    r%u.a = aab;\n", d);
            d = aocw & 15u;
            if (d) put(&g, "    r%u.a = acd;\n", d);
            d = (aocw >> 8) & 15u;
            if (d) put(&g, "    r%u.a = ams;\n", d);
            put(&g, "    r0 = vec4(0.0);\n  }\n");
        }
        (void)need_mux;
        /* Final combiner. */
        if (spec[36] || spec[37]) {
            uint32_t f0 = spec[36], f1 = spec[37], ff = f1 & 255u;
            char A[96], Bv[96], C[96], D[96], E[96], F[96], G[96];
            put(&g, "  r1 = unpk(rc.fin.z); r2 = unpk(rc.fin.w);\n");
            put(&g, "  {\n    vec3 a = %s, b = %s;\n",
                (ff & 0x40u) ? "1.0 - r5.rgb" : "r5.rgb",
                (ff & 0x20u) ? "1.0 - r12.rgb" : "r12.rgb");
            put(&g, "    r14 = vec4(%s, 0.0);\n  }\n",
                (ff & 0x80u) ? "clamp(a + b, 0.0, 1.0)" : "a + b");
            in_rgb(E, sizeof E, f1 >> 24);
            in_rgb(F, sizeof F, f1 >> 16);
            put(&g, "  r15 = vec4(%s * %s, 0.0);\n", E, F);
            in_rgb(A, sizeof A, f0 >> 24); in_rgb(Bv, sizeof Bv, f0 >> 16);
            in_rgb(C, sizeof C, f0 >> 8);  in_rgb(D, sizeof D, f0);
            in_a(G, sizeof G, f1 >> 8);
            put(&g, "  {\n    vec3 A = %s;\n", A);
            put(&g, "    c = vec4(%s + %s * (1.0 - A) + %s * A, %s);\n  }\n", D, C, Bv, G);
        } else {
            put(&g, "  c = r12;\n");
        }
    }
    put(&g, "  c = clamp(c, 0.0, 1.0);\n");
    if (spec[44]) {
        static const char *cmp[8] = { "false", "v < r", "v == r", "v <= r",
                                      "v > r", "v != r", "v >= r", "true" };
        uint32_t f = spec[45];
        put(&g, "  {\n    int v = int(c.a * 255.0 + 0.5), r = %u;\n", spec[46] & 255u);
        put(&g, "    if (!(%s)) discard;\n  }\n",
            (f >= 0x200 && f <= 0x207) ? cmp[f - 0x200] : "true");
    }
    put(&g, "  outColor = c;\n");
    if (zofs)
        put(&g, "  gl_FragDepth = clamp((zu + rc.fogp.w + rc.fogp.z * zsl) * pc.surf.z, 0.0, 1.0);\n");
    put(&g, "}\n");
    return g.over ? 0 : g.len;
}
