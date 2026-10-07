/* render.h - Direct3D 9: device, multitexture rendering, LoD, culling, FXAA, loading screen, lighting refresh */
#pragma once

static IDirect3D9* g_d3d; static IDirect3DDevice9* g_dev; static D3DPRESENT_PARAMETERS g_pp;
static HWND g_hwnd; static volatile int g_running = 1;
static float g_yaw = 0.0f, g_pitch = 0.0f; static V3 g_cpos = { 0, 0, -16.5f };   /* free camera: position, yaw, pitch */
static DWORD g_maxaniso = 1; static int g_cube_linear = 1;
static IDirect3DTexture9 *g_tex_far, *g_tex_white, *g_rt; static IDirect3DSurface9* g_rts;
static IDirect3DPixelShader9* g_fxaa_ps; static int g_fxaa_on;
static int g_rt_w, g_rt_h, g_rt_bw, g_rt_bh; static float g_rt_ks = -1.0f;   /* offscreen scene target: size, backbuffer size it was built for, scale */
static M4 g_world, g_view, g_proj;
static float g_lyaw = 1e30f, g_lpitch = 1e30f; static V3 g_lcpos = { 1e30f, 0, 0 }; static int g_cam_valid;
static void gui_frame_draw(void);          /* ui.h: menu drawn on top of the frame */
static int g_drawn;

static void fatal(const char* msg) { MessageBoxA(NULL, msg, "Error", MB_ICONERROR); ExitProcess(1); }

/* ---------- textures ---------- */
static IDirect3DTexture9* make_texture(const uint32_t* px, int w, int h, int mips) {
    IDirect3DTexture9* t; int lv = 0, cw = w, ch = h; const uint32_t* cur = px; uint32_t* alloc = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, mips ? 0 : 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) {
        char msg[160];
        sprintf(msg, "CreateTexture failed (%dx%d, max %d).\nLower lightmap_max_size in engine.ini.", w, h, g_maxtex);
        fatal(msg);
    }
    for (;;) {
        D3DLOCKED_RECT lr; int y;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(t, lv, &lr, NULL, 0))) {
            for (y = 0; y < ch; y++) memcpy((char*)lr.pBits + (size_t)y * lr.Pitch, cur + (size_t)y * cw, (size_t)cw * 4);
            IDirect3DTexture9_UnlockRect(t, lv);
        }
        if (!mips || (cw == 1 && ch == 1)) break;
        {   /* next mip: 2x2 box filter */
            int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1, x, y2, c;
            uint32_t* nx = (uint32_t*)malloc((size_t)nw * nh * 4);
            for (y2 = 0; y2 < nh; y2++) for (x = 0; x < nw; x++) {
                uint32_t o = 0;
                for (c = 0; c < 3; c++) {
                    int s = 0, dx, dy;
                    for (dy = 0; dy < 2; dy++) for (dx = 0; dx < 2; dx++) {
                        int sx = x * 2 + dx, sy = y2 * 2 + dy;
                        if (sx >= cw) sx = cw - 1;
                        if (sy >= ch) sy = ch - 1;
                        s += (cur[sy * cw + sx] >> (c * 8)) & 255;
                    }
                    o |= (uint32_t)(s / 4) << (c * 8);
                }
                nx[y2 * nw + x] = o;
            }
            free(alloc); alloc = nx; cur = nx; cw = nw; ch = nh; lv++;
        }
    }
    free(alloc);
    return t;
}
/* A8R8G8B8 texture with a full mip chain (3D model textures: alpha for cut-outs / transparency) */
static IDirect3DTexture9* make_texture_a(const uint32_t* px, int w, int h) {
    IDirect3DTexture9* t; int lv = 0, cw = w, ch = h; const uint32_t* cur = px; uint32_t* alloc = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) {
        char msg[160]; sprintf(msg, "CreateTexture failed for a model texture (%dx%d, max %d).\nLower model_tex_max in engine.ini.", w, h, g_maxtex); fatal(msg);
    }
    for (;;) {
        D3DLOCKED_RECT lr; int y;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(t, lv, &lr, NULL, 0))) {
            for (y = 0; y < ch; y++) memcpy((char*)lr.pBits + (size_t)y * lr.Pitch, cur + (size_t)y * cw, (size_t)cw * 4);
            IDirect3DTexture9_UnlockRect(t, lv);
        }
        if (cw == 1 && ch == 1) break;
        {   int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1, x, y2, c; uint32_t* nx = (uint32_t*)malloc((size_t)nw * nh * 4);
            for (y2 = 0; y2 < nh; y2++) for (x = 0; x < nw; x++) {
                uint32_t o = 0;
                for (c = 0; c < 4; c++) {
                    int s = 0, dx, dy;
                    for (dy = 0; dy < 2; dy++) for (dx = 0; dx < 2; dx++) {
                        int sx = x * 2 + dx, sy = y2 * 2 + dy; if (sx >= cw) sx = cw - 1; if (sy >= ch) sy = ch - 1;
                        s += (cur[(size_t)sy * cw + sx] >> (c * 8)) & 255;
                    }
                    o |= (uint32_t)(s / 4) << (c * 8);
                }
                nx[(size_t)y2 * nw + x] = o;
            }
            free(alloc); alloc = nx; cur = nx; cw = nw; ch = nh; lv++;
        }
    }
    free(alloc);
    return t;
}
static IDirect3DTexture9* make_empty_texture(int w, int h) {
    IDirect3DTexture9* t;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) fatal("CreateTexture (lightmap) failed");
    return t;
}

/* ---------- LUT : tone mapping + gamma ---------- */
#define LUTN 4096
static float g_lut_lin[LUTN + 2], g_lut_enc[LUTN + 2], g_enc1[1026];
static void build_luts(void) {
    int i;
    for (i = 0; i <= LUTN + 1; i++) {
        float y = i * (8.0f / LUTN), lin = 1.0f - expf(-y);
        g_lut_lin[i] = lin; g_lut_enc[i] = C.gamma_correct ? powf(lin, 1.0f / 2.2f) : lin;
    }
    for (i = 0; i <= 1025; i++) { float x = clampf(i / 1024.0f, 0, 1); g_enc1[i] = C.gamma_correct ? powf(x, 1.0f / 2.2f) : x; }
}
static inline float lut_get(const float* t, float y, float maxy, int n) {
    float f; int i;
    if (y <= 0) return t[0];
    if (y >= maxy) return t[n];
    f = y * (n / maxy); i = (int)f;
    return t[i] + (t[i + 1] - t[i]) * (f - i);
}
static inline uint32_t pack8(float r, float g, float b) {
    return ((uint32_t)(clampf(r, 0, 1) * 255.0f + 0.5f) << 16) | ((uint32_t)(clampf(g, 0, 1) * 255.0f + 0.5f) << 8) | (uint32_t)(clampf(b, 0, 1) * 255.0f + 0.5f);
}

/* ---------- applying the lighting (color / intensity / ambient / exposure) ---------- */
/* ---------- distant lighting: pre-multiplied vertex color ----------
 * Beyond C.overlay_distance the lightmap is no longer sampled: the vertex
 * color already contains (color x average light of the object). One texture is dropped
 * per pixel and per object; the loss of detail equals the average of the lightmap. */
static unsigned char mul8(unsigned a, unsigned b) {
    unsigned v = (a * b + 127u) / 255u; return (unsigned char)(v > 255u ? 255u : v);
}

/* avgT already contains the colored lighting (sum of the lights): lc is no longer needed. */
static void update_far_vb(Obj* o, V3 lc, V3 amb) {
    void* p; Vtx* v; int i; unsigned char lr, lg, lb;
    (void)lc;
    if (!o->vbf || !o->lightmap || !o->vert) return;
    lr = (unsigned char)(clampf(lut_get(g_lut_enc, (o->avgT[0] + amb.x * o->avgA[0]) * C.exposure, 8.0f, LUTN), 0, 255) + 0.5f);
    lg = (unsigned char)(clampf(lut_get(g_lut_enc, (o->avgT[1] + amb.y * o->avgA[1]) * C.exposure, 8.0f, LUTN), 0, 255) + 0.5f);
    lb = (unsigned char)(clampf(lut_get(g_lut_enc, (o->avgT[2] + amb.z * o->avgA[2]) * C.exposure, 8.0f, LUTN), 0, 255) + 0.5f);
    if (FAILED(IDirect3DVertexBuffer9_Lock(o->vbf, 0, 0, &p, 0))) return;
    v = (Vtx*)p;
    for (i = 0; i < o->nv; i++) {
        unsigned c = o->vert[i].col;
        v[i] = o->vert[i];
        v[i].col = 0xFF000000u | ((DWORD)mul8((c >> 16) & 255, lr) << 16)
                             | ((DWORD)mul8((c >> 8) & 255, lg) << 8)
                             | (DWORD)mul8(c & 255, lb);
    }
    IDirect3DVertexBuffer9_Unlock(o->vbf);
}

/* number of mip levels of the reflection cubemap (automatic LOD by the GPU) */
static int cube_mips(int su) { int m = 1; while ((su >> m) >= 8) m++; return m; }

/* step 1: direct lighting -> lightmap T/A + distant vertex color.
 * Fast: redone whenever a light parameter changes. */
/* Total color emitted by the lights: used for the "distant" lighting (average of the
 * lightmaps) and to the lamp tint. The sum is only an approximation
 * (no angular weighting) but distant rendering does not need more. */
