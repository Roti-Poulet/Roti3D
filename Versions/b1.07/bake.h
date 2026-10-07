/* bake.h - CPU bake (soft shadows, AO, color bounce, reflections) + disk cache
 *
 * Light is baked as "unit" (white color, intensity 1): E = color*intensity * T + ambient * A.
 * => color / intensity / ambient / exposure can be changed live without re-baking (refresh_lighting).
 * Only the lamp position and size require a new bake (cached).
 *
 * v9: indirect lighting (bounces, caustics) is baked for light 0 as UNIT (white, intensity 1) and tinted
 * by the COLOR of light 0 during compositing: T = sum_l(color_l x mask_l) + color_0 x (bounce + caustic).
 * This is exact for a single light (the case for the maps): changing the color / intensity needs NO re-bake, and the
 * colored light correctly bounces with the tint of the surfaces AND of the lamp.
 */
#pragma once
#define BAKE_VERSION 12

static void loading_frame(float frac);            /* defined in render.h */
static volatile LONG g_abort;
static void models_prepare(void); static void bake_models(void);            /* modelbake.h */
static int model_cache_read(FILE* f); static void model_cache_write(FILE* f);
static double g_ph[6];                            /* durations of the phases of the last bake (s): direct, indirect, finalization, reflections */
static double bake_now(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }

static V3 g_bl; static float g_brad;              /* light frozen during a bake */


/* ---------- small generic pool: par_for(n, fn, ctx) ---------- */
typedef void (*Pfn)(void* ctx, int idx);
static HANDLE g_th[64]; static Pfn g_pfn; static void* g_pctx; static int g_pn; static volatile LONG g_pnext;
static DWORD WINAPI pworker(LPVOID a) {
    (void)a;
    for (;;) {
        LONG i = InterlockedIncrement(&g_pnext) - 1;
        if (i >= g_pn) break;
        g_pfn(g_pctx, (int)i);
    }
    return 0;
}
static void par_for(int n, Pfn fn, void* ctx) {
    SYSTEM_INFO si; int i, nth;
    if (n <= 0) return;
    if (n < 4) { for (i = 0; i < n; i++) fn(ctx, i); return; }
    GetSystemInfo(&si);
    nth = C.bake_threads > 0 ? C.bake_threads : (int)si.dwNumberOfProcessors;
    if (nth > n) nth = n;
    if (nth > 64) nth = 64;
    if (nth < 1) nth = 1;
    g_pfn = fn; g_pctx = ctx; g_pn = n; g_pnext = 0;
    for (i = 0; i < nth; i++) {
        g_th[i] = CreateThread(NULL, 0, pworker, NULL, 0, NULL);
        if (!g_th[i]) { nth = i; break; }
    }
    for (i = 0; i < nth; i++) { WaitForSingleObject(g_th[i], INFINITE); CloseHandle(g_th[i]); }
}

static inline float rnd(uint32_t* s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}
static void basis(V3 n, V3* t, V3* b) {
    V3 a = fabsf(n.x) > 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
    *t = vnorm(vcross(n, a)); *b = vcross(n, *t);
}

/* ---------- RGBE (4 octets / texel) ---------- */
static float g_exp_tab[256];
static void rgbe_init(void) { int i; for (i = 0; i < 256; i++) g_exp_tab[i] = ldexpf(1.0f, i - 136); }
static uint32_t rgbe_pack(float r, float g, float b) {
    float m = fmaxf(r, fmaxf(g, b)), f; int e;
    if (m < 1e-12f) return 0;
    f = frexpf(m, &e) * 256.0f / m;
    return (uint32_t)(r * f) | ((uint32_t)(g * f) << 8) | ((uint32_t)(b * f) << 16) | ((uint32_t)(e + 128) << 24);
}
static inline V3 rgbe_unpack(uint32_t p) {
    float f = g_exp_tab[p >> 24];
    return v3((float)(p & 255) * f, (float)((p >> 8) & 255) * f, (float)((p >> 16) & 255) * f);
}

