/* model.h - real 3D models (.glb) as engine objects.
 *
 * A .glb is converted into one or more Obj (one per distinct material, split every ~40000 vertices because the
 * index buffers are 16-bit). Unlike the quad-based objects of the demo maps, a model has NO lightmap: it is lit
 * PER VERTEX (see modelbake.h): direct light with soft shadows, ambient occlusion and color bounces are baked at
 * every vertex with the same BVH ray tracer, then stored in the vertex color. The model still casts shadows and
 * occludes bounces on the lightmapped objects (it is an occluder in the BVH and in the GPU shadow passes).
 *
 * Axes: glTF is right-handed, +Y up, faces toward +Z. The engine is left-handed. Z is negated and the triangle
 * winding reversed, so a model faces the default camera (which looks toward +Z) as it does in a glTF viewer.
 * The model is scaled so that its largest dimension equals `size`, centered on (pos.x, pos.z) and placed with
 * its lowest point at pos.y.
 *
 * Files are searched in models\<name> then <name>, relative to the current directory.
 */
#pragma once
#include "glb.h"

static int g_model_skipped;                                   /* material groups dropped because the object table is full */
typedef struct { float x, y, z, nx, ny, nz, u, v, r, g, b, a; } MV;      /* working vertex */
typedef struct { MV* v; int nv, cap; uint32_t* idx; int ni, cap_i; } MGroup;

static void mg_reserve(MGroup* g, int nv, int ni) {
    if (g->nv + nv > g->cap) { g->cap = (g->nv + nv) * 2 + 64; g->v = (MV*)realloc(g->v, sizeof(MV) * (size_t)g->cap); }
    if (g->ni + ni > g->cap_i) { g->cap_i = (g->ni + ni) * 2 + 192; g->idx = (uint32_t*)realloc(g->idx, 4 * (size_t)g->cap_i); }
}
static void mg_free(MGroup* g) { free(g->v); free(g->idx); memset(g, 0, sizeof *g); }

static float mv_edge2(const MV* a, const MV* b) { float x = a->x - b->x, y = a->y - b->y, z = a->z - b->z; return x * x + y * y + z * z; }

/* Splits every triangle whose longest edge exceeds max_edge (bisecting the longest edge, repeatedly).
 * Needed because the lighting is per vertex: a big triangle (floor, wall) could not show a shadow.
 * Returns 0 if the vertex budget (16-bit indices) is exceeded. */
static int mg_tessellate(MGroup* g, float max_edge, int vmax) {
    float m2 = max_edge * max_edge; int t;
    if (max_edge <= 0) return g->nv <= vmax;
    for (t = 0; t < g->ni / 3; t++) {
        uint32_t* tr = &g->idx[3 * t]; MV *a = &g->v[tr[0]], *b = &g->v[tr[1]], *c = &g->v[tr[2]];
        float eab = mv_edge2(a, b), ebc = mv_edge2(b, c), eca = mv_edge2(c, a), emax = eab > ebc ? (eab > eca ? eab : eca) : (ebc > eca ? ebc : eca);
        uint32_t i0, i1, i2, ia, ib, mi; MV m; float l;
        if (!(emax > m2)) continue;            /* also skips NaN edges */
        if (g->nv + 1 > vmax) return 0;
        mg_reserve(g, 1, 3);
        tr = &g->idx[3 * t]; a = &g->v[tr[0]]; b = &g->v[tr[1]]; c = &g->v[tr[2]];      /* realloc may have moved the buffers */
        if (eab >= ebc && eab >= eca) { ia = tr[0]; ib = tr[1]; i2 = tr[2]; }
        else if (ebc >= eca)          { ia = tr[1]; ib = tr[2]; i2 = tr[0]; }
        else                          { ia = tr[2]; ib = tr[0]; i2 = tr[1]; }
        i0 = ia; i1 = ib;                                  /* (i0,i1) = longest edge, i2 = opposite corner; winding i0,i1,i2 preserved */
        { const MV *p = &g->v[i0], *q = &g->v[i1];
          m.x = (p->x + q->x) * 0.5f; m.y = (p->y + q->y) * 0.5f; m.z = (p->z + q->z) * 0.5f;
          m.nx = p->nx + q->nx; m.ny = p->ny + q->ny; m.nz = p->nz + q->nz;
          l = sqrtf(m.nx * m.nx + m.ny * m.ny + m.nz * m.nz); if (l > 1e-12f) { m.nx /= l; m.ny /= l; m.nz /= l; } else { m.nx = p->nx; m.ny = p->ny; m.nz = p->nz; }
          m.u = (p->u + q->u) * 0.5f; m.v = (p->v + q->v) * 0.5f;
          m.r = (p->r + q->r) * 0.5f; m.g = (p->g + q->g) * 0.5f; m.b = (p->b + q->b) * 0.5f; m.a = (p->a + q->a) * 0.5f; }
        mi = (uint32_t)g->nv; g->v[g->nv++] = m;
        g->idx[3 * t] = i0; g->idx[3 * t + 1] = mi; g->idx[3 * t + 2] = i2;                 /* (i0, m, i2) */
        g->idx[g->ni] = mi; g->idx[g->ni + 1] = i1; g->idx[g->ni + 2] = i2; g->ni += 3;     /* (m, i1, i2) */
        t--;                                                /* re-examine the shortened triangle */
    }
    return 1;
}

