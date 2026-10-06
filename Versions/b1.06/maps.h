/* maps.h - the 3 technical demo maps (keys 1, 2, 3 on the numpad or top row)
 *
 *   1 : Cornell box + glass sphere + box       -> path tracing (bounces, refraction, caustics)
 *   2 : Cornell box + cube + shiny blue sphere       -> global illumination (color bleeding)
 *   3 : white platform + about a hundred small colored blocks + 1 adjustable lamp (right-click)
 *                                                        -> colored lights, color bounces, many objects
 *
 * Each map defines its SCENE properties in map_config() (lamp position, glass, caustics, ambience...).
 * User settings (quality, display, lamp color / intensity...) are general: same engine.ini for every map.
 * Wall boxes have NO boxdef (box = -1): the "covered face" test would otherwise ignore them at the joints (black lines).
 */
#pragma once

#define NMAPS 3
static int g_map = 0;                        /* 0 = nothing loaded, otherwise 1..NMAPS */
static int g_want_map = 1;                   /* change request (set by wndproc, handled by the main loop) */
static V3 g_room_lo, g_room_hi;              /* room volume: parallax correction of the glass reflections / refractions */
static const char* MAP_NAME[NMAPS + 1] = { "", "Path tracing", "Global illumination", "Colored lights" };

/* ---------- small helpers ---------- */
static uint32_t g_lcg;
static float lcg_f(void) { g_lcg = g_lcg * 1664525u + 1013904223u; return (g_lcg >> 8) * (1.0f / 16777216.0f); }
static void hsv2rgb(float h, float s, float v, int* r, int* g, int* b) {
    float c = v * s, x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1)), m = v - c, rr, gg, bb; int hi = (int)(h * 6.0f) % 6;
    switch (hi) { case 0: rr = c; gg = x; bb = 0; break; case 1: rr = x; gg = c; bb = 0; break; case 2: rr = 0; gg = c; bb = x; break;
                  case 3: rr = 0; gg = x; bb = c; break; case 4: rr = x; gg = 0; bb = c; break; default: rr = c; gg = 0; bb = x; }
    *r = (int)((rr + m) * 255); *g = (int)((gg + m) * 255); *b = (int)((bb + m) * 255);
}

/* slab (wall, floor, ceiling): box without boxdef, 6 faces */
static Obj* wall(const char* name, V3 c, V3 half, V3 alb, int reflect, float refl, float rough) {
    Obj* o = new_obj(name, 1, reflect, refl, rough, v3(1, 1, 1));
    add_faces(o, c, half, 0, 0, alb, col_pack(alb), -1, 1, 0);
    return o;
}

/* Cornell room open on the camera side (-z). Room: x,z in [-5,5], y in [-5,5] (center = orbit camera origin).
 * Walls 0.2 thick placed outside: their inner faces are exactly x/y/z = +-5. */
static void cornell_room(V3 left, V3 right, V3 white, int floor_reflect, float floor_refl, float floor_rough) {
    wall("floor",     v3(0, -5.1f, 0),    v3(5.0f, 0.1f, 5.0f), white, floor_reflect, floor_refl, floor_rough);
    wall("ceiling", v3(0, 5.1f, 0),     v3(5.0f, 0.1f, 5.0f), white, 0, 0, 0);
    wall("back",     v3(0, 0, 5.1f),     v3(5.0f, 5.2f, 0.1f), white, 0, 0, 0);
    wall("left",     v3(-5.1f, 0, 0),    v3(0.1f, 5.2f, 5.2f), left,  0, 0, 0);
    wall("right",    v3(5.1f, 0, 0),     v3(0.1f, 5.2f, 5.2f), right, 0, 0, 0);
    g_room_lo = v3(-5, -5, -5); g_room_hi = v3(5, 5, 5);
}

/* ceiling light: square source (shape = 1) + its visible emissive panel, in world coordinates */
static void ceiling_light(float half) {
    Obj* o;
    g_nlight = 1; g_lsel = 0;
    g_lightset[0].pos = v3(C.light_x, C.light_y, C.light_z);
    g_lightset[0].col = v3(C.light_r * C.light_intensity, C.light_g * C.light_intensity, C.light_b * C.light_intensity);
    g_lightset[0].radius = half; g_lightset[0].shadow = 1; g_lightset[0].fixed = 1; g_lightset[0].shape = 1;
    g_light = g_lightset[0].pos;
    o = new_obj("lamp0", 0, 0, 0, 0, v3(1, 1, 1));
    o->occluder = 0; o->is_lamp = 1; o->light_idx = 0; o->lamp_world = 1; o->in_refl = 1; g_lamp = g_nobj - 1;
    add_faces(o, v3(C.light_x, C.light_y + 0.0125f, C.light_z), v3(half, 0.0125f, half), 0, 0, v3(1, 1, 1), 0xFFFFFFFFu, -1, 1, 0);
}