/* ---------- direct lighting (spherical light) ---------- */
static float* g_sph; static float* g_sqr; static int g_nsph;            /* points on the lamp sphere (Fibonacci), interleaved order */
static int gcd_i(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
static void sph_init(int ns) {
    int i, stride; if (ns < 1) ns = 1;
    free(g_sph); g_sph = (float*)malloc((size_t)ns * 3 * sizeof(float)); g_nsph = ns;
    free(g_sqr); g_sqr = (float*)malloc((size_t)ns * 2 * sizeof(float));
    for (i = 0; i < ns; i++) {                      /* R2 sequence (low discrepancy) for square lights */
        float u = 0.5f + 0.7548776662f * (float)(i + 1), v = 0.5f + 0.5698402910f * (float)(i + 1);
        g_sqr[2 * i] = u - floorf(u); g_sqr[2 * i + 1] = v - floorf(v);
    }
    stride = (int)(0.618034f * ns + 0.5f); if (stride < 1) stride = 1;
    while (gcd_i(stride, ns) != 1) stride++;
    for (i = 0; i < ns; i++) {                      /* the first 8 rays are already well spread over the sphere */
        int k = (int)(((long long)i * stride) % ns); float z = 1.0f - (2.0f * k + 1.0f) / ns, r = sqrtf(fmaxf(0, 1 - z * z)), ph = k * 2.39996323f;
        g_sph[3 * i] = r * cosf(ph); g_sph[3 * i + 1] = z; g_sph[3 * i + 2] = r * sinf(ph);
    }
}

/* Direct irradiance of a light, as luminance (the bake only stores the intensity;
 * the color is applied later, from the masks S[]). Early exit:
 * light below the horizon -> 0; if the first 8 rays are all lit or all
 * blocked, the texel is not in the penumbra: no need to draw more. */
static float direct_irr1(V3 p, V3 n, const Light* lp, int full) {
    int ns = g_nsph, i, lit = 0, shadow = 0, first = ns > 16 ? 8 : ns; float acc = 0, a0, ca, sa, cl = 1.0f, ou = 0, ov = 0;
    V3 org = vadd(p, vmul(n, 0.004f));
    if (lp->shape) {                                   /* ceiling light: only lights downward (emission cosine, direction of the center) */
        V3 w0 = vsub(lp->pos, p); float l0 = vlen(w0);
        cl = l0 > 1e-6f ? w0.y / l0 : 0.0f;
        if (cl <= 0.0f) return 0;
    } else if (vdot(n, vsub(lp->pos, p)) < -lp->radius) return 0;
    { uint32_t h = (uint32_t)(p.x * 7919.0f) * 73856093u ^ (uint32_t)(p.y * 7919.0f) * 19349663u ^ (uint32_t)(p.z * 7919.0f) * 83492791u;
      h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; a0 = (h & 0xFFFF) * (2 * PI / 65536.0f);   /* rotation specific to the point */
      ou = (h & 0xFFFF) * (1.0f / 65536.0f); ov = ((h >> 16) & 0xFFFF) * (1.0f / 65536.0f); }   /* offset specific to the point (square) */
    ca = cosf(a0); sa = sinf(a0);
    for (i = 0; i < ns; i++) {
        V3 ls;
        if (lp->shape) {
            float ux = g_sqr[2 * i] + ou, uy = g_sqr[2 * i + 1] + ov;
            ux -= floorf(ux); uy -= floorf(uy);
            ls = v3(lp->pos.x + (ux * 2 - 1) * lp->radius, lp->pos.y, lp->pos.z + (uy * 2 - 1) * lp->radius);
        } else {
            float sx = g_sph[3 * i], sy = g_sph[3 * i + 1], sz = g_sph[3 * i + 2];
            ls = vadd(lp->pos, vmul(v3(sx * ca - sz * sa, sy, sx * sa + sz * ca), lp->radius));
        }
        {   V3 w = vsub(ls, p); float d2 = vdot(w, w), d = sqrtf(d2), ndl; V3 L = vmul(w, 1.0f / d);
            ndl = vdot(n, L);
            if (ndl > 0) {
                if (!occluded(org, L, d - 0.02f)) { acc += ndl / d2; lit++; } else shadow++;
            }
        }
        if (!full && i + 1 == first && ns > first && (lit == 0 || shadow == 0)) return cl * acc / (float)(i + 1);
    }
    return cl * acc / ns;
}

/* Lights without shadows only add a diffuse term: no ray tracing,
 * so 6 lights cost about 1 light (instead of x6).
 * gvis >= 0: light visibility already computed by the GPU (fraction of points of the
 * lamp sphere that are not blocked) -> E = visibility x irradiance at the lamp center. */
static float direct_irr(V3 p, V3 n, int li, float gvis, int full) {
    const Light* lp = &g_lightset[li];
    V3 w; float d2, ndl;
    if (!lp->shadow || gvis >= 0.0f) {
        float inv, cl = 1.0f;
        w = vsub(lp->pos, p); d2 = vdot(w, w);
        if (d2 < 1e-6f) return 0.0f;
        inv = 1.0f / sqrtf(d2);
        ndl = vdot(n, vmul(w, inv));
        if (lp->shape) cl = w.y * inv;                    /* ceiling light: emission cosine */
        if (ndl <= 0 || cl <= 0) return 0.0f;
        return (lp->shadow ? gvis : 1.0f) * ndl * cl / d2;
    }
    return direct_irr1(p, n, lp, full);
}

#include "gi.h"

/* indirect (AO + multiple bounces), smooth: computed only at the nodes of a reduced grid, then interpolated */
static void bake_indirect(V3 p, V3 n, uint32_t* rs, V3* B, V3* A) {
    float vis, f; V3 bounce;
    gi_gather(vadd(p, vmul(n, 0.004f)), n, rs, &bounce, &vis);
    *B = vmul(bounce, C.bounce_strength);
    f = 1.0f - C.ao_strength * (1.0f - vis);
    *A = v3(f, f, f);
}

/* reflection rendering by rasterization (render.h): returns 1 if the GPU produced the G-buffer */
static int bake_reflections_gpu(void);
/* soft shadows by the GPU (gpubake.h): fills o->gv[l]; returns 1 if the GPU computed all the shadow-casting lights */
static int bake_direct_gpu(void);
static int bake_indirect_gpu(void);               /* gpugi.h: AO + bounces by the GPU (1 = fA/fB filled for all lightmaps) */
static int g_gi_gpu = 0;
static int gpu_probe(void);                        /* 1 if the GPU can do the bake (SM 2.0 + render cubemaps) */
static int g_gpu_K = 0, g_gpu_direct = 0;          /* number of GPU shadow rays per texel; 1 if this bake uses the GPU */

/* ---------- multithreaded jobs: pass 0 = direct (shadows), pass 1 = indirect (bounces, reads pass 0) ---------- */
typedef struct { int obj, quad, y; } Job;
static Job* g_jobs; static int g_njobs, g_phase;
static volatile LONG g_next, g_done;

static volatile LONG g_rt_refined;                 /* texels (per light) recomputed by ray tracing (bench.log stats) */

/* Ray-traced shadows, hybrid mode: the GPU (shadow maps) classifies each texel, the CPU only re-traces the
 * texels where the shadow map is unreliable: penumbra (partial visibility) and the neighborhood of a light/shadow
 * edge (rt_shadow_margin texels). The rest (large lit areas or hard shadow)
 * keeps the GPU value, identical to what the rays would give. */
static int rt_needs_rays(const Obj* o, const Quad* q, int l, int lx, int ly) {
    const uint8_t* gv = o->gv[l]; int W = q->tw + 2, H = q->th + 2, m = C.rt_shadow_margin, dx, dy;
    uint8_t c = gv[(size_t)(q->ty + ly) * o->aw + q->tx + lx];
    if (c != 0 && c != (uint8_t)g_gpu_K) return 1;                   /* penumbra */
    for (dy = -m; dy <= m; dy++) for (dx = -m; dx <= m; dx++) {
        int nx = lx + dx, ny = ly + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        if (gv[(size_t)(q->ty + ny) * o->aw + q->tx + nx] != c) return 1;   /* shadow edge */
    }
    return 0;
}

static inline int is_node(int l, int n) { int st = C.indirect_step < 1 ? 1 : C.indirect_step; return (l % st == 0) || l == n - 1; }

static void bake_row(int ji) {
    Job* j = &g_jobs[ji]; Obj* o = &g_obj[j->obj]; Quad* q = &o->q[j->quad];
    int x, y = j->y - 1, W = q->tw + 2, H = q->th + 2;
    for (x = -1; x <= q->tw; x++) {
        float s = clampf((x + 0.5f) / q->tw, 0, 1), t = clampf((y + 0.5f) / q->th, 0, 1);
        V3 n, p = quad_at(q, s, t, &n);
        int lx = x + 1; size_t k = ((size_t)(q->ty + y + 1) * o->aw + q->tx + x + 1) * 3;
        int cov = q->cover && covered(p, q->box);                 /* face hidden by another object: no bake */
        if (q->tri && s + t > 1.0f) continue;                      /* outside the triangle: wasted if computed */
        if (g_phase == 0) {
            /* a shading mask per light and per texel (RGBE: intensity only,
             * the color is applied later by compose_light) */
            int l; size_t ks = (size_t)(q->ty + y + 1) * o->aw + q->tx + x + 1; float d0 = 0.0f;
            for (l = 0; l < g_nlight; l++) {
                float gvis = (o->gv[l] && g_gpu_K > 0) ? (float)o->gv[l][ks] / (float)g_gpu_K : -1.0f;
                int full = 0; float d;
                if (C.rt_shadows && !cov && g_lightset[l].shadow) {
                    if (gvis >= 0.0f && rt_needs_rays(o, q, l, x + 1, y + 1)) gvis = -1.0f;   /* real rays in the doubtful zone */
                    if (gvis < 0.0f) { full = 1; InterlockedIncrement(&g_rt_refined); }
                }
                d = cov ? 0.0f : direct_irr(p, n, l, gvis, full);
                o->S[l][ks] = rgbe_pack(d, d, d);
                if (l == 0) d0 = d;
            }
            /* direct light of light 0 as UNIT: this is what the bounces read back (gi.h) */
            o->fT[ks * 3] = d0;
        } else {
            V3 B = { 0, 0, 0 }, A; uint32_t rs;
            if (!is_node(lx, W)) continue;
            /* seed derived from the absolute position in the atlas (k) and the object, not from the job index (ji):
               ji depends on the order rows are assigned, and correlates noise between neighboring nodes (visible bands/checkerboard) */
            rs = ((uint32_t)j->obj * 0x27d4eb2fu) ^ ((uint32_t)k * 73856093u) ^ ((uint32_t)((uint64_t)k >> 32) * 19349663u) ^ 0x9E3779B9u;
            rs |= 1; rnd(&rs); rnd(&rs);
            if (cov) A = v3(1.0f - C.ao_strength, 1.0f - C.ao_strength, 1.0f - C.ao_strength);
            else bake_indirect(p, n, &rs, &B, &A);
            o->fB[k] = B.x; o->fB[k + 1] = B.y; o->fB[k + 2] = B.z;
            o->fA[k] = A.x; o->fA[k + 1] = A.y; o->fA[k + 2] = A.z;
        }
        (void)H;
    }
}

/* bilinear interpolation of the indirect light between nodes (A = visibility/AO AND B = bounce: both are computed
 * only at the grid nodes; B must be interpolated like A, otherwise the 3x3 blur dilutes it to 1/9 and it appears in blocks). */
static void interp_buf(Obj* o, float* buf) {
    int qi, x, y, c, st = C.indirect_step < 1 ? 1 : C.indirect_step; float* A2 = (float*)malloc((size_t)o->aw * o->ah * 3 * sizeof(float));
    memcpy(A2, buf, (size_t)o->aw * o->ah * 3 * sizeof(float));
    for (qi = 0; qi < o->nq; qi++) {
        Quad* q = &o->q[qi]; int W = q->tw + 2, H = q->th + 2;
        for (y = 0; y < H; y++) {
            int y0 = (y / st) * st, y1 = y0 + st < H - 1 ? y0 + st : H - 1; float wy = (y1 > y0) ? (float)(y - y0) / (y1 - y0) : 0;
            if (is_node(y, H)) { y0 = y; y1 = y; wy = 0; }
            for (x = 0; x < W; x++) {
                int x0 = (x / st) * st, x1 = x0 + st < W - 1 ? x0 + st : W - 1; float wx = (x1 > x0) ? (float)(x - x0) / (x1 - x0) : 0;
                size_t k = ((size_t)(q->ty + y) * o->aw + q->tx + x) * 3;
                if (is_node(x, W)) { x0 = x; x1 = x; wx = 0; }
                for (c = 0; c < 3; c++) {
                    size_t a = ((size_t)(q->ty + y0) * o->aw + q->tx + x0) * 3 + c, b = ((size_t)(q->ty + y0) * o->aw + q->tx + x1) * 3 + c,
                           d = ((size_t)(q->ty + y1) * o->aw + q->tx + x0) * 3 + c, e = ((size_t)(q->ty + y1) * o->aw + q->tx + x1) * 3 + c;
                    float am = (A2[a] * (1 - wx) + A2[b] * wx) * (1 - wy) + (A2[d] * (1 - wx) + A2[e] * wx) * wy;
                    buf[k + c] = am;
                }
            }
        }
    }
    free(A2);
}
static void interp_indirect(Obj* o) { interp_buf(o, o->fA); interp_buf(o, o->fB); }
static DWORD WINAPI bake_thread(LPVOID a) {
    (void)a;
    for (;;) {
        LONG j = InterlockedIncrement(&g_next) - 1;
        if (j >= g_njobs || g_abort) break;
        bake_row(j);
        InterlockedIncrement(&g_done);
    }
    return 0;
}

/* same 3x3 blur per tile, on an RGBE buffer (light masks) */
static void blur_rgb(Obj* o, uint32_t* buf) {
    int qi, p, x, y; uint32_t* tmp;
    if (C.blur_passes <= 0) return;
    tmp = (uint32_t*)malloc((size_t)(o->aw + 2) * (o->ah + 2) * sizeof(uint32_t));
    for (qi = 0; qi < o->nq; qi++) {
        Quad* q = &o->q[qi]; int W = q->tw + 2, H = q->th + 2;
        for (p = 0; p < C.blur_passes; p++) {
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                V3 s = { 0, 0, 0 }; int n = 0, dx, dy;
                for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    s = vadd(s, rgbe_unpack(buf[((size_t)(q->ty + yy) * o->aw + q->tx + xx)])); n++;
                }
                tmp[(size_t)y * W + x] = rgbe_pack(s.x / n, s.y / n, s.z / n);
            }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++)
                buf[((size_t)(q->ty + y) * o->aw + q->tx + x)] = tmp[(size_t)y * W + x];
        }
    }
    free(tmp);
}

