/* modelbake.h - per-vertex lighting of the 3D models (.glb, see model.h)
 *
 * Same ingredients as the lightmaps, evaluated at the vertices instead of at the texels:
 *   - direct light of each lamp (unit): soft shadows by CPU rays against the whole scene BVH (direct_irr)
 *   - ambient visibility + color bounces (bake_indirect: reads the direct light already baked on the lightmaps)
 * The results are independent of the lamp color / intensity, so the right-click color menu works live:
 * model_apply_light() recomposes the vertex colors without re-tracing anything.
 */
#pragma once

static void models_prepare(void) {
    int oi, l;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int i;
        if (!o->is_model) continue;
        for (l = 0; l < MAXLIGHT; l++) { free(o->ml_S[l]); o->ml_S[l] = l < g_nlight ? (float*)calloc((size_t)o->nv, sizeof(float)) : NULL; }
        free(o->ml_A); free(o->ml_B);
        o->ml_A = (float*)malloc((size_t)o->nv * sizeof(float)); o->ml_B = (float*)calloc((size_t)o->nv * 3, sizeof(float));
        for (i = 0; i < o->nv; i++) o->ml_A[i] = 1.0f;
    }
}

typedef struct { int obj, v0, v1; } MJob;
static MJob* g_mj; static int g_nmj; static volatile LONG g_mjn, g_mjdone;

static void mbake_job(const MJob* j) {
    Obj* o = &g_obj[j->obj]; int v, l;
    for (v = j->v0; v < j->v1 && !g_abort; v++) {
        const Vtx* s = &o->vert[v]; V3 p = v3(s->x, s->y, s->z), n = v3(s->nx, s->ny, s->nz), B, A; uint32_t rs;
        for (l = 0; l < g_nlight; l++) o->ml_S[l][v] = direct_irr(p, n, l, -1.0f, 0);
        rs = ((uint32_t)j->obj * 0x27d4eb2fu) ^ ((uint32_t)v * 73856093u) ^ 0x9E3779B9u; rs |= 1; rnd(&rs); rnd(&rs);
        bake_indirect(p, n, &rs, &B, &A);
        o->ml_B[3 * v] = B.x; o->ml_B[3 * v + 1] = B.y; o->ml_B[3 * v + 2] = B.z; o->ml_A[v] = A.x;
    }
}
static DWORD WINAPI mbake_thread(LPVOID a) {
    (void)a;
    for (;;) {
        LONG j = InterlockedIncrement(&g_mjn) - 1;
        if (j >= g_nmj || g_abort) break;
        mbake_job(&g_mj[j]); InterlockedIncrement(&g_mjdone);
    }
    return 0;
}


/* ---------- smoothing of the indirect layer (AO + bounces) ----------
 * The paths are random per vertex: in the shadows (where only the indirect light remains) that noise shows up as blotches.
 * The vertices are welded by position (UV seams / split groups duplicate them), then A and B are averaged with the neighbours
 * along the mesh edges, weighted by the similarity of the normals so that creases do not leak into each other. */
