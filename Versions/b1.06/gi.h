/* gi.h - baked global illumination: light bounces N times (bounce_count in engine.ini)
 *
 * Path-based radiosity: for each node of the indirect lighting grid, ao_samples paths start
 * in cosine-weighted directions. At each hit we READ the direct light already computed (pass 1) instead of
 * casting a new shadow ray, and tint it by the surface color (a red cube tints the platform). The path then
 * continues for up to bounce_count bounces. Everything is computed with a unit lamp: color and
 * intensity can still be changed live.
 */
#pragma once

static float* g_hemi; static int g_nhemi;      /* cosine directions (Vogel disk), tangent space */
static void gi_init_tables(void) {
    int i, N = C.ao_samples < 1 ? 1 : C.ao_samples;
    free(g_hemi); g_hemi = (float*)malloc((size_t)N * 3 * sizeof(float)); g_nhemi = N;
    for (i = 0; i < N; i++) {
        float r = sqrtf((i + 0.5f) / N), ph = i * 2.39996323f;
        g_hemi[3 * i] = r * cosf(ph); g_hemi[3 * i + 1] = r * sinf(ph); g_hemi[3 * i + 2] = sqrtf(fmaxf(0, 1 - r * r));
    }
}

/* direct light (unit) already baked at the hit point */
static inline float hit_direct(const Tri* tr, const Hit* h) {
    const Obj* ho = &g_obj[tr->obj]; float w0 = 1 - h->u - h->v;
    int ix = 0, iy = 0;
    if (!ho->fT) return 0.0f;                       /* no lightmap (glass, emitter): nothing to read back */
    ix = (int)(w0 * tr->lu[0] + h->u * tr->lu[1] + h->v * tr->lu[2]); iy = (int)(w0 * tr->lv[0] + h->u * tr->lv[1] + h->v * tr->lv[2]);
    if (ix >= ho->aw) ix = ho->aw - 1;
    if (iy >= ho->ah) iy = ho->ah - 1;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    return ho->fT[((size_t)iy * ho->aw + ix) * 3];
}

static void gi_gather(V3 org, V3 n, uint32_t* rs, V3* bounce, float* vis) {
    int N = g_nhemi, nb = C.bounce_count < 1 ? 1 : C.bounce_count, i, k;
    float maxd = fmaxf(C.ao_distance, C.bounce_distance), v = 0, a0 = rnd(rs) * 2 * PI, ca = cosf(a0), sa = sinf(a0);
    V3 t, b, acc = { 0, 0, 0 };
    basis(n, &t, &b);
    for (i = 0; i < N; i++) {
        float hx = g_hemi[3 * i] * ca - g_hemi[3 * i + 1] * sa, hy = g_hemi[3 * i] * sa + g_hemi[3 * i + 1] * ca;   /* random rotation per node */
        V3 d = vadd(vadd(vmul(t, hx), vmul(b, hy)), vmul(n, g_hemi[3 * i + 2]));
        V3 o = org, tp = { 1, 1, 1 }; Hit h;
        if (!trace(o, d, maxd, &h, -1)) { v += 1; continue; }
        if (h.t >= C.ao_distance) v += 1;                         /* AO: distant hit = no occlusion */
        for (k = 0; k < nb; k++) {
            const Tri* tr = &g_tri[h.tri]; V3 hn = tr->n, hp = vadd(o, vmul(d, h.t)), t2, b2;
            float r3, r4, ph2, rr2;
            if (vdot(hn, d) > 0) hn = vmul(hn, -1);
            acc = vadd(acc, vmulv(tp, vmul(tr->alb, hit_direct(tr, &h))));         /* bounce k+1 */
            tp = vmulv(tp, tr->alb);
            if (k + 1 >= nb || (tp.x + tp.y + tp.z) < 0.01f) break;
            basis(hn, &t2, &b2);                                    /* next direction of the path */
            r3 = rnd(rs); r4 = rnd(rs); ph2 = 2 * PI * r3; rr2 = sqrtf(r4);
            d = vadd(vadd(vmul(t2, rr2 * cosf(ph2)), vmul(b2, rr2 * sinf(ph2))), vmul(hn, sqrtf(1 - r4)));
            o = vadd(hp, vmul(hn, 0.004f));
            if (!trace(o, d, maxd, &h, -1)) break;
        }
    }
    *vis = v / N;
    *bounce = vmul(acc, 1.0f / N);
}