static V3 light_total(void) {
    V3 s = { 0, 0, 0 }; int l;
    for (l = 0; l < g_nlight; l++) { s.x += g_lightset[l].col.x; s.y += g_lightset[l].col.y; s.z += g_lightset[l].col.z; }
    return s;
}

static void model_apply_light(Obj* o);       /* modelbake.h: per-vertex lighting of a 3D model */
static void refresh_one_lighting(void* ctx, int oi) {
    Obj* o = &g_obj[oi];
    if (o->is_model) { model_apply_light(o); return; }
    V3 lc = light_total();
    V3 amb = v3(C.ambient_r, C.ambient_g, C.ambient_b);
    update_far_vb(o, lc, amb);
    if (o->lightmap && o->tex_lm) {
        D3DLOCKED_RECT lr; int x, y;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(o->tex_lm, 0, &lr, NULL, 0))) {
            for (y = 0; y < o->ah; y++) {
                uint32_t* row = (uint32_t*)((char*)lr.pBits + (size_t)y * lr.Pitch);
                for (x = 0; x < o->aw; x++) {
                    size_t i = (size_t)y * o->aw + x; V3 T = rgbe_unpack(o->T[i]), A = rgbe_unpack(o->A[i]);
                    /* T is already colored (compose_light applied the light colors) */
                    float ex = (T.x + amb.x * A.x) * C.exposure, ey = (T.y + amb.y * A.y) * C.exposure, ez = (T.z + amb.z * A.z) * C.exposure;
                    row[x] = pack8(lut_get(g_lut_enc, ex, 8.0f, LUTN), lut_get(g_lut_enc, ey, 8.0f, LUTN), lut_get(g_lut_enc, ez, 8.0f, LUTN));
                }
            }
            IDirect3DTexture9_UnlockRect(o->tex_lm, 0);
        }
    }
}

/* step 2: color of the reflection cubemaps (reads the bake G-buffer).
 * This is the most expensive step: it is spread over several frames. */
static void refresh_one_cube(void* ctx, int oi) {
    Obj* o = &g_obj[oi];
    V3 amb = v3(C.ambient_r, C.ambient_g, C.ambient_b);
    int S = C.reflection_size, U = C.reflection_upscale < 1 ? 1 : C.reflection_upscale;
    if (o->reflect && o->tex_cube) {
        float* cf = (float*)malloc((size_t)6 * S * S * 3 * sizeof(float)); int f, x, y, k;
        for (f = 0; f < 6; f++) for (y = 0; y < S; y++) for (x = 0; x < S; x++) {
            V3 acc = { 0, 0, 0 };
            for (k = 0; k < o->nsamp; k++) {
                const unsigned char* g = &o->gbuf[((((size_t)f * S + y) * S + x) * o->nsamp + k) * 4];
                int ob = GB_OBJ(g), ix = GB_IDX(g), n; V3 col;
                const Obj* ho;
                if (ob >= g_nobj) {   /* ray lost in the void (sky): no reflection -> ambient only (exposed), not bg */
                    col = v3(lut_get(g_lut_lin, amb.x * C.exposure, 8.0f, LUTN),
                             lut_get(g_lut_lin, amb.y * C.exposure, 8.0f, LUTN),
                             lut_get(g_lut_lin, amb.z * C.exposure, 8.0f, LUTN));
                } else {
                    ho = &g_obj[ob];
                    n = ho->aw * ho->ah;
                    if (ix < 0 || ix >= n) ix = 0;
                    if (ho->lamp_refl) {                                                    /* movable lamp seen in a reflection: bright source (highlights on metal) */
                        V3 lc = g_nlight ? g_lightset[0].col : v3(1, 1, 1); float mx = fmaxf(lc.x, fmaxf(lc.y, lc.z)); if (mx < 1e-3f) mx = 1.0f;
                        col = v3(lut_get(g_lut_lin, 6.0f * lc.x / mx, 8.0f, LUTN), lut_get(g_lut_lin, 6.0f * lc.y / mx, 8.0f, LUTN), lut_get(g_lut_lin, 6.0f * lc.z / mx, 8.0f, LUTN));
                    } else if (ho->is_model) {                                              /* another part of a 3D model: average albedo x average light */
                        col = v3(ho->malb.x * ho->mlit[0], ho->malb.y * ho->mlit[1], ho->malb.z * ho->mlit[2]);
                    } else if (!ho->T) col = v3(lut_get(g_lut_lin, C.exposure, 8.0f, LUTN),    /* emissive object (lamp): no lightmap */
                                          lut_get(g_lut_lin, C.exposure, 8.0f, LUTN),
                                          lut_get(g_lut_lin, C.exposure, 8.0f, LUTN));
                    else {
                        V3 T = rgbe_unpack(ho->T[ix]), A = rgbe_unpack(ho->A[ix]), ab = v3(1, 1, 1);
                        if (ho->alb_px) { const unsigned char* q = &ho->alb_px[(size_t)ix * 3]; ab = v3(q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f); }
                        col = v3(ab.x * lut_get(g_lut_lin, (T.x + amb.x * A.x) * C.exposure, 8.0f, LUTN),
                                 ab.y * lut_get(g_lut_lin, (T.y + amb.y * A.y) * C.exposure, 8.0f, LUTN),
                                 ab.z * lut_get(g_lut_lin, (T.z + amb.z * A.z) * C.exposure, 8.0f, LUTN));
                    }
                }
                acc = vadd(acc, col);
            }
            acc = vmulv(vmul(acc, 1.0f / o->nsamp), o->tint);
            { float* d = &cf[(((size_t)f * S + y) * S + x) * 3]; d[0] = acc.x; d[1] = acc.y; d[2] = acc.z; }
        }
        /* mip levels: level 0 = S x S enlarged to SU x SU, then 2x2 average going down.
         * The GPU picks the level according to the screen footprint: distant reflections
         * are blurry and cost less, without seams. */
        {   int m, SU = S * U, sz = SU, s2 = S;
            float* lv = (float*)malloc((size_t)sz * sz * 3 * sizeof(float));
            uint32_t* px = (uint32_t*)malloc((size_t)sz * sz * 4);
            if (lv && px) {
              for (f = 0; f < 6; f++) {
                int sz = SU;
                for (m = 0; m < cube_mips((int)SU); m++) {
                    D3DLOCKED_RECT lr; int y, x;
                    if (m == 0) {                       /* S -> SU : interpolation bilineaire */
                        for (y = 0; y < sz; y++) {
                            float ft = ((y + 0.5f) / U - 0.5f), y0 = clampf(ft, 0, (float)(s2 - 1)), wt = ft - y0;
                            int iy0 = (int)y0, iy1 = iy0 + 1 < s2 ? iy0 + 1 : s2 - 1;
                            for (x = 0; x < sz; x++) {
                                float fs = ((x + 0.5f) / U - 0.5f), x0 = clampf(fs, 0, (float)(s2 - 1)), ws = fs - x0;
                                int ix0 = (int)x0, ix1 = ix0 + 1 < s2 ? ix0 + 1 : s2 - 1; int c;
                                for (c = 0; c < 3; c++) {
                                    const float* b = &cf[(size_t)f * s2 * s2 * 3];
                                    float a = b[((size_t)iy0 * s2 + ix0) * 3 + c] * (1 - ws) + b[((size_t)iy0 * s2 + ix1) * 3 + c] * ws;
                                    float d = b[((size_t)iy1 * s2 + ix0) * 3 + c] * (1 - ws) + b[((size_t)iy1 * s2 + ix1) * 3 + c] * ws;
                                    lv[((size_t)y * sz + x) * 3 + c] = a * (1 - wt) + d * wt;
                                }
                            }
                        }
                    } else {                            /* 2x2 average of the previous level */
                        int hs = sz / 2;
                        for (y = 0; y < hs; y++) for (x = 0; x < hs; x++) {
                            int c, dy, dx;
                            for (c = 0; c < 3; c++) {
                                float a = 0;
                                for (dy = 0; dy < 2; dy++) for (dx = 0; dx < 2; dx++)
                                    a += lv[((size_t)(y * 2 + dy) * sz + x * 2 + dx) * 3 + c];
                                lv[((size_t)y * hs + x) * 3 + c] = a * 0.25f;
                            }
                        }
                        sz = hs;
                    }
                    for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
                        float* q = &lv[((size_t)y * sz + x) * 3];
                        px[(size_t)y * sz + x] = pack8(lut_get(g_enc1, q[0], 1.0f, 1024), lut_get(g_enc1, q[1], 1.0f, 1024), lut_get(g_enc1, q[2], 1.0f, 1024));
                    }
                    if (SUCCEEDED(IDirect3DCubeTexture9_LockRect(o->tex_cube, (D3DCUBEMAP_FACES)f, (UINT)m, &lr, NULL, 0))) {
                        int cp = lr.Pitch / 4;
                        for (y = 0; y < sz; y++) memcpy(&((uint32_t*)lr.pBits)[(size_t)y * cp], &px[(size_t)y * sz], (size_t)sz * 4);
                        IDirect3DCubeTexture9_UnlockRect(o->tex_cube, (D3DCUBEMAP_FACES)f, (UINT)m);
                    }
                }
              }
            }
            free(lv); free(px);
        }
        free(cf);
    }
}
static void refresh_one(void* ctx, int oi) {
    refresh_one_lighting(ctx, oi);
    refresh_one_cube(ctx, oi);
}


