/* glb.h - minimal glTF 2.0 binary (.glb) loader. Standalone C99: no D3D, no Windows API.
 *
 * Supports: scene graph (node matrix / translation-rotation-scale), meshes made of triangle lists / strips / fans,
 * POSITION / NORMAL / TEXCOORD_0 / COLOR_0, any index type, strided + normalized accessors, PBR metallic-roughness
 * materials (baseColorFactor, baseColorTexture, emissiveFactor, alphaMode, doubleSided) and JPEG / PNG textures
 * embedded in the file (decoded with stb_image.h, included in the same folder: public domain).
 * Not supported (reported in g_glb_err or silently ignored): sparse accessors, skins, animations, morph targets,
 * Draco / meshopt compression, KHR_texture_transform.
 *
 * Output: every primitive is returned in WORLD space (node transforms already applied), still in glTF axes
 * (right-handed, +Y up, -Z forward). Conversion to the engine's axes is done by the caller (model.h).
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "stb_image.h"

typedef struct {
    char name[48];
    float base[4];                 /* baseColorFactor (linear RGBA) */
    float emis[3];                 /* emissiveFactor */
    float metal, rough;
    int alpha_mode;                /* 0 = OPAQUE, 1 = MASK, 2 = BLEND */
    float alpha_cut;
    int double_sided;
    unsigned char* tex;            /* decoded base color texture, RGBA8, top row first (NULL if none) */
    int tw, th;
    int tex_id;                    /* index of the glTF image (materials sharing one image share the pixels) */
    /* PBR layer: metallicRoughnessTexture (G = roughness, B = metallic), RGBA8, may differ in size from the base color texture */
    unsigned char* mr; int mrw, mrh; int mr_id;
} GlbMat;

typedef struct {
    char name[48];
    int nv, ni, mat;
    float* pos;                    /* nv * 3, world space */
    float* nrm;                    /* nv * 3, world space, unit (generated if the file has none) */
    float* uv;                     /* nv * 2 (zeros if none) */
    float* col;                    /* nv * 4 vertex color COLOR_0 (NULL if none) */
    uint32_t* idx;                 /* ni (3 per triangle), counter-clockwise (glTF) */
} GlbPrim;

typedef struct {
    GlbMat* mat; int nmat;
    GlbPrim* prim; int nprim;
    float mn[3], mx[3];            /* world-space bounds of all the vertices */
    unsigned char** imgs; int* imgw; int* imgh; int nimg;   /* decoded images (owned here, materials point into them) */
} GlbModel;

static char g_glb_err[256];

/* ======================= tiny JSON DOM ======================= */
enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ };
typedef struct JV { int t; double n; const char* s; int slen; struct JV* kid; int nk; const char* key; int klen; struct JV* next; } JV;
typedef struct { JV* pool; int used, cap; const char* p; const char* end; int err; } JP;

