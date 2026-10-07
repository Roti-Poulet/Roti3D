/* vcache.h - triangle reordering for the GPU post-transform vertex cache (Forsyth's algorithm).
 * Only the ORDER of the triangles changes (same triangles, same vertices): the image is identical, the bake is not affected
 * (Obj.idx is left untouched, the optimized copy lives in Obj.idx_vc). Useful on old GPUs where vertex processing is a bottleneck. */
#pragma once

#define VC_CACHE 32
static float g_vc_cs[VC_CACHE + 1], g_vc_vs[65]; static int g_vc_init;

static void vc_init(void) {
    int i;
    if (g_vc_init) return;
    g_vc_init = 1;
    for (i = 0; i < VC_CACHE; i++) {
        float s;
        if (i < 3) s = 0.75f; else s = powf(1.0f - (float)(i - 3) / (float)(VC_CACHE - 3), 1.5f);
        g_vc_cs[i] = s;
    }
    g_vc_cs[VC_CACHE] = 0.0f;
    g_vc_vs[0] = 0.0f; for (i = 1; i < 65; i++) g_vc_vs[i] = 2.0f / sqrtf((float)i);
}
static inline float vc_score(int pos, int valence) {
    if (valence <= 0) return -1.0f;
    return (pos >= 0 ? g_vc_cs[pos] : 0.0f) + g_vc_vs[valence > 64 ? 64 : valence];
}

/* Average cache Miss Ratio (vertex cache misses per triangle) with a simulated FIFO cache of 'csz' entries. Lower is better (0.5 = ideal on a grid). */
static double vc_acmr(const uint16_t* idx, int nt, int nv, int csz) {
    int* tag = (int*)malloc((size_t)nv * sizeof(int)); int* fifo = (int*)malloc((size_t)csz * sizeof(int)); int head = 0, used = 0, i, k, miss = 0;
    double r;
    if (!tag || !fifo) { free(tag); free(fifo); return 0; }
    for (i = 0; i < nv; i++) tag[i] = 0;                       /* 1 = in cache */
    for (i = 0; i < nt * 3; i++) {
        int v = idx[i];
        if (tag[v]) continue;
        miss++;
        if (used == csz) { tag[fifo[head]] = 0; fifo[head] = v; head = (head + 1) % csz; }
        else { fifo[(head + used) % csz] = v; used++; }
        tag[v] = 1; (void)k;
    }
    r = nt ? (double)miss / (double)nt : 0.0; free(tag); free(fifo); return r;
}

/* out: nt*3 indices (may not alias 'in'). Returns 1 on success, 0 if allocation failed (out is then a plain copy). */
static int vcache_optimize(const uint16_t* in, uint16_t* out, int nt, int nv) {
    int *valence, *vstart, *vtris, *fill, *cpos, *cache, *newc; float *vscore, *tscore; unsigned char* added;
    int t, v, i, j, ncache = 0, nadded, best, k;
    vc_init();
    memcpy(out, in, (size_t)nt * 3 * sizeof(uint16_t));
    if (nt < 4) return 1;
    valence = (int*)calloc((size_t)nv, sizeof(int)); vstart = (int*)calloc((size_t)nv + 1, sizeof(int)); fill = (int*)calloc((size_t)nv, sizeof(int));
    vtris = (int*)malloc((size_t)nt * 3 * sizeof(int)); cpos = (int*)malloc((size_t)nv * sizeof(int));
    cache = (int*)malloc((VC_CACHE + 4) * sizeof(int)); newc = (int*)malloc((VC_CACHE + 4) * sizeof(int));
    vscore = (float*)malloc((size_t)nv * sizeof(float)); tscore = (float*)malloc((size_t)nt * sizeof(float)); added = (unsigned char*)calloc((size_t)nt, 1);
    if (!valence || !vstart || !fill || !vtris || !cpos || !cache || !newc || !vscore || !tscore || !added) {
        free(valence); free(vstart); free(fill); free(vtris); free(cpos); free(cache); free(newc); free(vscore); free(tscore); free(added); return 0;
    }
    for (i = 0; i < nt * 3; i++) valence[in[i]]++;
    for (v = 0; v < nv; v++) vstart[v + 1] = vstart[v] + valence[v];
    for (t = 0; t < nt; t++) for (k = 0; k < 3; k++) { v = in[t * 3 + k]; vtris[vstart[v] + fill[v]++] = t; }
    for (v = 0; v < nv; v++) { cpos[v] = -1; vscore[v] = vc_score(-1, valence[v]); }
    best = 0;
    for (t = 0; t < nt; t++) {
        tscore[t] = vscore[in[t * 3]] + vscore[in[t * 3 + 1]] + vscore[in[t * 3 + 2]];
        if (tscore[t] > tscore[best]) best = t;
    }
    for (nadded = 0; nadded < nt; nadded++) {
        int tv[3], nn = 0;
        if (best < 0 || added[best]) {                         /* dead end: full scan */
            float bs = -1e30f; best = -1;
            for (t = 0; t < nt; t++) if (!added[t] && tscore[t] > bs) { bs = tscore[t]; best = t; }
            if (best < 0) break;
        }
        t = best; added[t] = 1;
        for (k = 0; k < 3; k++) { tv[k] = in[t * 3 + k]; out[nadded * 3 + k] = (uint16_t)tv[k]; valence[tv[k]]--; }
        /* new cache = the 3 vertices of the triangle, then the old entries (without duplicates), truncated */
        for (k = 0; k < 3; k++) { int d = 0; for (j = 0; j < nn; j++) if (newc[j] == tv[k]) d = 1; if (!d) newc[nn++] = tv[k]; }
        for (i = 0; i < ncache && nn < VC_CACHE + 3; i++) { int d = 0; for (j = 0; j < nn; j++) if (newc[j] == cache[i]) d = 1; if (!d) newc[nn++] = cache[i]; }
        for (i = 0; i < ncache; i++) cpos[cache[i]] = -1;      /* everything leaves, then comes back with its new position */
        for (i = 0; i < nn; i++) cpos[newc[i]] = i < VC_CACHE ? i : -1;
        /* rescore: vertices of the old and new cache, then the triangles that touch them */
        for (i = 0; i < ncache; i++) vscore[cache[i]] = vc_score(cpos[cache[i]], valence[cache[i]]);
        for (i = 0; i < nn; i++) vscore[newc[i]] = vc_score(cpos[newc[i]], valence[newc[i]]);
        best = -1; { float bs = -1e30f;
            for (i = 0; i < nn; i++) { v = newc[i];
                for (j = vstart[v]; j < vstart[v + 1]; j++) { int tt = vtris[j]; if (added[tt]) continue;
                    tscore[tt] = vscore[in[tt * 3]] + vscore[in[tt * 3 + 1]] + vscore[in[tt * 3 + 2]];
                    if (tscore[tt] > bs) { bs = tscore[tt]; best = tt; } } }
            for (i = 0; i < ncache; i++) { v = cache[i];            /* vertices that left the cache: their triangles must be rescored too */
                if (cpos[v] >= 0) continue;
                for (j = vstart[v]; j < vstart[v + 1]; j++) { int tt = vtris[j]; if (added[tt]) continue;
                    tscore[tt] = vscore[in[tt * 3]] + vscore[in[tt * 3 + 1]] + vscore[in[tt * 3 + 2]]; } } }
        ncache = nn < VC_CACHE ? nn : VC_CACHE; for (i = 0; i < ncache; i++) cache[i] = newc[i];
    }
    free(valence); free(vstart); free(fill); free(vtris); free(cpos); free(cache); free(newc); free(vscore); free(tscore); free(added);
    return 1;
}
