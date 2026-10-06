/* gpubake.h - GPU-accelerated bake, Shader Model 2.0 (vs_2_0 / ps_2_0)
 *
 * SM 2.0 constraints respected (so any D3D9 "SM2" GPU works):
 *   - no floating-point texture, no MRT, no dynamic loop or branch
 *   - 64 arithmetic / 32 texture instructions per pixel shader
 * An SM2 GPU cannot traverse a BVH: ray tracing stays on the CPU. Soft shadows,
 * which cost most of the bake (see engine.ini), are done with shadow mapping:
 *
 *   for each shadow-casting light, for each batch of GPU_TAPS points sampled on the lamp sphere:
 *     1. render the scene depth into GPU_TAPS cubemaps (distance to the point, RGBA8: 16 bits)
 *     2. "draw" each lightmap in UV space: the pixel shader reads the GPU_TAPS cubemaps back
 *        and adds (with 8-bit additive blending, so exact integers) the number of visible points
 *   Result per texel: visibility = number of visible points / number of points (0..1).
 *   The CPU keeps the irradiance at the lamp center (ndl/d^2): E = visibility x ndl/d^2.
 *
 * Any failure (GPU without SM2, render cubemap refused...) returns 0: the CPU bake takes over. */
#pragma once

#define GPU_MAXTAPS 4

static int gpu_alloc_res(void);
static int g_gpu_state = -1;                         /* -1 unknown, 0 unavailable, 1 ready */
static int g_gpu_taps, g_gpu_res;
static IDirect3DCubeTexture9* g_gpu_cube[GPU_MAXTAPS];
static IDirect3DSurface9* g_gpu_face[GPU_MAXTAPS][6];
static IDirect3DSurface9* g_gpu_ds;
static IDirect3DVertexShader9 *g_gpu_vs_cube, *g_gpu_vs_lm;
static IDirect3DPixelShader9 *g_gpu_ps_cube, *g_gpu_ps_shadow;

/* ---------- shaders (HLSL -> bytecode via d3dx9, cached; embedded version as a fallback) ---------- */
static const char GPU_VS_CUBE_SRC[] =
    "float4 c0 : register(c0); float4 c1 : register(c1); float4 c2 : register(c2); float4 c3 : register(c3);\n"
    "float4 ls : register(c4);\n"                                       /* lamp point */
    "struct VOut { float4 p : POSITION; float3 v : TEXCOORD0; };\n"
    "VOut main(float3 pos : POSITION) {\n"
    "  VOut o; float4 p = float4(pos, 1.0);\n"
    "  o.p = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"  /* c0..c3 = view*projection columns */
    "  o.v = pos - ls.xyz;\n"
    "  return o;\n"
    "}\n";

/* distance to the lamp point, encoded on 2 8-bit channels (16 bits) */
static const char GPU_PS_CUBE_SRC[] =
    "float4 prm : register(c0);\n"                                      /* x = 1 / zfar */
    "float4 main(float3 v : TEXCOORD0) : COLOR {\n"
    "  float d = sqrt(dot(v, v)) * prm.x;\n"
    "  float2 e = frac(d * float2(1.0, 255.0));\n"
    "  e.x -= e.y * (1.0 / 255.0);\n"
    "  return float4(e.x, e.y, 0.0, 1.0);\n"
    "}\n";

/* lightmap in UV space: the vertex is placed at its lightmap UV; the world position is offset along
 * the normal (anti-acne bias) HERE, in the vertex shader: the ps_2_0 pixel shader only has 64 instructions. */
static const char GPU_VS_LM_SRC[] =
    "float4 ctl : register(c0);\n"                                      /* x = 1/aw, y = 1/ah (D3D9 half-texel) */
    "float4 lamp : register(c1);\n"                                     /* center of the light */
    "float4 bias : register(c2);\n"                                     /* x = fixed offset, y = offset / distance */
    "struct VOut { float4 p : POSITION; float3 w : TEXCOORD0; };\n"
    "VOut main(float3 pos : POSITION, float3 nrm : NORMAL, float2 t : TEXCOORD1) {\n"
    "  VOut o; float3 d = pos - lamp.xyz;\n"
    "  o.p = float4(t.x * 2.0 - 1.0 - ctl.x, 1.0 - t.y * 2.0 + ctl.y, 0.5, 1.0);\n"
    "  o.w = pos + nrm * (bias.x + bias.y * sqrt(dot(d, d)));\n"
    "  return o;\n"
    "}\n";

static char g_gpu_ps_shadow_src[4096];
/* Per lamp point: sub, dp3, mul/rsq, 1 cubemap read, 2 mad, 1 add  (~9 instructions)
 *   visible = saturate((stored_depth - distance + eps) * 4096 + 0.5): binary except to about 1/4096. */
static void gpu_build_shadow_src(int taps) {
    char* o = g_gpu_ps_shadow_src; int i;
    for (i = 0; i < taps; i++) o += sprintf(o, "samplerCUBE S%d : register(s%d);\nfloat4 L%d : register(c%d);\n", i, i, i, i);
    o += sprintf(o, "float4 K : register(c%d);\n", taps);                /* x = zf*4096, y = eps*4096+0.5, z = 4096, w = 1/255 */
    o += sprintf(o, "float4 main(float3 P : TEXCOORD0) : COLOR {\n  float s = 0.0;\n");
    for (i = 0; i < taps; i++) {
        o += sprintf(o, "  { float3 v = P - L%d.xyz; float d2 = dot(v, v) + 1e-8;\n", i);
        o += sprintf(o, "    float2 t = texCUBE(S%d, v).xy;\n", i);
        o += sprintf(o, "    s += saturate((t.x + t.y * (1.0 / 255.0)) * K.x - d2 * rsqrt(d2) * K.z + K.y); }\n");
    }
    o += sprintf(o, "  return float4(s * K.w, 0.0, 0.0, 1.0);\n}\n");
}