static JV* jnew(JP* j) {
    if (j->used >= j->cap) { j->err = 1; return NULL; }
    memset(&j->pool[j->used], 0, sizeof(JV)); return &j->pool[j->used++];
}
static void jws(JP* j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }
static JV* jparse(JP* j, int depth) {
    JV* v; jws(j);
    if (j->err || j->p >= j->end || depth > 64) { j->err = 1; return NULL; }
    v = jnew(j); if (!v) return NULL;
    if (*j->p == '{' || *j->p == '[') {
        int obj = *j->p == '{'; JV* last = NULL; j->p++; v->t = obj ? JV_OBJ : JV_ARR;
        jws(j);
        if (j->p < j->end && *j->p == (obj ? '}' : ']')) { j->p++; return v; }
        for (;;) {
            const char* ks = NULL; int kl = 0; JV* c;
            jws(j);
            if (obj) {
                if (j->p >= j->end || *j->p != '"') { j->err = 1; return NULL; }
                j->p++; ks = j->p;
                while (j->p < j->end && *j->p != '"') { if (*j->p == '\\') j->p++; j->p++; }
                kl = (int)(j->p - ks); j->p++; jws(j);
                if (j->p >= j->end || *j->p != ':') { j->err = 1; return NULL; }
                j->p++;
            }
            c = jparse(j, depth + 1); if (!c) return NULL;
            c->key = ks; c->klen = kl;
            if (last) last->next = c; else v->kid = c;
            last = c; v->nk++;
            jws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == (obj ? '}' : ']')) { j->p++; return v; }
            j->err = 1; return NULL;
        }
    }
    if (*j->p == '"') {
        j->p++; v->t = JV_STR; v->s = j->p;
        while (j->p < j->end && *j->p != '"') { if (*j->p == '\\') j->p++; j->p++; }
        v->slen = (int)(j->p - v->s); j->p++; return v;
    }
    if (j->end - j->p >= 4 && !strncmp(j->p, "true", 4)) { v->t = JV_BOOL; v->n = 1; j->p += 4; return v; }
    if (j->end - j->p >= 5 && !strncmp(j->p, "false", 5)) { v->t = JV_BOOL; v->n = 0; j->p += 5; return v; }
    if (j->end - j->p >= 4 && !strncmp(j->p, "null", 4)) { v->t = JV_NULL; j->p += 4; return v; }
    {   char buf[40]; int n = 0;
        while (j->p < j->end && n < 39 && (strchr("+-0123456789.eE", *j->p))) buf[n++] = *j->p++;
        if (!n) { j->err = 1; return NULL; }
        buf[n] = 0; v->t = JV_NUM; v->n = atof(buf); return v;
    }
}
static const JV* jget(const JV* o, const char* key) {
    const JV* c; int kl = (int)strlen(key);
    if (!o || o->t != JV_OBJ) return NULL;
    for (c = o->kid; c; c = c->next) if (c->klen == kl && !strncmp(c->key, key, (size_t)kl)) return c;
    return NULL;
}
static const JV* jat(const JV* a, int i) {
    const JV* c; if (!a || a->t != JV_ARR || i < 0 || i >= a->nk) return NULL;
    for (c = a->kid; i > 0 && c; i--) c = c->next;
    return c;
}
static double jnum(const JV* v, double def) { return (v && (v->t == JV_NUM || v->t == JV_BOOL)) ? v->n : def; }
static int jcount(const JV* v) { return v ? v->nk : 0; }
static void jstr(const JV* v, char* out, int cap) {
    int n = (v && v->t == JV_STR) ? v->slen : 0; if (n > cap - 1) n = cap - 1;
    if (n > 0) memcpy(out, v->s, (size_t)n);
    out[n] = 0;
}

