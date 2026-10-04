/* glass.h - real-time glass sphere (vs_2_0 + ps_2_0, no ray tracing per frame)
 *
 * Per vertex (vs_2_0, 256 instructions available) we solve the exact path of a ray through a sphere:
 *   entry (Snell, 1/ior) -> chord inside the sphere -> exit (Snell, ior); plus the ray reflected off the front face.
 * The two final directions are "parallax-corrected" against the room volume (ray / AABB intersection),
 * then read from ONE cubemap: the reflection bake cubemap, taken at the center of the sphere.
 * Per pixel (ps_2_0): 2 cubemap reads + Fresnel (Schlick): lerp(refraction, reflection, F).
 * The mesh sphere has 24 segments per face: fine enough that the direction interpolation looks exact.
 */
#pragma once

static const char GLASS_VS_SRC[] =
    "float4 c0 : register(c0); float4 c1 : register(c1); float4 c2 : register(c2); float4 c3 : register(c3);\n"
    "float4 eyep : register(c4);\n"      /* xyz = camera (object space) */
    "float4 ctr  : register(c5);\n"      /* xyz = center, w = radius */
    "float4 prm  : register(c6);\n"      /* x = 1/ior, y = ior */
    "float4 blo  : register(c7);\n"      /* room volume */
    "float4 bhi  : register(c8);\n"
    "float4 flip : register(c9);\n"    /* x = -1 mirrors the reflection X axis */
    "struct VOut { float4 p : POSITION; float3 rd : TEXCOORD0; float3 td : TEXCOORD1; float3 n : TEXCOORD2; float3 v : TEXCOORD3; };\n"
    "float3 envdir(float3 p, float3 d) {\n"
    "  d = d + 0.00001;\n"
    "  float3 far = lerp(blo.xyz, bhi.xyz, step(0.0, d));\n"
    "  float3 t = (far - p) / d;\n"
    "  float tm = min(t.x, min(t.y, t.z));\n"
    "  return p + d * tm - ctr.xyz;\n"
    "}\n"
    "VOut main(float3 pos : POSITION) {\n"
    "  VOut o; float4 p4 = float4(pos, 1.0);\n"
    "  o.p = float4(dot(p4, c0), dot(p4, c1), dot(p4, c2), dot(p4, c3));\n"
    "  float3 N = normalize(pos - ctr.xyz);\n"
    "  float3 I = normalize(pos - eyep.xyz);\n"
    "  o.rd = envdir(pos, reflect(I, N)) * float3(flip.x, 1, 1);\n"
    "  float3 d1 = refract(I, N, prm.x);\n"
    "  float t = -2.0 * dot(d1, pos - ctr.xyz);\n"
    "  float3 q = pos + d1 * t;\n"
    "  float3 n2 = normalize(q - ctr.xyz);\n"
    "  float3 d2 = refract(d1, -n2, prm.y);\n"
    "  d2 = lerp(reflect(d1, -n2), d2, step(0.01, dot(d2, d2)));\n"
    "  o.td = envdir(q, d2) * float3(flip.x, 1, 1);\n"
    "  o.n = N; o.v = -I;\n"
    "  return o;\n"
    "}\n";

static const char GLASS_PS_SRC[] =
    "samplerCUBE S0 : register(s0);\n"
    "float4 k : register(c0);\n"         /* x = F0, y = reflection gain, z = absorption (tint) */
    "float4 main(float3 rd : TEXCOORD0, float3 td : TEXCOORD1, float3 n : TEXCOORD2, float3 v : TEXCOORD3) : COLOR {\n"
    "  float c = saturate(dot(normalize(n), normalize(v)));\n"
    "  float x = 1.0 - c; float x2 = x * x;\n"
    "  float F = k.x + (1.0 - k.x) * x2 * x2 * x;\n"
    "  float3 refl = texCUBE(S0, rd).rgb;\n"
    "  float3 refr = texCUBE(S0, td).rgb;\n"
    "  return float4(refr * (1.0 - F) * k.z + refl * (F * k.y), 1.0);\n"
    "}\n";

static IDirect3DVertexShader9* g_glass_vs; static IDirect3DPixelShader9* g_glass_ps; static int g_glass_tried;

static void draw_glass(Obj* o, M4 w) {
    M4 wvp = m_mul(m_mul(w, g_view), g_proj); float mt[16], c[4]; int r, cc; V3 eye;
    if (!g_glass_tried) {
        g_glass_tried = 1;
        g_glass_vs = gpu_make_vs(GLASS_VS_SRC, "glass_vs");
        g_glass_ps = gpu_make_ps(GLASS_PS_SRC, "glass_ps");
    }
    if (!g_glass_vs || !g_glass_ps || !o->tex_cube) return;      /* no ps_2_0 / d3dx9: the glass is not drawn */
    for (r = 0; r < 4; r++) for (cc = 0; cc < 4; cc++) mt[cc * 4 + r] = wvp.m[r][cc];
    IDirect3DDevice9_SetVertexShader(g_dev, g_glass_vs); IDirect3DDevice9_SetPixelShader(g_dev, g_glass_ps);
    IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, mt, 4);
    eye = m_vec_inv(v3(0, 0, -g_dist), g_world);                 /* camera in object space (world = rotation only) */
    c[0] = eye.x; c[1] = eye.y; c[2] = eye.z; c[3] = 0; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 4, c, 1);
    c[0] = o->gc.x; c[1] = o->gc.y; c[2] = o->gc.z; c[3] = o->gr; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 5, c, 1);
    { float ior = C.glass_ior < 1.01f ? 1.01f : C.glass_ior; c[0] = 1.0f / ior; c[1] = ior; c[2] = c[3] = 0; }
    IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 6, c, 1);
    c[0] = g_room_lo.x; c[1] = g_room_lo.y; c[2] = g_room_lo.z; c[3] = 0; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 7, c, 1);
    c[0] = g_room_hi.x; c[1] = g_room_hi.y; c[2] = g_room_hi.z; c[3] = 0; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 8, c, 1);
    c[0] = C.glass_flip_x ? -1.0f : 1.0f; c[1] = c[2] = c[3] = 0; IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 9, c, 1);
    { float ior = C.glass_ior < 1.01f ? 1.01f : C.glass_ior, f0 = (ior - 1) / (ior + 1);
      c[0] = f0 * f0; c[1] = C.glass_spec; c[2] = 0.97f; c[3] = 0; }   /* slight veil (absorption) so the glass does not look like a hole */
    IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, c, 1);
    IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9*)o->tex_cube);
    ss(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); ss(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); ss(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    ss(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    ss(0, D3DSAMP_SRGBTEXTURE, C.gamma_correct ? TRUE : FALSE);
    if (C.depth_prepass) RS(D3DRS_ZFUNC, D3DCMP_LESS);
    IDirect3DDevice9_SetStreamSource(g_dev, 0, o->vb, 0, sizeof(Vtx));
    D9(SetIndices)(g_dev, o->ib[0], 0);
    IDirect3DDevice9_DrawIndexedPrimitive(g_dev, D3DPT_TRIANGLELIST, 0, 0, o->nv, 0, o->nt[0]);
    if (C.depth_prepass) RS(D3DRS_ZFUNC, D3DCMP_EQUAL);
    IDirect3DDevice9_SetVertexShader(g_dev, NULL); IDirect3DDevice9_SetPixelShader(g_dev, NULL);
    IDirect3DDevice9_SetFVF(g_dev, FVF);
}