/* ---------- the same shaders in assembly (sm2asm.h): no d3dx9, exact instruction counts ---------- */
static const char GPU_VS_CUBE_ASM[] =
    "vs_2_0\n dcl_position v0\n def c8, 1.0, 0.0, 0.0, 0.0\n"
    " mov r0.xyz, v0\n mov r0.w, c8.x\n"
    " dp4 oPos.x, r0, c0\n dp4 oPos.y, r0, c1\n dp4 oPos.z, r0, c2\n dp4 oPos.w, r0, c3\n"
    " add oT0.xyz, r0, -c4\n";
static const char GPU_PS_CUBE_ASM[] =
    "ps_2_0\n dcl t0.xyz\n def c1, 1.0, 255.0, 0.003921568627, 0.0\n"
    " dp3 r0.x, t0, t0\n rsq r0.y, r0.x\n mul r0.x, r0.x, r0.y\n mul r0.x, r0.x, c0.x\n"
    " mul r1.xy, r0.x, c1.xy\n frc r2.xy, r1\n mul r3.x, r2.y, c1.z\n sub r2.x, r2.x, r3.x\n"
    " mov r2.z, c1.w\n mov r2.w, c1.x\n mov oC0, r2\n";
static const char GPU_VS_LM_ASM[] =
    "vs_2_0\n dcl_position v0\n dcl_normal v1\n dcl_texcoord1 v2\n def c8, 2.0, -2.0, 0.5, 1.0\n"
    " mad r0.x, v2.x, c8.x, -c8.w\n sub r0.x, r0.x, c0.x\n mad r0.y, v2.y, c8.y, c8.w\n add r0.y, r0.y, c0.y\n"
    " mov r0.z, c8.z\n mov r0.w, c8.w\n mov oPos, r0\n"
    " mov r3.xyz, v0\n mov r4.xyz, v1\n"
    " sub r1.xyz, r3, c1\n dp3 r2.x, r1, r1\n rsq r2.y, r2.x\n mul r2.x, r2.x, r2.y\n"
    " mul r2.x, r2.x, c2.y\n add r2.x, r2.x, c2.x\n mad oT0.xyz, r4, r2.x, r3\n";
static const char GPU_VS_REFL_ASM[] =
    "vs_2_0\n dcl_position v0\n dcl_texcoord1 v1\n def c8, 1.0, 0.0, 0.0, 0.0\n"
    " mov r0.xyz, v0\n mov r0.w, c8.x\n"
    " dp4 oPos.x, r0, c0\n dp4 oPos.y, r0, c1\n dp4 oPos.z, r0, c2\n dp4 oPos.w, r0, c3\n"
    " mul oT0.xy, v1, c4\n";
static const char GPU_PS_REFL_ASM[] =
    "ps_2_0\n dcl t0.xy\n def c4, 0.00390625, 256.0, 0.003921568627, 16.0\n"
    " frc r0.xy, t0\n sub r1.xy, t0, r0\n mul r2.xy, r1, c4.x\n frc r3.xy, r2\n sub r2.xy, r2, r3\n"
    " mul r3.xy, r2, c4.y\n sub r3.xy, r1, r3\n"
    " mul r5.yz, r3.xxyy, c4.z\n mul r6.x, r2.x, c4.w\n add r6.x, r6.x, r2.y\n mul r5.w, r6.x, c4.z\n"
    " mov r5.x, c0.x\n mov oC0, r5\n";

static char g_gpu_ps_shadow_asm[4096];
static void gpu_build_shadow_asm(int taps) {
    char* o = g_gpu_ps_shadow_asm; int t;
    o += sprintf(o, "ps_2_0\n dcl t0.xyz\n");
    for (t = 0; t < taps; t++) o += sprintf(o, " dcl_cube s%d\n", t);
    o += sprintf(o, " def c20, 0.0, 0.0, 0.0, 1.0\n");
    for (t = 0; t < taps; t++) o += sprintf(o, " sub r%d.xyz, t0, c%d\n", t, t);
    for (t = 0; t < taps; t++) o += sprintf(o, " texld r%d, r%d, s%d\n", 4 + t, t, t);
    for (t = 0; t < taps; t++) {
        o += sprintf(o, " dp3 r8.x, r%d, r%d\n rsq r8.y, r8.x\n mul r8.x, r8.x, r8.y\n", t, t);
        o += sprintf(o, " mad r9.x, r%d.y, c%d.w, r%d.x\n mul r9.x, r9.x, c%d.x\n mad r9.x, r8.x, -c%d.z, r9.x\n", 4 + t, taps, 4 + t, taps, taps);
        if (t == 0) o += sprintf(o, " add_sat r10.x, r9.x, c%d.y\n", taps);
        else        o += sprintf(o, " add_sat r9.x, r9.x, c%d.y\n add r10.x, r10.x, r9.x\n", taps);
    }
    o += sprintf(o, " mov r11, c20\n mul r11.x, r10.x, c%d.w\n mov oC0, r11\n", taps);
}
static IDirect3DVertexShader9* gpu_make_vs(const char* src, const char* name);
static IDirect3DPixelShader9* gpu_make_ps(const char* src, const char* name);
static IDirect3DVertexShader9* gpu_make_vs2(const char* a, const char* hlsl, const char* name) {
    IDirect3DVertexShader9* v = gpu_make_vs(a, name); if (!v && hlsl) v = gpu_make_vs(hlsl, name); return v;
}
static IDirect3DPixelShader9* gpu_make_ps2(const char* a, const char* hlsl, const char* name) {
    IDirect3DPixelShader9* p = gpu_make_ps(a, name); if (!p && hlsl) p = gpu_make_ps(hlsl, name); return p;
}

