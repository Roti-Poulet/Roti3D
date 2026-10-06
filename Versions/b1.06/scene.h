/* scene.h - objects, meshes (grid of quads), LoD by edge-collapse, BVH, demo scene */
#pragma once
#define MAXOBJ  32
#define MAXQUAD 512
#define MAXBOX  512
#define MAXLOD  6
#define MAXLIGHT 8
#define MAXPRISM 12

typedef struct { float x, y, z, nx, ny, nz; DWORD col; float u0, v0, u1, v1; } Vtx;
#define FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2)

typedef struct {
    V3 o, u, v, n; float lu, lv;          /* corner, axes, normal, size */
    int tx, ty, tw, th;                   /* tile in the atlas (tw,th = interior, +1 texel margin) */
    int gn;                               /* NxN subdivision (sphere) */
    int tri;                              /* 1 = triangle (corners (0,0),(1,0),(0,1)) instead of a quad */
    int box, cover;                       /* owner box; "covered face" test */
    V3 sc; float sr;                      /* sphere: center, radius (sr>0) */
    V3 alb; DWORD col;                    /* linear albedo (bake); vertex color (rendering) */
} Quad;
typedef struct { V3 c, X, Z, h; } Box;

/* Reflection G-buffer: 4 bytes per cubemap texel and per sample.
 *   [0] hit object (GB_SKY = sky)   [1..3] lightmap texel index (24 bits)
 * The albedo is not stored here: it is read from Obj.alb_px (albedo map per lightmap texel). */
#define GB_SKY 255   /* must be > MAXOBJ: 15 used to collide with a real object (lamp0) as soon as the scene had more than 15 objects */
static void gb_set(unsigned char* p, int obj, int idx) {
    p[0] = (unsigned char)obj;
    p[1] = (unsigned char)((idx >> 16) & 255);
    p[2] = (unsigned char)((idx >> 8) & 255);
    p[3] = (unsigned char)(idx & 255);
}
#define GB_OBJ(p) ((int)(p)[0])
#define GB_IDX(p) (((int)(p)[1] << 16) | ((int)(p)[2] << 8) | (int)(p)[3])

typedef struct {
    char name[16];
    int nq; Quad q[MAXQUAD];
    int lightmap, reflect, occluder, textured, is_lamp, light_idx;
    int in_refl;                               /*Visible in reflections ("refl_filter" filter) */
    float refl, rough; V3 tint;
    unsigned char c0[3], c1[3]; int texsz; float uv_tile; int texpat;
    V3 bs_c; float bs_r;
    int aw, ah; uint32_t *T, *A; float *fT, *fA, *fB;    /* light (RGBE): T = per unit of light, A = ambient */
    /* Shading mask per texel and per light (RGBE). Essential so that we can
     * change the color/intensity of a light WITHOUT re-baking: T = sum of
     * (light_color x light_mask), recomputed in one pass over the atlas. */
    uint32_t* S[MAXLIGHT];
    uint8_t* gv[MAXLIGHT];                 /* visibility (number of unblocked shadow rays) computed by the GPU, per texel */
    /* Indirect bounce per texel, independent of the light colors (like A):
     * T = sum (light_color x light_mask) + bounce. */
    uint32_t* Bo;
    /* Caustics (bake photons): irradiance per texel for a unit white light, added to Bo during compositing */
    uint32_t* Ca; float* fCa;
    int glass; V3 gc; float gr;                          /* glass sphere (no lightmap): center, radius */
    int lamp_world;                                      /* lamp whose mesh is already in world coordinates (visible in reflections) */
    unsigned char* gbuf; int ngbuf, nsamp;               /* reflection hits (4 bytes/texel) */
    unsigned char* alb_px;                               /* 8-bit linear albedo per lightmap texel */
    uint32_t* tex_px;                                    /* albedo pixels (generated only once) */
    float avgT[3], avgA[3];                         /* average lightmap (distant lighting) */
    Vtx* vert; uint16_t* vq; int nv; uint16_t* idx[MAXLOD]; int nt[MAXLOD]; int nlod;
    IDirect3DVertexBuffer9* vbf;                   /* same vertices, color already multiplied by the light */
    IDirect3DTexture9* tex_albedo; IDirect3DTexture9* tex_lm; IDirect3DCubeTexture9* tex_cube;
    IDirect3DVertexBuffer9* vb; IDirect3DIndexBuffer9* ib[MAXLOD];
} Obj;

typedef struct { V3 a, e1, e2, n, alb; int obj; float lu[3], lv[3];
                 V3 N, U, V; float nd; } Tri;   /* N = e1 x e2 ; U,V = reciprocal basis ; nd = N.a (plane test then barycentric) */

static Obj g_obj[MAXOBJ]; static int g_nobj;
static Box g_box[MAXBOX]; static int g_nbox;
static Tri* g_tri; static int g_ntri;
static int g_lamp = -1;
static V3 g_light;

/* ---------- Light sources ---------- */
typedef struct {
    V3 pos;                 /* world position */
    V3 col;                 /* linear color (already multiplied by the intensity) */
    float radius;           /* light radius (area sampling) */
    int shadow;             /* 1 = casts shadows, 0 = diffuse fill */
    int fixed;              /* 1 = does not move with the arrows */
    int shape;              /* 0 = sphere (radius = radius) ; 1 = square horizontal ceiling light, lights downward (radius = half-side) */
} Light;
static Light g_lightset[MAXLIGHT]; static int g_nlight; static int g_lsel;   /* g_lsel = selected light */
static int g_maxtex = 2048;                  /* largest texture accepted by the driver */

/* AABB of an oriented box (X, Z unit, Y = vertical): used for the test
 * "capsule" of the shadow pre-filter and to bound the atlas. */