static void blur_buf_n(Obj* o, float* buf, int passes) {
    int qi, p, x, y, c; float* tmp;
    if (passes <= 0) return;
    tmp = (float*)malloc((size_t)(o->aw + 2) * (o->ah + 2) * 3 * sizeof(float));
    for (qi = 0; qi < o->nq; qi++) {
        Quad* q = &o->q[qi]; int W = q->tw + 2, H = q->th + 2;
        for (p = 0; p < passes; p++) {
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) for (c = 0; c < 3; c++) {
                float s = 0; int n = 0, dx, dy;
                for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    s += buf[((size_t)(q->ty + yy) * o->aw + q->tx + xx) * 3 + c]; n++;
                }
                tmp[((size_t)y * W + x) * 3 + c] = s / n;
            }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) for (c = 0; c < 3; c++)
                buf[((size_t)(q->ty + y) * o->aw + q->tx + x) * 3 + c] = tmp[((size_t)y * W + x) * 3 + c];
        }
    }
    free(tmp);
}

static void blur_buf(Obj* o, float* buf) { blur_buf_n(o, buf, C.blur_passes); }

/* ---------- reflections: ray hits (independent of color/intensity) ---------- */
static const V3 CUBE_LOOK[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const V3 CUBE_UP[6]   = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

static V3 obj_probe(const Obj* o) { return (o->textured && !o->is_model) ? v3(0, 0.6f, 0) : o->bs_c; }

/* One spot = one face of a cubemap. The RNG stream is pre-generated in the exact order
 * of the original code (per ray), so the result is bit-exact despite the parallelism. */
typedef struct { int obj, face, w0; } ReflJob;
static ReflJob g_rjob[6 * MAXOBJ];
static float* g_rrnd;                              /* 2 random draws per ray */

static void refl_face(void* ctx, int i) {
    const ReflJob* j = &g_rjob[i]; Obj* o = &g_obj[j->obj];
    int S = C.reflection_size, f = j->face, x, y, k;
    V3 L = CUBE_LOOK[f], U = CUBE_UP[f], R = vcross(U, L);
    V3 probe = obj_probe(o); float spread = o->rough * 1.2f;
    for (y = 0; y < S; y++) for (x = 0; x < S; x++) for (k = 0; k < o->nsamp; k++) {
        int w = ((f * S + y) * S + x) * o->nsamp + k;
        float s = (x + 0.5f) / S * 2 - 1, t = (y + 0.5f) / S * 2 - 1; Hit h; unsigned char* ch = &o->gbuf[(size_t)w * 4];
        V3 d = vnorm(vsub(vadd(L, vmul(R, s)), vmul(U, t)));
        if (o->nsamp > 1) {                          /* roughness: cone of rays */
            V3 tt, bb; basis(d, &tt, &bb);
            /* draw order: in the original code the two rnd() calls are in the
             * same expression, so GCC evaluates them right to left: the first
             * first draw of the pair goes to term bb, the second to term tt. */
            d = vnorm(vadd(d, vadd(vmul(tt, (g_rrnd[j->w0 + 2 * w + 1] - 0.5f) * 2 * spread),
                                    vmul(bb, (g_rrnd[j->w0 + 2 * w] - 0.5f) * 2 * spread))));
        }
        if (trace_in(&g_rbvh, probe, d, 1000.0f, &h, j->obj)) {
            const Tri* tr = &g_rbvh.tri[h.tri]; const Obj* ho = &g_obj[tr->obj];
            float w0 = 1 - h.u - h.v, lx = w0 * tr->lu[0] + h.u * tr->lu[1] + h.v * tr->lu[2],
                  ly = w0 * tr->lv[0] + h.u * tr->lv[1] + h.v * tr->lv[2];
            int ix = (int)lx, iy = (int)ly;
            if (ix >= ho->aw) ix = ho->aw - 1;
            if (iy >= ho->ah) iy = ho->ah - 1;
            if (ix < 0) ix = 0;
            if (iy < 0) iy = 0;
            gb_set(ch, tr->obj, iy * ho->aw + ix);
        } else gb_set(ch, GB_SKY, 0);
    }
}

/* Pre-generates the RNG stream of each object in the same order as the code
 * of the original, then runs all faces in parallel: each object has its own
 * slice in g_rrnd, so no sharing between threads. Bit-exact. */
static void bake_reflections_all(void) {
    int oi, f, n = 0, off = 0, total = 0;
    size_t need = 0;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].reflect) need += (size_t)g_obj[oi].ngbuf * 2;
    free(g_rrnd); g_rrnd = (float*)malloc((need ? need : 1) * sizeof(float));
    if (!g_rrnd) return;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; uint32_t rs; int w;
        if (!o->reflect) continue;
        rs = 0x1234567u + (uint32_t)oi * 7919u;
        for (w = 0; w < o->ngbuf; w++) { g_rrnd[off + 2 * w] = rnd(&rs); g_rrnd[off + 2 * w + 1] = rnd(&rs); }
        for (f = 0; f < 6; f++) { g_rjob[n].obj = oi; g_rjob[n].face = f; g_rjob[n].w0 = off; n++; }
        off += o->ngbuf * 2; total += o->ngbuf;
    }
    (void)total;
    if (bake_reflections_gpu()) return;              /* rasterization: nsamp is only used for display */
    par_for(n, refl_face, NULL);                     /* CPU fallback (no ps_2_0, or rendering unavailable) */
}