typedef struct { int x, y, z, v; } WKey;
static int wkey_cmp(const void* a, const void* b) {
    const WKey *p = (const WKey*)a, *q = (const WKey*)b;
    if (p->x != q->x) return p->x < q->x ? -1 : 1; if (p->y != q->y) return p->y < q->y ? -1 : 1; if (p->z != q->z) return p->z < q->z ? -1 : 1;
    return p->v - q->v;
}
static void model_smooth(Obj* o) {
    int nv = o->nv, nt = o->nt[0], i, t, e, pass, nr = 0, passes = C.model_smooth_passes;
    WKey* k; int *rep, *rid, *deg, *off, *adj, *fill; float *A, *A2, *B, *B2;
    if (passes <= 0 || nv < 3 || nt < 1 || !o->ml_A || !o->ml_B) return;
    k = (WKey*)malloc(sizeof(WKey) * (size_t)nv); rep = (int*)malloc(4 * (size_t)nv); rid = (int*)malloc(4 * (size_t)nv);
    for (i = 0; i < nv; i++) { k[i].x = (int)floorf(o->vert[i].x * 2000.0f + 0.5f); k[i].y = (int)floorf(o->vert[i].y * 2000.0f + 0.5f); k[i].z = (int)floorf(o->vert[i].z * 2000.0f + 0.5f); k[i].v = i; }
    qsort(k, (size_t)nv, sizeof(WKey), wkey_cmp);
    for (i = 0; i < nv; i++) {                                   /* rid[v] = welded node of vertex v ; rep[node] = its first vertex */
        if (i == 0 || k[i].x != k[i - 1].x || k[i].y != k[i - 1].y || k[i].z != k[i - 1].z) rep[nr++] = k[i].v;
        rid[k[i].v] = nr - 1;
    }
    free(k);
    deg = (int*)calloc((size_t)nr + 1, 4); off = (int*)calloc((size_t)nr + 2, 4);
    for (t = 0; t < nt; t++) for (e = 0; e < 3; e++) { int a = rid[o->idx[0][3 * t + e]], b = rid[o->idx[0][3 * t + (e + 1) % 3]]; if (a != b) { deg[a]++; deg[b]++; } }
    for (i = 0; i < nr; i++) off[i + 1] = off[i] + deg[i];
    adj = (int*)malloc(4 * (size_t)(off[nr] ? off[nr] : 1)); fill = (int*)calloc((size_t)nr + 1, 4);
    for (t = 0; t < nt; t++) for (e = 0; e < 3; e++) {
        int a = rid[o->idx[0][3 * t + e]], b = rid[o->idx[0][3 * t + (e + 1) % 3]];
        if (a != b) { adj[off[a] + fill[a]++] = b; adj[off[b] + fill[b]++] = a; }
    }
    A = (float*)malloc(4 * (size_t)nr); A2 = (float*)malloc(4 * (size_t)nr); B = (float*)malloc(12 * (size_t)nr); B2 = (float*)malloc(12 * (size_t)nr);
    for (i = 0; i < nr; i++) { int v = rep[i]; A[i] = o->ml_A[v]; B[3 * i] = o->ml_B[3 * v]; B[3 * i + 1] = o->ml_B[3 * v + 1]; B[3 * i + 2] = o->ml_B[3 * v + 2]; }
    for (pass = 0; pass < passes; pass++) {
        for (i = 0; i < nr; i++) {
            const Vtx* vi = &o->vert[rep[i]]; float wsum = 1.0f, a = A[i], b0 = B[3 * i], b1 = B[3 * i + 1], b2 = B[3 * i + 2]; int j;
            for (j = off[i]; j < off[i + 1]; j++) {
                int n = adj[j]; const Vtx* vn = &o->vert[rep[n]];
                float d = vi->nx * vn->nx + vi->ny * vn->ny + vi->nz * vn->nz, w;
                if (d <= 0.2f) continue;                          /* sharp crease: no exchange */
                w = d * d * d;
                a += A[n] * w; b0 += B[3 * n] * w; b1 += B[3 * n + 1] * w; b2 += B[3 * n + 2] * w; wsum += w;
            }
            A2[i] = a / wsum; B2[3 * i] = b0 / wsum; B2[3 * i + 1] = b1 / wsum; B2[3 * i + 2] = b2 / wsum;
        }
        { float* t1 = A; A = A2; A2 = t1; t1 = B; B = B2; B2 = t1; }
    }
    for (i = 0; i < nv; i++) { int r = rid[i]; o->ml_A[i] = A[r]; o->ml_B[3 * i] = B[3 * r]; o->ml_B[3 * i + 1] = B[3 * r + 1]; o->ml_B[3 * i + 2] = B[3 * r + 2]; }
    free(rep); free(rid); free(deg); free(off); free(adj); free(fill); free(A); free(A2); free(B); free(B2);
}

/* called by bake_all_inner after the lightmap passes (the bounces need their direct light, still in memory) */
static void bake_models(void) {
    int oi, n = 0, i, nth, save; HANDLE th[64]; SYSTEM_INFO si; const int CH = 128;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].is_model) n += (g_obj[oi].nv + CH - 1) / CH;
    if (!n) return;
    g_mj = (MJob*)malloc(sizeof(MJob) * (size_t)n); g_nmj = 0; g_mjn = 0; g_mjdone = 0;
    for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].is_model) {
        int v; for (v = 0; v < g_obj[oi].nv; v += CH) { MJob* j = &g_mj[g_nmj++]; j->obj = oi; j->v0 = v; j->v1 = v + CH < g_obj[oi].nv ? v + CH : g_obj[oi].nv; }
    }
    save = C.ao_samples; C.ao_samples = C.model_ao_samples < 4 ? 4 : C.model_ao_samples; gi_init_tables();     /* fewer paths per vertex than per lightmap node */
    GetSystemInfo(&si); nth = C.bake_threads > 0 ? C.bake_threads : (int)si.dwNumberOfProcessors; if (nth < 1) nth = 1; if (nth > 64) nth = 64;
    for (i = 0; i < nth; i++) { th[i] = CreateThread(NULL, 0, mbake_thread, NULL, 0, NULL); if (th[i]) SetThreadPriority(th[i], THREAD_PRIORITY_BELOW_NORMAL); }
    for (;;) {
        DWORD w = WaitForMultipleObjects((DWORD)nth, th, TRUE, (DWORD)(1000 / (C.loading_hz > 0 ? C.loading_hz : 5)));
        loading_frame(0.90f + 0.02f * (float)g_mjdone / (float)g_nmj);
        if (w != WAIT_TIMEOUT) break;
    }
    for (i = 0; i < nth; i++) if (th[i]) CloseHandle(th[i]);
    free(g_mj); g_mj = NULL; g_nmj = 0;
    if (!g_abort) for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].is_model) model_smooth(&g_obj[oi]);      /* removes the per-vertex noise of AO / bounces */
    C.ao_samples = save; gi_init_tables();
}