static inline void box_aabb(const Box* b, float* lo, float* hi) {
    lo[0] = b->c.x - fabsf(b->X.x) * b->h.x - fabsf(b->Z.x) * b->h.z;
    lo[1] = b->c.y - b->h.y;
    lo[2] = b->c.z - fabsf(b->X.z) * b->h.x - fabsf(b->Z.z) * b->h.z;
    hi[0] = b->c.x + fabsf(b->X.x) * b->h.x + fabsf(b->Z.x) * b->h.z;
    hi[1] = b->c.y + b->h.y;
    hi[2] = b->c.z + fabsf(b->X.z) * b->h.x + fabsf(b->Z.z) * b->h.z;
}

/* ---------- colors ---------- */
static V3 col_lin(int r, int g, int b) {
    V3 c = v3(r / 255.0f, g / 255.0f, b / 255.0f);
    if (C.gamma_correct) c = v3(powf(c.x, 2.2f), powf(c.y, 2.2f), powf(c.z, 2.2f));
    return c;
}
static DWORD col_pack(V3 c) {
    return 0xFF000000u | ((DWORD)(clampf(c.x, 0, 1) * 255.0f + 0.5f) << 16) |
           ((DWORD)(clampf(c.y, 0, 1) * 255.0f + 0.5f) << 8) | (DWORD)(clampf(c.z, 0, 1) * 255.0f + 0.5f);
}

/* ---------- construction ---------- */
static Obj* new_obj(const char* name, int lightmap, int reflect, float refl, float rough, V3 tint) {
    Obj* o = &g_obj[g_nobj++];
    memset(o, 0, sizeof *o);
    strncpy(o->name, name, 15);
    o->lightmap = lightmap; o->reflect = reflect; o->refl = refl; o->rough = rough; o->tint = tint;
    o->occluder = 1; o->uv_tile = 1.0f; o->in_refl = 1; o->light_idx = -1;
    return o;
}

static int add_boxdef(V3 c, V3 X, V3 Z, V3 h) {
    Box* b = &g_box[g_nbox]; b->c = c; b->X = X; b->Z = Z; b->h = h; return g_nbox++;
}

/* 6 faces (5 if skip_bottom) of an oriented box; gn>1 + sr>0 = "cube-sphere" sphere */
static void add_faces(Obj* o, V3 c, V3 half, float yaw, int skip_bottom, V3 alb, DWORD col, int box, int gn, float sr) {
    V3 X = v3(cosf(yaw), 0, sinf(yaw)), Y = v3(0, 1, 0), Z = v3(-sinf(yaw), 0, cosf(yaw));
    struct { V3 d, t1, t2; float h1, h2, ext; } F[6] = {
        { X, Y, Z, half.y, half.z, half.x }, { vmul(X, -1), Y, Z, half.y, half.z, half.x },
        { Y, X, Z, half.x, half.z, half.y }, { vmul(Y, -1), X, Z, half.x, half.z, half.y },
        { Z, X, Y, half.x, half.y, half.z }, { vmul(Z, -1), X, Y, half.x, half.y, half.z },
    };
    int i;
    for (i = 0; i < 6; i++) {
        Quad* q; V3 ctr, u, v;
        if (skip_bottom && i == 3) continue;
        if (o->nq >= MAXQUAD) return;
        q = &o->q[o->nq++]; memset(q, 0, sizeof *q);
        ctr = vadd(c, vmul(F[i].d, F[i].ext));
        u = vmul(F[i].t1, 2 * F[i].h1); v = vmul(F[i].t2, 2 * F[i].h2);
        q->o = vsub(vsub(ctr, vmul(F[i].t1, F[i].h1)), vmul(F[i].t2, F[i].h2));
        if (vdot(vcross(u, v), F[i].d) < 0) { V3 t = u; u = v; v = t; }   /* u x v = outward normal */
        q->u = u; q->v = v; q->n = F[i].d; q->lu = vlen(u); q->lv = vlen(v);
        q->gn = gn < 1 ? 1 : gn; q->box = box; q->cover = (box >= 0);
        q->sc = c; q->sr = sr; q->alb = alb; q->col = col;
        if (sr > 0) q->lu = q->lv = sr * 1.75f;
    }
}

static uint32_t g_lcg;
static float lcg_f(void);

/* Prism with a parallelogram section: the mesh can only represent
 * parallelograms (a Quad = o + u*s + v*t), so the section must be one too.
 * 4 sides + 2 bottoms, all exact. The two base vectors are free (lengths
 * and angles differ) and the section is rotated: this gives crystals, pebbles,
 * oblique prisms, far from a simple box. */
static void add_prism(Obj* o, V3 c, float r1, float r2, float h, float skew, float yaw,
                      V3 alb, DWORD col, int box) {
    V3 e1 = v3(cosf(yaw) * r1, 0, sinf(yaw) * r1);
    V3 e2 = v3(-sinf(yaw + skew) * r2, 0, cosf(yaw + skew) * r2);
    V3 pt[8];      /* 0..3 = bottom, 4..7 = top */
    int i, k;
    for (k = 0; k < 2; k++) {
        float y = k ? h * 0.5f : -h * 0.5f;
        pt[k * 4 + 0] = v3(c.x, y, c.z);
        pt[k * 4 + 1] = vadd(pt[k * 4 + 0], e1);
        pt[k * 4 + 2] = vadd(vadd(pt[k * 4 + 0], e1), e2);
        pt[k * 4 + 3] = vadd(pt[k * 4 + 0], e2);
    }
    /* 4 sides: bottom edge x height */
    for (i = 0; i < 4; i++) {
        int j = (i + 1) & 3; Quad* q; V3 d, u, vv;
        if (o->nq >= MAXQUAD) return;
        q = &o->q[o->nq++]; memset(q, 0, sizeof *q);
        u = vsub(pt[j], pt[i]); vv = v3(0, h, 0);
        d = vnorm(vcross(u, vv));
        if (vdot(d, vsub(vmul(vadd(vadd(pt[i], pt[j]), vadd(pt[4 + i], pt[4 + j])), 0.25f), c)) < 0) d = vmul(d, -1.0f);
        q->o = pt[i]; q->u = u; q->v = vv; q->n = d; q->lu = vlen(u); q->lv = h;
        q->gn = 1; q->box = box; q->cover = (box >= 0); q->alb = alb; q->col = col;
    }
    /* 2 bottoms: 1 quad each (the section is a parallelogram) */
    for (k = 0; k < 2; k++) {
        Quad* q; V3 nrm = v3(0, k ? 1.0f : -1.0f, 0), u = e1, v2 = e2;
        if (o->nq >= MAXQUAD) return;
        q = &o->q[o->nq++]; memset(q, 0, sizeof *q);
        if (vdot(vcross(u, v2), nrm) < 0) { V3 t = u; u = v2; v2 = t; }
        q->o = pt[k * 4 + 0]; q->u = u; q->v = v2; q->n = nrm; q->lu = r1; q->lv = r2;
        q->gn = 1; q->box = box; q->cover = (box >= 0); q->alb = alb; q->col = col;
    }
}