/* ---------- reflections: update spread over several frames ----------
 * On an old iGPU, rebuilding all the cubemaps at once freezes the image
 * (hundreds of ms at high resolution) and the FPS counter collapses. However, the
 * reflection does not need to follow the camera: 15 to 20 frames per second are enough
 * easily. So we note the objects to redo, wait reflection_update_interval
 * frames, then advance through the queue as long as we stay under reflection_budget_ms. */
static int g_refl_q[MAXOBJ], g_refl_n, g_refl_pos, g_refl_wait;

/* GetTickCount does not go below 15.6 ms: a 2 ms budget would be
 * always exceeded and the amortization would be pointless. High-resolution timer. */
static double refl_now_ms(void) {
    static LARGE_INTEGER f; static int ok = 0;
    LARGE_INTEGER c;
    if (!ok) { if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) ok = 1; else return (double)GetTickCount(); }
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

static void reflections_mark_dirty(void) {
    int i, n = 0;
    build_luts();
    for (i = 0; i < g_nobj; i++) if (g_obj[i].reflect && g_obj[i].tex_cube) g_refl_q[n++] = i;
    g_refl_n = n; g_refl_pos = 0;
    g_refl_wait = C.reflection_update_interval > 0 ? C.reflection_update_interval : 1;
}

static int reflections_pending(void) { return g_refl_n - g_refl_pos; }

/* advances the queue; returns 1 when the update is finished */
static int reflections_step(void) {
    double t0, budget;
    if (g_refl_pos >= g_refl_n) return 1;
    if (g_refl_wait > 0) { g_refl_wait--; return 0; }      /* respecte l'intervalle demande */
    budget = C.reflection_budget_ms > 0 ? C.reflection_budget_ms : 2.0;
    t0 = refl_now_ms();
    do {
        int list[1]; list[0] = g_refl_q[g_refl_pos++];
        par_for(1, refresh_one_cube, list);                /* 1 objet = 1 tache : cout maitrise */
    } while (g_refl_pos < g_refl_n && refl_now_ms() - t0 < budget);
    return g_refl_pos >= g_refl_n;
}

static void refresh_lighting(void) {
    static int list[MAXOBJ]; int n = 0, oi;
    build_luts();
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].is_model) model_apply_light(&g_obj[oi]);     /* before the cubemaps: they read mlit */
    for (oi = 0; oi < g_nobj; oi++) if ((g_obj[oi].lightmap && g_obj[oi].tex_lm) || (g_obj[oi].reflect && g_obj[oi].tex_cube)) list[n++] = oi;
    par_for(n, refresh_one, list);        /* 1 object = 1 task (each task writes its own textures) */
}

/* step 1 only: the lightmaps follow the light immediately, the reflections do not */
static void refresh_lightmaps(void) {
    static int list[MAXOBJ]; int n = 0, oi;
    build_luts();
    for (oi = 0; oi < g_nobj; oi++) if ((g_obj[oi].lightmap && g_obj[oi].tex_lm) || g_obj[oi].is_model) list[n++] = oi;
    par_for(n, refresh_one_lighting, list);
}

/* ================================================================================
 * d3d9x - prototypes D3D9 corrects, independants de l'en-tete MinGW.
 *
 * The d3d9.h shipped with MinGW declares several vtbl methods in a NON
 * standard way (SetRenderTarget without the z-stencil argument, SetIndices without
 * BaseVertexIndex, D3DVERTEXELEMENT9 with a Usage field instead of Semantic,
 * D3DVIEWPORT9 with MinZ/MaxZ...). The header macros therefore call these
 * methods with a wrong signature: the missing argument comes from the stack and
 * is unreliable. The vtbl slot positions, however, are correct,
 * so it is enough to call the slot with the right function type.
 *
 * We go through explicitly typed function pointers for the whole
 * GPU bake pass. The engine's normal rendering path is not modified here (it
 * works), but it would suffer from the same issues: see SetIndices and the FXAA path.
 * ================================================================================ */
typedef struct { WORD Stream; WORD Offset; BYTE Type; BYTE Semantic; BYTE UsageIndex; BYTE Method; void* pNext; } D3DELEM9;