static void tex_halve(uint32_t** px, int* w, int* h) {
    int nw = *w > 1 ? *w / 2 : 1, nh = *h > 1 ? *h / 2 : 1, x, y, c; uint32_t* o = (uint32_t*)malloc((size_t)nw * nh * 4);
    for (y = 0; y < nh; y++) for (x = 0; x < nw; x++) {
        uint32_t r = 0;
        for (c = 0; c < 4; c++) {
            int s = 0, dx, dy;
            for (dy = 0; dy < 2; dy++) for (dx = 0; dx < 2; dx++) {
                int sx = x * 2 + dx, sy = y * 2 + dy; if (sx >= *w) sx = *w - 1; if (sy >= *h) sy = *h - 1;
                s += ((*px)[(size_t)sy * *w + sx] >> (c * 8)) & 255;
            }
            r |= (uint32_t)(s / 4) << (c * 8);
        }
        o[(size_t)y * nw + x] = r;
    }
    free(*px); *px = o; *w = nw; *h = nh;
}

static int model_open(const char* file, GlbModel* m) {
    char p[300]; FILE* f;
    if (strchr(file, ':') || strchr(file, '\\') || strchr(file, '/')) {      /* full / relative path (file dialog, drag and drop) */
        f = fopen(file, "rb"); if (f) { fclose(f); return glb_load(file, m); }
    }
    sprintf(p, "models\\%s", file); f = fopen(p, "rb"); if (f) { fclose(f); return glb_load(p, m); }
    sprintf(p, "%s", file);          f = fopen(p, "rb"); if (f) { fclose(f); return glb_load(p, m); }
    snprintf(g_glb_err, sizeof g_glb_err, "file not found: models\\%s (or %s next to the exe)", file, file);
    return 0;
}

/* Reflectivity of a glTF PBR material = how much of the reflection cubemap replaces the lit surface.
 * Dielectric (plastic, paint): F0 ~ 4 %, a little more when smooth (sheen).  Metal (iron, steel): 90 % when polished, still
 * 65 % when rough (rough metal reflects a BLURRED environment, it does not turn matte like plastic). */
static float pbr_refl(float metal, float rough) {
    float kd, km; metal = clampf(metal, 0, 1); rough = clampf(rough, 0, 1);
    kd = 0.04f + 0.10f * (1.0f - rough); km = 0.92f - 0.35f * rough;
    return kd + (km - kd) * metal;
}