/* ---------- shapes with triangular faces ----------
 * A parallelogram cannot degenerate into a triangle: a real triangle support is needed
 * in the mesh (Quad.tri). These shapes finally give silhouettes
 * that are not boxes: tetrahedron, pyramid, octahedron, wedge (triangular
 * triangular prism). Each vertex is computed so that u x v = outward normal. */
static void add_tri(Obj* o, V3 a, V3 b, V3 c, V3 alb, DWORD col, int box) {
    Quad* q; V3 u = vsub(b, a), v = vsub(c, a), n;
    if (o->nq >= MAXQUAD) return;
    q = &o->q[o->nq++]; memset(q, 0, sizeof *q);
    n = vnorm(vcross(u, v));
    q->o = a; q->u = u; q->v = v; q->n = n; q->lu = vlen(u); q->lv = vlen(v);
    q->gn = 1; q->tri = 1; q->box = box; q->cover = (box >= 0); q->alb = alb; q->col = col;
}
/* explicit parallelogram quad (o, o+u, o+u+v, o+v), outward normal pointing "outside" */
static void add_quadf(Obj* o, V3 p0, V3 u, V3 v, V3 dehors, V3 alb, DWORD col, int box) {
    Quad* q; V3 n = vcross(u, v);
    if (o->nq >= MAXQUAD) return;
    if (vlen(n) < 1e-6f) return;                        /* no degenerate face */
    if (vdot(n, dehors) < 0) { V3 t = u; u = v; v = t; n = vmul(n, -1.0f); }
    q = &o->q[o->nq++]; memset(q, 0, sizeof *q);
    q->o = p0; q->u = u; q->v = v; q->n = vnorm(n); q->lu = vlen(u); q->lv = vlen(v);
    q->gn = 1; q->box = box; q->cover = (box >= 0); q->alb = alb; q->col = col;
}
/* reoriented to point outward from a center */
static V3 tri_out(Obj* o, V3 a, V3 b, V3 c, V3 centre, V3 alb, DWORD col, int box) {
    V3 n = vcross(vsub(b, a), vsub(c, a));
    if (vdot(n, vsub(vmul(vadd(vadd(a, b), c), 1.0f / 3.0f), centre)) < 0) { V3 t = b; b = c; c = t; }
    add_tri(o, a, b, c, alb, col, box);
    return a;
}
/* square-base pyramid: 4 triangles + 1 quad (the "real" polygon) */
static void add_pyramid(Obj* o, V3 c, float r, float h, float yaw, V3 alb, DWORD col, int box) {
    V3 p[4], apex = v3(c.x, c.y + h * 0.5f, c.z);
    int i;
    for (i = 0; i < 4; i++) { float a = yaw + (float)i * (PI * 0.5f); p[i] = v3(c.x + cosf(a) * r, c.y - h * 0.5f, c.z + sinf(a) * r); }
    for (i = 0; i < 4; i++) tri_out(o, p[i], p[(i + 1) & 3], apex, c, alb, col, box);
    add_quadf(o, p[0], vsub(p[1], p[0]), vsub(p[3], p[0]), v3(0, -1, 0), alb, col, box);
}
/* tetrahedron: 4 triangles */
static void add_tetra(Obj* o, V3 c, float r, float h, float yaw, V3 alb, DWORD col, int box) {
    V3 p[4]; int i;
    for (i = 0; i < 4; i++) { float a = yaw + (float)i * (PI * 0.5f); p[i] = v3(c.x + cosf(a) * r, c.y - h * 0.5f, c.z + sinf(a) * r); }
    for (i = 0; i < 4; i++) tri_out(o, p[i], p[(i + 1) & 3], v3(c.x, c.y + h * 0.5f, c.z), c, alb, col, box);
}
/* octahedron: 8 triangles (two pyramids base to base) */
static void add_octa(Obj* o, V3 c, float r, float h, float yaw, V3 alb, DWORD col, int box) {
    V3 p[4], up = v3(c.x, c.y + h * 0.5f, c.z), dn = v3(c.x, c.y - h * 0.5f, c.z);
    int i;
    for (i = 0; i < 4; i++) { float a = yaw + (float)i * (PI * 0.5f); p[i] = v3(c.x + cosf(a) * r, c.y, c.z + sinf(a) * r); }
    for (i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        tri_out(o, p[i], p[j], up, c, alb, col, box);
        tri_out(o, p[j], p[i], dn, c, alb, col, box);
    }
}
/* wedge / triangular prism: 2 triangles + 3 quads */
static void add_wedge(Obj* o, V3 c, float r, float h, float yaw, V3 alb, DWORD col, int box) {
    V3 X = v3(cosf(yaw), 0, sinf(yaw)), Z = v3(-sinf(yaw), 0, cosf(yaw));
    V3 base = vsub(c, vmul(vadd(vmul(X, r), vmul(Z, r)), 0.5f));
    V3 a = base, b = vadd(base, vmul(X, 2 * r)), d2 = vadd(base, vmul(Z, 2 * r));
    V3 up = v3(0, h, 0);
    int i;
    tri_out(o, a, b, d2, c, alb, col, box);                    /* base */
    tri_out(o, vadd(b, up), vadd(a, up), vadd(d2, up), c, alb, col, box);   /* sommet */
    /* 3 sides: (bottom edge) x (height) -> 3 exact parallelograms */
    {   V3 e[3][2];
        e[0][0] = a; e[0][1] = b;
        e[1][0] = b; e[1][1] = d2;
        e[2][0] = d2; e[2][1] = a;
        for (i = 0; i < 3; i++) {
            V3 p0 = e[i][0], u = vsub(e[i][1], e[i][0]), nn;
            if (o->nq >= MAXQUAD) return;
            nn = vnorm(vcross(u, up));
            if (vdot(nn, vsub(p0, c)) < 0) nn = vmul(nn, -1.0f);
            {   Quad* q = &o->q[o->nq++]; memset(q, 0, sizeof *q);
                q->o = p0; q->u = u; q->v = up; q->n = nn; q->lu = vlen(u); q->lv = h;
                q->gn = 1; q->box = box; q->cover = (box >= 0); q->alb = alb; q->col = col; }
        }
    }
}