static void gpu_release(void) {
    int t, f;
    for (t = 0; t < GPU_MAXTAPS; t++) {
        for (f = 0; f < 6; f++) if (g_gpu_face[t][f]) { IDirect3DSurface9_Release(g_gpu_face[t][f]); g_gpu_face[t][f] = NULL; }
        if (g_gpu_cube[t]) { IDirect3DCubeTexture9_Release(g_gpu_cube[t]); g_gpu_cube[t] = NULL; }
    }
    if (g_gpu_ds) { IDirect3DSurface9_Release(g_gpu_ds); g_gpu_ds = NULL; }
}

/* Counts the instructions of an SM2 bytecode (D3D9 tokens): the compiler may not enforce the limit
 * of the profile (64 arithmetic / 32 texture for ps_2_0), but the driver or an old GPU does enforce it. */
static int sm2_count(const DWORD* t, int* arith, int* tex) {
    int i = 1; *arith = 0; *tex = 0;
    for (;;) {
        DWORD w = t[i]; DWORD op = w & 0xFFFF;
        if (op == 0xFFFF) return 1;                                   /* end */
        if (op == 0xFFFE) { i += 1 + (int)((w >> 16) & 0x7FFF); continue; }   /* comment */
        if (op == 66) (*tex)++;                                       /* texld */
        else if (op != 31 && op != 81 && op != 82 && op != 83 && op != 0) (*arith)++;   /* except dcl / def* / nop */
        i += 1 + (int)((w >> 24) & 15);
        if (i > 8000) return 0;
    }
}
static void gpu_dump(const char* name, const DWORD* t) {
    FILE* f = fopen("engine_gpu.log", "a"); int i;
    if (!f) return;
    fprintf(f, "  bytecode %s:", name);
    for (i = 0; i < 400; i++) { fprintf(f, " %08lX", (unsigned long)t[i]); if (t[i] == 0xFFFF) break; if (i > 0 && (t[i] & 0xFFFF) == 0xFFFE) { int n = (int)((t[i] >> 16) & 0x7FFF); i += n; } }
    fputc('\n', f); fclose(f);
}
static IDirect3DPixelShader9* gpu_make_ps(const char* src, const char* name) {
    void* code = bake_compile(src, "ps_2_0", name); IDirect3DPixelShader9* ps = NULL; int a, t;
    if (code) {
        if (sm2_count((const DWORD*)code, &a, &t) && a <= 64 && t <= 32) {
            HRESULT hr = IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD*)code, &ps);
            if (FAILED(hr)) { ps = NULL; gpu_log("shader %s: CreatePixelShader hr=0x%08lx (arith %d, tex %d)", name, (unsigned long)hr, a, t); gpu_dump(name, (const DWORD*)code); }
        } else gpu_log("shader %s refused: %d arithmetic / %d texture instructions (limit 64 / 32)", name, a, t);
        free(code);
    }
    return ps;
}
static IDirect3DVertexShader9* gpu_make_vs(const char* src, const char* name) {
    void* code = bake_compile(src, "vs_2_0", name); IDirect3DVertexShader9* vs = NULL;
    if (code) { HRESULT hr = IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD*)code, &vs); if (FAILED(hr)) { vs = NULL; gpu_log("shader %s: CreateVertexShader hr=0x%08lx", name, (unsigned long)hr); gpu_dump(name, (const DWORD*)code); } free(code); }
    return vs;
}

/* D3DPOOL_DEFAULT resources of the GPU bake: created for a bake, released right after (otherwise Reset() fails) */
static int gpu_alloc_res(void) {
    int t, f, res = g_gpu_res, taps = g_gpu_taps;
    if (g_gpu_cube[0] && g_gpu_ds) return 1;
    gpu_release();
    for (t = 0; t < taps; t++) {
        if (FAILED(IDirect3DDevice9_CreateCubeTexture(g_dev, res, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_gpu_cube[t], NULL))) { gpu_release(); return 0; }
        for (f = 0; f < 6; f++)
            if (FAILED(IDirect3DCubeTexture9_GetCubeMapSurface(g_gpu_cube[t], (D3DCUBEMAP_FACES)f, 0, &g_gpu_face[t][f]))) { gpu_release(); return 0; }
    }
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, res, res, g_pp.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &g_gpu_ds, NULL))
        && FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, res, res, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &g_gpu_ds, NULL))) { gpu_release(); return 0; }
    return 1;
}