/* vertex color = albedo x tone-mapped light (the lightmaps do the same through stage 1) */
static void model_apply_light(Obj* o) {
    void* p; Vtx* d; int i, l; V3 amb = v3(C.ambient_r, C.ambient_g, C.ambient_b), c0 = g_nlight ? g_lightset[0].col : v3(0, 0, 0);
    if (!o->vb || !o->ml_A || !o->ml_B || !o->vert) return;
    if (FAILED(IDirect3DVertexBuffer9_Lock(o->vb, 0, 0, &p, 0))) return;
    d = (Vtx*)p;
    double sl[3] = { 0, 0, 0 };
    for (i = 0; i < o->nv; i++) {
        V3 T = { 0, 0, 0 }; float A = o->ml_A[i], lr, lg, lb; DWORD c = o->vert[i].col; float r, g, b;
        for (l = 0; l < g_nlight; l++) if (o->ml_S[l]) { float s = o->ml_S[l][i]; T.x += g_lightset[l].col.x * s; T.y += g_lightset[l].col.y * s; T.z += g_lightset[l].col.z * s; }
        if (g_gi_on) { T.x += c0.x * o->ml_B[3 * i]; T.y += c0.y * o->ml_B[3 * i + 1]; T.z += c0.z * o->ml_B[3 * i + 2]; }
        lr = lut_get(g_lut_lin, (T.x + amb.x * A) * C.exposure, 8.0f, LUTN);
        lg = lut_get(g_lut_lin, (T.y + amb.y * A) * C.exposure, 8.0f, LUTN);
        lb = lut_get(g_lut_lin, (T.z + amb.z * A) * C.exposure, 8.0f, LUTN);
        sl[0] += lr; sl[1] += lg; sl[2] += lb;
        r = ((c >> 16) & 255) * (1.0f / 255.0f) * lr + o->emis[0]; g = ((c >> 8) & 255) * (1.0f / 255.0f) * lg + o->emis[1]; b = (c & 255) * (1.0f / 255.0f) * lb + o->emis[2];
        d[i] = o->vert[i];
        d[i].col = (c & 0xFF000000u) | ((DWORD)(clampf(r, 0, 1) * 255.0f + 0.5f) << 16) | ((DWORD)(clampf(g, 0, 1) * 255.0f + 0.5f) << 8) | (DWORD)(clampf(b, 0, 1) * 255.0f + 0.5f);
    }
    IDirect3DVertexBuffer9_Unlock(o->vb);
    if (o->nv > 0) { float k = 1.0f / (float)o->nv; o->mlit[0] = (float)sl[0] * k; o->mlit[1] = (float)sl[1] * k; o->mlit[2] = (float)sl[2] * k; }   /* what a reflection of this object shows */
}

/* bake cache: appended after the lightmap blocks */
static int model_cache_read(FILE* f) {
    int oi, l;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int hd[2]; size_t nv = (size_t)o->nv;
        if (!o->is_model) continue;
        if (fread(hd, sizeof hd, 1, f) != 1 || hd[0] != o->nv || hd[1] != g_nlight) return 0;
        for (l = 0; l < g_nlight; l++) if (fread(o->ml_S[l], 4, nv, f) != nv) return 0;
        if (fread(o->ml_A, 4, nv, f) != nv || fread(o->ml_B, 4, nv * 3, f) != nv * 3) return 0;
    }
    return 1;
}
static void model_cache_write(FILE* f) {
    int oi, l;
    for (oi = 0; oi < g_nobj; oi++) {
        Obj* o = &g_obj[oi]; int hd[2]; size_t nv = (size_t)o->nv;
        if (!o->is_model) continue;
        hd[0] = o->nv; hd[1] = g_nlight; fwrite(hd, sizeof hd, 1, f);
        for (l = 0; l < g_nlight; l++) fwrite(o->ml_S[l], 4, nv, f);
        fwrite(o->ml_A, 4, nv, f); fwrite(o->ml_B, 4, nv * 3, f);
    }
}