/* position / normal of a point (s,t) on the face */
static V3 quad_at(const Quad* q, float s, float t, V3* nrm) {
    V3 p = vadd(q->o, vadd(vmul(q->u, s), vmul(q->v, t)));
    if (q->sr > 0) { V3 d = vnorm(vsub(p, q->sc)); *nrm = d; return vadd(q->sc, vmul(d, q->sr)); }
    *nrm = q->n; return p;
}

/* face covered by another box (never visible)? */
static int covered(V3 p, int self_box) {
    int b; const float eps = 0.01f;
    for (b = 0; b < g_nbox; b++) {
        V3 d; const Box* B = &g_box[b];
        if (b == self_box) continue;
        d = vsub(p, B->c);
        if (fabsf(vdot(d, B->X)) <= B->h.x + eps && fabsf(d.y) <= B->h.y + eps && fabsf(vdot(d, B->Z)) <= B->h.z + eps) return 1;
    }
    return 0;
}

/* ---------- procedural textures (platform) ---------- */
/* pow(r/255, 2.2) as a table: 256 doubles computed once (bit-exact with pow()) */
static double g_pow_lut[256];
static void pow_lut_init(void) {
    int i;
    for (i = 0; i < 256; i++) g_pow_lut[i] = pow(i / 255.0, C.gamma_correct ? 2.2 : 1.0);
}
static void gen_albedo(Obj* o, uint32_t* px, V3* avg_out) {
    int S = o->texsz, x, y, pat = o->texpat; double sum[3] = { 0, 0, 0 };
    for (y = 0; y < S; y++) for (x = 0; x < S; x++) {
        int cell, c, r[3];
        uint32_t h = (uint32_t)(x * 374761393u + y * 668265263u); float n, k = 1.0f;
        h = (h ^ (h >> 13)) * 1274126177u; n = ((h >> 8) & 255) / 255.0f - 0.5f;
        if (pat == 1) {
            /* grid: light cells + dark lines, tile-like */
            int u = x * 8 / S, v = y * 8 / S;
            int lu = (x * 8 % S) < (S / 32 + 1), lv = (y * 8 % S) < (S / 32 + 1);
            cell = (u + v) & 1;
            if (lu || lv) k = 0.42f; else if (((u * 5 + v * 3) % 7) == 0) k = 0.78f;
        } else if (pat == 2) {
            /* lignes diagonales + grille fine */
            int d = (x + y) % (S / 4 < 4 ? 4 : S / 4);
            cell = ((x * 16 / S) + (y * 16 / S)) & 1;
            if (d < 2) k = 0.5f;
            else if (((x + y) % (S / 8 < 3 ? 3 : S / 8)) == 0) k = 0.7f;
        } else {
            cell = ((x * 4 / S) + (y * 4 / S)) & 1;
            if (((x * 4) % S) < 4 || ((y * 4) % S) < 4) k = 0.82f;   /* joints */
        }
        for (c = 0; c < 3; c++) {
            float base = cell ? o->c1[c] : o->c0[c];
            r[c] = (int)clampf(base * k + n * 10.0f, 0, 255);
            sum[c] += g_pow_lut[r[c]];
        }
        px[y * S + x] = ((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | (uint32_t)r[2];
    }
    *avg_out = v3((float)(sum[0] / (S * S)), (float)(sum[1] / (S * S)), (float)(sum[2] / (S * S)));
}

/* ---------- atlas de lightmap ---------- */
static int pack_try(Obj* o, int W, int H, const int* tw, const int* th, const int* order) {
    int x = 0, y = 0, rowh = 0, k;
    for (k = 0; k < o->nq; k++) {
        int i = order[k], w = tw[i] + 2, h = th[i] + 2;
        if (w > W || h > H) return 0;
        if (x + w > W) { x = 0; y += rowh; rowh = 0; }
        if (y + h > H) return 0;
        o->q[i].tx = x; o->q[i].ty = y; o->q[i].tw = tw[i]; o->q[i].th = th[i];
        x += w; if (h > rowh) rowh = h;
    }
    return 1;
}
static int layout_obj(Obj* o) {
    float tpu = C.texels_per_unit; int attempt, i, j;
    int tw[MAXQUAD], th[MAXQUAD], order[MAXQUAD];
    /* the atlas must stay within the driver limits (MaxTextureWidth/Height),
     * otherwise CreateTexture fails. texels_per_unit is reduced if needed. */
    int maxs = (C.lightmap_max_size < g_maxtex ? C.lightmap_max_size : g_maxtex) - 2;
    if (maxs < 30) maxs = 30;
    for (attempt = 0; attempt < 24; attempt++, tpu *= 0.8f) {
        int W, H, bestA = 0x7fffffff, bw = 0, bh = 0;
        for (i = 0; i < o->nq; i++) {
            tw[i] = (int)ceilf(o->q[i].lu * tpu); th[i] = (int)ceilf(o->q[i].lv * tpu);
            if (tw[i] < 2) tw[i] = 2;
            if (th[i] < 2) th[i] = 2;
            if (tw[i] > maxs) tw[i] = maxs;
            if (th[i] > maxs) th[i] = maxs;
            order[i] = i;
        }
        for (i = 1; i < o->nq; i++) {          /* sort by decreasing height (denser packing) */
            int k = order[i]; j = i - 1;
            while (j >= 0 && th[order[j]] < th[k]) { order[j + 1] = order[j]; j--; }
            order[j + 1] = k;
        }
        for (W = 16; W <= C.lightmap_max_size; W <<= 1)
            for (H = 16; H <= C.lightmap_max_size; H <<= 1)
                if (W * H < bestA && pack_try(o, W, H, tw, th, order)) { bestA = W * H; bw = W; bh = H; }
        if (bw) { pack_try(o, bw, bh, tw, th, order); o->aw = bw; o->ah = bh; return 1; }
    }
    return 0;
}

/* ---------- maillage ---------- */
static void build_mesh(Obj* o, uint8_t** border_out) {
    int qi, i, j, nv = 0, nt = 0, vb = 0, ti = 0; uint8_t* border;
    /* nt must count the triangles ACTUALLY emitted: a triangular face emits
     * only n*n (not 2*n*n). Without this, decimate() would read beyond the
     * index array and write with corrupted vertex indices. */
    for (qi = 0; qi < o->nq; qi++) {
        int n = o->q[qi].gn;
        nv += (n + 1) * (n + 1);
        nt += o->q[qi].tri ? n * n : 2 * n * n;
    }
    o->vert = (Vtx*)malloc((size_t)nv * sizeof(Vtx));
    o->idx[0] = (uint16_t*)malloc((size_t)nt * 3 * 2);
    border = (uint8_t*)calloc((size_t)nv, 1);
    o->vq = (uint16_t*)malloc((size_t)nv * 2);
    for (qi = 0; qi < o->nq; qi++) {
        const Quad* q = &o->q[qi]; int n = q->gn;
        for (j = 0; j <= n; j++) for (i = 0; i <= n; i++) {
            float s = (float)i / n, t = (float)j / n; V3 nr, p = quad_at(q, s, t, &nr);
            Vtx* v = &o->vert[vb + j * (n + 1) + i];
            v->x = p.x; v->y = p.y; v->z = p.z; v->nx = nr.x; v->ny = nr.y; v->nz = nr.z; v->col = q->col;
            v->u0 = s * q->lu / o->uv_tile; v->v0 = t * q->lv / o->uv_tile;
            if (o->lightmap) { v->u1 = (q->tx + 1 + s * q->tw) / o->aw; v->v1 = (q->ty + 1 + t * q->th) / o->ah; }
            else v->u1 = v->v1 = 0;
            border[vb + j * (n + 1) + i] = (uint8_t)(i == 0 || j == 0 || i == n || j == n);
            o->vq[vb + j * (n + 1) + i] = (uint16_t)qi;
        }
        for (j = 0; j < n; j++) for (i = 0; i < n; i++) {
            uint16_t a = (uint16_t)(vb + j * (n + 1) + i), b = (uint16_t)(a + 1),
                     d = (uint16_t)(a + (n + 1)), c = (uint16_t)(d + 1);
            if (q->tri) {
                /* the affine quad cannot degenerate into a triangle: we only emit
                 * the triangles actually inside s + t <= 1. The vertices of the
                 * 4th row exist but stay unused (and protected by border). */
                if (i + j <= n - 2) { o->idx[0][ti++] = a; o->idx[0][ti++] = b; o->idx[0][ti++] = c;
                                      o->idx[0][ti++] = a; o->idx[0][ti++] = c; o->idx[0][ti++] = d; }
                else if (i + j == n - 1) { o->idx[0][ti++] = a; o->idx[0][ti++] = b; o->idx[0][ti++] = d; }
            } else {
                o->idx[0][ti++] = a; o->idx[0][ti++] = b; o->idx[0][ti++] = c;
                o->idx[0][ti++] = a; o->idx[0][ti++] = c; o->idx[0][ti++] = d;
            }
        }
        vb += (n + 1) * (n + 1);
    }
    o->nv = nv; o->nt[0] = nt; o->nlod = 1;
    /* bounding sphere (culling) */
    { V3 mn = v3(1e30f, 1e30f, 1e30f), mx = v3(-1e30f, -1e30f, -1e30f); float r = 0;
      for (i = 0; i < nv; i++) {
        mn = v3(fminf(mn.x, o->vert[i].x), fminf(mn.y, o->vert[i].y), fminf(mn.z, o->vert[i].z));
        mx = v3(fmaxf(mx.x, o->vert[i].x), fmaxf(mx.y, o->vert[i].y), fmaxf(mx.z, o->vert[i].z));
      }
      o->bs_c = vmul(vadd(mn, mx), 0.5f);
      for (i = 0; i < nv; i++) { float d = vlen(vsub(v3(o->vert[i].x, o->vert[i].y, o->vert[i].z), o->bs_c)); if (d > r) r = d; }
      o->bs_r = r; }
    *border_out = border;
}

/* ---------- LoD: edge collapse of the nearest interior vertices ---------- */
typedef struct { int a, b; float l; } Cand;
static int cand_cmp(const void* x, const void* y) {
    float a = ((const Cand*)x)->l, b = ((const Cand*)y)->l; return a < b ? -1 : (a > b ? 1 : 0);
}
static V3 vpos(const Vtx* v, int i) { return v3(v[i].x, v[i].y, v[i].z); }

/* merges vertex a into b (shortest edges first, independent set per pass) */
static int decimate(const Vtx* v, int nv, const uint8_t* border, uint16_t* idx, int nt, int target) {
    uint8_t* dead = (uint8_t*)calloc((size_t)nt, 1), *touched = (uint8_t*)malloc((size_t)nv);
    Cand* cs = (Cand*)malloc(sizeof(Cand) * (size_t)nt * 6);
    int alive = nt, pass, t, k, w;
    for (pass = 0; pass < 64 && alive > target; pass++) {
        int nc = 0, removed = 0, ci;
        for (t = 0; t < nt; t++) if (!dead[t]) for (k = 0; k < 3; k++) {
            int a = idx[3 * t + k], b = idx[3 * t + (k + 1) % 3]; V3 d = vsub(vpos(v, a), vpos(v, b)); float l = vdot(d, d);
            if (!border[a]) { cs[nc].a = a; cs[nc].b = b; cs[nc].l = l; nc++; }
            if (!border[b]) { cs[nc].a = b; cs[nc].b = a; cs[nc].l = l; nc++; }
        }
        qsort(cs, (size_t)nc, sizeof(Cand), cand_cmp);
        memset(touched, 0, (size_t)nv);
        for (ci = 0; ci < nc && alive > target; ci++) {
            int a = cs[ci].a, b = cs[ci].b, ok = 1;
            if (touched[a] || touched[b]) continue;
            for (t = 0; t < nt && ok; t++) {       /* test anti-retournement */
                int ha = -1, hb = 0; V3 p[3], q2[3], n0, n1;
                if (dead[t]) continue;
                for (k = 0; k < 3; k++) { if (idx[3 * t + k] == a) ha = k; if (idx[3 * t + k] == b) hb = 1; }
                if (ha < 0 || hb) continue;
                for (k = 0; k < 3; k++) { p[k] = vpos(v, idx[3 * t + k]); q2[k] = (k == ha) ? vpos(v, b) : p[k]; }
                n0 = vcross(vsub(p[1], p[0]), vsub(p[2], p[0]));
                n1 = vcross(vsub(q2[1], q2[0]), vsub(q2[2], q2[0]));
                if (vlen(n1) < 1e-8f || vdot(n0, n1) < 0.4f * vlen(n0) * vlen(n1)) ok = 0;
            }
            if (!ok) continue;
            for (t = 0; t < nt; t++) {
                int ha = -1, hb = 0;
                if (dead[t]) continue;
                for (k = 0; k < 3; k++) { if (idx[3 * t + k] == a) ha = k; if (idx[3 * t + k] == b) hb = 1; }
                if (ha < 0) continue;
                for (k = 0; k < 3; k++) touched[idx[3 * t + k]] = 1;
                if (hb) { dead[t] = 1; alive--; } else idx[3 * t + ha] = (uint16_t)b;
            }
            touched[a] = touched[b] = 1; removed++;
        }
        if (!removed) break;
    }
    for (t = 0, w = 0; t < nt; t++) if (!dead[t]) { idx[3 * w] = idx[3 * t]; idx[3 * w + 1] = idx[3 * t + 1]; idx[3 * w + 2] = idx[3 * t + 2]; w++; }
    free(dead); free(touched); free(cs);
    return w;
}

static void gen_lods(Obj* o, const uint8_t* border) {
    int l, nt0 = o->nt[0];
    for (l = 1; l <= C.lod_levels && l < MAXLOD; l++) {
        int target = (int)(nt0 * powf(1.0f - C.lod_reduction, (float)l)), n;
        uint16_t* c = (uint16_t*)malloc((size_t)o->nt[l - 1] * 3 * 2);
        memcpy(c, o->idx[l - 1], (size_t)o->nt[l - 1] * 3 * 2);
        n = decimate(o->vert, o->nv, border, c, o->nt[l - 1], target);
        if (n >= o->nt[l - 1]) { free(c); break; }       /* nothing to simplify (flat faces / borders) */
        o->idx[l] = c; o->nt[l] = n; o->nlod = l + 1;
    }
}

/* ---------- triangles + BVH (bake ray tracing) ---------- */
typedef struct { float mn[3], mx[3]; int l, r, first, n, axis; } BNode;
typedef struct { Tri* tri; int ntri; BNode* bn; int nn; } BVH;
static int g_sort_axis;
static BVH g_bvh, g_rbvh;      /* g_bvh = full bake, g_rbvh = subset visible in reflections */
static Tri* g_tri; static int g_ntri;        /* shortcuts to g_bvh (bake, GI) */
static BNode* g_bn; static int g_nbn;

static float tri_c(const Tri* t, int ax) {
    V3 c = vadd(t->a, vmul(vadd(t->e1, t->e2), 1.0f / 3.0f));
    return ax == 0 ? c.x : (ax == 1 ? c.y : c.z);
}
static int tri_cmp(const void* x, const void* y) {
    float a = tri_c((const Tri*)x, g_sort_axis), b = tri_c((const Tri*)y, g_sort_axis); return a < b ? -1 : (a > b ? 1 : 0);
}
static int bvh_rec(BVH* b, int first, int count) {
    Tri* g_tri = b->tri;
    int ni = b->nn++, i, ax = 0; float cmn[3] = { 1e30f, 1e30f, 1e30f }, cmx[3] = { -1e30f, -1e30f, -1e30f };
    BNode* nd = &b->bn[ni];
    nd->mn[0] = nd->mn[1] = nd->mn[2] = 1e30f; nd->mx[0] = nd->mx[1] = nd->mx[2] = -1e30f;
    for (i = first; i < first + count; i++) {
        const Tri* t = &g_tri[i]; V3 p[3]; int k;
        p[0] = t->a; p[1] = vadd(t->a, t->e1); p[2] = vadd(t->a, t->e2);
        for (k = 0; k < 3; k++) {
            float c[3] = { p[k].x, p[k].y, p[k].z }; int a2;
            for (a2 = 0; a2 < 3; a2++) { if (c[a2] < nd->mn[a2]) nd->mn[a2] = c[a2]; if (c[a2] > nd->mx[a2]) nd->mx[a2] = c[a2]; }
        }
        for (k = 0; k < 3; k++) { float c = tri_c(t, k); if (c < cmn[k]) cmn[k] = c; if (c > cmx[k]) cmx[k] = c; }
    }
    if (count <= 4) { nd->first = first; nd->n = count; nd->l = nd->r = -1; nd->axis = 0; return ni; }
    if (cmx[1] - cmn[1] > cmx[ax] - cmn[ax]) ax = 1;
    if (cmx[2] - cmn[2] > cmx[ax] - cmn[ax]) ax = 2;
    g_sort_axis = ax;
    qsort(g_tri + first, (size_t)count, sizeof(Tri), tri_cmp);
    { int half = count / 2, l = bvh_rec(b, first, half), r = bvh_rec(b, first + half, count - half);
      b->bn[ni].l = l; b->bn[ni].r = r; b->bn[ni].n = 0; b->bn[ni].first = 0; b->bn[ni].axis = ax; }
    return ni;
}

/* Builds a BVH over a subset of objects.
 *   refl_only = 0: all occluders (surface bake, shadows, GI)
 *   refl_only = 1: only the "in_refl" objects (reflection content) - the
 *                   non-occluders are included too (the lamp must be visible in reflections) */
static void build_bvh_into(BVH* b, int refl_only) {
    int oi, total = 0, w = 0;
    /* bake rays use a mesh LoD level (occluder_lod): fewer triangles, almost identical shadows */
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int l = C.occluder_lod < o->nlod ? C.occluder_lod : o->nlod - 1;
        if (refl_only ? !o->in_refl : !o->occluder) continue;
        total += o->nt[l < 0 ? 0 : l];
    }
    b->tri = (Tri*)malloc(sizeof(Tri) * (size_t)(total ? total : 1));
    b->ntri = 0;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int t, l = C.occluder_lod < o->nlod ? C.occluder_lod : o->nlod - 1;
        if (refl_only ? !o->in_refl : !o->occluder) continue;
        if (l < 0) l = 0;
        for (t = 0; t < o->nt[l]; t++) {
            Tri* tr = &b->tri[w++]; V3 p[3]; int e;
            for (e = 0; e < 3; e++) {
                const Vtx* v = &o->vert[o->idx[l][3 * t + e]];
                p[e] = v3(v->x, v->y, v->z);
                tr->lu[e] = v->u1 * o->aw; tr->lv[e] = v->v1 * o->ah;
            }
            tr->a = p[0]; tr->e1 = vsub(p[1], p[0]); tr->e2 = vsub(p[2], p[0]);
            tr->n = vnorm(vcross(tr->e1, tr->e2)); tr->obj = oi; tr->alb = o->q[o->vq[o->idx[l][3 * t]]].alb;
            {   /* plane intersection: t = (N.a - N.o) / (N.d) then (u,v) by 2 dot products */
                V3 N = vcross(tr->e1, tr->e2); float n2 = vdot(N, N);
                if (n2 < 1e-20f) { tr->N = v3(0, 0, 0); tr->U = tr->V = tr->N; tr->nd = 0; }
                else {
                    tr->N = N; tr->nd = vdot(N, tr->a);
                    tr->U = vmul(vcross(tr->e2, N), 1.0f / n2); tr->V = vmul(vcross(N, tr->e1), 1.0f / n2);
                }
            }
        }
    }
    b->ntri = w;
    b->bn = (BNode*)malloc(sizeof(BNode) * (size_t)(2 * w + 2)); b->nn = 0;
    if (w) bvh_rec(b, 0, w);
}