typedef HRESULT (WINAPI *D9t_SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*, IDirect3DSurface9*);
typedef HRESULT (WINAPI *D9t_GetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
typedef HRESULT (WINAPI *D9t_SetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT (WINAPI *D9t_GetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9**);
typedef HRESULT (WINAPI *D9t_SetIndices)(IDirect3DDevice9*, IDirect3DIndexBuffer9*, UINT);
typedef HRESULT (WINAPI *D9t_SetStreamSource)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
typedef HRESULT (WINAPI *D9t_DrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI *D9t_SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (WINAPI *D9t_GetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9**);
typedef HRESULT (WINAPI *D9t_SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT (WINAPI *D9t_GetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9**);
typedef HRESULT (WINAPI *D9t_SetVertexShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT (WINAPI *D9t_SetPixelShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT (WINAPI *D9t_CreateVertexDeclaration)(IDirect3DDevice9*, const D3DELEM9*, IDirect3DVertexDeclaration9**);
typedef HRESULT (WINAPI *D9t_SetVertexDeclaration)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
typedef HRESULT (WINAPI *D9t_GetVertexDeclaration)(IDirect3DDevice9*, IDirect3DVertexDeclaration9**);
typedef HRESULT (WINAPI *D9t_SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT (WINAPI *D9t_GetViewport)(IDirect3DDevice9*, D3DVIEWPORT9*);
typedef HRESULT (WINAPI *D9t_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (WINAPI *D9t_Scene)(IDirect3DDevice9*);
typedef HRESULT (WINAPI *D9t_SetRenderState)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (WINAPI *D9t_GetRenderState)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD*);
typedef HRESULT (WINAPI *D9t_SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT (WINAPI *D9t_GetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9**);
typedef HRESULT (WINAPI *D9t_GetRenderTargetData)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
typedef HRESULT (WINAPI *D9t_GetTransform)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
typedef HRESULT (WINAPI *D9t_SetTransform)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT (WINAPI *D9t_GetFVF)(IDirect3DDevice9*, DWORD*);
typedef HRESULT (WINAPI *D9t_SetFVF)(IDirect3DDevice9*, DWORD);
typedef HRESULT (WINAPI *D9t_CreateVertexShader)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
typedef HRESULT (WINAPI *D9t_CreatePixelShader)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
typedef HRESULT (WINAPI *D9t_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DTexture9**);
typedef HRESULT (WINAPI *D9t_CreateOffscreenPlainSurface)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*);

/* The vtbl slot, reinterpreted with the standard d3d9.dll signature. */
#define D9(n) ((D9t_##n)(void*)g_dev->lpVtbl->n)
/* BeginScene and EndScene share the same typedef */
#define D9_Scene(n) ((D9t_Scene)(void*)g_dev->lpVtbl->n)

/* resources of the GPU bake pass (declared here: vram_used() depends on them) */
static IDirect3DTexture9* g_bake_tex;      /* G-buffer: (S*nsamp) x (S*6) */
static IDirect3DTexture9* g_bake_tex_ds;
static IDirect3DSurface9* g_bake_rt, *g_bake_ds, *g_bake_sys;
static int g_bake_w, g_bake_h, g_bake_ok = -1;

/* resources of the GPU bake pass (declared here: vram_used() depends on them) */
static IDirect3DTexture9* g_bake_tex;      /* G-buffer: (S*nsamp) x (S*6) */
static IDirect3DTexture9* g_bake_tex_ds;
static IDirect3DSurface9* g_bake_rt, *g_bake_ds, *g_bake_sys;

/* ---------- video memory ---------- */
/* D3D9 exposes neither the total VRAM nor the VRAM in use.
 *  - total   : read from the registry (HKLM\SYSTEM\CurrentControlSet\Control\Class\{GUID display}\000N,
 *              HardwareInformation.qwMemorySize value in bytes). Entries without a value
 *              are ignored; if there are several GPUs, the largest memory is kept.
 *  - utilise : sum of the application's allocations (textures, cubemaps, buffers, targets).
 * Intentionally, no adapter identifier structure is read: the structure
 * declared by d3d9.h under MinGW is truncated and the driver then writes beyond it (crash). */
static double g_vram_total = 0.0;          /* octets, 0 = inconnu */

static void vram_query_total(void) {
    static const char* cls = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    /* Depending on the driver, the value is called "HardwareInformation.qwMemorySize" (QWORD, bytes)
     * or "HardwareInformation.MemorySize" (DWORD, kilobytes): we read the size first,
     * because RegQueryValueExA fails if the buffer is too small. */
    static const char* keys[2] = { "HardwareInformation.qwMemorySize", "HardwareInformation.MemorySize" };
    HKEY root, sub; DWORD i, n, k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, cls, 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (i = 0; i < 64; i++) {
        char name[64];
        n = (DWORD)sizeof name;
        if (RegEnumKeyExA(root, i, name, &n, 0, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExA(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        for (k = 0; k < 2; k++) {
            DWORD sz = 0, type = 0, lo = 0; DWORDLONG wide = 0;
            if (RegQueryValueExA(sub, keys[k], NULL, &type, NULL, &sz) != ERROR_SUCCESS || sz == 0) continue;
            if (type == REG_QWORD && sz == 8) {
                if (RegQueryValueExA(sub, keys[k], NULL, &type, (LPBYTE)&wide, &sz) == ERROR_SUCCESS && wide > 0) {
                    if ((double)wide > g_vram_total) g_vram_total = (double)wide;
                    break;
                }
            } else if (type == REG_DWORD && sz == 4) {
                if (RegQueryValueExA(sub, keys[k], NULL, &type, (LPBYTE)&lo, &sz) == ERROR_SUCCESS && lo > 0) {
                    if ((double)lo * 1024.0 > g_vram_total) g_vram_total = (double)lo * 1024.0;
                    break;
                }
            }
        }
        RegCloseKey(sub);
    }
    RegCloseKey(root);
}

/* VRAM used by the application: textures, cubemaps, buffers, render targets. */
static double vram_used(void) {
    double b = 0.0; int oi, l;
    size_t SU = (size_t)C.reflection_size * (C.reflection_upscale < 1 ? 1 : C.reflection_upscale);
    if (SU > (size_t)g_maxtex) SU = (size_t)g_maxtex;
    if (g_tex_white) b += 4;
    if (g_tex_far) b += 4;
    if (g_rt) b += (double)g_rt_w * g_rt_h * 4.0;
    if (g_bake_tex) b += (double)g_bake_w * g_bake_h * 4.0;
    if (g_bake_tex_ds) b += (double)g_bake_w * g_bake_h * 2.0;
    if (g_bake_sys) b += (double)g_bake_w * g_bake_h * 4.0;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi];
        if (o->tex_albedo) b += (double)o->texsz * o->texsz * 4.0;
        if (o->tex_lm) b += (double)o->aw * o->ah * 4.0;
        if (o->tex_cube) {         /* full mip chain: 4/3 of the base face */
            double f = 1.0, tot = 0.0; int mm;
            for (mm = 0; mm < cube_mips((int)SU); mm++) { tot += f; f *= 0.25; }
            b += 6.0 * (double)SU * SU * 4.0 * tot;
        }
        if (o->vb) b += (double)o->nv * sizeof(Vtx);
        if (o->vbf) b += (double)o->nv * sizeof(Vtx);
        for (l = 0; l < o->nlod; l++) if (o->ib[l]) b += (double)o->nt[l] * 3 * 2;
    }
    return b;
}

/* ---------- GPU resources of the objects ---------- */
static double g_acmr_b, g_acmr_a; static long g_acmr_n;     /* vertex cache miss ratio (FIFO 16), triangle-weighted sums: before / after reordering */
static int g_req_idx;                                       /* menu: re-fill the index buffers (vcache_opt toggled) */
static void apply_index_order(void) {
    int oi, l;
    for (oi = 0; oi < g_nobj; oi++) { Obj* o = &g_obj[oi];
        for (l = 0; l < o->nlod; l++) { void* p;
            if (!o->ib[l] || !o->idx[l]) continue;
            if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(o->ib[l], 0, 0, &p, 0))) {
                memcpy(p, (C.vcache_opt && o->idx_vc[l] && !o->blend) ? o->idx_vc[l] : o->idx[l], (size_t)o->nt[l] * 3 * 2); IDirect3DIndexBuffer9_Unlock(o->ib[l]);
            } } }
}
static void upload_object(Obj* o) {
    void* p; int l, SU = C.reflection_size * (C.reflection_upscale < 1 ? 1 : C.reflection_upscale);
    if (SU > g_maxtex) SU = g_maxtex;
    if (o->textured) {
        o->tex_albedo = o->is_model ? make_texture_a(o->tex_px, o->mtw, o->mth) : make_texture(o->tex_px, o->texsz, o->texsz, 1);
        free(o->tex_px); o->tex_px = NULL;
    }
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g_dev, o->nv * sizeof(Vtx), D3DUSAGE_WRITEONLY, FVF, D3DPOOL_MANAGED, &o->vb, NULL))) fatal("CreateVertexBuffer");
    /* same geometry, color already multiplied by the average light: far away we draw
     * with this buffer and skip sampling the lightmap (one texture fewer). */
    if (o->lightmap && FAILED(IDirect3DDevice9_CreateVertexBuffer(g_dev, o->nv * sizeof(Vtx), D3DUSAGE_WRITEONLY, FVF, D3DPOOL_MANAGED, &o->vbf, NULL))) o->vbf = NULL;
    IDirect3DVertexBuffer9_Lock(o->vb, 0, 0, &p, 0); memcpy(p, o->vert, (size_t)o->nv * sizeof(Vtx)); IDirect3DVertexBuffer9_Unlock(o->vb);
    for (l = 0; l < o->nlod; l++) {
        if (FAILED(IDirect3DDevice9_CreateIndexBuffer(g_dev, o->nt[l] * 3 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &o->ib[l], NULL))) fatal("CreateIndexBuffer");
        o->idx_vc[l] = (uint16_t*)malloc((size_t)o->nt[l] * 3 * 2);
        if (o->idx_vc[l] && !o->blend) {   /* alpha-blended meshes keep their order: blending inside one object depends on it */
            vcache_optimize(o->idx[l], o->idx_vc[l], o->nt[l], o->nv);
            g_acmr_n += o->nt[l]; g_acmr_b += vc_acmr(o->idx[l], o->nt[l], o->nv, 16) * o->nt[l]; g_acmr_a += vc_acmr(o->idx_vc[l], o->nt[l], o->nv, 16) * o->nt[l];
        }
        IDirect3DIndexBuffer9_Lock(o->ib[l], 0, 0, &p, 0);
        memcpy(p, (C.vcache_opt && o->idx_vc[l] && !o->blend) ? o->idx_vc[l] : o->idx[l], (size_t)o->nt[l] * 3 * 2); IDirect3DIndexBuffer9_Unlock(o->ib[l]);
    }
    if (o->lightmap) o->tex_lm = make_empty_texture(o->aw, o->ah);
    if (o->reflect && FAILED(IDirect3DDevice9_CreateCubeTexture(g_dev, SU, cube_mips((int)SU), 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &o->tex_cube, NULL))) fatal("CreateCubeTexture");
}

/* ---------- states ---------- */
static void ts(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) { IDirect3DDevice9_SetTextureStageState(g_dev, s, t, v); }
static void ss(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) { IDirect3DDevice9_SetSamplerState(g_dev, s, t, v); }
#define RS(a, b) IDirect3DDevice9_SetRenderState(g_dev, a, b)

/* ---------- redundant call filter (used by draw_object only) ----------
 * Objects drawn one after the other mostly share the same sampler / stage / buffer state, yet
 * stage_setup() used to re-send ~13 states per stage for each of them. Here the last value sent
 * is remembered and an identical one is not sent again: same image, fewer API calls.
 * Anything outside draw_object() (glass, GUI, bake, Reset) bypasses this filter, so the filter is
 * simply forgotten (sc_reset) at the start of every frame and after each glass draw. */
#define SC_STAGES 4
#define SC_TSS    40
#define SC_SAMP   16
static DWORD g_sc_tv[SC_STAGES][SC_TSS], g_sc_sv[SC_STAGES][SC_SAMP];
static unsigned char g_sc_tk[SC_STAGES][SC_TSS], g_sc_sk[SC_STAGES][SC_SAMP], g_sc_texk[SC_STAGES];
static IDirect3DBaseTexture9* g_sc_tex[SC_STAGES];
static IDirect3DVertexBuffer9* g_sc_vb; static IDirect3DIndexBuffer9* g_sc_ib;
static M4 g_sc_world; static int g_sc_worldk;
static void sc_reset(void) {
    memset(g_sc_tk, 0, sizeof g_sc_tk); memset(g_sc_sk, 0, sizeof g_sc_sk); memset(g_sc_texk, 0, sizeof g_sc_texk);
    g_sc_vb = NULL; g_sc_ib = NULL; g_sc_worldk = 0;
}
static void cts(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (C.state_cache && s < SC_STAGES && (DWORD)t < SC_TSS) {
        if (g_sc_tk[s][t] && g_sc_tv[s][t] == v) return;
        g_sc_tk[s][t] = 1; g_sc_tv[s][t] = v;
    }
    ts(s, t, v);
}
static void css(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (C.state_cache && s < SC_STAGES && (DWORD)t < SC_SAMP) {
        if (g_sc_sk[s][t] && g_sc_sv[s][t] == v) return;
        g_sc_sk[s][t] = 1; g_sc_sv[s][t] = v;
    }
    ss(s, t, v);
}
static void ctex(DWORD s, IDirect3DBaseTexture9* tex) {
    if (C.state_cache && s < SC_STAGES) {
        if (g_sc_texk[s] && g_sc_tex[s] == tex) return;
        g_sc_texk[s] = 1; g_sc_tex[s] = tex;
    }
    IDirect3DDevice9_SetTexture(g_dev, s, tex);
}

static void frame_states(void) {
    RS(D3DRS_LIGHTING, FALSE); RS(D3DRS_ZENABLE, D3DZB_TRUE); RS(D3DRS_ZWRITEENABLE, TRUE);
    RS(D3DRS_CULLMODE, D3DCULL_CCW); RS(D3DRS_SPECULARENABLE, FALSE); RS(D3DRS_ALPHABLENDENABLE, FALSE);
    RS(D3DRS_SRGBWRITEENABLE, C.gamma_correct ? TRUE : FALSE);
    if (g_pp.MultiSampleType != D3DMULTISAMPLE_NONE) RS(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
}

/* filt : 0 point, 1 bilinear, 2 anisotropic */
static void stage_setup(DWORD s, IDirect3DBaseTexture9* tex, DWORD op, DWORD a1, DWORD a2, DWORD coord, DWORD xform, int filt, int aniso, int wrap, int mip) {
    ctex(s, tex);
    cts(s, D3DTSS_COLOROP, op); cts(s, D3DTSS_COLORARG1, a1); cts(s, D3DTSS_COLORARG2, a2);
    cts(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    cts(s, D3DTSS_TEXCOORDINDEX, coord); cts(s, D3DTSS_TEXTURETRANSFORMFLAGS, xform);
    css(s, D3DSAMP_MAGFILTER, filt == 2 ? D3DTEXF_ANISOTROPIC : (filt ? D3DTEXF_LINEAR : D3DTEXF_POINT));
    css(s, D3DSAMP_MINFILTER, filt == 2 ? D3DTEXF_ANISOTROPIC : (filt ? D3DTEXF_LINEAR : D3DTEXF_POINT));
    css(s, D3DSAMP_MIPFILTER, mip ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    css(s, D3DSAMP_MAXANISOTROPY, aniso > 1 ? (DWORD)aniso : 1);
    css(s, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
    css(s, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
    css(s, D3DSAMP_SRGBTEXTURE, C.gamma_correct ? TRUE : FALSE);
}

static int frustum_visible(V3 c, float r);
static void draw_glass(Obj* o, M4 w);       /* glass.h */
static int g_depth_pass;             /* 1 during the depth pass (depth only) */

/* ---------- draw sorting (front -> back) ----------
 * The depth buffer makes the image independent of order, but an order
 * near->far makes hidden pixels get rejected early: a noticeable gain when the
 * GPU is fill-rate limited (old iGPUs). */
static int g_order[MAXOBJ], g_norder;
static float g_ordist[MAXOBJ];

static int build_draw_order(void) {
    int i, j, n = 0;
    for (i = 0; i < g_nobj; i++) {
        Obj* o = &g_obj[i];
        M4 w = g_world; V3 c;
        if (o->no_draw) continue;
        if (o->is_lamp && !o->lamp_world) { V3 lp = g_lightset[o->light_idx < 0 ? 0 : o->light_idx].pos;
                          w = m_mul(m_trans(lp.x, lp.y, lp.z), g_world); }
        c = m_pt(m_pt(o->bs_c, w), g_view);
        if (C.frustum_culling && !frustum_visible(c, o->bs_r)) continue;
        g_ordist[n] = c.z;
        g_order[n] = i;
        n++;
    }
    for (i = 1; i < n; i++) {                    /* insertion sort: n <= MAXOBJ */
        int oi = g_order[i]; float d = g_ordist[i];
        for (j = i - 1; j >= 0 && g_ordist[j] > d; j--) { g_ordist[j + 1] = g_ordist[j]; g_order[j + 1] = g_order[j]; }
        g_ordist[j + 1] = d; g_order[j + 1] = oi;
    }
    return n;
}

/* ---------- culling ---------- */
static float g_fr_tx, g_fr_ty, g_fr_nx, g_fr_ny; static int g_fr_valid;

static int frustum_visible(V3 c, float r) {
    if (!g_fr_valid) {
        float ty = tanf(C.fov * PI / 360.0f), tx = ty * (float)g_pp.BackBufferWidth / (float)g_pp.BackBufferHeight;
        g_fr_ty = ty; g_fr_tx = tx;
        g_fr_nx = 1.0f / sqrtf(1 + tx * tx); g_fr_ny = 1.0f / sqrtf(1 + ty * ty);
        g_fr_valid = 1;
    }
    if (c.z + r < 0.2f || c.z - r > 300.0f) return 0;
    if ((c.x - g_fr_tx * c.z) * g_fr_nx > r) return 0;
    if ((-c.x - g_fr_tx * c.z) * g_fr_nx > r) return 0;
    if ((c.y - g_fr_ty * c.z) * g_fr_ny > r) return 0;
    if ((-c.y - g_fr_ty * c.z) * g_fr_ny > r) return 0;
    return 1;
}

/* ---------- adaptive quality ----------
 * adaptive_fps > 0: the frame rate (smoothed) is compared to the target every 250 ms. Below it, the lightmap / reflection / LoD
 * distances are multiplied by g_adapt (down to adaptive_min): more objects switch to the cheap "distant" path. With adaptive_res = 1 the
 * internal resolution (render_scale) follows the same scale down to adaptive_res_min (quantized, so the offscreen target is rarely rebuilt).
 * Above the target (with margin) everything goes back up towards full quality. */
static float g_adapt = 1.0f, g_adapt_rs = 1.0f; static double g_ad_last, g_ad_ema, g_ad_t, g_ad_rs_t;
static double ad_now(void) { LARGE_INTEGER c, fq; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&fq); return (double)c.QuadPart / (double)fq.QuadPart; }
static void adaptive_tick(void) {
    double t = ad_now(), dt = t - g_ad_last; g_ad_last = t;
    if (C.adaptive_fps <= 0) { g_adapt = 1.0f; g_adapt_rs = 1.0f; g_ad_ema = 0; return; }
    if (dt <= 0 || dt > 0.25) { g_ad_ema = 0; return; }               /* pause (bake, menu reload, window drag): ignore */
    g_ad_ema = g_ad_ema > 0 ? g_ad_ema * 0.92 + dt * 0.08 : dt;
    if (t - g_ad_t < 0.25) return;
    g_ad_t = t;
    {   double fps = 1.0 / g_ad_ema;
        if (fps < C.adaptive_fps * 0.97) g_adapt -= 0.06f * (float)((C.adaptive_fps - fps) / C.adaptive_fps > 0.3 ? 2 : 1);
        else if (fps > C.adaptive_fps * 1.15) g_adapt += 0.03f;
        if (g_adapt < C.adaptive_min) g_adapt = C.adaptive_min;
        if (g_adapt > 1.0f) g_adapt = 1.0f;
    }
    if (C.adaptive_res) {                                              /* resolution factor from the same scale, 1/16 steps, at most every 0.5 s */
        float k = C.adaptive_min < 0.999f ? (g_adapt - C.adaptive_min) / (1.0f - C.adaptive_min) : 1.0f, f;
        f = C.adaptive_res_min + (1.0f - C.adaptive_res_min) * k; f = floorf(f * 16.0f + 0.5f) / 16.0f;
        if (f != g_adapt_rs && t - g_ad_rs_t >= 0.5) { g_adapt_rs = f; g_ad_rs_t = t; }
    } else g_adapt_rs = 1.0f;
}

static int g_blend_pass;             /* 1 while drawing the alpha-blended objects (after all the opaque ones, far to near) */
static void draw_object(Obj* o) {
    DWORD s = 1; int aniso = C.anisotropy < (int)g_maxaniso ? C.anisotropy : (int)g_maxaniso, lod = 0;
    M4 w = g_world; V3 c; float dist; IDirect3DVertexBuffer9* g_vb = o->vb;
    if (o->is_lamp && !o->lamp_world) { V3 lp = g_lightset[o->light_idx < 0 ? 0 : o->light_idx].pos;
                      w = m_mul(m_trans(lp.x, lp.y, lp.z), g_world); }
    c = m_pt(m_pt(o->bs_c, w), g_view); dist = vlen(c);
    if (o->no_draw) return;
    if (C.frustum_culling && !frustum_visible(c, o->bs_r)) return;
    if ((o->blend != 0) != (g_blend_pass != 0)) return;          /* blended objects only in the blend pass, the others only before */
    if (g_depth_pass && o->atest) return;                        /* cut-outs write no depth in the prepass (they are tested in the color pass) */
    if (o->nlod > 1 && dist > C.lod_start * g_adapt) {                       /* LoD based on distance */
        float lstep = C.lod_step * g_adapt;
        lod = 1 + (int)((dist - C.lod_start * g_adapt) / (lstep > 1 ? lstep : 1));
        if (lod > o->nlod - 1) lod = o->nlod - 1;
    }
    if (o->glass) { if (!g_depth_pass) { draw_glass(o, w); sc_reset(); g_drawn++; } return; }   /* glass changes states behind the filter's back */
    if (!C.state_cache || !g_sc_worldk || memcmp(&g_sc_world, &w, sizeof w) != 0) {
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, (D3DMATRIX*)&w); g_sc_world = w; g_sc_worldk = 1;
    }
    if (g_depth_pass) {
        /* depth pass : write only, no texture sampling.
         * The color is hidden anyway, only the depth buffer matters. */
        cts(0, D3DTSS_COLOROP, D3DTOP_DISABLE); cts(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        cts(2, D3DTSS_COLOROP, D3DTOP_DISABLE); cts(3, D3DTSS_COLOROP, D3DTOP_DISABLE);
    } else
    if (o->is_lamp) {
        V3 lc = g_lightset[o->light_idx < 0 ? 0 : o->light_idx].col;
        float m = fmaxf(lc.x, fmaxf(lc.y, lc.z)); if (m < 0.01f) m = 1.0f;
        RS(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(clampf(lc.x / m, 0, 1), clampf(lc.y / m, 0, 1), clampf(lc.z / m, 0, 1), 1.0f));
        stage_setup(0, (IDirect3DBaseTexture9*)g_tex_white, D3DTOP_SELECTARG1, D3DTA_TFACTOR, D3DTA_CURRENT, 0, D3DTTFF_DISABLE, 0, 1, 0, 0);
    } else if (C.lean_stages && !o->textured) {
        /* Untextured object: stage 0 would only multiply the vertex color by a 1x1 white texture (x1.0).
         * Same image without it: the lightmap (or nothing) becomes stage 0, one fewer stage and fetch per pixel. */
        s = 0;
        if (o->lightmap && dist >= C.overlay_distance * g_adapt && o->vbf) {
            g_vb = o->vbf;
            stage_setup(s++, NULL, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_DIFFUSE, 0, D3DTTFF_DISABLE, 0, 1, 0, 0);
        } else if (o->lightmap) {
            IDirect3DBaseTexture9* lt = (dist < C.overlay_distance * g_adapt) ? (IDirect3DBaseTexture9*)o->tex_lm : (IDirect3DBaseTexture9*)g_tex_far;
            stage_setup(s++, lt, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, 1, D3DTTFF_DISABLE, 1, 1, 0, 0);
        } else {
            stage_setup(s++, NULL, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_DIFFUSE, 0, D3DTTFF_DISABLE, 0, 1, 0, 0);
        }
        if (o->reflect && dist < C.reflection_distance * g_adapt) {
            M4 mv = m_mul(w, g_view), T = m_ident(); int i, j;
            for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) T.m[i][j] = mv.m[j][i];
            IDirect3DDevice9_SetTransform(g_dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), (D3DMATRIX*)&T);
            RS(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB((int)(clampf(o->refl, 0, 1) * 255.0f), 0, 0, 0));
            stage_setup(s, (IDirect3DBaseTexture9*)o->tex_cube, D3DTOP_BLENDFACTORALPHA, D3DTA_TEXTURE, D3DTA_CURRENT,
                        D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR, D3DTTFF_COUNT3, g_cube_linear ? 1 : 0, 1, 0, 1);
            if (C.reflection_lod_bias != 0.0f) {
                float b = C.reflection_lod_bias; DWORD db; memcpy(&db, &b, 4);
                cts(s, (D3DTEXTURESTAGESTATETYPE)19, db);
            }
            s++;
        }
    } else {
        IDirect3DTexture9* t = o->textured ? o->tex_albedo : g_tex_white;
        stage_setup(0, (IDirect3DBaseTexture9*)t, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, 0, D3DTTFF_DISABLE, aniso > 1 ? 2 : 1, aniso, 1, o->textured);
        if (o->refl_layer) {                    /* model reflection layer: the alpha of the albedo texture is the per-texel reflectivity */
            cts(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); cts(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        }
        if (o->lightmap && dist >= C.overlay_distance * g_adapt && o->vbf) {
            g_vb = o->vbf;             /* average light in the vertex color: stage 1 not needed */
        } else if (o->lightmap) {  /* overlay 1: shadows / AO / colored light */
            IDirect3DBaseTexture9* lt = (dist < C.overlay_distance * g_adapt) ? (IDirect3DBaseTexture9*)o->tex_lm : (IDirect3DBaseTexture9*)g_tex_far;
            stage_setup(s++, lt, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, 1, D3DTTFF_DISABLE, 1, 1, 0, 0);
        }
        if (o->reflect && dist < C.reflection_distance * g_adapt) {   /* overlay 2: reflections */
            M4 mv = m_mul(w, g_view), T = m_ident(); int i, j;
            for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) T.m[i][j] = mv.m[j][i];   /* view -> object */
            IDirect3DDevice9_SetTransform(g_dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), (D3DMATRIX*)&T);
            RS(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB((int)(clampf(o->refl, 0, 1) * 255.0f), 0, 0, 0));
            stage_setup(s, (IDirect3DBaseTexture9*)o->tex_cube, o->refl_layer ? D3DTOP_BLENDCURRENTALPHA : D3DTOP_BLENDFACTORALPHA, D3DTA_TEXTURE, D3DTA_CURRENT,
                        D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR, D3DTTFF_COUNT3, g_cube_linear ? 1 : 0, 1, 0, 1);  /* mips enabled */
            /* Mip level bias (D3DTSS_MIPLODBIAS = 19, float value).
             * Forcing smaller levels = blurrier reflections but ~4x less
             * memory to read per pixel: this is the bandwidth lever that
             * matters on an iGPU that reads from shared DDR2.
             * Models add their own bias (roughness): rough surfaces see a blurrier environment. */
            if (C.reflection_lod_bias != 0.0f || o->lodb != 0.0f || o->is_model) {
                float b = C.reflection_lod_bias + o->lodb; DWORD db; memcpy(&db, &b, 4);
                cts(s, (D3DTEXTURESTAGESTATETYPE)19, db);
            }
            s++;
        }
    }
    cts(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (o->is_model && !g_depth_pass) {                              /* 3D model: cut-out / transparency / double-sided states */
        if (o->two_sided) RS(D3DRS_CULLMODE, D3DCULL_NONE);
        if (o->atest) {
            RS(D3DRS_ALPHATESTENABLE, TRUE); RS(D3DRS_ALPHAREF, 128); RS(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
            if (C.depth_prepass) RS(D3DRS_ZFUNC, D3DCMP_LESS);           /* its depth was not written by the prepass */
        }
        if (o->blend) { RS(D3DRS_ALPHABLENDENABLE, TRUE); RS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); RS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); RS(D3DRS_ZWRITEENABLE, FALSE); }
        if (o->blend || o->atest) {
            cts(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); cts(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            cts(0, D3DTSS_ALPHAOP, o->textured ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        }
    }
    if (!C.state_cache || g_sc_vb != g_vb) { IDirect3DDevice9_SetStreamSource(g_dev, 0, g_vb, 0, sizeof(Vtx)); g_sc_vb = g_vb; }
    if (!C.state_cache || g_sc_ib != o->ib[lod]) { D9(SetIndices)(g_dev, o->ib[lod], 0); g_sc_ib = o->ib[lod]; }   /* explicit BaseVertexIndex: the MinGW macro omits it */
    IDirect3DDevice9_DrawIndexedPrimitive(g_dev, D3DPT_TRIANGLELIST, 0, 0, o->nv, 0, o->nt[lod]);
    g_drawn++;
    if (o->is_model && !g_depth_pass) {
        if (o->two_sided) RS(D3DRS_CULLMODE, D3DCULL_CCW);
        if (o->atest) { RS(D3DRS_ALPHATESTENABLE, FALSE); if (C.depth_prepass) RS(D3DRS_ZFUNC, D3DCMP_EQUAL); }
        if (o->blend) { RS(D3DRS_ALPHABLENDENABLE, FALSE); RS(D3DRS_ZWRITEENABLE, TRUE); }
    }
}

/* ---------- FXAA (ps_2_0) ---------- */
typedef struct { HRESULT (WINAPI *QI)(void*, REFIID, void**); ULONG (WINAPI *AddRef)(void*); ULONG (WINAPI *Release)(void*);
                 LPVOID (WINAPI *GetBufferPointer)(void*); DWORD (WINAPI *GetBufferSize)(void*); } XBufVtbl;
typedef struct { const XBufVtbl* v; } XBuf;
typedef HRESULT (WINAPI *PFN_XCompile)(LPCSTR, UINT, const void*, void*, LPCSTR, LPCSTR, DWORD, XBuf**, XBuf**, void**);

static const char FXAA_SRC[] =
    "sampler2D s0 : register(s0);\n"
    "float2 px : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 lm = float3(0.299, 0.587, 0.114);\n"
    "  float3 cNW = tex2D(s0, uv + float2(-1, -1) * px).rgb;\n"
    "  float3 cNE = tex2D(s0, uv + float2( 1, -1) * px).rgb;\n"
    "  float3 cSW = tex2D(s0, uv + float2(-1,  1) * px).rgb;\n"
    "  float3 cSE = tex2D(s0, uv + float2( 1,  1) * px).rgb;\n"
    "  float3 cM  = tex2D(s0, uv).rgb;\n"
    "  float lNW = dot(cNW, lm), lNE = dot(cNE, lm), lSW = dot(cSW, lm), lSE = dot(cSE, lm), lM = dot(cM, lm);\n"
    "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  float2 dir;\n"
    "  dir.x = -((lNW + lNE) - (lSW + lSE));\n"
    "  dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
    "  float red = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);\n"
    "  float inv = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);\n"
    "  dir = clamp(dir * inv, -8.0, 8.0) * px;\n"
    "  float3 cA = 0.5 * (tex2D(s0, uv + dir * (1.0 / 3.0 - 0.5)).rgb + tex2D(s0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  float3 cB = cA * 0.5 + 0.25 * (tex2D(s0, uv + dir * -0.5).rgb + tex2D(s0, uv + dir * 0.5).rgb);\n"
    "  float lB = dot(cB, lm);\n"
    "  float3 r = (lB < lMin || lB > lMax) ? cA : cB;\n"
    "  return float4(r, 1.0);\n"
    "}\n";

static int load_fxaa(void) {
    char dir[MAX_PATH], path[MAX_PATH]; FILE* f; void* code = NULL; size_t sz = 0; int i;
    cache_dir(dir); sprintf(path, "%sfxaa_ps20.psc", dir);
    f = fopen(path, "rb");                                        /* already compiled bytecode? */
    if (f) {
        fseek(f, 0, SEEK_END); sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) { code = malloc(sz); if (fread(code, 1, sz, f) != sz) { free(code); code = NULL; } }
        fclose(f);
    }
    if (!code) {                                                  /* otherwise: compile via d3dx9_xx.dll (loaded dynamically) */
        PFN_XCompile comp = NULL;
        for (i = 43; i >= 24 && !comp; i--) {
            char nm[32]; HMODULE m; sprintf(nm, "d3dx9_%d.dll", i);
            m = LoadLibraryA(nm);
            if (m) comp = (PFN_XCompile)(void*)GetProcAddress(m, "D3DXCompileShader");
        }
        if (comp) {
            XBuf* b = NULL; XBuf* err = NULL;
            if (SUCCEEDED(comp(FXAA_SRC, (UINT)strlen(FXAA_SRC), NULL, NULL, "main", "ps_2_0", 0, &b, &err, NULL)) && b) {
                sz = b->v->GetBufferSize(b); code = malloc(sz); memcpy(code, b->v->GetBufferPointer(b), sz);
                b->v->Release(b);
                f = fopen(path, "wb"); if (f) { fwrite(code, 1, sz, f); fclose(f); }
            }
            if (err) err->v->Release(err);
        }
    }
    if (!code) return 0;
    i = SUCCEEDED(IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD*)code, &g_fxaa_ps));
    free(code);
    return i;
}

/* ---------- GPU bake: reflections by rasterization ----------
 * Instead of casting 6*S*S*nsamp rays per object, we draw the scene 6 times
 * (once per cubemap face, nsamp times for roughness) from the
 * probe point, into a 4-byte G-buffer: (hit object, lightmap texel index).
 * The matrix is sent row by row (with row vectors, like the rest of the engine).
 * ps_2_0: no dependency on ps_3_0 (no MRT, no float format). */

/* log file next to the exe: explains why a GPU path was refused */
static void gpu_log(const char* fmt, ...) {
    FILE* f = fopen("engine_gpu.log", "a"); va_list ap;
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
/* compiles a shader via d3dx9 (loaded dynamically) with a disk cache, like FXAA */
static void* bake_compile(const char* src, const char* target, const char* name) {
    char dir[MAX_PATH], path[MAX_PATH]; FILE* f; void* code = NULL; size_t sz = 0; int i;
    PFN_XCompile comp = NULL; XBuf *b = NULL, *err = NULL;
    if (!strncmp(src, "ps_2_0", 6) || !strncmp(src, "vs_2_0", 6)) {         /* assembly: built-in assembler, no d3dx9 */
        char em[160]; int nt = 0; DWORD* tk = sm_assemble(src, &nt, em, sizeof em);
        if (!tk) { gpu_log("shader %s: assembly error: %s", name, em); return NULL; }
        return tk;
    }
    /* the cache name includes a fingerprint of the source: editing a shader does not reuse
       the old bytecode (otherwise the changes are silently ignored) */
    {   unsigned long hsh = 2166136261u; const char* q = src;
        while (*q) { hsh = (hsh ^ (unsigned char)*q++) * 16777619u; }
        cache_dir(dir); sprintf(path, "%s%s_%08lx_%lu.psc", dir, name, hsh, (unsigned long)strlen(src));
    }
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) { code = malloc(sz); if (fread(code, 1, sz, f) != sz) { free(code); code = NULL; } }
        fclose(f);
    }
    if (!code) {
        for (i = 43; i >= 24 && !comp; i--) {
            char nm[32]; HMODULE m; sprintf(nm, "d3dx9_%d.dll", i);
            m = LoadLibraryA(nm);
            if (m) comp = (PFN_XCompile)(void*)GetProcAddress(m, "D3DXCompileShader");
        }
        if (!comp) gpu_log("shader %s: no d3dx9_xx.dll (install the DirectX End-User Runtime) -> GPU bake unavailable", name);
        else {
            HRESULT hr = comp(src, (UINT)strlen(src), NULL, NULL, "main", target, 0, &b, &err, NULL);
            if (SUCCEEDED(hr) && b) {
                sz = b->v->GetBufferSize(b); code = malloc(sz); memcpy(code, b->v->GetBufferPointer(b), sz);
                f = fopen(path, "wb"); if (f) { fwrite(code, 1, sz, f); fclose(f); }
            } else gpu_log("shader %s (%s): compilation failed hr=0x%08lx %s", name, target, (unsigned long)hr, err ? (const char*)err->v->GetBufferPointer(err) : "");
        }
    }
    if (b) b->v->Release(b);
    if (err) err->v->Release(err);
    return code;
}

/* 4 taps on a disk: roughness (the CPU ray cone becomes a camera tilt) */
static const float BAKE_TAP[4][2] = { { 0.7f, 0.0f }, { 0.0f, 0.7f }, { -0.7f, 0.0f }, { 0.0f, -0.7f } };

/* entry point called by the bake: 1 = G-buffer produced by the GPU (gpubake.h), 0 = CPU fallback */
static int gpu_reflections_all(void);
static int bake_reflections_gpu(void) {
    if (!C.bake_gpu || !C.bake_gpu_reflections || !g_dev) return 0;
    return gpu_reflections_all();
}

/* Offscreen scene target. Its size is backbuffer * render_scale; the (larger) auto depth buffer is reused as is
 * (D3D9 only requires depth >= render target). Used by FXAA and/or by render_scale < 1. */
static float rt_scale(void) { float s = C.render_scale * g_adapt_rs; return s < 0.35f ? 0.35f : (s > 1.0f ? 1.0f : s); }
static void create_rt(void) {
    float sc = rt_scale(); int w, h;
    g_rt_bw = (int)g_pp.BackBufferWidth; g_rt_bh = (int)g_pp.BackBufferHeight; g_rt_ks = sc;
    w = (int)(g_rt_bw * sc + 0.5f); h = (int)(g_rt_bh * sc + 0.5f); if (w < 64) w = 64; if (h < 64) h = 64;
    g_rt_w = w; g_rt_h = h;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_rt, NULL))) { g_fxaa_on = 0; return; }
    IDirect3DTexture9_GetSurfaceLevel(g_rt, 0, &g_rts);
}
static void release_rt(void) {
    g_rt_ks = -1.0f;
    if (g_rts) { IDirect3DSurface9_Release(g_rts); g_rts = NULL; }
    if (g_rt) { IDirect3DTexture9_Release(g_rt); g_rt = NULL; }
}