/* ---------- disk cache ---------- */
static uint64_t g_hash;
static void hash_bytes(const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p; size_t i;
    for (i = 0; i < n; i++) { g_hash ^= c[i]; g_hash *= 1099511628211ULL; }
}
#define HASH_F(x) do { float _v = (float)(x); hash_bytes(&_v, sizeof _v); } while (0)
#define HASH_I(x) do { int _v = (int)(x); hash_bytes(&_v, sizeof _v); } while (0)

static void compute_hash(void) {
    int oi, qi; int ver = BAKE_VERSION;
    g_hash = 1469598103934665603ULL;
    HASH_I(ver);
    HASH_F(C.texels_per_unit); HASH_I(C.lightmap_max_size); HASH_I(C.shadow_samples); HASH_I(C.ao_samples);
    HASH_I(C.blur_passes); HASH_F(C.ao_distance); HASH_F(C.ao_strength); HASH_F(C.bounce_distance); HASH_F(C.bounce_strength); HASH_I(C.bounce_count); HASH_I(C.indirect_step); HASH_I(C.occluder_lod);
    HASH_I(C.reflection_size); HASH_I(C.reflection_samples); HASH_I(C.gamma_correct); HASH_I(C.refl_filter);
    HASH_I(C.bake_gpu && gpu_probe()); HASH_I(C.bake_gpu && C.bake_gpu_reflections && gpu_probe());  /* GPU shadows (fixed lamp points) != CPU shadows (rays) */
    HASH_I(C.bake_gpu_gi && gpu_probe()); if (C.bake_gpu_gi) { HASH_I(C.gpu_gi_vpl); HASH_I(C.gpu_gi_res); HASH_F(C.gpu_gi_bias); HASH_F(C.gpu_gi_ao_gain); }
    HASH_I(C.rt_shadows); if (C.rt_shadows) { HASH_I(C.rt_shadow_samples); HASH_I(C.rt_shadow_margin); }
    HASH_F(C.model_max_edge); HASH_I(C.model_ao_samples); HASH_I(C.model_smooth_passes);
    HASH_I(C.caustics); HASH_I(C.caustic_photons); HASH_I(C.caustic_blur); HASH_F(C.glass_ior);
    HASH_I(g_maxtex);      /* the driver texture limit bounds the atlas size: the cache depends on it */
    /* The cache depends on the light geometry (positions, radii, shadows) but
     * NOT on their color: the color is re-applied after loading (compose_light),
     * which allows changing it without re-baking. */
    {   int l; HASH_I(g_nlight);
        for (l = 0; l < g_nlight; l++) {
            HASH_F(g_lightset[l].pos.x); HASH_F(g_lightset[l].pos.y); HASH_F(g_lightset[l].pos.z);
            HASH_F(g_lightset[l].radius); HASH_I(g_lightset[l].shadow); HASH_I(g_lightset[l].shape);
            /* v9: the bounce (Bo) and caustics are baked for a unit light: the cache no longer depends on the intensity */
        }
    }
    for (oi = 0; oi < g_nobj; oi++) {
        const Obj* o = &g_obj[oi];
        HASH_I(o->nq); HASH_I(o->lightmap); HASH_I(o->reflect); HASH_F(o->rough); HASH_I(o->occluder); HASH_I(o->aw); HASH_I(o->ah);
        HASH_I(o->glass); if (o->glass) { hash_bytes(&o->gc, sizeof o->gc); HASH_F(o->gr); }
        if (o->is_model) { HASH_I(o->nv); HASH_I(o->nt[0]); HASH_I(o->blend); hash_bytes(o->vert, sizeof(Vtx) * (size_t)o->nv); hash_bytes(o->idx[0], (size_t)o->nt[0] * 6); }
        HASH_I(o->in_refl);   /* reflection filter: the cache must change if an object enters or leaves the set */
        for (qi = 0; qi < o->nq; qi++) {
            const Quad* q = &o->q[qi];
            hash_bytes(&q->o, sizeof q->o); hash_bytes(&q->u, sizeof q->u); hash_bytes(&q->v, sizeof q->v);
            hash_bytes(&q->alb, sizeof q->alb); HASH_I(q->gn); HASH_F(q->sr); HASH_I(q->tx); HASH_I(q->ty); HASH_I(q->tw); HASH_I(q->th);
        }
    }
}