static void build_tris_bvh(void) {
    build_bvh_into(&g_bvh, 0);
    g_tri = g_bvh.tri; g_ntri = g_bvh.ntri; g_bn = g_bvh.bn; g_nbn = g_bvh.nn;
    if (C.refl_filter) { build_bvh_into(&g_rbvh, 1); }
    else { g_rbvh = g_bvh; }        /* filter disabled: reflections see everything, as before */
}

typedef struct { float t, u, v; int tri; } Hit;

static inline int ray_tri(const Tri* tr, V3 o, V3 d, float tmax, float* th, float* bu, float* bv) {
    float den = vdot(tr->N, d), t, u, v; V3 p;
    if (fabsf(den) < 1e-12f) return 0;
    t = (tr->nd - vdot(tr->N, o)) / den;
    if (t < 1e-4f || t >= tmax) return 0;                     /* fast rejection before the barycentric computation */
    p = vsub(vadd(o, vmul(d, t)), tr->a);
    u = vdot(p, tr->U); if (u < 0 || u > 1) return 0;
    v = vdot(p, tr->V); if (v < 0 || u + v > 1) return 0;
    *th = t; *bu = u; *bv = v; return 1;
}
static inline int slab(const BNode* b, V3 o, V3 inv, float tmax) {
    float t1 = (b->mn[0] - o.x) * inv.x, t2 = (b->mx[0] - o.x) * inv.x;
    float tn = fminf(t1, t2), tf = fmaxf(t1, t2);
    t1 = (b->mn[1] - o.y) * inv.y; t2 = (b->mx[1] - o.y) * inv.y;
    tn = fmaxf(tn, fminf(t1, t2)); tf = fminf(tf, fmaxf(t1, t2));
    t1 = (b->mn[2] - o.z) * inv.z; t2 = (b->mx[2] - o.z) * inv.z;
    tn = fmaxf(tn, fminf(t1, t2)); tf = fminf(tf, fmaxf(t1, t2));
    return tf >= fmaxf(tn, 0.0f) && tn < tmax;
}
static inline V3 safe_inv(V3 d) {
    return v3(1.0f / (fabsf(d.x) < 1e-12f ? 1e-12f : d.x), 1.0f / (fabsf(d.y) < 1e-12f ? 1e-12f : d.y), 1.0f / (fabsf(d.z) < 1e-12f ? 1e-12f : d.z));
}
static int trace_in(const BVH* b, V3 o, V3 d, float tmax, Hit* h, int skip_obj) {
    V3 inv = safe_inv(d); int st[64], sp = 0, hit = 0, i;
    if (!b->ntri) return 0;
    st[sp++] = 0;
    while (sp) {
        const BNode* nd = &b->bn[st[--sp]];
        if (!slab(nd, o, inv, tmax)) continue;
        if (nd->n > 0) {
            for (i = nd->first; i < nd->first + nd->n; i++) {
                float t, u, v;
                if (b->tri[i].obj == skip_obj) continue;
                if (ray_tri(&b->tri[i], o, d, tmax, &t, &u, &v)) { tmax = t; h->t = t; h->u = u; h->v = v; h->tri = i; hit = 1; }
            }
        } else {                                              /* the nearest child (along the ray direction) will be popped first */
            int neg = (nd->axis == 0 ? d.x : (nd->axis == 1 ? d.y : d.z)) < 0;
            st[sp++] = neg ? nd->l : nd->r; st[sp++] = neg ? nd->r : nd->l;
        }
    }
    return hit;
}
static int trace(V3 o, V3 d, float tmax, Hit* h, int skip_obj) { return trace_in(&g_bvh, o, d, tmax, h, skip_obj); }
static int occluded(V3 o, V3 d, float tmax) {
    V3 inv = safe_inv(d); int st[64], sp = 0, i;
    if (!g_ntri) return 0;
    st[sp++] = 0;
    while (sp) {
        const BNode* nd = &g_bn[st[--sp]];
        if (!slab(nd, o, inv, tmax)) continue;
        if (nd->n > 0) {
            for (i = nd->first; i < nd->first + nd->n; i++) { float t, u, v; if (ray_tri(&g_tri[i], o, d, tmax, &t, &u, &v)) return 1; }
        } else {
            int neg = (nd->axis == 0 ? d.x : (nd->axis == 1 ? d.y : d.z)) < 0;
            st[sp++] = neg ? nd->l : nd->r; st[sp++] = neg ? nd->r : nd->l;
        }
    }
    return 0;
}