typedef struct { float x, y, z, rhw, u, v; } TLV;
#define FVF_TLV (D3DFVF_XYZRHW | D3DFVF_TEX1)
typedef struct { float x, y, z, rhw; DWORD c; } CV;
#define FVF_CV (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* Creates / resizes / frees the offscreen target when FXAA or render_scale changed (a failed creation is not retried
 * until something changes). render_scale < 1 is ignored on a multisampled device: the RT would not match the depth buffer. */
static void rt_sync(void) {
    float sc = rt_scale();
    int want = g_fxaa_on || (sc < 0.999f && g_pp.MultiSampleType == D3DMULTISAMPLE_NONE);
    if (!want) { if (g_rts || g_rt) release_rt(); return; }
    if (g_rt_ks != sc || g_rt_bw != (int)g_pp.BackBufferWidth || g_rt_bh != (int)g_pp.BackBufferHeight) { release_rt(); create_rt(); }
}
static void draw_fxaa_quad(void) {
    float w = (float)g_pp.BackBufferWidth, h = (float)g_pp.BackBufferHeight, rc[4] = { 1.0f / (float)g_rt_w, 1.0f / (float)g_rt_h, 0, 0 };
    TLV q[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
    RS(D3DRS_ZENABLE, D3DZB_FALSE); RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetFVF(g_dev, FVF_TLV);
    IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9*)g_rt);
    ts(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); ts(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); ts(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    ts(0, D3DTSS_TEXCOORDINDEX, 0); ts(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE); ts(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    ss(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); ss(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); ss(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    ss(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(0, D3DSAMP_SRGBTEXTURE, FALSE);
    if (g_fxaa_on && g_fxaa_ps) { IDirect3DDevice9_SetPixelShader(g_dev, g_fxaa_ps); IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, rc, 1); }   /* else: plain bilinear upscale */
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(TLV));
    IDirect3DDevice9_SetPixelShader(g_dev, NULL);
}

/* ---------- frame ---------- */
static int dev_ready(void) {
    HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(g_dev);
    if (hr == D3DERR_DEVICELOST) { Sleep(50); return 0; }
    if (hr == D3DERR_DEVICENOTRESET) {
        release_rt();
        if (FAILED(IDirect3DDevice9_Reset(g_dev, &g_pp))) { Sleep(50); return 0; }
        create_rt();
        frame_states(); g_cam_valid = 0;   /* after Reset, the GPU state is lost */
    }
    return 1;
}

/* timing (title bar): "draws" = CPU time spent building and submitting the frame (BeginScene..EndScene),
 * "wait" = the rest of the frame (Clear + Present: this is where the driver blocks when the GPU is the
 * slower one). Heuristic: wait >> draws = GPU bound; draws ~ frame time = CPU bound. */
static double g_tm_draw, g_tm_wait; static int g_tm_n;
static double tm_now(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return 1000.0 * (double)c.QuadPart / (double)f.QuadPart; }

static void render(void) {
    float aspect = (float)g_pp.BackBufferWidth / (float)g_pp.BackBufferHeight; int i, fx;
    IDirect3DSurface9* bb = NULL;
    D3DCOLOR bg = D3DCOLOR_XRGB((int)(C.background_r * 255), (int)(C.background_g * 255), (int)(C.background_b * 255));
    double t0, t1, t2, t3;
    if (!dev_ready()) return;
    adaptive_tick();
    t0 = tm_now(); sc_reset();
    rt_sync();
    fx = g_rts != NULL;
    /* the camera only moves with the mouse: matrices and frustum are only recomputed when needed */
    if (!g_cam_valid || g_lyaw != g_yaw || g_lpitch != g_pitch || g_lcpos.x != g_cpos.x || g_lcpos.y != g_cpos.y || g_lcpos.z != g_cpos.z) {
        float cp = cosf(g_pitch); V3 L = v3(sinf(g_yaw) * cp, sinf(g_pitch), cosf(g_yaw) * cp), R = v3(cosf(g_yaw), 0.0f, -sinf(g_yaw)), U = vcross(L, R);
        g_world = m_ident();                     /* the scene is in world coordinates; the whole camera is in g_view */
        g_view = m_ident();
        g_view.m[0][0] = R.x; g_view.m[0][1] = U.x; g_view.m[0][2] = L.x;
        g_view.m[1][0] = R.y; g_view.m[1][1] = U.y; g_view.m[1][2] = L.y;
        g_view.m[2][0] = R.z; g_view.m[2][1] = U.z; g_view.m[2][2] = L.z;
        g_view.m[3][0] = -vdot(R, g_cpos); g_view.m[3][1] = -vdot(U, g_cpos); g_view.m[3][2] = -vdot(L, g_cpos);
        g_proj = m_persp(C.fov * PI / 180.0f, aspect, 0.2f, 300.0f);
        g_lyaw = g_yaw; g_lpitch = g_pitch; g_lcpos = g_cpos;
        g_cam_valid = 1; g_fr_valid = 0;
    }
    if (fx) { IDirect3DDevice9_GetRenderTarget(g_dev, 0, &bb); IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_rts); }
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, bg, 1.0f, 0);
    t1 = tm_now();
    IDirect3DDevice9_BeginScene(g_dev);
    frame_states();
    IDirect3DDevice9_SetFVF(g_dev, FVF);
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, (D3DMATRIX*)&g_view);
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, (D3DMATRIX*)&g_proj);
    g_drawn = 0;
    g_norder = build_draw_order();
    /* ---------- optional depth pass ----------
     * We first write the depth without sampling any texture, then the
     * color pass tests for equality: only the nearest surface gets colored.
     * EQUAL (not LESS) avoids holes when two surfaces are coplanar. */
    if (C.depth_prepass) {
        g_depth_pass = 1;
        RS(D3DRS_COLORWRITEENABLE, 0);            /* color writing disabled */
        RS(D3DRS_ZFUNC, D3DCMP_LESS);
        for (i = 0; i < g_norder; i++) draw_object(&g_obj[g_order[i]]);
        RS(D3DRS_COLORWRITEENABLE, 0xF);
        RS(D3DRS_ZFUNC, D3DCMP_EQUAL);
        g_depth_pass = 0;
    }
    for (i = 0; i < g_norder; i++) draw_object(&g_obj[g_order[i]]);
    if (C.depth_prepass) RS(D3DRS_ZFUNC, D3DCMP_LESS);
    {   int any = 0;                                           /* transparent objects (3D models): after the opaque ones, far to near */
        for (i = 0; i < g_norder; i++) if (g_obj[g_order[i]].blend) { any = 1; break; }
        if (any) { g_blend_pass = 1; for (i = g_norder - 1; i >= 0; i--) draw_object(&g_obj[g_order[i]]); g_blend_pass = 0; }
    }
    t2 = tm_now();
    if (!fx) gui_frame_draw();                    /* HUD / menu in the same scene: no extra BeginScene/EndScene */
    IDirect3DDevice9_EndScene(g_dev);
    if (fx) {
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, bb); IDirect3DSurface9_Release(bb);
        IDirect3DDevice9_BeginScene(g_dev); draw_fxaa_quad(); gui_frame_draw(); IDirect3DDevice9_EndScene(g_dev);
    }
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    t3 = tm_now();
    g_tm_draw += t2 - t1; g_tm_wait += (t1 - t0) + (t3 - t2); g_tm_n++;
}