static void cache_dir(char* out) {
    GetTempPathA(MAX_PATH - 40, out);
    strcat(out, "d3d9engine_cache\\");
    CreateDirectoryA(out, NULL);
}
static void cache_path(char* out) {
    char d[MAX_PATH]; cache_dir(d);
    sprintf(out, "%s%08lx%08lx.bin", d, (unsigned long)(g_hash >> 32), (unsigned long)(g_hash & 0xFFFFFFFFu));
}

/* average of the lightmap: used to light a distant object with its average color
 * (one texture less per pixel, the lightmap is no longer sampled).
 * Computed on packed T/A, so identical after a bake and after a cache load. */
static void obj_avg_light(Obj* o) {
    size_t n = (size_t)o->aw * o->ah, k; double st[3] = { 0, 0, 0 }, sa[3] = { 0, 0, 0 };
    for (k = 0; k < n; k++) {
        V3 T = rgbe_unpack(o->T[k]), A = rgbe_unpack(o->A[k]);
        st[0] += T.x; st[1] += T.y; st[2] += T.z;
        sa[0] += A.x; sa[1] += A.y; sa[2] += A.z;
    }
    if (n) { o->avgT[0] = (float)(st[0] / n); o->avgT[1] = (float)(st[1] / n); o->avgT[2] = (float)(st[2] / n);
             o->avgA[0] = (float)(sa[0] / n); o->avgA[1] = (float)(sa[1] / n); o->avgA[2] = (float)(sa[2] / n); }
}

/* T = sum (source_color x source_mask): depends only on the colors,
 * so it is fast and redone after each color change without re-tracing rays. */
static void compose_light(Obj* o);

static int cache_load(void) {
    char path[MAX_PATH]; FILE* f; char magic[8]; int oi, ok = 1;
    if (!C.cache_enabled) return 0;
    cache_path(path);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "D3D9LMC5", 8)) ok = 0;
    for (oi = 0; ok && oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int hd[4];
        if (!o->lightmap && !o->reflect) continue;
        if (fread(hd, sizeof hd, 1, f) != 1 || hd[0] != o->aw || hd[1] != o->ah || hd[2] != o->ngbuf || hd[3] != g_nlight) { ok = 0; break; }
        if (o->lightmap) {
            int l, bad = 0; size_t n = (size_t)o->aw * o->ah;
            /* the per-source masks first: T is then recomposed with the current colors */
            for (l = 0; l < g_nlight; l++) if (fread(o->S[l], 4, n, f) != n) bad = 1;
            if (bad) { ok = 0; break; }
            if (fread(o->A, 4, n, f) != n || fread(o->Bo, 4, n, f) != n || fread(o->Ca, 4, n, f) != n) { ok = 0; break; }
        }
        if (o->ngbuf && fread(o->gbuf, 4, (size_t)o->ngbuf, f) != (size_t)o->ngbuf) ok = 0;   /* glass/mirrors without a lightmap also have a G-buffer */
    }
    if (ok) ok = model_cache_read(f);
    fclose(f);
    /* color re-applied after loading: the cache only stores the white lighting */
    if (ok) { int oi; for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].lightmap) compose_light(&g_obj[oi]); }
    return ok;
}

