/* gpugi.h - indirect lighting (ambient occlusion + colored bounces) baked by the GPU, Shader Model 2.0
 *
 * Replaces the CPU path tracer of pass 2 (gi.h), which took 80 % of the bake time.
 *
 *   1. CPU: nv "virtual point lights" (VPL) are scattered uniformly over the lightmapped surfaces
 *      (position, normal, albedo, direct light already baked, area dA = total area / nv).
 *   2. CPU: multi-bounce transport BETWEEN the VPLs (nv x nv rays on the BVH, a few ms):
 *        S0 = direct,  S(k+1)_i = sum_j F_ij * albedo_j * S(k)_j,   Phi_j = albedo_j * (S0 + ... + S(bounces-1))_j
 *      with the disk form factor F = cos_i cos_j dA / (pi d^2 + dA).
 *   3. GPU: one depth cubemap per VPL (low resolution, 16 bits in RGBA8, same encoding as the shadows).
 *   4. GPU: each lightmap is drawn in UV space, 2 VPL per pass, additive blending in an A16B16G16R16F target:
 *        rgb += Phi_j * F * visibility * window(bounce_distance)        -> bounce  (B)
 *        a   +=         F * visibility * window(ao_distance)            -> blocked fraction of the hemisphere (AO)
 *   5. readback: B = rgb * bounce_strength,  A = 1 - ao_strength * blocked.
 *
 * Failure at any step -> returns 0, the CPU path tracer takes over (identical to the previous behavior). */
#pragma once

static IDirect3DVertexShader9* g_gi_vs; static IDirect3DPixelShader9* g_gi_ps; static int g_gi_taps, g_gi_tried;
static char g_gi_ps_src[6144];
static char g_gi_why[160];                                   /* why the GPU path was refused (log) */

static const char GPU_VS_GI_SRC[] =
    "float4 ctl : register(c0);\n"                           /* x = 1/aw, y = 1/ah */
    "float4 off : register(c1);\n"                           /* x = offset along the normal */
    "struct VOut { float4 p : POSITION; float3 w : TEXCOORD0; float3 n : TEXCOORD1; };\n"
    "VOut main(float3 pos : POSITION, float3 nrm : NORMAL, float2 t : TEXCOORD1) {\n"
    "  VOut o;\n"
    "  o.p = float4(t.x * 2.0 - 1.0 - ctl.x, 1.0 - t.y * 2.0 + ctl.y, 0.5, 1.0);\n"
    "  o.w = pos + nrm * off.x;\n"
    "  o.n = nrm;\n"
    "  return o;\n"
    "}\n";

/* registers: per tap  c(3t) = position, c(3t+1) = normal.xyz + dA, c(3t+2) = Phi (rgb) ; then K = c(3*taps), W = c(3*taps+1)
 * (assembly: 24 arithmetic instructions per VPL, one constant register per instruction) */