/* ---------- loading screen (loading_hz frames/s) ---------- */
static void rect_cv(CV* v, float x0, float y0, float x1, float y1, DWORD c) {
    CV r[6] = { { x0, y0, 0, 1, c }, { x1, y0, 0, 1, c }, { x0, y1, 0, 1, c }, { x1, y0, 0, 1, c }, { x1, y1, 0, 1, c }, { x0, y1, 0, 1, c } };
    memcpy(v, r, sizeof r);
}
static void loading_frame(float frac) {
    MSG msg; char title[96]; CV v[18]; float w, h, bx0, bx1, by0, by1, sp;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { g_running = 0; g_abort = 1; }
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    sprintf(title, "D3D9 Engine - loading / bake (%s)... %d%%", g_gpu_direct ? "GPU+CPU" : "CPU", (int)(frac * 100.0f));
    SetWindowTextA(g_hwnd, title);
    if (!g_dev || !dev_ready()) return;
    w = (float)g_pp.BackBufferWidth; h = (float)g_pp.BackBufferHeight;
    bx0 = w * 0.2f; bx1 = w * 0.8f; by0 = h * 0.47f; by1 = h * 0.53f;
    sp = bx0 + (bx1 - bx0 - 40.0f) * (0.5f + 0.5f * sinf(GetTickCount() * 0.004f));
    rect_cv(v, bx0 - 4, by0 - 4, bx1 + 4, by1 + 4, 0xFF5A5F6B);               /* frame */
    rect_cv(v + 6, bx0, by0, bx1, by1, 0xFF14161B);                            /* background */
    rect_cv(v + 12, bx0, by0, bx0 + (bx1 - bx0) * clampf(frac, 0, 1), by1, 0xFF4D8EF0);   /* progress */
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0xFF0A0B0E, 1.0f, 0);
    IDirect3DDevice9_BeginScene(g_dev);
    RS(D3DRS_ZENABLE, D3DZB_FALSE); RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_SRGBWRITEENABLE, FALSE); RS(D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetPixelShader(g_dev, NULL);
    IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
    ts(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); ts(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE); ts(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    ts(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE); ts(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetFVF(g_dev, FVF_CV);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 6, v, sizeof(CV));
    { CV s[6]; rect_cv(s, sp, by1 + 14, sp + 40.0f, by1 + 22, 0xFF4D8EF0);   /* activity indicator */
      IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 2, s, sizeof(CV)); }
    IDirect3DDevice9_EndScene(g_dev);
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
}