/* ======================= 4x4 matrices (column-major, as in glTF) ======================= */
typedef struct { double m[16]; } GM4;
static GM4 gm_ident(void) { GM4 r; memset(&r, 0, sizeof r); r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static GM4 gm_mul(GM4 a, GM4 b) {                     /* a * b */
    GM4 r; int i, j, k;
    for (j = 0; j < 4; j++) for (i = 0; i < 4; i++) {
        double s = 0; for (k = 0; k < 4; k++) s += a.m[k * 4 + i] * b.m[j * 4 + k];
        r.m[j * 4 + i] = s;
    }
    return r;
}
static GM4 gm_trs(const double* t, const double* q, const double* s) {
    double x = q[0], y = q[1], z = q[2], w = q[3], n = sqrt(x * x + y * y + z * z + w * w); GM4 r = gm_ident();
    if (n > 0) { x /= n; y /= n; z /= n; w /= n; } else { x = y = z = 0; w = 1; }
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = (2 * (x * y + z * w)) * s[0];     r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1];     r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2];     r.m[9] = (2 * (y * z - x * w)) * s[2];     r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}
static double gm_det3(const GM4* a) {
    const double* m = a->m;
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

/* ======================= accessors ======================= */
typedef struct {
    const JV* root; const unsigned char* bin; size_t binlen;
} GCtx;

/* reads accessor `ai` as floats (ncomp per element); unsigned ints are returned as-is (not normalized) unless the accessor says so */
static int glb_read_acc(const GCtx* g, int ai, float* out, int ncomp, int* count_out) {
    const JV* a = jat(jget(g->root, "accessors"), ai); const JV* bv; int count, ct, type_n, bi, i, k;
    size_t off, stride, csz; int norm; const char* ty; char tbuf[16];
    if (!a) return 0;
    count = (int)jnum(jget(a, "count"), 0); ct = (int)jnum(jget(a, "componentType"), 0); norm = (int)jnum(jget(a, "normalized"), 0);
    jstr(jget(a, "type"), tbuf, 16); ty = tbuf;
    type_n = !strcmp(ty, "SCALAR") ? 1 : !strcmp(ty, "VEC2") ? 2 : !strcmp(ty, "VEC3") ? 3 : !strcmp(ty, "VEC4") ? 4 : 0;
    if (!type_n || count <= 0) return 0;
    csz = (ct == 5120 || ct == 5121) ? 1 : (ct == 5122 || ct == 5123) ? 2 : (ct == 5125 || ct == 5126) ? 4 : 0;
    if (!csz) return 0;
    if (count_out) *count_out = count;
    if (!out) return 1;
    if (jget(a, "sparse")) snprintf(g_glb_err, sizeof g_glb_err, "sparse accessors are not supported (ignored)");
    bi = (int)jnum(jget(a, "bufferView"), -1);
    memset(out, 0, sizeof(float) * (size_t)count * (size_t)ncomp);
    if (bi < 0) return 1;
    bv = jat(jget(g->root, "bufferViews"), bi); if (!bv) return 0;
    off = (size_t)jnum(jget(bv, "byteOffset"), 0) + (size_t)jnum(jget(a, "byteOffset"), 0);
    stride = (size_t)jnum(jget(bv, "byteStride"), 0); if (!stride) stride = csz * (size_t)type_n;
    if (off + stride * (size_t)(count - 1) + csz * (size_t)type_n > g->binlen) return 0;
    for (i = 0; i < count; i++) {
        const unsigned char* p = g->bin + off + stride * (size_t)i;
        for (k = 0; k < type_n && k < ncomp; k++) {
            float v;
            switch (ct) {
            case 5120: { signed char c = (signed char)p[k]; v = norm ? (c / 127.0f < -1 ? -1 : c / 127.0f) : (float)c; } break;
            case 5121: v = norm ? p[k] / 255.0f : (float)p[k]; break;
            case 5122: { int16_t c; memcpy(&c, p + k * 2, 2); v = norm ? (c / 32767.0f < -1 ? -1 : c / 32767.0f) : (float)c; } break;
            case 5123: { uint16_t c; memcpy(&c, p + k * 2, 2); v = norm ? c / 65535.0f : (float)c; } break;
            case 5125: { uint32_t c; memcpy(&c, p + k * 4, 4); v = (float)c; } break;
            default:   memcpy(&v, p + k * 4, 4); break;
            }
            out[(size_t)i * ncomp + k] = v;
        }
    }
    return 1;
}
/* indices keep full 32-bit precision (a float would lose it above 16M, but also be slower) */
static uint32_t* glb_read_idx(const GCtx* g, int ai, int* n_out) {
    const JV* a = jat(jget(g->root, "accessors"), ai); const JV* bv; int count, ct, bi, i; size_t off, stride, csz; uint32_t* r;
    if (!a) return NULL;
    count = (int)jnum(jget(a, "count"), 0); ct = (int)jnum(jget(a, "componentType"), 0); bi = (int)jnum(jget(a, "bufferView"), -1);
    csz = ct == 5121 ? 1 : ct == 5123 ? 2 : ct == 5125 ? 4 : 0;
    if (!csz || count <= 0 || bi < 0) return NULL;
    bv = jat(jget(g->root, "bufferViews"), bi); if (!bv) return NULL;
    off = (size_t)jnum(jget(bv, "byteOffset"), 0) + (size_t)jnum(jget(a, "byteOffset"), 0);
    stride = (size_t)jnum(jget(bv, "byteStride"), 0); if (!stride) stride = csz;
    if (off + stride * (size_t)(count - 1) + csz > g->binlen) return NULL;
    r = (uint32_t*)malloc((size_t)count * 4); if (!r) return NULL;
    for (i = 0; i < count; i++) {
        const unsigned char* p = g->bin + off + stride * (size_t)i;
        if (csz == 1) r[i] = p[0]; else if (csz == 2) { uint16_t c; memcpy(&c, p, 2); r[i] = c; } else memcpy(&r[i], p, 4);
    }
    *n_out = count; return r;
}

/* ======================= model assembly ======================= */
static GlbPrim* glb_new_prim(GlbModel* m, int* cap) {
    if (m->nprim >= *cap) { *cap = *cap ? *cap * 2 : 16; m->prim = (GlbPrim*)realloc(m->prim, sizeof(GlbPrim) * (size_t)*cap); }
    memset(&m->prim[m->nprim], 0, sizeof(GlbPrim)); return &m->prim[m->nprim++];
}

static void glb_add_mesh(const GCtx* g, GlbModel* m, int* cap, int mesh_i, const GM4* W, const char* node_name) {
    const JV* mesh = jat(jget(g->root, "meshes"), mesh_i); const JV* prims = jget(mesh, "primitives"); int pi, np = jcount(prims);
    double det = gm_det3(W); int flip = det < 0;
    /* normal matrix = inverse-transpose of the 3x3 part (cofactors / det) */
    double nm[9]; const double* w = W->m; double id = fabs(det) > 1e-30 ? 1.0 / det : 1.0;
    nm[0] = (w[5] * w[10] - w[9] * w[6]) * id;  nm[1] = (w[9] * w[2] - w[1] * w[10]) * id;  nm[2] = (w[1] * w[6] - w[5] * w[2]) * id;
    nm[3] = (w[8] * w[6] - w[4] * w[10]) * id;  nm[4] = (w[0] * w[10] - w[8] * w[2]) * id;  nm[5] = (w[4] * w[2] - w[0] * w[6]) * id;
    nm[6] = (w[4] * w[9] - w[8] * w[5]) * id;   nm[7] = (w[8] * w[1] - w[0] * w[9]) * id;   nm[8] = (w[0] * w[5] - w[4] * w[1]) * id;
    /* nm = cofactor matrix / det = inverse transpose, row-major: n'_i = nm[3i] x + nm[3i+1] y + nm[3i+2] z */
    for (pi = 0; pi < np; pi++) {
        const JV* p = jat(prims, pi); const JV* at = jget(p, "attributes"); int mode = (int)jnum(jget(p, "mode"), 4);
        int pa = (int)jnum(jget(at, "POSITION"), -1), na = (int)jnum(jget(at, "NORMAL"), -1), ta = (int)jnum(jget(at, "TEXCOORD_0"), -1), ca = (int)jnum(jget(at, "COLOR_0"), -1);
        int nv = 0, ni = 0, i; float* tp; uint32_t* ix = NULL; uint32_t* tri; int ntri; GlbPrim* o; char nm_[48];
        if (mode < 4 || mode > 6) { snprintf(g_glb_err, sizeof g_glb_err, "primitive mode %d ignored (only triangles)", mode); continue; }
        if (jget(jget(p, "extensions"), "KHR_draco_mesh_compression")) { snprintf(g_glb_err, sizeof g_glb_err, "Draco compression is not supported"); continue; }
        if (pa < 0 || !glb_read_acc(g, pa, NULL, 3, &nv) || nv < 3) continue;
        tp = (float*)malloc(sizeof(float) * 3 * (size_t)nv); if (!tp) continue;
        if (!glb_read_acc(g, pa, tp, 3, &nv)) { free(tp); continue; }
        if (jget(p, "indices")) ix = glb_read_idx(g, (int)jnum(jget(p, "indices"), -1), &ni);
        if (!ix) { ni = nv; ix = (uint32_t*)malloc((size_t)ni * 4); for (i = 0; i < ni; i++) ix[i] = (uint32_t)i; }
        /* triangle list from list / strip / fan */
        if (mode == 4) { ntri = ni / 3; tri = (uint32_t*)malloc((size_t)(ntri ? ntri : 1) * 12); memcpy(tri, ix, (size_t)ntri * 12); }
        else {
            ntri = ni >= 3 ? ni - 2 : 0; tri = (uint32_t*)malloc((size_t)(ntri ? ntri : 1) * 12);
            for (i = 0; i < ntri; i++) {
                if (mode == 5) { tri[3 * i] = ix[i]; tri[3 * i + 1] = ix[i + 1 + (i & 1)]; tri[3 * i + 2] = ix[i + 2 - (i & 1)]; }
                else { tri[3 * i] = ix[0]; tri[3 * i + 1] = ix[i + 1]; tri[3 * i + 2] = ix[i + 2]; }
            }
        }
        free(ix);
        for (i = 0; i < ntri * 3; i++) if (tri[i] >= (uint32_t)nv) tri[i] = 0;
        o = glb_new_prim(m, cap);
        snprintf(nm_, sizeof nm_, "%s", node_name && node_name[0] ? node_name : "mesh"); strcpy(o->name, nm_);
        o->nv = nv; o->ni = ntri * 3; o->mat = (int)jnum(jget(p, "material"), -1); o->idx = tri;
        o->pos = (float*)malloc(sizeof(float) * 3 * (size_t)nv); o->nrm = (float*)calloc((size_t)nv * 3, sizeof(float)); o->uv = (float*)calloc((size_t)nv * 2, sizeof(float));
        for (i = 0; i < nv; i++) {
            double x = tp[3 * i], y = tp[3 * i + 1], z = tp[3 * i + 2];
            float wx = (float)(w[0] * x + w[4] * y + w[8] * z + w[12]), wy = (float)(w[1] * x + w[5] * y + w[9] * z + w[13]), wz = (float)(w[2] * x + w[6] * y + w[10] * z + w[14]);
            o->pos[3 * i] = wx; o->pos[3 * i + 1] = wy; o->pos[3 * i + 2] = wz;
            if (wx < m->mn[0]) m->mn[0] = wx; if (wx > m->mx[0]) m->mx[0] = wx;
            if (wy < m->mn[1]) m->mn[1] = wy; if (wy > m->mx[1]) m->mx[1] = wy;
            if (wz < m->mn[2]) m->mn[2] = wz; if (wz > m->mx[2]) m->mx[2] = wz;
        }
        free(tp);
        if (flip) for (i = 0; i < ntri; i++) { uint32_t t = o->idx[3 * i + 1]; o->idx[3 * i + 1] = o->idx[3 * i + 2]; o->idx[3 * i + 2] = t; }   /* mirrored node: keep the faces outward */
        if (na >= 0) {
            float* n = (float*)malloc(sizeof(float) * 3 * (size_t)nv);
            if (n && glb_read_acc(g, na, n, 3, &i) && i == nv) {
                for (i = 0; i < nv; i++) {
                    double x = n[3 * i], y = n[3 * i + 1], z = n[3 * i + 2];
                    double a = nm[0] * x + nm[1] * y + nm[2] * z, b = nm[3] * x + nm[4] * y + nm[5] * z, c = nm[6] * x + nm[7] * y + nm[8] * z, l = sqrt(a * a + b * b + c * c);
                    if (l > 1e-12) { a /= l; b /= l; c /= l; }
                    o->nrm[3 * i] = (float)a; o->nrm[3 * i + 1] = (float)b; o->nrm[3 * i + 2] = (float)c;
                }
            } else na = -1;
            free(n);
        }
        if (na < 0) {                                   /* smooth normals generated from the faces (area-weighted) */
            for (i = 0; i < ntri; i++) {
                const float *a = &o->pos[3 * o->idx[3 * i]], *b = &o->pos[3 * o->idx[3 * i + 1]], *c = &o->pos[3 * o->idx[3 * i + 2]];
                float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] }; int k, q;
                for (k = 0; k < 3; k++) for (q = 0; q < 3; q++) o->nrm[3 * o->idx[3 * i + k] + q] += n[q];
            }
            for (i = 0; i < nv; i++) {
                float* n = &o->nrm[3 * i]; float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 1e-20f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[0] = 0; n[1] = 1; n[2] = 0; }
            }
        }
        if (ta >= 0) { int c2 = 0; if (!glb_read_acc(g, ta, o->uv, 2, &c2) || c2 != nv) memset(o->uv, 0, sizeof(float) * 2 * (size_t)nv); }
        if (ca >= 0) {
            const JV* acc = jat(jget(g->root, "accessors"), ca); char tb[16]; int comps; float* tmp;
            jstr(jget(acc, "type"), tb, 16); comps = !strcmp(tb, "VEC3") ? 3 : 4;
            tmp = (float*)malloc(sizeof(float) * 4 * (size_t)nv);
            if (tmp) {
                int c2 = 0; float* raw = (float*)calloc((size_t)nv * 4, sizeof(float));
                if (raw && glb_read_acc(g, ca, raw, comps, &c2) && c2 == nv) {
                    for (i = 0; i < nv; i++) { tmp[4 * i] = raw[comps * i]; tmp[4 * i + 1] = raw[comps * i + 1]; tmp[4 * i + 2] = raw[comps * i + 2]; tmp[4 * i + 3] = comps == 4 ? raw[4 * i + 3] : 1.0f; }
                    o->col = tmp; tmp = NULL;
                }
                free(raw); free(tmp);
            }
        }
    }
}