static void cache_purge(void) {
    char d[MAX_PATH], pat[MAX_PATH]; WIN32_FIND_DATAA fd; HANDLE h; static char names[256][MAX_PATH]; static FILETIME ft[256];
    int n = 0, i;
    cache_dir(d); sprintf(pat, "%s*.bin", d);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (n < 256) { sprintf(names[n], "%s%s", d, fd.cFileName); ft[n] = fd.ftLastWriteTime; n++; } } while (FindNextFileA(h, &fd));
    FindClose(h);
    while (n > C.cache_max_files && n > 1) {          /* delete the oldest ones */
        int old = 0;
        for (i = 1; i < n; i++) if (CompareFileTime(&ft[i], &ft[old]) < 0) old = i;
        DeleteFileA(names[old]);
        strcpy(names[old], names[n - 1]); ft[old] = ft[n - 1]; n--;
    }
}

static void cache_save(void) {
    char path[MAX_PATH], tmp[MAX_PATH + 8]; FILE* f; int oi;
    if (!C.cache_enabled) return;
    cache_path(path); sprintf(tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    fwrite("D3D9LMC5", 1, 8, f);
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int hd[4], l; size_t n;
        if (!o->lightmap && !o->reflect) continue;
        n = (size_t)o->aw * o->ah;
        hd[0] = o->aw; hd[1] = o->ah; hd[2] = o->ngbuf; hd[3] = g_nlight;
        fwrite(hd, sizeof hd, 1, f);
        if (o->lightmap) {
            for (l = 0; l < g_nlight; l++) fwrite(o->S[l], 4, n, f);   /* per-source masks (white lighting) */
            fwrite(o->A, 4, n, f); fwrite(o->Bo, 4, n, f); fwrite(o->Ca, 4, n, f);
        }
        if (o->ngbuf) fwrite(o->gbuf, 4, (size_t)o->ngbuf, f);
    }
    model_cache_write(f);
    if (fclose(f) == 0) { DeleteFileA(path); MoveFileA(tmp, path); cache_purge(); } else DeleteFileA(tmp);
}

/* ---------- caustics: photons through glass spheres ----------
 * Glass is opaque to direct light (it casts shadows): all the energy that passes through it is
 * rendered here by photons. Each photon starts at a point on the light, is aimed at the sphere (cone
 * of solid angle omega), undergoes Snell + Fresnel (Russian roulette: reflection or refraction, internal reflections
 * included) and is deposited, by bilinear splat, into the lightmap of the first surface it hits.
 *   photon flux (unit light) = cos_emission x omega / N        E(texel) = flux / texel area
 * The result is an irradiance for a unit white light: the compositing tints it (color of light 0). */
static float fresnel_d(float cosi, float n1, float n2) {          /* unpolarized dielectric reflectance */
    float sini2 = fmaxf(0.0f, 1.0f - cosi * cosi), st2 = (n1 / n2) * (n1 / n2) * sini2, cost, rs, rp;
    if (st2 >= 1.0f) return 1.0f;
    cost = sqrtf(1.0f - st2);
    rs = (n1 * cosi - n2 * cost) / (n1 * cosi + n2 * cost); rp = (n2 * cosi - n1 * cost) / (n2 * cosi + n1 * cost);
    return 0.5f * (rs * rs + rp * rp);
}
static V3 refract_v(V3 d, V3 n, float eta) {                       /* d unit incoming, n unit opposite to d, eta = n1/n2 */
    float c = -vdot(d, n), k = 1.0f - eta * eta * (1.0f - c * c);
    if (k < 0) return v3(0, 0, 0);
    return vadd(vmul(d, eta), vmul(n, eta * c - sqrtf(k)));
}

#define CAUS_TASKS 16
static float* g_cbuf[CAUS_TASKS][MAXOBJ];                         /* private buffer per task and per object: no race between threads */
static int g_caus_glass;

static void caustic_splat(float** tb, const Hit* h, float flux) {
    const Tri* tr = &g_tri[h->tri]; Obj* ho = &g_obj[tr->obj]; float w0 = 1 - h->u - h->v, lx, ly, wa, au, fx, fy, wx, wy; int x0, y0, k;
    if (!ho->lightmap) return;
    lx = w0 * tr->lu[0] + h->u * tr->lu[1] + h->v * tr->lu[2];
    ly = w0 * tr->lv[0] + h->u * tr->lv[1] + h->v * tr->lv[2];
    wa = 0.5f * vlen(tr->N);                                       /* world area of the triangle */
    au = 0.5f * fabsf((tr->lu[1] - tr->lu[0]) * (tr->lv[2] - tr->lv[0]) - (tr->lu[2] - tr->lu[0]) * (tr->lv[1] - tr->lv[0]));   /* area in texels */
    if (au < 1e-4f || wa < 1e-8f) return;
    flux *= au / wa;                                               /* E = flux / area of a texel (world) */
    if (!tb[tr->obj]) { tb[tr->obj] = (float*)calloc((size_t)ho->aw * ho->ah * 3, sizeof(float)); if (!tb[tr->obj]) return; }
    fx = lx - 0.5f; fy = ly - 0.5f; x0 = (int)floorf(fx); y0 = (int)floorf(fy); wx = fx - x0; wy = fy - y0;
    for (k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1); float w = ((k & 1) ? wx : 1 - wx) * ((k >> 1) ? wy : 1 - wy); size_t id;
        if (x < 0 || y < 0 || x >= ho->aw || y >= ho->ah) continue;
        id = ((size_t)y * ho->aw + x) * 3;
        tb[tr->obj][id] += flux * w; tb[tr->obj][id + 1] += flux * w; tb[tr->obj][id + 2] += flux * w;
    }
}