/* ---------- initialization ---------- */
static void init_d3d(void) {
    D3DCAPS9 caps; DWORD flags; HRESULT hr; D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) fatal("Direct3DCreate9 failed");
    IDirect3D9_GetDeviceCaps(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    g_maxaniso = caps.MaxAnisotropy ? caps.MaxAnisotropy : 1;
    g_cube_linear = (caps.CubeTextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) ? 1 : 0;
    /* A driver can report a maximum size lower than lightmap_max_size
     * (often 1024 or 2048 on old iGPUs): layout_obj bounds the atlas. */
    g_maxtex = caps.MaxTextureWidth;
    if (caps.MaxTextureHeight && caps.MaxTextureHeight < (DWORD)g_maxtex) g_maxtex = (int)caps.MaxTextureHeight;
    if (g_maxtex < 32) g_maxtex = 32;
    if (C.reflection_size > g_maxtex) C.reflection_size = g_maxtex;
    memset(&g_pp, 0, sizeof g_pp);
    g_pp.BackBufferWidth = C.width; g_pp.BackBufferHeight = C.height;
    g_pp.BackBufferFormat = D3DFMT_X8R8G8B8; g_pp.BackBufferCount = 1;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD; g_pp.hDeviceWindow = g_hwnd; g_pp.Windowed = !C.fullscreen;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = SUCCEEDED(IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24X8)) ? D3DFMT_D24X8 : D3DFMT_D16;
    g_pp.PresentationInterval = C.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!C.fxaa && C.render_scale >= 0.999f && C.msaa >= 2
        && SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, g_pp.Windowed, (D3DMULTISAMPLE_TYPE)C.msaa, NULL))
        && SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(g_d3d, 0, D3DDEVTYPE_HAL, g_pp.AutoDepthStencilFormat, g_pp.Windowed, (D3DMULTISAMPLE_TYPE)C.msaa, NULL)))
        ms = (D3DMULTISAMPLE_TYPE)C.msaa;
    g_pp.MultiSampleType = ms;
    flags = D3DCREATE_FPU_PRESERVE | ((caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING);
    /* PUREDEVICE: the runtime stops validating / shadowing the device state (Get*State no longer work;
     * the engine does not use them). Needs hardware vertex processing. If refused: normal device. */
    hr = D3DERR_NOTAVAILABLE;
    if (C.pure_device && (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING))
        hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags | D3DCREATE_PUREDEVICE, &g_pp, &g_dev);
    if (FAILED(hr)) hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &g_pp, &g_dev);
    if (FAILED(hr)) fatal("CreateDevice failed (GPU without D3D9 support?)");
    if (caps.MaxTextureBlendStages < 3) fatal("The GPU must provide at least 3 texture stages");
    if (C.fxaa) {
        g_fxaa_on = (caps.PixelShaderVersion >= D3DPS_VERSION(2, 0)) && load_fxaa();
        if (g_fxaa_on) create_rt();
    }
}

static void init_common_textures(void) {
    uint32_t w = 0xFFFFFFFFu, g;
    g_tex_white = make_texture(&w, 1, 1, 0);
    { float e = C.gamma_correct ? powf(clampf(C.far_brightness, 0, 1), 1.0f / 2.2f) : C.far_brightness; g = pack8(e, e, e); }
    g_tex_far = make_texture(&g, 1, 1, 0);
}