/* builds the Obj of one group of triangles sharing a material */
static void model_emit(MGroup* g, const GlbMat* mt, const char* name, int part, int maxv_tess) {
    Obj* o; int i, nt; float edge = C.model_max_edge, mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    MGroup work; double alb[3] = { 0, 0, 0 }; char nm[32];
    float mavg = mt->metal, ravg = mt->rough, kavg, kmax; int want_refl = 0, layer = 0;
    if (g->nv < 3 || g->ni < 3) return;
    /* ---- PBR layer: average + maximum reflectivity of the material (factors x metallicRoughnessTexture) ---- */
    if (mt->mr && mt->mrw > 0 && mt->mrh > 0) {
        size_t n = (size_t)mt->mrw * mt->mrh, k, cnt = 0; double sg = 0, sb = 0; float km = 0;
        for (k = 0; k < n; k += 61) {
            const unsigned char* q = &mt->mr[k * 4]; float mr_ = mt->metal * q[2] / 255.0f, rr_ = mt->rough * q[1] / 255.0f, kk = pbr_refl(mr_, rr_);
            sg += q[1]; sb += q[2]; cnt++; if (kk > km) km = kk;
        }
        if (cnt) { mavg = mt->metal * (float)(sb / (255.0 * cnt)); ravg = mt->rough * (float)(sg / (255.0 * cnt)); }
        kmax = km;
    } else kmax = pbr_refl(mavg, ravg);
    kavg = pbr_refl(mavg, ravg);
    want_refl = C.model_reflections && mt->alpha_mode != 2 && kmax >= C.model_refl_min;
    if (g_nobj >= MAXOBJ - OBJ_RESERVE) { g_model_skipped++; return; }          /* no blocking box: the count is shown in the window after loading */
    /* tessellation with a retry: if 16-bit indices overflow, the maximum edge is relaxed */
    for (;;) {
        work.v = (MV*)malloc(sizeof(MV) * (size_t)(g->nv + 16)); work.nv = g->nv; work.cap = g->nv + 16;
        work.idx = (uint32_t*)malloc(4 * (size_t)(g->ni + 48)); work.ni = g->ni; work.cap_i = g->ni + 48;
        memcpy(work.v, g->v, sizeof(MV) * (size_t)g->nv); memcpy(work.idx, g->idx, 4 * (size_t)g->ni);
        if (mg_tessellate(&work, edge, maxv_tess)) break;
        mg_free(&work); edge = edge > 0 ? edge * 1.5f : 1.0f;
    }
    if (work.nv > 65535) { mg_free(&work); MessageBoxA(NULL, "A model group exceeds 65535 vertices: skipped", "Model", MB_ICONWARNING); return; }
    nt = work.ni / 3;
    if (part) sprintf(nm, "%.11s#%d", name, part); else sprintf(nm, "%.15s", name);
    o = new_obj(nm, 0, 0, 0, 0, v3(1, 1, 1));
    o->is_model = 1; o->in_refl = 1;
    o->blend = mt->alpha_mode == 2; o->atest = mt->alpha_mode == 1; o->two_sided = mt->double_sided && mt->alpha_mode != 0;
    o->occluder = !o->blend;                      /* glass / light covers do not cast shadows */
    o->emis[0] = mt->emis[0]; o->emis[1] = mt->emis[1]; o->emis[2] = mt->emis[2];
    o->nv = work.nv; o->nt[0] = nt; o->nlod = 1; o->uv_tile = 1.0f;
    o->vert = (Vtx*)malloc(sizeof(Vtx) * (size_t)o->nv); o->vq = (uint16_t*)calloc((size_t)o->nv, 2);
    o->idx[0] = (uint16_t*)malloc((size_t)nt * 6);
    for (i = 0; i < o->nv; i++) {
        const MV* s = &work.v[i]; Vtx* v = &o->vert[i]; V3 c = v3(s->r, s->g, s->b); float k;
        if (!C.gamma_correct) c = v3(powf(clampf(c.x, 0, 1), 1.0f / 2.2f), powf(clampf(c.y, 0, 1), 1.0f / 2.2f), powf(clampf(c.z, 0, 1), 1.0f / 2.2f));
        v->x = s->x; v->y = s->y; v->z = s->z; v->nx = s->nx; v->ny = s->ny; v->nz = s->nz; v->u0 = s->u; v->v0 = s->v; v->u1 = v->v1 = 0;
        v->col = ((DWORD)(clampf(o->blend ? s->a : 1.0f, 0, 1) * 255.0f + 0.5f) << 24) | (DWORD)(col_pack(c) & 0x00FFFFFFu);
        k = 1.0f / (float)o->nv; alb[0] += c.x * k; alb[1] += c.y * k; alb[2] += c.z * k;
        if (s->x < mn[0]) mn[0] = s->x; if (s->x > mx[0]) mx[0] = s->x;
        if (s->y < mn[1]) mn[1] = s->y; if (s->y > mx[1]) mx[1] = s->y;
        if (s->z < mn[2]) mn[2] = s->z; if (s->z > mx[2]) mx[2] = s->z;
    }
    for (i = 0; i < nt * 3; i++) o->idx[0][i] = (uint16_t)work.idx[i];
    o->bs_c = v3((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f);
    { float r = 0; for (i = 0; i < o->nv; i++) { float d = vlen(vsub(v3(o->vert[i].x, o->vert[i].y, o->vert[i].z), o->bs_c)); if (d > r) r = d; } o->bs_r = r; }
    /* texture: RGBA -> 0xAARRGGBB, reduced if larger than model_tex_max / the driver limit */
    if (mt->tex) {
        int w = mt->tw, h = mt->th, lim = C.model_tex_max < g_maxtex ? C.model_tex_max : g_maxtex; uint32_t* px; size_t k, n = (size_t)w * h; double ta[3] = { 0, 0, 0 };
        px = (uint32_t*)malloc(n * 4);
        layer = want_refl && mt->mr && mt->alpha_mode == 0;        /* per-texel reflectivity goes into the alpha channel (opaque materials only) */
        for (k = 0; k < n; k++) {
            const unsigned char* s = &mt->tex[k * 4]; unsigned a8 = s[3];
            if (layer) {
                int x = (int)(k % (size_t)mt->tw), y = (int)(k / (size_t)mt->tw), mx = (int)((long long)x * mt->mrw / mt->tw), my = (int)((long long)y * mt->mrh / mt->th);
                const unsigned char* q = &mt->mr[((size_t)my * mt->mrw + mx) * 4];
                a8 = (unsigned)(pbr_refl(mt->metal * q[2] / 255.0f, mt->rough * q[1] / 255.0f) * 255.0f + 0.5f);
            }
            px[k] = ((uint32_t)a8 << 24) | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
            if (!(k & 63)) { ta[0] += s[0]; ta[1] += s[1]; ta[2] += s[2]; }
        }
        while ((w > lim || h > lim) && w > 1 && h > 1) tex_halve(&px, &w, &h);
        o->tex_px = px; o->mtw = w; o->mth = h; o->textured = 1;
        { double cnt = (double)((n + 63) / 64) * 255.0;                    /* average texture color (sRGB -> linear) for the bounces */
          for (i = 0; i < 3; i++) alb[i] *= C.gamma_correct ? pow(ta[i] / cnt, 2.2) : ta[i] / cnt; }
    }
    o->malb = v3((float)alb[0], (float)alb[1], (float)alb[2]);
    /* ---- reflection layer: cubemap probe + reflectivity ---- */
    o->rough = clampf(ravg, 0, 1); o->refl = kavg; o->lodb = clampf(ravg, 0, 1) * 2.0f;
    if (want_refl) {
        float m = clampf(mavg, 0, 1);
        o->reflect = 1; o->in_refl = 1; o->refl_layer = layer;
        /* metal reflections are tinted by the base color; plastic reflects white */
        o->tint = v3((1 - m) + m * clampf(o->malb.x * 1.6f, 0, 1), (1 - m) + m * clampf(o->malb.y * 1.6f, 0, 1), (1 - m) + m * clampf(o->malb.z * 1.6f, 0, 1));
    } else o->in_refl = !o->blend;      /* matte models still appear in the reflections of the others */
    mg_free(&work);
}

/* Loads `file` and adds it to the scene. Returns the number of objects created (0 = failure, message shown).
 *   size   : largest dimension of the model after scaling (world units)
 *   rot_x  : rotation about the X axis in degrees (glTF axes, before anything else) ; yaw : then about the vertical axis
 *   pos    : x, z = center of the footprint ; y = height of the lowest point */
static int add_glb_model(const char* file, float size, float rot_x_deg, float yaw_deg, V3 pos) {
    GlbModel m; int first = g_nobj, mi, pi, i; float s, cx = cosf(rot_x_deg * PI / 180.0f), sx = sinf(rot_x_deg * PI / 180.0f), cy = cosf(yaw_deg * PI / 180.0f), sy = sinf(yaw_deg * PI / 180.0f);
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f }, ext, off[3];
    unsigned char* done;
    if (!model_open(file, &m)) {
        char msg[400]; sprintf(msg, "Cannot load the model:\n%s", g_glb_err); MessageBoxA(NULL, msg, "Model", MB_ICONWARNING); return 0;
    }
    /* 1) rotated bounds -> scale and offset */
    for (pi = 0; pi < m.nprim; pi++) for (i = 0; i < m.prim[pi].nv; i++) {
        const float* p = &m.prim[pi].pos[3 * i]; float x = p[0], y = p[1] * cx - p[2] * sx, z = p[1] * sx + p[2] * cx, x2 = x * cy + z * sy, z2 = -x * sy + z * cy;
        if (x2 < mn[0]) mn[0] = x2; if (x2 > mx[0]) mx[0] = x2; if (y < mn[1]) mn[1] = y; if (y > mx[1]) mx[1] = y; if (z2 < mn[2]) mn[2] = z2; if (z2 > mx[2]) mx[2] = z2;
    }
    ext = fmaxf(mx[0] - mn[0], fmaxf(mx[1] - mn[1], mx[2] - mn[2])); s = ext > 1e-9f ? size / ext : 1.0f;
    off[0] = -(mn[0] + mx[0]) * 0.5f; off[1] = -mn[1]; off[2] = -(mn[2] + mx[2]) * 0.5f;
    /* 2) one group per distinct material; the materials with identical parameters are merged */
    done = (unsigned char*)calloc((size_t)m.nmat, 1);
    for (mi = 0; mi < m.nmat; mi++) {
        MGroup g; int part = 0, mj; const GlbMat* mt = &m.mat[mi]; char nm[48];
        if (done[mi]) continue;
        memset(&g, 0, sizeof g); strcpy(nm, mt->name[0] ? mt->name : "mat");
        for (mj = mi; mj < m.nmat; mj++) {
            const GlbMat* o2 = &m.mat[mj];
            if (mj != mi && (done[mj] || memcmp(o2->base, mt->base, sizeof mt->base) || memcmp(o2->emis, mt->emis, sizeof mt->emis) || o2->alpha_mode != mt->alpha_mode
                             || o2->tex_id != mt->tex_id || o2->double_sided != mt->double_sided
                             || o2->mr_id != mt->mr_id || fabsf(o2->metal - mt->metal) > 0.02f || fabsf(o2->rough - mt->rough) > 0.02f)) continue;
            done[mj] = 1;
            for (pi = 0; pi < m.nprim; pi++) {
                const GlbPrim* p = &m.prim[pi]; int base, t;
                if (p->mat != mj) continue;
                if (g.nv + p->nv > 40000 && g.nv > 0) { model_emit(&g, mt, nm, part++, 65000); g.nv = g.ni = 0; }
                mg_reserve(&g, p->nv, p->ni); base = g.nv;
                for (i = 0; i < p->nv; i++) {
                    const float* q = &p->pos[3 * i], *nn = &p->nrm[3 * i]; MV* d = &g.v[g.nv++];
                    float x = q[0], y = q[1] * cx - q[2] * sx, z = q[1] * sx + q[2] * cx, x2 = x * cy + z * sy, z2 = -x * sy + z * cy;
                    float nx = nn[0], ny = nn[1] * cx - nn[2] * sx, nz = nn[1] * sx + nn[2] * cx, nx2 = nx * cy + nz * sy, nz2 = -nx * sy + nz * cy;
                    d->x = (x2 + off[0]) * s + pos.x; d->y = (y + off[1]) * s + pos.y; d->z = -((z2 + off[2]) * s) + pos.z;       /* z negated: right-handed -> left-handed */
                    d->nx = nx2; d->ny = ny; d->nz = -nz2;
                    d->u = p->uv[2 * i]; d->v = p->uv[2 * i + 1];
                    d->r = mt->base[0]; d->g = mt->base[1]; d->b = mt->base[2]; d->a = mt->base[3];
                    if (p->col) { d->r *= p->col[4 * i]; d->g *= p->col[4 * i + 1]; d->b *= p->col[4 * i + 2]; d->a *= p->col[4 * i + 3]; }
                }
                for (t = 0; t < p->ni / 3; t++) {                  /* the winding is reversed together with the Z flip */
                    g.idx[g.ni++] = (uint32_t)base + p->idx[3 * t]; g.idx[g.ni++] = (uint32_t)base + p->idx[3 * t + 2]; g.idx[g.ni++] = (uint32_t)base + p->idx[3 * t + 1];
                }
            }
        }
        model_emit(&g, mt, nm, part, 65000); mg_free(&g);
    }
    free(done); glb_free(&m);
    return g_nobj - first;
}