/* atlas + meshes + LoD + BVH, once the scene is described */
static void scene_finalize(void) {
    int oi, qi; int S = C.reflection_size;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; uint8_t* border; V3 avg;
        memset(&avg, 0, sizeof avg);
        if (o->textured) {
            o->tex_px = (uint32_t*)malloc((size_t)o->texsz * o->texsz * 4);
            if (!o->tex_px) { MessageBoxA(NULL, "Out of memory", "Error", MB_ICONERROR); ExitProcess(1); }
            gen_albedo(o, o->tex_px, &avg);
            for (qi = 0; qi < o->nq; qi++) o->q[qi].alb = avg;
        }
        if (o->lightmap) {
            int l;
            if (!layout_obj(o)) { MessageBoxA(NULL, "Lightmap atlas too large: lower texels_per_unit", "Error", MB_ICONERROR); ExitProcess(1); }
            o->T = (uint32_t*)calloc((size_t)o->aw * o->ah, 4); o->A = (uint32_t*)calloc((size_t)o->aw * o->ah, 4);
            o->Bo = (uint32_t*)calloc((size_t)o->aw * o->ah, 4);
            o->Ca = (uint32_t*)calloc((size_t)o->aw * o->ah, 4);
            for (l = 0; l < MAXLIGHT; l++) o->S[l] = (uint32_t*)calloc((size_t)o->aw * o->ah, 4);
            /* albedo map per texel: used for reflections (the hit only stores the object + the texel) */
            o->alb_px = (unsigned char*)malloc((size_t)o->aw * o->ah * 3);
            if (o->alb_px) {
                memset(o->alb_px, 255, (size_t)o->aw * o->ah * 3);
                for (qi = 0; qi < o->nq; qi++) {
                    Quad* q = &o->q[qi]; DWORD c = col_pack(q->alb); int x, y;
                    unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
                    for (y = q->ty; y < q->ty + q->th + 2 && y < o->ah; y++)
                        for (x = q->tx; x < q->tx + q->tw + 2 && x < o->aw; x++)
                            memcpy(&o->alb_px[((size_t)y * o->aw + x) * 3], rgb, 3);
                }
            }
        }
        build_mesh(o, &border);
        if (!o->glass) gen_lods(o, border);          /* glass keeps its full mesh (per-vertex refraction) */
        free(border);
        if (o->reflect) {
            o->nsamp = (o->rough > 0 && C.reflection_samples > 1) ? C.reflection_samples : 1;
            o->ngbuf = 6 * S * S * o->nsamp; o->gbuf = (unsigned char*)calloc((size_t)o->ngbuf, 4);
        }
    }
    build_tris_bvh();
}