static void box_on_floor(const char* name, V3 pos, V3 half, float yaw, V3 alb, int reflect, float refl, float rough) {
    Obj* o = new_obj(name, 1, reflect, refl, rough, v3(1, 1, 1));
    add_faces(o, pos, half, yaw, 1, alb, col_pack(alb),
              add_boxdef(pos, v3(cosf(yaw), 0, sinf(yaw)), v3(-sinf(yaw), 0, cosf(yaw)), half), 1, 0);
}

/* ============================== MAP 1: path tracing ============================== */
static void build_map1(void) {
    V3 white = col_lin(215, 215, 215), red = col_lin(190, 28, 28), blue = col_lin(40, 95, 205);
    Obj* o; V3 gc = v3(-2.3f, -1.6f, -0.8f); float gr = 1.65f;
    cornell_room(red, blue, white, 0, 0, 0);
    box_on_floor("boite", v3(1.9f, -5.0f + 2.75f, 1.0f), v3(1.7f, 2.75f, 1.7f), 0.32f, white, 0, 0, 0);
    /* glass sphere, floating: no lightmap (lit by its reflections and refractions), 1 reflection cubemap taken at its center */
    o = new_obj("glass", 0, 1, 1.0f, 0.0f, v3(1, 1, 1));
    o->glass = 1; o->gc = gc; o->gr = gr;
    add_faces(o, gc, v3(gr, gr, gr), 0, 0, v3(0, 0, 0), 0xFFFFFFFFu, -1, 24, gr);
    ceiling_light(C.light_radius);
}

/* ============================== MAP 2: global illumination ============================== */
static void build_map2(void) {
    V3 white = col_lin(215, 215, 215), pink = col_lin(205, 90, 90), green = col_lin(95, 205, 105), azure = col_lin(120, 150, 235);
    Obj* o; V3 sc = v3(2.3f, -5.0f + 1.6f, -0.2f);
    cornell_room(pink, green, white, 1, 0.14f, 0.45f);
    box_on_floor("cube", v3(-1.7f, -5.0f + 1.8f, 0.7f), v3(1.8f, 1.8f, 1.8f), 0.38f, white, 0, 0, 0);
    o = new_obj("sphere", 1, 1, 0.38f, 0.04f, v3(1, 1, 1));         /* shiny blue sphere (sharp reflections) */
    add_faces(o, sc, v3(1.6f, 1.6f, 1.6f), 0, 0, azure, col_pack(azure), -1, 20, 1.6f);
    ceiling_light(C.light_radius);
}

/* ============================== MAP 3: colored lights ============================== */
static void build_map3(void) {
    V3 white = col_lin(232, 232, 232); Obj* o;
    int lot[6] = { -1, -1, -1, -1, -1, -1 };
    int n = C.object_count < 1 ? 1 : (C.object_count > 450 ? 450 : C.object_count), cols, rows, i;
    const float HX = 9.6f, HZ = 6.4f;                                 /* useful half-extent of the 20 x 14 platform */
    float sx, sz;
    o = new_obj("platform", 1, 0, 0, 0, v3(1, 1, 1));               /* white, matte: it reveals the color bounces */
    add_faces(o, v3(0, -0.25f, 0), v3(10, 0.25f, 7), 0, 1, white, col_pack(white),
              add_boxdef(v3(0, -0.25f, 0), v3(1, 0, 0), v3(0, 0, 1), v3(10, 0.25f, 7)), 1, 0);
    g_room_lo = v3(-10, -1, -7); g_room_hi = v3(10, 12, 7);

    g_lcg = (uint32_t)C.block_seed * 2654435761u + 1u;
    cols = (int)(sqrtf((float)n * (HX / HZ)) + 0.999f); if (cols < 1) cols = 1;
    rows = (n + cols - 1) / cols;
    sx = 2 * HX / (float)cols; sz = 2 * HZ / (float)rows;
    for (i = 0; i < n; i++) {
        int gx = i % cols, gz = i / cols, r, g, b, li;
        float cx = -HX + (gx + 0.5f) * sx, cz = -HZ + (gz + 0.5f) * sz, k, hx, hz, hy, yaw = lcg_f() * PI, lim, hue, sat, val, jx, jz;
        V3 pos, a; Obj* lo;
        k = lcg_f() < 0.75f ? 0.13f + 0.12f * lcg_f() : 0.25f + 0.13f * lcg_f();
        hx = k * (0.85f + 0.4f * lcg_f()); hz = k * (0.85f + 0.4f * lcg_f());
        hy = k * (lcg_f() < 0.5f ? (0.9f + 0.2f * lcg_f()) : (1.0f + 2.4f * lcg_f()));    /* cubes (50%) and upright boxes */
        lim = 0.5f * (sx < sz ? sx : sz) - 0.5f * sqrtf(hx * hx + hz * hz) * 1.45f; if (lim < 0) lim = 0;
        jx = (lcg_f() * 2 - 1) * lim; jz = (lcg_f() * 2 - 1) * lim;
        pos = v3(cx + jx, hy, cz + jz);
        hue = lcg_f(); sat = lcg_f() < 0.65f ? 0.75f + 0.25f * lcg_f() : 0.30f + 0.25f * lcg_f();   /* vivid or light */
        val = 0.80f + 0.20f * lcg_f();
        hsv2rgb(hue, sat, val, &r, &g, &b); a = col_lin(r, g, b);
        li = (gx * 3 / cols) + 3 * (gz * 2 / rows);                        /* 6 spatial batches: 1 draw call each, efficient culling */
        if (lot[li] < 0) { char nm[16]; sprintf(nm, "lot%d", li); new_obj(nm, 1, 0, 0, 0, v3(1, 1, 1)); lot[li] = g_nobj - 1; }
        lo = &g_obj[lot[li]];
        if (lo->nq + 6 > MAXQUAD) continue;
        add_faces(lo, pos, v3(hx, hy, hz), yaw, 1, a, col_pack(a),
                  add_boxdef(pos, v3(cosf(yaw), 0, sinf(yaw)), v3(-sinf(yaw), 0, cosf(yaw)), v3(hx, hy, hz)), 1, 0);
    }

    /* a single lamp: color / intensity via right-click, position with arrows + PgUp/PgDn */
    g_nlight = 1; g_lsel = 0;
    g_lightset[0].pos = v3(C.light_x, C.light_y, C.light_z);
    g_lightset[0].col = v3(C.light_r * C.light_intensity, C.light_g * C.light_intensity, C.light_b * C.light_intensity);
    g_lightset[0].radius = C.light_radius; g_lightset[0].shadow = 1; g_lightset[0].fixed = 0; g_lightset[0].shape = 0;
    g_light = g_lightset[0].pos;
    o = new_obj("lamp0", 0, 0, 0, 0, v3(1, 1, 1));
    o->occluder = 0; o->is_lamp = 1; o->light_idx = 0; o->in_refl = 0; g_lamp = g_nobj - 1;
    add_faces(o, v3(0, 0, 0), v3(0.2f, 0.2f, 0.2f), 0, 0, v3(1, 1, 1), 0xFFFFFFFFu, -1, 1, 0);
}