static void glb_walk(const GCtx* g, GlbModel* m, int* cap, int ni, GM4 parent, int depth) {
    const JV* n = jat(jget(g->root, "nodes"), ni); GM4 loc, W; double t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 }; int k; const JV* v; char nm[48];
    if (!n || depth > 64) return;
    if ((v = jget(n, "matrix")) && jcount(v) == 16) { for (k = 0; k < 16; k++) loc.m[k] = jnum(jat(v, k), 0); }
    else {
        if ((v = jget(n, "translation"))) for (k = 0; k < 3; k++) t[k] = jnum(jat(v, k), 0);
        if ((v = jget(n, "rotation")))    for (k = 0; k < 4; k++) q[k] = jnum(jat(v, k), k == 3 ? 1 : 0);
        if ((v = jget(n, "scale")))       for (k = 0; k < 3; k++) s[k] = jnum(jat(v, k), 1);
        loc = gm_trs(t, q, s);
    }
    W = gm_mul(parent, loc);
    jstr(jget(n, "name"), nm, 48);
    if (jget(n, "mesh")) glb_add_mesh(g, m, cap, (int)jnum(jget(n, "mesh"), 0), &W, nm);
    if ((v = jget(n, "children"))) for (k = 0; k < jcount(v); k++) glb_walk(g, m, cap, (int)jnum(jat(v, k), 0), W, depth + 1);
}