/* ---------- unloading a scene (map change) ---------- */
#define REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)
static void scene_unload(void) {
    int oi, l;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi];
        REL(o->tex_albedo); REL(o->tex_lm); REL(o->tex_cube); REL(o->vb); REL(o->vbf);
        for (l = 0; l < MAXLOD; l++) { REL(o->ib[l]); free(o->idx[l]); o->idx[l] = NULL; }
        free(o->T); free(o->A); free(o->fT); free(o->fA); free(o->fB); free(o->Bo); free(o->Ca); free(o->fCa);
        for (l = 0; l < MAXLIGHT; l++) { free(o->S[l]); free(o->gv[l]); }
        free(o->gbuf); free(o->alb_px); free(o->tex_px); free(o->vert); free(o->vq);
    }
    memset(g_obj, 0, sizeof g_obj);
    g_nobj = 0; g_nbox = 0; g_lamp = -1; g_nlight = 0; g_lsel = 0;
    if (g_rbvh.tri != g_bvh.tri) { free(g_rbvh.tri); free(g_rbvh.bn); }
    free(g_bvh.tri); free(g_bvh.bn);
    memset(&g_bvh, 0, sizeof g_bvh); memset(&g_rbvh, 0, sizeof g_rbvh);
    g_tri = NULL; g_ntri = 0; g_bn = NULL; g_nbn = 0;
}
#undef REL