/* ============================== scene properties per map ==============================
 * Only the properties of the SCENE are defined here (SCN_LIST in config.h): lamp position, glass, caustics,
 * ambience, LoD, reflection cubemap size. Everything the user can tune (bake quality, display, lamp color and
 * intensity, exposure...) is general and comes from engine.ini: it is never touched here. */
static void map_config(int id) {
    /* common base: Cornell-type room, matte black surroundings */
    C.light_x = 0; C.light_y = 4.97f; C.light_z = 0; C.light_radius = 0.65f;
    C.ambient_r = C.ambient_g = C.ambient_b = 0.02f;
    C.background_r = C.background_g = C.background_b = 0.0f;
    C.lod_levels = 0; C.reflection_size = 64; C.reflection_samples = 4;
    C.caustics = 0; C.caustic_photons = 1200000; C.caustic_blur = 2; C.glass_ior = 1.5f; C.glass_spec = 1.0f; C.glass_flip_x = 0;
    if (id == 1) {                       /* glass sphere: sharp mirror reflections, caustics */
        C.light_radius = 0.65f; C.reflection_size = 128; C.reflection_samples = 1; C.caustics = 1;
    } else if (id == 2) {
        C.light_radius = 1.0f;
    } else {                             /* map 3: platform + blocks + movable spherical lamp */
        C.light_y = 6.5f; C.light_radius = C.lamp_size;
        C.ambient_r = C.ambient_g = 0.03f; C.ambient_b = 0.035f;
        C.background_r = C.background_g = 0.02f; C.background_b = 0.03f;
        C.object_count = 200; C.block_seed = 1234; C.lod_levels = 2;
    }
}

static void sanitize_cfg(void) {
    if (C.reflection_size < 4) C.reflection_size = 4;
    if (C.reflection_size > g_maxtex) C.reflection_size = g_maxtex;
    if (C.reflection_upscale < 1) C.reflection_upscale = 1;
    if (C.rt_shadows < 0) C.rt_shadows = 0;
    if (C.rt_shadows > 2) C.rt_shadows = 2;
    if (C.rt_shadow_samples < 8) C.rt_shadow_samples = 8;
    if (C.rt_shadow_samples > 1024) C.rt_shadow_samples = 1024;
    if (C.rt_shadow_margin < 0) C.rt_shadow_margin = 0;
    if (C.rt_shadow_margin > 8) C.rt_shadow_margin = 8;
    if (C.caustic_photons > 40000000) C.caustic_photons = 40000000;
    if (C.gamma_correct) C.gamma_correct = 1;
}