/* 1 if the GPU bake is possible (tested only once) */
static int gpu_probe(void) {
    D3DCAPS9 cap; int res, taps;
    if (g_gpu_state >= 0) return g_gpu_state;
    g_gpu_state = 0;
    if (!g_dev || !g_d3d) return 0;
    if (FAILED(IDirect3DDevice9_GetDeviceCaps(g_dev, &cap))) return 0;
    if (cap.VertexShaderVersion < D3DVS_VERSION(2, 0) || cap.PixelShaderVersion < D3DPS_VERSION(2, 0)) return 0;
    if (!(cap.TextureCaps & D3DPTEXTURECAPS_CUBEMAP)) return 0;
    /* additive blending on an A8R8G8B8 render target: essential to accumulate the visibilities */
    if (FAILED(IDirect3D9_CheckDeviceFormat(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
               D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8))) return 0;
    res = C.gpu_shadow_res < 64 ? 64 : (C.gpu_shadow_res > 1024 ? 1024 : C.gpu_shadow_res);
    { int p = 64; while (p < res) p <<= 1; res = p; }                    /* power of 2 (SM2 cubemap) */
    if (res > g_maxtex) res = g_maxtex;
    taps = C.gpu_taps < 1 ? 1 : (C.gpu_taps > GPU_MAXTAPS ? GPU_MAXTAPS : C.gpu_taps);
    g_gpu_res = res;

    g_gpu_vs_cube = gpu_make_vs2(GPU_VS_CUBE_ASM, GPU_VS_CUBE_SRC, "gpu_vs_cube");
    g_gpu_ps_cube = gpu_make_ps2(GPU_PS_CUBE_ASM, GPU_PS_CUBE_SRC, "gpu_ps_cube");
    g_gpu_vs_lm = gpu_make_vs2(GPU_VS_LM_ASM, GPU_VS_LM_SRC, "gpu_vs_lm");
    if (!g_gpu_vs_cube || !g_gpu_ps_cube || !g_gpu_vs_lm) return 0;
    /* ps_2_0: 64 arithmetic instructions. 4 points per pass if the driver accepts, otherwise 2, otherwise 1 */
    for (; taps >= 1; taps = taps > 2 ? 2 : taps - 1) {
        gpu_build_shadow_src(taps); gpu_build_shadow_asm(taps);
        g_gpu_ps_shadow = gpu_make_ps2(g_gpu_ps_shadow_asm, g_gpu_ps_shadow_src, taps == 4 ? "gpu_ps_sh4" : (taps == 2 ? "gpu_ps_sh2" : "gpu_ps_sh1"));
        if (g_gpu_ps_shadow) break;
    }
    if (!g_gpu_ps_shadow) return 0;
    g_gpu_taps = taps;

    g_gpu_state = 1;
    if (!gpu_alloc_res()) { g_gpu_state = 0; return 0; }          /* allocation test */
    gpu_release();
    return 1;
}

/* view * projection of a cubemap face (engine convention: row vectors, translation in row 3) */
static M4 gpu_face_vp(V3 eye, int f, float zf) {
    V3 L = CUBE_LOOK[f], U = CUBE_UP[f], R = vnorm(vcross(U, L)); M4 v = m_ident();
    v.m[0][0] = R.x; v.m[0][1] = U.x; v.m[0][2] = L.x;
    v.m[1][0] = R.y; v.m[1][1] = U.y; v.m[1][2] = L.y;
    v.m[2][0] = R.z; v.m[2][1] = U.z; v.m[2][2] = L.z;
    v.m[3][0] = -vdot(R, eye); v.m[3][1] = -vdot(U, eye); v.m[3][2] = -vdot(L, eye);
    return m_mul(v, m_persp(PI * 0.5f, 1.0f, 0.05f, zf));
}

/* does the bounding sphere touch the 90-degree frustum of this face? */
static int gpu_face_sees(V3 eye, int f, V3 c, float r) {
    V3 L = CUBE_LOOK[f], U = CUBE_UP[f], R = vnorm(vcross(U, L)), d = vsub(c, eye);
    const float k = 0.70710678f;
    if (vdot(d, vadd(L, R)) * k < -r) return 0;
    if (vdot(d, vsub(L, R)) * k < -r) return 0;
    if (vdot(d, vadd(L, U)) * k < -r) return 0;
    if (vdot(d, vsub(L, U)) * k < -r) return 0;
    return 1;
}

static void gpu_draw_obj(const Obj* o, int lod) {
    if (!o->vb || !o->ib[lod] || !o->nt[lod]) return;
    IDirect3DDevice9_SetStreamSource(g_dev, 0, o->vb, 0, sizeof(Vtx));
    IDirect3DDevice9_SetIndices(g_dev, o->ib[lod]);
    IDirect3DDevice9_DrawIndexedPrimitive(g_dev, D3DPT_TRIANGLELIST, 0, 0, o->nv, 0, o->nt[lod]);
}

/* copies the value of the first interior texel into the 1-texel margin of each tile
 * (the rasterizer only covers the interior; the CPU computes the margin at the face edge) */