static void gi_build_ps_asm(int taps) {
    char* o = g_gi_ps_src; int t, ck = 3 * taps, cw = 3 * taps + 1;
    o += sprintf(o, "ps_2_0\n dcl t0.xyz\n dcl t1.xyz\n");
    for (t = 0; t < taps; t++) o += sprintf(o, " dcl_cube s%d\n", t);
    o += sprintf(o, " def c20, 0.003921568627, 3.14159265, 0.000001, 0.0\n def c21, 0.0, 0.0, 0.0, 0.0\n");
    o += sprintf(o, " mov r9, c%d\n mov r11, c%d\n mov r10, c21\n", cw, ck);                  /* r9 = W, r11 = K, r10 = accumulator */
    for (t = 0; t < taps; t++) o += sprintf(o, " sub r%d.xyz, t0, c%d\n", t, 3 * t);
    for (t = 0; t < taps; t++) o += sprintf(o, " texld r%d, r%d, s%d\n", taps + t, t, t);
    for (t = 0; t < taps; t++) {
        int rv = t, rs = taps + t, cn = 3 * t + 1, cf = 3 * t + 2;
        o += sprintf(o, " dp3 r4.x, r%d, r%d\n add r4.x, r4.x, c20.z\n rsq r4.y, r4.x\n mul r4.z, r4.x, r4.y\n mul r5.xyz, r%d, r4.y\n", rv, rv, rv);
        o += sprintf(o, " mad r6.x, r%d.y, c20.x, r%d.x\n mul r6.x, r6.x, r11.x\n mad r6.x, r4.z, -r11.z, r6.x\n add_sat r6.x, r6.x, r11.y\n", rs, rs);
        o += sprintf(o, " dp3_sat r7.x, -r5, t1\n dp3_sat r7.y, r5, c%d\n mul r7.x, r7.x, r7.y\n mul r7.x, r7.x, c%d.w\n", cn, cn);
        o += sprintf(o, " mul r7.y, r4.x, c20.y\n add r7.y, r7.y, c%d.w\n rcp r7.y, r7.y\n mul r7.x, r7.x, r7.y\n mul r7.x, r7.x, r6.x\n", cn);
        o += sprintf(o, " mad_sat r8.x, r4.z, -r9.y, r9.x\n mad_sat r8.y, r4.z, -r9.w, r9.z\n mul r8.x, r8.x, r7.x\n mul r8.y, r8.y, r7.x\n");
        o += sprintf(o, " mad r10.xyz, c%d, r8.x, r10\n add r10.w, r10.w, r8.y\n", cf);
    }
    o += sprintf(o, " mov oC0, r10\n");
}
static const char GPU_VS_GI_ASM[] =
    "vs_2_0\n dcl_position v0\n dcl_normal v1\n dcl_texcoord1 v2\n def c8, 2.0, -2.0, 0.5, 1.0\n"
    " mad r0.x, v2.x, c8.x, -c8.w\n sub r0.x, r0.x, c0.x\n mad r0.y, v2.y, c8.y, c8.w\n add r0.y, r0.y, c0.y\n"
    " mov r0.z, c8.z\n mov r0.w, c8.w\n mov oPos, r0\n"
    " mov r3.xyz, v0\n mov r4.xyz, v1\n mad oT0.xyz, r4, c1.x, r3\n mov oT1.xyz, r4\n";

static float half2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u; float f;
    if (e == 0) f = ldexpf((float)m, -24);
    else if (e == 31) f = m ? 0.0f : 65504.0f;
    else f = ldexpf((float)(m + 1024u), (int)e - 25);
    return s ? -f : f;
}

typedef struct { V3 p, n, alb; float D0, dA; } VPL;

/* area element |dP/ds x dP/dt| of a quad at (s,t) (exact for a flat quad, finite differences for a sphere) */
static float gi_area_elem(const Quad* q, float s, float t) {
    V3 n0, n1, n2, p0, p1, p2; const float h = 0.01f;
    if (q->sr <= 0) return vlen(vcross(q->u, q->v));
    p0 = quad_at(q, s, t, &n0); p1 = quad_at(q, s + h, t, &n1); p2 = quad_at(q, s, t + h, &n2);
    return vlen(vcross(vsub(p1, p0), vsub(p2, p0))) / (h * h);
}

static int gi_make_vpls(VPL* out, int M, float* atot_out) {
    int oi, qi, nv = 0; double atot = 0.0, acc = 0.0, step, next; int j = 0;
    for (oi = 0; oi < g_nobj; oi++) { const Obj* o = &g_obj[oi]; if (!o->lightmap || !o->fT) continue;
        for (qi = 0; qi < o->nq; qi++) { const Quad* q = &o->q[qi]; atot += gi_area_elem(q, 0.5f, 0.5f) * (q->tri ? 0.5f : 1.0f); } }
    if (atot <= 0.0) return 0;
    step = atot / M; next = step * 0.5;
    for (oi = 0; oi < g_nobj && j < M; oi++) {
        Obj* o = &g_obj[oi]; if (!o->lightmap || !o->fT) continue;
        for (qi = 0; qi < o->nq && j < M; qi++) {
            const Quad* q = &o->q[qi]; double aq = gi_area_elem(q, 0.5f, 0.5f) * (q->tri ? 0.5f : 1.0f);
            while (j < M && next < acc + aq) {
                float u = 0.5f + 0.7548776662f * (float)(j + 1), v = 0.5f + 0.5698402910f * (float)(j + 1), s = u - floorf(u), t = v - floorf(v);
                V3 n, p; int ix, iy; VPL* w;
                next += step; j++;
                if (q->tri && s + t > 1.0f) { s = 1.0f - s; t = 1.0f - t; }
                p = quad_at(q, s, t, &n);
                if (q->cover && covered(p, q->box)) continue;                  /* face hidden by another object: no emission */
                ix = (int)(s * q->tw); iy = (int)(t * q->th);
                if (ix > q->tw - 1) ix = q->tw - 1;
                if (iy > q->th - 1) iy = q->th - 1;
                w = &out[nv++];
                w->p = p; w->n = n; w->alb = q->alb; w->dA = (float)step;
                w->D0 = o->fT[((size_t)(q->ty + 1 + iy) * o->aw + q->tx + 1 + ix) * 3];
            }
            acc += aq;
        }
    }
    *atot_out = (float)atot;
    return nv;
}