/* decodes image `ii` (bufferView or external file), cached per image index */
static int glb_image(const GCtx* g, GlbModel* m, int ii, const char* dir) {
    const JV* im = jat(jget(g->root, "images"), ii); int w, h, n; unsigned char* px = NULL; const unsigned char* src = NULL; size_t len = 0; unsigned char* filebuf = NULL;
    if (!im || ii < 0 || ii >= m->nimg) return -1;
    if (m->imgs[ii]) return ii;
    if (jget(im, "bufferView")) {
        const JV* bv = jat(jget(g->root, "bufferViews"), (int)jnum(jget(im, "bufferView"), -1));
        size_t off = (size_t)jnum(jget(bv, "byteOffset"), 0); len = (size_t)jnum(jget(bv, "byteLength"), 0);
        if (!bv || off + len > g->binlen) return -1;
        src = g->bin + off;
    } else if (jget(im, "uri")) {
        char uri[256], path[512]; FILE* f; long sz;
        jstr(jget(im, "uri"), uri, 256);
        if (!strncmp(uri, "data:", 5)) { snprintf(g_glb_err, sizeof g_glb_err, "image with an inline data: URI not supported"); return -1; }
        snprintf(path, sizeof path, "%s%s", dir, uri);
        f = fopen(path, "rb"); if (!f) { snprintf(g_glb_err, sizeof g_glb_err, "image file not found: %s", uri); return -1; }
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        filebuf = (unsigned char*)malloc((size_t)sz); if (!filebuf || fread(filebuf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(filebuf); return -1; }
        fclose(f); src = filebuf; len = (size_t)sz;
    } else return -1;
    px = stbi_load_from_memory(src, (int)len, &w, &h, &n, 4);
    free(filebuf);
    if (!px) return -1;
    m->imgs[ii] = px; m->imgw[ii] = w; m->imgh[ii] = h;
    return ii;
}

static void glb_free(GlbModel* m) {
    int i;
    for (i = 0; i < m->nprim; i++) { free(m->prim[i].pos); free(m->prim[i].nrm); free(m->prim[i].uv); free(m->prim[i].col); free(m->prim[i].idx); }
    for (i = 0; i < m->nimg; i++) if (m->imgs[i]) stbi_image_free(m->imgs[i]);
    free(m->prim); free(m->mat); free(m->imgs); free(m->imgw); free(m->imgh);
    memset(m, 0, sizeof *m);
}

/* returns 1 if OK. Error text in g_glb_err. */
static int glb_load(const char* path, GlbModel* m) {
    FILE* f; long sz; unsigned char* d; uint32_t magic, ver, len, jl, jt; const unsigned char* bin = NULL; size_t binlen = 0;
    JP jp; JV* root; GCtx g; int i, cap = 0, sc; char dir[512]; const char* sl;
    memset(m, 0, sizeof *m); g_glb_err[0] = 0;
    m->mn[0] = m->mn[1] = m->mn[2] = 1e30f; m->mx[0] = m->mx[1] = m->mx[2] = -1e30f;
    f = fopen(path, "rb"); if (!f) { snprintf(g_glb_err, sizeof g_glb_err, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 28) { fclose(f); snprintf(g_glb_err, sizeof g_glb_err, "%s: file too small", path); return 0; }
    d = (unsigned char*)malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); snprintf(g_glb_err, sizeof g_glb_err, "%s: read error", path); return 0; }
    fclose(f);
    memcpy(&magic, d, 4); memcpy(&ver, d + 4, 4); memcpy(&len, d + 8, 4); memcpy(&jl, d + 12, 4); memcpy(&jt, d + 16, 4);
    if (magic != 0x46546C67u || ver != 2 || jt != 0x4E4F534Au || 20 + (size_t)jl > (size_t)sz) { free(d); snprintf(g_glb_err, sizeof g_glb_err, "%s: not a valid .glb (glTF 2.0)", path); return 0; }
    if (20 + (size_t)jl + 8 <= (size_t)sz) {                      /* BIN chunk */
        uint32_t bl, bt; memcpy(&bl, d + 20 + jl, 4); memcpy(&bt, d + 24 + jl, 4);
        if (bt == 0x004E4942u && 28 + (size_t)jl + bl <= (size_t)sz) { bin = d + 28 + jl; binlen = bl; }
    }
    (void)len;
    jp.cap = (int)(jl / 2 + 64); jp.pool = (JV*)malloc(sizeof(JV) * (size_t)jp.cap); jp.used = 0; jp.p = (const char*)d + 20; jp.end = jp.p + jl; jp.err = 0;
    root = jp.pool ? jparse(&jp, 0) : NULL;
    if (!root || jp.err || root->t != JV_OBJ) { free(jp.pool); free(d); snprintf(g_glb_err, sizeof g_glb_err, "%s: invalid JSON", path); return 0; }
    {   const JV* req = jget(root, "extensionsRequired");
        if (jcount(req)) { char e[64]; jstr(jat(req, 0), e, 64); snprintf(g_glb_err, sizeof g_glb_err, "%s: required extension not supported (%s)", path, e); free(jp.pool); free(d); return 0; } }
    g.root = root; g.bin = bin; g.binlen = binlen;
    sl = strrchr(path, '/'); { const char* sl2 = strrchr(path, '\\'); if (sl2 > sl) sl = sl2; }
    if (sl) { size_t n = (size_t)(sl - path) + 1; if (n > 510) n = 510; memcpy(dir, path, n); dir[n] = 0; } else dir[0] = 0;

    /* images + materials */
    m->nimg = jcount(jget(root, "images"));
    m->imgs = (unsigned char**)calloc((size_t)(m->nimg ? m->nimg : 1), sizeof(void*)); m->imgw = (int*)calloc((size_t)(m->nimg ? m->nimg : 1), sizeof(int)); m->imgh = (int*)calloc((size_t)(m->nimg ? m->nimg : 1), sizeof(int));
    m->nmat = jcount(jget(root, "materials")) + 1;                /* +1: default material (index nmat-1) for primitives without one */
    m->mat = (GlbMat*)calloc((size_t)m->nmat, sizeof(GlbMat));
    for (i = 0; i < m->nmat; i++) {
        GlbMat* mt = &m->mat[i]; const JV* jm = jat(jget(root, "materials"), i), *pbr = jget(jm, "pbrMetallicRoughness"), *v; int k; char am[16];
        mt->base[0] = mt->base[1] = mt->base[2] = mt->base[3] = 1.0f; mt->metal = 1.0f; mt->rough = 1.0f; mt->alpha_cut = 0.5f; mt->tex_id = -1; mt->mr_id = -1;
        if (!jm) { strcpy(mt->name, "default"); mt->metal = 0.0f; continue; }
        jstr(jget(jm, "name"), mt->name, 48);
        if ((v = jget(pbr, "baseColorFactor"))) for (k = 0; k < 4; k++) mt->base[k] = (float)jnum(jat(v, k), 1);
        mt->metal = (float)jnum(jget(pbr, "metallicFactor"), pbr ? 1 : 0); mt->rough = (float)jnum(jget(pbr, "roughnessFactor"), 1);
        if ((v = jget(jm, "emissiveFactor"))) for (k = 0; k < 3; k++) mt->emis[k] = (float)jnum(jat(v, k), 0);
        jstr(jget(jm, "alphaMode"), am, 16);
        mt->alpha_mode = !strcmp(am, "MASK") ? 1 : !strcmp(am, "BLEND") ? 2 : 0;
        mt->alpha_cut = (float)jnum(jget(jm, "alphaCutoff"), 0.5);
        mt->double_sided = (int)jnum(jget(jm, "doubleSided"), 0);
        if ((v = jget(jget(pbr, "baseColorTexture"), "index"))) {
            const JV* tx = jat(jget(root, "textures"), (int)jnum(v, -1)); int src = (int)jnum(jget(tx, "source"), -1);
            if (src >= 0 && glb_image(&g, m, src, dir) >= 0) { mt->tex = m->imgs[src]; mt->tw = m->imgw[src]; mt->th = m->imgh[src]; mt->tex_id = src; }
        }
        if ((v = jget(jget(pbr, "metallicRoughnessTexture"), "index"))) {      /* PBR layer: roughness (G) + metallic (B) per texel */
            const JV* tx = jat(jget(root, "textures"), (int)jnum(v, -1)); int src = (int)jnum(jget(tx, "source"), -1);
            if (src >= 0 && glb_image(&g, m, src, dir) >= 0) { mt->mr = m->imgs[src]; mt->mrw = m->imgw[src]; mt->mrh = m->imgh[src]; mt->mr_id = src; }
        }
    }
    /* meshes through the scene graph */
    sc = (int)jnum(jget(root, "scene"), 0);
    {   const JV* scn = jat(jget(root, "scenes"), sc); const JV* roots = jget(scn, "nodes"); int k;
        if (scn && jcount(roots)) for (k = 0; k < jcount(roots); k++) glb_walk(&g, m, &cap, (int)jnum(jat(roots, k), 0), gm_ident(), 0);
        else {                                                       /* no scene: all the nodes that nobody references as a child */
            int nn = jcount(jget(root, "nodes")); unsigned char* isch = (unsigned char*)calloc((size_t)(nn ? nn : 1), 1);
            for (k = 0; k < nn; k++) { const JV* ch = jget(jat(jget(root, "nodes"), k), "children"); int c; for (c = 0; c < jcount(ch); c++) { int ci = (int)jnum(jat(ch, c), -1); if (ci >= 0 && ci < nn) isch[ci] = 1; } }
            for (k = 0; k < nn; k++) if (!isch[k]) glb_walk(&g, m, &cap, k, gm_ident(), 0);
            free(isch);
        }
    }
    for (i = 0; i < m->nprim; i++) {                                  /* primitive without material -> default */
        if (m->prim[i].mat < 0 || m->prim[i].mat >= m->nmat - 1) m->prim[i].mat = m->nmat - 1;
    }
    free(jp.pool); free(d);
    if (!m->nprim) { snprintf(g_glb_err, sizeof g_glb_err, "%s: no triangle mesh found", path); glb_free(m); return 0; }
    return 1;
}