static void gpu_fill_margins(const Obj* o, uint8_t* buf) {
    int qi, x, y;
    for (qi = 0; qi < o->nq; qi++) {
        const Quad* q = &o->q[qi];
        for (y = 0; y < q->th + 2; y++) for (x = 0; x < q->tw + 2; x++) {
            int cx, cy;
            if (x > 0 && x <= q->tw && y > 0 && y <= q->th) continue;
            cx = x < 1 ? 1 : (x > q->tw ? q->tw : x); cy = y < 1 ? 1 : (y > q->th ? q->th : y);
            buf[(size_t)(q->ty + y) * o->aw + q->tx + x] = buf[(size_t)(q->ty + cy) * o->aw + q->tx + cx];
        }
    }
}

static void gpu_restore_targets(IDirect3DSurface9* bb, IDirect3DSurface9* dsorig) {
    IDirect3DDevice9_SetRenderTarget(g_dev, 0, bb);
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, dsorig);
    IDirect3DDevice9_SetVertexShader(g_dev, NULL); IDirect3DDevice9_SetPixelShader(g_dev, NULL);
    IDirect3DDevice9_SetFVF(g_dev, FVF);
}

/* soft shadows by the GPU for all shadow-casting lights. Returns 1 if o->gv[l] is filled for all of them. */
static int bake_direct_gpu(void) {
    int oi, l, b, t, f, nb, K, ok = 1, nshadow = 0, ndone = 0;
    IDirect3DSurface9 *bb = NULL, *dsorig = NULL; static IDirect3DTexture9* acc[MAXOBJ];
    float (*pts)[3];
    if (!gpu_probe() || !gpu_alloc_res()) return 0;
    for (l = 0; l < g_nlight; l++) if (g_lightset[l].shadow) nshadow++;
    if (!nshadow) { gpu_release(); return 0; }
    nb = (C.shadow_samples + g_gpu_taps - 1) / g_gpu_taps; if (nb < 1) nb = 1; if (nb * g_gpu_taps > 255) nb = 255 / g_gpu_taps;
    K = nb * g_gpu_taps;
    pts = (float (*)[3])malloc((size_t)K * 3 * sizeof(float));
    IDirect3DDevice9_GetRenderTarget(g_dev, 0, &bb); IDirect3DDevice9_GetDepthStencilSurface(g_dev, &dsorig);
    memset(acc, 0, sizeof acc);

    for (l = 0; l < g_nlight && ok; l++) {
        const Light* lp = &g_lightset[l]; float zf = 1.0f; D3DLOCKED_RECT lr; IDirect3DSurface9 *rs, *sys;
        if (!lp->shadow) continue;
        for (b = 0; b < K; b++) {                                 /* light points: Fibonacci on the sphere, or R2 sequence on the square ceiling light */
            if (lp->shape) {
                float u = 0.5f + 0.7548776662f * (float)(b + 1), v = 0.5f + 0.5698402910f * (float)(b + 1);
                pts[b][0] = (u - floorf(u)) * 2.0f - 1.0f; pts[b][1] = 0.0f; pts[b][2] = (v - floorf(v)) * 2.0f - 1.0f;
            } else {
                float z = 1.0f - (2.0f * b + 1.0f) / K, r = sqrtf(fmaxf(0, 1 - z * z)), ph = b * 2.39996323f;
                pts[b][0] = r * cosf(ph); pts[b][1] = z; pts[b][2] = r * sinf(ph);
            }
        }
        for (oi = 0; oi < g_nobj; oi++) {                         /* maximum distance lamp -> scene */
            const Obj* o = &g_obj[oi]; float d = vlen(vsub(o->bs_c, lp->pos)) + o->bs_r;
            if (o->occluder || o->lightmap) if (d > zf) zf = d;
        }
        zf += lp->radius + 2.0f;
        for (oi = 0; oi < g_nobj; oi++) {                         /* one A8R8G8B8 render per lightmap, reset to zero */
            Obj* o = &g_obj[oi]; if (!o->lightmap) continue;
            if (!acc[oi] && FAILED(IDirect3DDevice9_CreateTexture(g_dev, o->aw, o->ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &acc[oi], NULL))) { ok = 0; break; }
            IDirect3DTexture9_GetSurfaceLevel(acc[oi], 0, &rs);
            IDirect3DDevice9_SetRenderTarget(g_dev, 0, rs); IDirect3DDevice9_SetDepthStencilSurface(g_dev, NULL);
            IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
            IDirect3DSurface9_Release(rs);
        }
        for (b = 0; b < nb && ok; b++) {
            float kc[4], cc[4];
            IDirect3DDevice9_BeginScene(g_dev);
            /* --- 1. scene depth seen from each lamp point of the batch (6 faces per point) --- */
            IDirect3DDevice9_SetVertexShader(g_dev, g_gpu_vs_cube); IDirect3DDevice9_SetPixelShader(g_dev, g_gpu_ps_cube);
            IDirect3DDevice9_SetFVF(g_dev, FVF);
            RS(D3DRS_ZENABLE, D3DZB_TRUE); RS(D3DRS_ZWRITEENABLE, TRUE); RS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_ALPHABLENDENABLE, FALSE); RS(D3DRS_SRGBWRITEENABLE, FALSE);
            cc[0] = 1.0f / zf; cc[1] = cc[2] = cc[3] = 0;
            IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, cc, 1);
            for (t = 0; t < g_gpu_taps; t++) {
                const float* s = pts[b * g_gpu_taps + t]; float lc[4]; V3 eye;
                eye = vadd(lp->pos, vmul(v3(s[0], s[1], s[2]), lp->radius));
                lc[0] = eye.x; lc[1] = eye.y; lc[2] = eye.z; lc[3] = 0;
                IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 4, lc, 1);
                for (f = 0; f < 6; f++) {
                    M4 m = gpu_face_vp(eye, f, zf); float mt[16]; int r2, c2;
                    for (r2 = 0; r2 < 4; r2++) for (c2 = 0; c2 < 4; c2++) mt[c2 * 4 + r2] = m.m[r2][c2];   /* colonnes -> c0..c3 */
                    IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_gpu_face[t][f]);
                    IDirect3DDevice9_SetDepthStencilSurface(g_dev, g_gpu_ds);
                    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFF00u, 1.0f, 0);   /* distance max */
                    IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, mt, 4);
                    for (oi = 0; oi < g_nobj; oi++) {
                        const Obj* o = &g_obj[oi]; int lod = C.occluder_lod < o->nlod ? C.occluder_lod : o->nlod - 1;
                        if (!o->occluder || !gpu_face_sees(eye, f, o->bs_c, o->bs_r)) continue;
                        gpu_draw_obj(o, lod < 0 ? 0 : lod);
                    }
                }
            }
            /* --- 2. accumulation in each lightmap (UV space) --- */
            IDirect3DDevice9_SetDepthStencilSurface(g_dev, NULL);
            IDirect3DDevice9_SetVertexShader(g_dev, g_gpu_vs_lm); IDirect3DDevice9_SetPixelShader(g_dev, g_gpu_ps_shadow);
            RS(D3DRS_ZENABLE, D3DZB_FALSE); RS(D3DRS_ZWRITEENABLE, FALSE); RS(D3DRS_CULLMODE, D3DCULL_NONE);
            RS(D3DRS_ALPHABLENDENABLE, TRUE); RS(D3DRS_SRCBLEND, D3DBLEND_ONE); RS(D3DRS_DESTBLEND, D3DBLEND_ONE);
            for (t = 0; t < g_gpu_taps; t++) {
                const float* s = pts[b * g_gpu_taps + t]; V3 eye = vadd(lp->pos, vmul(v3(s[0], s[1], s[2]), lp->radius));
                float lc[4]; lc[0] = eye.x; lc[1] = eye.y; lc[2] = eye.z; lc[3] = 0;
                IDirect3DDevice9_SetPixelShaderConstantF(g_dev, t, lc, 1);
                IDirect3DDevice9_SetTexture(g_dev, t, (IDirect3DBaseTexture9*)g_gpu_cube[t]);
                ss(t, D3DSAMP_MAGFILTER, D3DTEXF_POINT); ss(t, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(t, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                ss(t, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(t, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(t, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
                ss(t, D3DSAMP_SRGBTEXTURE, FALSE);
            }
            kc[0] = zf * 4096.0f; kc[1] = 0.02f * 4096.0f + 0.5f; kc[2] = 4096.0f; kc[3] = 1.0f / 255.0f;
            IDirect3DDevice9_SetPixelShaderConstantF(g_dev, g_gpu_taps, kc, 1);
            kc[0] = lp->pos.x; kc[1] = lp->pos.y; kc[2] = lp->pos.z; kc[3] = 0;
            IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 1, kc, 1);
            kc[0] = 0.015f * C.gpu_shadow_bias; kc[1] = C.gpu_shadow_bias * 2.0f / (float)g_gpu_res; kc[2] = kc[3] = 0;   /* bias: offset along the normal */
            IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 2, kc, 1);
            for (oi = 0; oi < g_nobj; oi++) {
                Obj* o = &g_obj[oi]; float ct[4]; IDirect3DSurface9* sf;
                if (!o->lightmap || !acc[oi]) continue;
                ct[0] = 1.0f / o->aw; ct[1] = 1.0f / o->ah; ct[2] = ct[3] = 0;
                IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, ct, 1);
                IDirect3DTexture9_GetSurfaceLevel(acc[oi], 0, &sf);
                IDirect3DDevice9_SetRenderTarget(g_dev, 0, sf); IDirect3DSurface9_Release(sf);
                gpu_draw_obj(o, 0);
            }
            IDirect3DDevice9_EndScene(g_dev);
            for (t = 0; t < g_gpu_taps; t++) IDirect3DDevice9_SetTexture(g_dev, t, NULL);
            gpu_restore_targets(bb, dsorig);
            loading_frame(0.15f * ((float)ndone + (float)(b + 1) / nb) / (float)nshadow);
            if (g_abort) ok = 0;
        }
        /* --- 3. readback: the red channel holds the number of visible points --- */
        for (oi = 0; oi < g_nobj && ok; oi++) {
            Obj* o = &g_obj[oi]; size_t n, k; IDirect3DSurface9* sf;
            if (!o->lightmap || !acc[oi]) continue;
            n = (size_t)o->aw * o->ah;
            if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, o->aw, o->ah, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) { ok = 0; break; }
            IDirect3DTexture9_GetSurfaceLevel(acc[oi], 0, &sf);
            if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, sf, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
                int x, y; o->gv[l] = (uint8_t*)malloc(n);
                for (y = 0; y < o->ah; y++) for (x = 0; x < o->aw; x++)
                    o->gv[l][(size_t)y * o->aw + x] = (uint8_t)(((const uint32_t*)((const char*)lr.pBits + (size_t)y * lr.Pitch))[x] >> 16);
                IDirect3DSurface9_UnlockRect(sys);
                gpu_fill_margins(o, o->gv[l]);
            } else ok = 0;
            IDirect3DSurface9_Release(sf); IDirect3DSurface9_Release(sys);
            (void)k;
        }
        ndone++;
    }
    for (oi = 0; oi < MAXOBJ; oi++) if (acc[oi]) { IDirect3DTexture9_Release(acc[oi]); acc[oi] = NULL; }
    gpu_restore_targets(bb, dsorig);
    if (bb) IDirect3DSurface9_Release(bb);
    if (dsorig) IDirect3DSurface9_Release(dsorig);
    RS(D3DRS_ALPHABLENDENABLE, FALSE); RS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); RS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    free(pts); gpu_release();
    if (!ok) {                                                    /* failure along the way: discard everything, the CPU redoes it */
        for (oi = 0; oi < g_nobj; oi++) for (l = 0; l < MAXLIGHT; l++) { free(g_obj[oi].gv[l]); g_obj[oi].gv[l] = NULL; }
        return 0;
    }
    g_gpu_K = K;
    return 1;
}