typedef struct { const VPL* v; int n; float* F; float bd2; } GiTr;
static void gi_transport_row(void* ctx, int i) {
    GiTr* g = (GiTr*)ctx; const VPL* a = &g->v[i]; int j;
    for (j = 0; j < g->n; j++) {
        const VPL* b = &g->v[j]; V3 d; float d2, dist, ci, cj, F; V3 l;
        g->F[(size_t)i * g->n + j] = 0.0f;
        if (j == i) continue;
        d = vsub(b->p, a->p); d2 = vdot(d, d);
        if (d2 > g->bd2 || d2 < 1e-8f) continue;
        dist = sqrtf(d2); l = vmul(d, 1.0f / dist);
        ci = vdot(l, a->n); cj = -vdot(l, b->n);
        if (ci <= 0.0f || cj <= 0.0f) continue;
        F = ci * cj * b->dA / (PI * d2 + b->dA);
        if (F < 1e-6f) continue;
        if (occluded(vadd(a->p, vmul(a->n, 0.01f)), l, dist - 0.02f)) continue;
        g->F[(size_t)i * g->n + j] = F;
    }
}

static int gi_probe(void) {
    D3DCAPS9 cap; int taps;
    if (g_gi_tried) return g_gi_vs && g_gi_ps;
    g_gi_tried = 1;
    if (!gpu_probe()) { strcpy(g_gi_why, "shadow GPU unavailable (gpu_probe)"); return 0; }
    if (FAILED(IDirect3DDevice9_GetDeviceCaps(g_dev, &cap))) return 0;
    if (FAILED(IDirect3D9_CheckDeviceFormat(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
               D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F))) {
        strcpy(g_gi_why, "A16B16G16R16F render target + blending not supported"); return 0; }
    g_gi_vs = gpu_make_vs2(GPU_VS_GI_ASM, GPU_VS_GI_SRC, "gpu_vs_gi");
    if (!g_gi_vs) { strcpy(g_gi_why, "vertex shader gi did not compile"); return 0; }
    for (taps = 2; taps >= 1; taps--) {
        gi_build_ps_asm(taps);
        g_gi_ps = gpu_make_ps(g_gi_ps_src, taps == 2 ? "gpu_ps_gi2" : "gpu_ps_gi1");
        if (g_gi_ps) break;
    }
    if (!g_gi_ps) { strcpy(g_gi_why, "pixel shader gi did not compile (ps_2_0 limits?)"); return 0; }
    g_gi_taps = taps;
    return 1;
}

#define gi_log gpu_log