static void caustic_task(void* ctx, int task) {
    const Light* lp = &g_lightset[0]; float** tb = g_cbuf[task]; long P = (long)C.caustic_photons, per = P / CAUS_TASKS, i; int gi = g_caus_glass;
    const Obj* go = &g_obj[gi]; float ior = C.glass_ior < 1.01f ? 1.01f : C.glass_ior;
    uint32_t rs = 0x9E3779B9u * (uint32_t)(task + 1) ^ 0x1234ABCDu;
    (void)ctx; rs |= 1; rnd(&rs); rnd(&rs);
    for (i = 0; i < per; i++) {
        V3 s, a, t1, b1, d, p; float dc, sinA, cosA, omega, cosT, sinT, ph, cl, flux; int inside = 0, ev, alive = 1;
        if (lp->shape) s = v3(lp->pos.x + (rnd(&rs) * 2 - 1) * lp->radius, lp->pos.y, lp->pos.z + (rnd(&rs) * 2 - 1) * lp->radius);
        else { float z = 2 * rnd(&rs) - 1, r = sqrtf(fmaxf(0, 1 - z * z)), a2 = 2 * PI * rnd(&rs);
               s = vadd(lp->pos, vmul(v3(r * cosf(a2), z, r * sinf(a2)), lp->radius)); }
        a = vsub(go->gc, s); dc = vlen(a);
        if (dc < go->gr * 1.05f) continue;
        a = vmul(a, 1.0f / dc);
        sinA = go->gr * 1.02f / dc; cosA = sqrtf(1.0f - sinA * sinA); omega = 2 * PI * (1.0f - cosA);
        cosT = 1.0f - rnd(&rs) * (1.0f - cosA); sinT = sqrtf(fmaxf(0, 1 - cosT * cosT)); ph = 2 * PI * rnd(&rs);
        basis(a, &t1, &b1);
        d = vadd(vmul(a, cosT), vadd(vmul(t1, sinT * cosf(ph)), vmul(b1, sinT * sinf(ph))));
        cl = lp->shape ? -d.y : 1.0f;
        if (cl <= 0) continue;
        flux = cl * omega / (float)(per * CAUS_TASKS);
        p = s;
        for (ev = 0; ev < 10 && alive; ev++) {
            V3 oc = vsub(p, go->gc), hp, n; float bq = vdot(oc, d), cq = vdot(oc, oc) - go->gr * go->gr, disc = bq * bq - cq, t, F;
            if (disc <= 0) { alive = 0; break; }                   /* misses the sphere */
            t = inside ? -bq + sqrtf(disc) : -bq - sqrtf(disc);
            if (t <= 1e-5f) { alive = 0; break; }
            hp = vadd(p, vmul(d, t)); n = vmul(vsub(hp, go->gc), 1.0f / go->gr);
            if (!inside) {
                Hit hh;
                if (ev == 0 && trace(p, d, t - 1e-3f, &hh, gi)) { alive = 0; break; }   /* another object casts a shadow on the sphere */
                F = fresnel_d(-vdot(d, n), 1.0f, ior);
                if (rnd(&rs) < F) { d = vsub(d, vmul(n, 2.0f * vdot(d, n))); p = hp; break; }   /* reflected: goes back into the scene */
                d = refract_v(d, n, 1.0f / ior); p = hp; inside = 1;
            } else {
                float ci = vdot(d, n);                             /* > 0: the photon hits the wall from the inside */
                F = fresnel_d(ci, ior, 1.0f);
                if (F >= 1.0f || rnd(&rs) < F) { d = vsub(d, vmul(n, 2.0f * ci)); p = hp; }   /* total / internal reflection */
                else { d = refract_v(d, vmul(n, -1.0f), ior); p = hp; inside = 0; break; }
            }
        }
        if (!alive || inside) continue;
        {   Hit h;
            if (trace(vadd(p, vmul(d, 2e-3f)), d, 1000.0f, &h, gi)) caustic_splat(tb, &h, flux);
        }
    }
}

static void bake_caustics(void) {
    int gi, oi, t;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].lightmap) { memset(g_obj[oi].Ca, 0, (size_t)g_obj[oi].aw * g_obj[oi].ah * 4); }
    if (!g_nlight || C.caustics <= 0 || C.caustic_photons < 1000 || !g_ntri) return;
    for (gi = 0; gi < g_nobj; gi++) {
        if (!g_obj[gi].glass) continue;
        memset(g_cbuf, 0, sizeof g_cbuf); g_caus_glass = gi;
        par_for(CAUS_TASKS, caustic_task, NULL);
        for (oi = 0; oi < g_nobj; oi++) {                          /* merge the private buffers into fCa */
            Obj* o = &g_obj[oi]; size_t n = (size_t)o->aw * o->ah * 3, k;
            if (!o->lightmap) continue;
            for (t = 0; t < CAUS_TASKS; t++) {
                if (!g_cbuf[t][oi]) continue;
                if (!o->fCa) o->fCa = (float*)calloc(n, sizeof(float));
                if (o->fCa) for (k = 0; k < n; k++) o->fCa[k] += g_cbuf[t][oi][k];
                free(g_cbuf[t][oi]); g_cbuf[t][oi] = NULL;
            }
        }
    }
}

/* ---------- orchestration: cache or full bake ---------- */
/* one pass: jobs = tile rows (pass 1: only the rows that hold indirect nodes) */
static int run_phase(int phase, float f0, float f1) {
    int oi, qi, y, i, nthreads; HANDLE th[64]; SYSTEM_INFO si;
    g_phase = phase; g_next = 0; g_done = 0; g_njobs = 0;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].lightmap) for (qi = 0; qi < g_obj[oi].nq; qi++) {
        int H = g_obj[oi].q[qi].th + 2;
        for (y = 0; y < H; y++) if (phase == 0 || is_node(y, H)) g_njobs++;
    }
    g_jobs = (Job*)malloc((size_t)(g_njobs ? g_njobs : 1) * sizeof(Job));
    i = 0;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].lightmap) for (qi = 0; qi < g_obj[oi].nq; qi++) {
        int H = g_obj[oi].q[qi].th + 2;
        for (y = 0; y < H; y++) if (phase == 0 || is_node(y, H)) { g_jobs[i].obj = oi; g_jobs[i].quad = qi; g_jobs[i].y = y; i++; }
    }
    GetSystemInfo(&si);
    nthreads = C.bake_threads > 0 ? C.bake_threads : (int)si.dwNumberOfProcessors;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    for (i = 0; i < nthreads; i++) {
        th[i] = CreateThread(NULL, 0, bake_thread, NULL, 0, NULL);
        if (th[i]) SetThreadPriority(th[i], THREAD_PRIORITY_BELOW_NORMAL);
    }
    for (;;) {                                        /* loading screen: loading_hz times per second */
        DWORD w = WaitForMultipleObjects((DWORD)nthreads, th, TRUE, (DWORD)(1000 / (C.loading_hz > 0 ? C.loading_hz : 5)));
        loading_frame(f0 + (f1 - f0) * (float)g_done / (float)(g_njobs ? g_njobs : 1));
        if (w != WAIT_TIMEOUT) break;
    }
    for (i = 0; i < nthreads; i++) if (th[i]) CloseHandle(th[i]);
    free(g_jobs); g_jobs = NULL;
    return !g_abort;
}