/* =====================================================================================
 * Reflections: G-buffer by rasterization (ps_2_0)
 * A reflective object = one target (S*ns) x (S*6): 6 cubemap faces x ns roughness samples
 * (roughness = slight camera tilt per sample). Each pixel stores
 *   R = hit object (255 = sky)   G,B = lightmap texel x,y (low 8 bits)   A = (x>>8)<<4 | (y>>8)
 * Outputs are normalized to [0,1]: a ps_2_0 writes into a clamped register.
 * The texel is given by (x, y), not by x + y*width: a ps_2_0 float (fp24, 16 bits of
 * mantissa) cannot exactly represent indices of 18 to 24 bits. The CPU recomposes the index.
 * ===================================================================================== */
static const char GPU_VS_REFL_SRC[] =
    "float4 c0 : register(c0); float4 c1 : register(c1); float4 c2 : register(c2); float4 c3 : register(c3);\n"
    "float4 dim : register(c4);\n"                                   /* aw, ah */
    "struct VOut { float4 p : POSITION; float2 t : TEXCOORD0; };\n"
    "VOut main(float3 pos : POSITION, float2 t : TEXCOORD1) {\n"
    "  VOut o; float4 p = float4(pos, 1.0);\n"
    "  o.p = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"
    "  o.t = t * dim.xy;\n"
    "  return o;\n"
    "}\n";