static int bake_indirect_gpu(void) {
    VPL* vpl = NULL; float* Fm = NULL; V3 *S0 = NULL, *S1 = NULL, *Phi = NULL; float atot = 0.0f, zf = 1.0f;
    int M, nv, i, j, k, oi, f, res, ok = 0, nb = C.bounce_count < 1 ? 1 : C.bounce_count;
    IDirect3DCubeTexture9** cube = NULL; IDirect3DSurface9 *bb = NULL, *dsorig = NULL; double t0;
    if (!C.bake_gpu_gi) return 0;
    if (!gi_probe()) { gi_log("GI GPU refused: %s", g_gi_why); return 0; }
    if (!gpu_alloc_res()) { gi_log("GI GPU: depth surface unavailable"); return 0; }
    t0 = bake_now();
    M = C.gpu_gi_vpl < 32 ? 32 : (C.gpu_gi_vpl > 4096 ? 4096 : C.gpu_gi_vpl);
    res = C.gpu_gi_res < 32 ? 32 : (C.gpu_gi_res > 256 ? 256 : C.gpu_gi_res);
    { int p = 32; while (p < res) p <<= 1; res = p; }
    if (res > g_gpu_res) res = g_gpu_res;                         /* the depth surface of the shadows (g_gpu_ds) bounds the size */
    vpl = (VPL*)malloc((size_t)M * sizeof(VPL));
    nv = gi_make_vpls(vpl, M, &atot);
    if (nv < 16) { gi_log("GI GPU: too few VPL (%d)", nv); free(vpl); return 0; }

    /* --- 2. transport between the VPLs --- */
    Fm = (float*)malloc((size_t)nv * nv * sizeof(float));
    S0 = (V3*)malloc((size_t)nv * sizeof(V3)); S1 = (V3*)malloc((size_t)nv * sizeof(V3)); Phi = (V3*)malloc((size_t)nv * sizeof(V3));
    {   GiTr g; g.v = vpl; g.n = nv; g.F = Fm; g.bd2 = C.bounce_distance * C.bounce_distance; par_for(nv, gi_transport_row, &g); }
    for (i = 0; i < nv; i++) { S0[i] = v3(vpl[i].D0, vpl[i].D0, vpl[i].D0); Phi[i] = S0[i]; }
    for (k = 1; k < nb; k++) {
        for (i = 0; i < nv; i++) {
            V3 s = { 0, 0, 0 };
            for (j = 0; j < nv; j++) { float F = Fm[(size_t)i * nv + j]; if (F > 0.0f) s = vadd(s, vmul(vmulv(vpl[j].alb, S0[j]), F)); }
            S1[i] = s;
        }
        for (i = 0; i < nv; i++) { S0[i] = S1[i]; Phi[i] = vadd(Phi[i], S1[i]); }
    }
    for (i = 0; i < nv; i++) Phi[i] = vmulv(vpl[i].alb, Phi[i]);
    free(Fm); free(S0); free(S1);

    for (oi = 0; oi < g_nobj; oi++) {                              /* distance zf: scene radius seen from any VPL */
        const Obj* o = &g_obj[oi]; if (!o->occluder && !o->lightmap) continue;
        for (i = 0; i < nv; i++) { float d = vlen(vsub(o->bs_c, vpl[i].p)) + o->bs_r; if (d > zf) zf = d; }
    }
    zf += 2.0f;

    /* --- 3. depth cubemaps --- */
    cube = (IDirect3DCubeTexture9**)calloc((size_t)nv, sizeof(*cube));
    IDirect3DDevice9_GetRenderTarget(g_dev, 0, &bb); IDirect3DDevice9_GetDepthStencilSurface(g_dev, &dsorig);
    for (i = 0; i < nv; i++)
        if (FAILED(IDirect3DDevice9_CreateCubeTexture(g_dev, res, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube[i], NULL))) {
            gi_log("GI GPU: CreateCubeTexture failed at VPL %d/%d (res %d)", i, nv, res); goto done; }
    for (i = 0; i < nv; i += 16) {
        int e = i + 16 < nv ? i + 16 : nv; float cc[4] = { 1.0f / zf, 0, 0, 0 };
        IDirect3DDevice9_BeginScene(g_dev);
        IDirect3DDevice9_SetVertexShader(g_dev, g_gpu_vs_cube); IDirect3DDevice9_SetPixelShader(g_dev, g_gpu_ps_cube);
        IDirect3DDevice9_SetFVF(g_dev, FVF);
        RS(D3DRS_ZENABLE, D3DZB_TRUE); RS(D3DRS_ZWRITEENABLE, TRUE); RS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_ALPHABLENDENABLE, FALSE); RS(D3DRS_SRGBWRITEENABLE, FALSE); RS(D3DRS_COLORWRITEENABLE, 0xF);
        IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, cc, 1);
        for (j = i; j < e; j++) {
            V3 eye = vadd(vpl[j].p, vmul(vpl[j].n, 0.03f)); float lc[4];
            lc[0] = eye.x; lc[1] = eye.y; lc[2] = eye.z; lc[3] = 0;
            IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 4, lc, 1);
            for (f = 0; f < 6; f++) {
                M4 m = gpu_face_vp(eye, f, zf); float mt[16]; int r2, c2; IDirect3DSurface9* fs = NULL;
                for (r2 = 0; r2 < 4; r2++) for (c2 = 0; c2 < 4; c2++) mt[c2 * 4 + r2] = m.m[r2][c2];
                IDirect3DCubeTexture9_GetCubeMapSurface(cube[j], (D3DCUBEMAP_FACES)f, 0, &fs);
                IDirect3DDevice9_SetRenderTarget(g_dev, 0, fs); IDirect3DSurface9_Release(fs);
                IDirect3DDevice9_SetDepthStencilSurface(g_dev, g_gpu_ds);
                IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFF00u, 1.0f, 0);
                if (vdot(CUBE_LOOK[f], vpl[j].n) < -0.82f) continue;       /* the whole face looks below the surface of the VPL: never read */
                IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, mt, 4);
                for (oi = 0; oi < g_nobj; oi++) {
                    const Obj* o = &g_obj[oi]; int lod = C.occluder_lod < o->nlod ? C.occluder_lod : o->nlod - 1;
                    if (!o->occluder || !gpu_face_sees(eye, f, o->bs_c, o->bs_r)) continue;
                    gpu_draw_obj(o, lod < 0 ? 0 : lod);
                }
            }
        }
        IDirect3DDevice9_EndScene(g_dev);
        gpu_restore_targets(bb, dsorig);
        loading_frame(0.25f + 0.30f * (float)e / nv);
        if (g_abort) goto done;
    }

    /* --- 4. accumulation in each lightmap (UV space) --- */
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; IDirect3DTexture9* rt = NULL; IDirect3DSurface9 *rs = NULL, *sys = NULL; D3DLOCKED_RECT lr; int b, t, qi, x, y;
        float cc[4], kc[4], wc[4], br = C.bounce_distance, ar = C.ao_distance, bk, ak;
        if (!o->lightmap || !o->fT) continue;
        if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, o->aw, o->ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &rt, NULL))) {
            gi_log("GI GPU: CreateTexture fp16 %dx%d failed", o->aw, o->ah); goto done; }
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, o->aw, o->ah, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
            gi_log("GI GPU: CreateOffscreenPlainSurface fp16 failed"); IDirect3DTexture9_Release(rt); goto done; }
        IDirect3DTexture9_GetSurfaceLevel(rt, 0, &rs);
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, rs); IDirect3DDevice9_SetDepthStencilSurface(g_dev, NULL);
        IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        IDirect3DDevice9_BeginScene(g_dev);
        IDirect3DDevice9_SetVertexShader(g_dev, g_gi_vs); IDirect3DDevice9_SetPixelShader(g_dev, g_gi_ps);
        IDirect3DDevice9_SetFVF(g_dev, FVF);
        RS(D3DRS_ZENABLE, D3DZB_FALSE); RS(D3DRS_ZWRITEENABLE, FALSE); RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_SRGBWRITEENABLE, FALSE);
        RS(D3DRS_ALPHABLENDENABLE, TRUE); RS(D3DRS_SRCBLEND, D3DBLEND_ONE); RS(D3DRS_DESTBLEND, D3DBLEND_ONE); RS(D3DRS_COLORWRITEENABLE, 0xF);
        cc[0] = 1.0f / o->aw; cc[1] = 1.0f / o->ah; cc[2] = cc[3] = 0; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, cc, 1);
        cc[0] = 0.02f * (C.gpu_shadow_bias > 0.1f ? C.gpu_shadow_bias : 0.1f); cc[1] = cc[2] = cc[3] = 0; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 1, cc, 1);
        /* K: x = zf*4096, y = eps*4096 + 0.5, z = 4096*(1 - relative tolerance) ; W: smooth windows (bounce_distance / ao_distance) */
        kc[0] = zf * 4096.0f; kc[1] = 0.02f * 4096.0f + 0.5f; kc[2] = 4096.0f * (1.0f - 0.04f * (C.gpu_gi_bias > 0.0f ? C.gpu_gi_bias : 1.0f)); kc[3] = 0;
        bk = 1.0f / (0.15f * br + 1e-3f); ak = 1.0f / (0.15f * ar + 1e-3f);
        wc[0] = br * bk; wc[1] = bk; wc[2] = ar * ak; wc[3] = ak;
        IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * g_gi_taps, kc, 1);
        IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * g_gi_taps + 1, wc, 1);
        for (t = 0; t < g_gi_taps; t++) {
            ss(t, D3DSAMP_MAGFILTER, D3DTEXF_POINT); ss(t, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(t, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            ss(t, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(t, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(t, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
            ss(t, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        for (b = 0; b < nv; b += g_gi_taps) {
            for (t = 0; t < g_gi_taps; t++) {
                int v = b + t; float pc[4];
                if (v < nv) {                                        /* real VPL */
                    pc[0] = vpl[v].p.x; pc[1] = vpl[v].p.y; pc[2] = vpl[v].p.z; pc[3] = 0; IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * t, pc, 1);
                    pc[0] = vpl[v].n.x; pc[1] = vpl[v].n.y; pc[2] = vpl[v].n.z; pc[3] = vpl[v].dA; IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * t + 1, pc, 1);
                    pc[0] = Phi[v].x; pc[1] = Phi[v].y; pc[2] = Phi[v].z; pc[3] = 0; IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * t + 2, pc, 1);
                    IDirect3DDevice9_SetTexture(g_dev, t, (IDirect3DBaseTexture9*)cube[v]);
                } else {                                             /* odd count: empty VPL (dA = 0 -> no contribution) */
                    pc[0] = pc[1] = pc[2] = 1.0e4f; pc[3] = 0; IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * t, pc, 1);
                    memset(pc, 0, sizeof pc); IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 3 * t + 1, pc, 1);
                    IDirect3DDevice9_SetTexture(g_dev, t, (IDirect3DBaseTexture9*)cube[0]);
                }
            }
            gpu_draw_obj(o, 0);
        }
        IDirect3DDevice9_EndScene(g_dev);
        for (t = 0; t < g_gi_taps; t++) IDirect3DDevice9_SetTexture(g_dev, t, NULL);
        gpu_restore_targets(bb, dsorig);
        RS(D3DRS_ALPHABLENDENABLE, FALSE);

        /* --- 5. readback --- */
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, rs, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            for (qi = 0; qi < o->nq; qi++) {
                const Quad* q = &o->q[qi];
                for (y = 0; y < q->th + 2; y++) for (x = 0; x < q->tw + 2; x++) {
                    int cx = x < 1 ? 1 : (x > q->tw ? q->tw : x), cy = y < 1 ? 1 : (y > q->th ? q->th : y);
                    const uint16_t* px = (const uint16_t*)((const char*)lr.pBits + (size_t)(q->ty + cy) * lr.Pitch) + (size_t)(q->tx + cx) * 4;
                    size_t kk = ((size_t)(q->ty + y) * o->aw + q->tx + x) * 3;
                    float r = half2f(px[0]), g = half2f(px[1]), bl = half2f(px[2]), blocked = half2f(px[3]), vis, fa;
                    int cov = 0;
                    if (q->cover) { float s = clampf((x - 0.5f) / q->tw, 0, 1), tt = clampf((y - 0.5f) / q->th, 0, 1); V3 n, p = quad_at(q, s, tt, &n); cov = covered(p, q->box); }
                    if (cov) { fa = 1.0f - C.ao_strength; r = g = bl = 0.0f; }
                    else { vis = clampf(1.0f - blocked * C.gpu_gi_ao_gain, 0.0f, 1.0f); fa = 1.0f - C.ao_strength * (1.0f - vis); }
                    o->fA[kk] = o->fA[kk + 1] = o->fA[kk + 2] = fa;
                    o->fB[kk] = r * C.bounce_strength; o->fB[kk + 1] = g * C.bounce_strength; o->fB[kk + 2] = bl * C.bounce_strength;
                }
            }
            IDirect3DSurface9_UnlockRect(sys);
        } else { gi_log("GI GPU: fp16 readback failed (%s)", o->name); IDirect3DSurface9_Release(rs); IDirect3DSurface9_Release(sys); IDirect3DTexture9_Release(rt); goto done; }
        IDirect3DSurface9_Release(rs); IDirect3DSurface9_Release(sys); IDirect3DTexture9_Release(rt);
        loading_frame(0.55f + 0.35f * (float)(oi + 1) / g_nobj);
        if (g_abort) goto done;
    }
    ok = 1;
done:
    gpu_restore_targets(bb, dsorig);
    if (bb) IDirect3DSurface9_Release(bb);
    if (dsorig) IDirect3DSurface9_Release(dsorig);
    if (cube) { for (i = 0; i < nv; i++) if (cube[i]) IDirect3DCubeTexture9_Release(cube[i]); free(cube); }
    free(vpl); free(Phi); gpu_release();
    gi_log("GI GPU: %s - %d VPL, cube %d, %d bounces, %.2f s (area %.1f)", ok ? "OK" : "FAILED (CPU takes over)", nv, res, nb, bake_now() - t0, atot);
    return ok;
}