/* Recomposes T = sum_l (color_l x mask_l) + color_0 x (bounce + caustic), then encodes.
 * The bounce and the caustic are baked for unit white light 0: the color of light 0 tints them here.
 * Separate from the bake so that a color/intensity can be changed without re-tracing rays. g_gi_on = 0: direct lighting only. */
static int g_gi_on = 1;
static void compose_light(Obj* o) {
    size_t n = (size_t)o->aw * o->ah, k; int l; V3 c0 = g_nlight ? g_lightset[0].col : v3(0, 0, 0);
    for (k = 0; k < n; k++) {
        V3 t = { 0, 0, 0 };
        for (l = 0; l < g_nlight; l++) {
            V3 s = rgbe_unpack(o->S[l][k]);
            t.x += g_lightset[l].col.x * s.x; t.y += g_lightset[l].col.y * s.y; t.z += g_lightset[l].col.z * s.z;
        }
        if (g_gi_on) {
            V3 b = rgbe_unpack(o->Bo[k]);
            if (o->Ca) b = vadd(b, rgbe_unpack(o->Ca[k]));
            t = vadd(t, vmulv(c0, b));
        }
        o->T[k] = rgbe_pack(t.x, t.y, t.z);
    }
    obj_avg_light(o);
}

/* interpolation + blur + RGBE encoding of an object (objects are independent) */
static void bake_finalize_obj(void* ctx, int i) {
    Obj* o = &g_obj[((const int*)ctx)[i]]; size_t n = (size_t)o->aw * o->ah, k; int l;
    if (!g_gi_gpu) interp_indirect(o);                     /* the GPU already computed every texel */
    for (l = 0; l < g_nlight; l++) blur_rgb(o, o->S[l]);
    blur_buf(o, o->fA); blur_buf(o, o->fB);
    for (k = 0; k < n; k++) {
        o->A[k] = rgbe_pack(o->fA[k * 3], o->fA[k * 3 + 1], o->fA[k * 3 + 2]);
        o->Bo[k] = rgbe_pack(o->fB[k * 3], o->fB[k * 3 + 1], o->fB[k * 3 + 2]);
    }
    if (o->fCa) {                                          /* caustics: smoothing of the photon noise, then encoding */
        blur_buf_n(o, o->fCa, C.caustic_blur);
        for (k = 0; k < n; k++) o->Ca[k] = rgbe_pack(o->fCa[k * 3], o->fCa[k * 3 + 1], o->fCa[k * 3 + 2]);
        free(o->fCa); o->fCa = NULL;
    }
    compose_light(o);
    free(o->fT); free(o->fA); free(o->fB); o->fT = o->fA = o->fB = NULL;
}

/* returns 2 = loaded from cache, 1 = baked, 0 = cancelled */
static int bake_all_inner(void) {
    int oi, nlist = 0;
    static int list[MAXOBJ];
    g_bl = g_light; g_brad = C.light_radius;
    compute_hash();
    models_prepare();
    loading_frame(0.0f);
    if (cache_load()) return 2;

    g_abort = 0;
    sph_init(C.rt_shadows ? C.rt_shadow_samples : C.shadow_samples); gi_init_tables(); g_rt_refined = 0;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; size_t n; int l;
        if (!o->lightmap) continue;
        n = (size_t)o->aw * o->ah;
        o->fT = (float*)calloc(n * 3, sizeof(float)); o->fA = (float*)calloc(n * 3, sizeof(float)); o->fB = (float*)calloc(n * 3, sizeof(float));
        for (l = 0; l < MAXLIGHT; l++) { free(o->S[l]); o->S[l] = (uint32_t*)calloc(n, sizeof(uint32_t)); }
        list[nlist++] = oi;
    }
    g_gpu_direct = 0; g_gpu_K = 0; g_ph[4] = bake_now();
    for (oi = 0; oi < g_nobj; oi++) { int l; for (l = 0; l < MAXLIGHT; l++) { free(g_obj[oi].gv[l]); g_obj[oi].gv[l] = NULL; } }
    g_gpu_direct = (C.bake_gpu && C.rt_shadows != 2) ? gpu_probe() : 0;   /* rt_shadows = 2: everything by CPU ray tracing */              /* title indicator during the bake */
    if (!(g_gpu_direct && bake_direct_gpu())) g_gpu_direct = 0;   /* soft shadows by the GPU (SM 2.0), otherwise CPU */
    g_ph[0] = bake_now() - g_ph[4]; g_ph[5] = bake_now();                /* g_ph[0] = GPU shadows */
    if (!run_phase(0, g_gpu_direct ? 0.15f : 0.00f, 0.25f)) return 0;   /* pass 1: direct lighting */
    g_ph[1] = bake_now() - g_ph[5]; g_ph[5] = bake_now();
    for (oi = 0; oi < g_nobj; oi++) { int l; for (l = 0; l < MAXLIGHT; l++) { free(g_obj[oi].gv[l]); g_obj[oi].gv[l] = NULL; } }
    g_gi_gpu = (C.bake_gpu_gi && gpu_probe()) ? bake_indirect_gpu() : 0;   /* pass 2 by GPU (VPL + depth cubemaps), otherwise CPU path tracer */
    if (g_abort) return 0;
    if (!g_gi_gpu && !run_phase(1, 0.25f, 0.90f)) return 0;        /* pass 2: AO + bounces (reads pass 1 again) */
    g_ph[2] = bake_now() - g_ph[5]; g_ph[5] = bake_now();
    bake_models();                                    /* 3D models: per-vertex shadows / AO / bounces (lightmap direct light still in memory) */
    if (g_abort) return 0;

    bake_caustics();                                  /* photons through the glass (fCa), then smoothing in bake_finalize_obj */
    par_for(nlist, bake_finalize_obj, list);           /* blur, A in RGBE, then T = sum (color x mask) */
    loading_frame(0.92f);
    g_ph[3] = bake_now() - g_ph[5]; g_ph[5] = bake_now();
    bake_reflections_all();
    g_ph[4] = bake_now() - g_ph[5];                          /* reflections: 1 face = 1 spot */
    if (g_abort) return 0;
    loading_frame(1.0f);
    cache_save();
    return 1;
}

static void gpu_release(void);
static int bake_all(void) { int r = bake_all_inner(); gpu_release(); return r; }