static const char GPU_PS_REFL_SRC[] =
    "float4 id : register(c0);\n"                                    /* x = object / 255 */
    "float4 main(float2 t : TEXCOORD0) : COLOR {\n"
    "  float2 f = floor(t);\n"
    "  float2 h = floor(f * 0.00390625);\n"                          /* / 256 */
    "  float2 l = f - h * 256.0;\n"
    "  return float4(id.x, l.x * (1.0 / 255.0), l.y * (1.0 / 255.0), (h.x * 16.0 + h.y) * (1.0 / 255.0));\n"
    "}\n";

static int gpu_reflections_all(void) {
    static IDirect3DVertexShader9* vs; static IDirect3DPixelShader9* ps; static int tried;
    int oi, S = C.reflection_size, f, k, W, H, ok = 1, nrefl = 0;
    IDirect3DTexture9* rt = NULL; IDirect3DSurface9 *rts = NULL, *ds = NULL, *sys = NULL, *bb = NULL, *dsorig = NULL;
    D3DLOCKED_RECT lr; int maxns = 1;
    if (!gpu_probe()) return 0;
    if (!tried) { tried = 1; vs = gpu_make_vs2(GPU_VS_REFL_ASM, GPU_VS_REFL_SRC, "gpu_vs_refl"); ps = gpu_make_ps2(GPU_PS_REFL_ASM, GPU_PS_REFL_SRC, "gpu_ps_refl"); }
    if (!vs || !ps || g_nobj >= GB_SKY) return 0;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].reflect && g_obj[oi].gbuf) { nrefl++; if (g_obj[oi].nsamp > maxns) maxns = g_obj[oi].nsamp; }
    if (!nrefl) return 1;
    if (maxns > 4) maxns = 4;                                      /* 4 camera tilts (BAKE_TAP table) */
    W = S * maxns; H = S * 6;
    if (W > g_maxtex || H > g_maxtex) return 0;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt, NULL))) return 0;
    IDirect3DTexture9_GetSurfaceLevel(rt, 0, &rts);
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, W, H, g_pp.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL))
        && FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, W, H, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL))) ok = 0;
    if (ok && FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) ok = 0;
    if (!ok) { if (ds) IDirect3DSurface9_Release(ds); if (rts) IDirect3DSurface9_Release(rts); IDirect3DTexture9_Release(rt); return 0; }

    IDirect3DDevice9_GetRenderTarget(g_dev, 0, &bb); IDirect3DDevice9_GetDepthStencilSurface(g_dev, &dsorig);
    IDirect3DDevice9_SetRenderTarget(g_dev, 0, rts); IDirect3DDevice9_SetDepthStencilSurface(g_dev, ds);
    IDirect3DDevice9_SetFVF(g_dev, FVF);
    IDirect3DDevice9_SetVertexShader(g_dev, vs); IDirect3DDevice9_SetPixelShader(g_dev, ps);
    RS(D3DRS_ZENABLE, D3DZB_TRUE); RS(D3DRS_ZWRITEENABLE, TRUE); RS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_ALPHABLENDENABLE, FALSE); RS(D3DRS_SRGBWRITEENABLE, FALSE);
    RS(D3DRS_COLORWRITEENABLE, 0xF);

    for (oi = 0; oi < g_nobj && ok; oi++) {
        Obj* o = &g_obj[oi]; int ns = o->nsamp < 1 ? 1 : (o->nsamp > 4 ? 4 : o->nsamp), i;
        V3 probe = obj_probe(o); float spread = o->rough * 1.2f;
        if (!o->reflect || !o->gbuf) continue;
        IDirect3DDevice9_BeginScene(g_dev);
        {   D3DVIEWPORT9 full; full.X = 0; full.Y = 0; full.Width = (DWORD)W; full.Height = (DWORD)H; full.MinZ = 0.0f; full.MaxZ = 1.0f;
            IDirect3DDevice9_SetViewport(g_dev, &full); }                    /* Clear(NULL) only clears the current viewport */
        IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00FF0000u, 1.0f, 0);   /* R = 255 = sky */
        for (f = 0; f < 6; f++) for (k = 0; k < ns; k++) {
            V3 L = CUBE_LOOK[f], U = CUBE_UP[f], R = vnorm(vcross(U, L)), Lj = L; M4 m; float mt[16]; int r2, c2; D3DVIEWPORT9 vp;
            if (o->nsamp > 1) Lj = vnorm(vadd(L, vadd(vmul(R, BAKE_TAP[k & 3][0] * spread), vmul(U, BAKE_TAP[k & 3][1] * spread))));
            { /* orthonormal basis of the tilted camera (same convention as gpu_face_vp) */
              V3 Rj = vnorm(vcross(U, Lj)), Uj = vcross(Lj, Rj); M4 v = m_ident();
              v.m[0][0] = Rj.x; v.m[0][1] = Uj.x; v.m[0][2] = Lj.x;
              v.m[1][0] = Rj.y; v.m[1][1] = Uj.y; v.m[1][2] = Lj.y;
              v.m[2][0] = Rj.z; v.m[2][1] = Uj.z; v.m[2][2] = Lj.z;
              v.m[3][0] = -vdot(Rj, probe); v.m[3][1] = -vdot(Uj, probe); v.m[3][2] = -vdot(Lj, probe);
              m = m_mul(v, m_persp(PI * 0.5f, 1.0f, 0.05f, 1000.0f)); }
            for (r2 = 0; r2 < 4; r2++) for (c2 = 0; c2 < 4; c2++) mt[c2 * 4 + r2] = m.m[r2][c2];
            IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, mt, 4);
            vp.X = (DWORD)(k * S); vp.Y = (DWORD)(f * S); vp.Width = (DWORD)S; vp.Height = (DWORD)S; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
            IDirect3DDevice9_SetViewport(g_dev, &vp);
            for (i = 0; i < g_nobj; i++) {
                const Obj* h = &g_obj[i]; int l = C.occluder_lod < h->nlod ? C.occluder_lod : h->nlod - 1; float c4[4], c0[4];
                if (i == oi) continue;                                       /* no self-reflection */
                if (C.refl_filter ? !h->in_refl : !h->occluder) continue;
                if (l < 0) l = 0;
                c4[0] = (float)h->aw; c4[1] = (float)h->ah; c4[2] = c4[3] = 0;
                c0[0] = (float)i / 255.0f; c0[1] = c0[2] = c0[3] = 0;
                IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 4, c4, 1);
                IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, c0, 1);
                gpu_draw_obj(h, l);
            }
        }
        IDirect3DDevice9_EndScene(g_dev);
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, rts, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            int x, y;
            for (f = 0; f < 6; f++) for (k = 0; k < o->nsamp; k++)
                for (y = 0; y < S; y++) for (x = 0; x < S; x++) {
                    int ksrc = k < ns ? k : (k % ns);                       /* nsamp > 4: we reuse the 4 tilts */
                    uint32_t v = ((const uint32_t*)((const char*)lr.pBits + (size_t)(f * S + y) * lr.Pitch))[ksrc * S + x];
                    int ob = (int)((v >> 16) & 255), tx = (int)(((v >> 24) >> 4) & 15) * 256 + (int)((v >> 8) & 255),
                        ty = (int)((v >> 24) & 15) * 256 + (int)(v & 255), idx = 0;
                    unsigned char* d = &o->gbuf[((((size_t)f * S + y) * S + x) * o->nsamp + k) * 4];
                    if (ob < g_nobj && g_obj[ob].aw > 0) { idx = ty * g_obj[ob].aw + tx; if (idx >= g_obj[ob].aw * g_obj[ob].ah) idx = 0; }
                    gb_set(d, ob < g_nobj ? ob : GB_SKY, idx);
                }
            IDirect3DSurface9_UnlockRect(sys);
        } else ok = 0;
    }
    gpu_restore_targets(bb, dsorig);
    if (bb) IDirect3DSurface9_Release(bb);
    if (dsorig) IDirect3DSurface9_Release(dsorig);
    IDirect3DSurface9_Release(sys); IDirect3DSurface9_Release(ds); IDirect3DSurface9_Release(rts); IDirect3DTexture9_Release(rt);
    return ok;
}
