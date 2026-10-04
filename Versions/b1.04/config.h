/* config.h - engine.ini: X-macro (key name = field name) */
#pragma once

#define CFG_LIST(XI, XF) \
    /* display */ \
    XI(width, 1280) XI(height, 720) XI(fullscreen, 0) XI(vsync, 1) \
    XI(msaa, 0) XI(fxaa, 0) XI(anisotropy, 8) XI(auto_low_end, 1) XI(low_end_vram_mb, 256) XI(gamma_correct, 1) XF(fov, 50) \
    /* bake: quality / cost (computed once, then cached) */ \
    XF(texels_per_unit, 24) XI(lightmap_max_size, 2048) XI(shadow_samples, 64) XI(ao_samples, 32) \
    XI(blur_passes, 1) XI(bake_threads, 0) \
    XF(ao_distance, 4) XF(ao_strength, 0.9) XF(bounce_distance, 10) XF(bounce_strength, 1.5) XI(bounce_count, 3) XI(indirect_step, 3) XI(occluder_lod, 1) XF(exposure, 1.4) \
    /* reflections */ \
    XI(reflection_size, 64) XI(reflection_upscale, 2) XI(reflection_samples, 4) XI(refl_filter, 1) XI(bake_gpu_reflections, 1) \
    XI(reflection_update_interval, 3) XF(reflection_budget_ms, 2.0) XF(reflection_lod_bias, 0.0) \
    /* GPU bake (SM 2.0) */ \
    /* ray-traced shadows (bake) */ \
    XI(rt_shadows, 1) XI(rt_shadow_samples, 64) XI(rt_shadow_margin, 2) \
    XI(bake_gpu, 1) XI(gpu_shadow_res, 256) XI(gpu_taps, 4) XF(gpu_shadow_bias, 1.0) \
    /* cache & loading */ \
    XI(cache_enabled, 1) XI(cache_max_files, 8) XI(loading_hz, 5) \
    /* LoD & culling */ \
    XI(lod_levels, 3) XF(lod_start, 30) XF(lod_step, 25) XF(lod_reduction, 0.5) XI(frustum_culling, 1) XI(depth_prepass, 0) \
    /* overlay distances */ \
    XF(overlay_distance, 90) XF(reflection_distance, 70) XF(far_brightness, 0.55) \
    /* scene */ \
    XF(ambient_r, 0.10) XF(ambient_g, 0.11) XF(ambient_b, 0.14) \
    XF(light_x, -2.0) XF(light_y, 7.0) XF(light_z, 0.5) \
    XF(light_r, 1.0) XF(light_g, 0.95) XF(light_b, 0.85) XF(light_intensity, 120) XF(light_radius, 0.5) \
    XI(light_count, 6) XI(light_shadows, 3) XF(fill_intensity, 0.10) \
    XF(background_r, 0.02) XF(background_g, 0.025) XF(background_b, 0.035) \
    XF(cube_reflect, 0.25) XF(cube_roughness, 0.30) XF(platform_reflect, 0.10) \
    XF(metal_reflect, 0.92) XF(metal_roughness, 0.06) \
    XI(sphere_segments, 12) XI(block_cols, 7) XI(block_rows, 7) XI(block_seed, 1234) \
    XI(object_count, 120) XI(scene_pattern, 0) XI(scene_chunks, 6) XI(textured_count, 14) \
    XI(allow_underside, 0) XF(lamp_speed, 5.0) XF(lamp_rebake_delay, 1.5) \
    /* maps: glass + caustics (photons launched during the bake) */ \
    XI(caustics, 1) XI(caustic_photons, 1200000) XI(caustic_blur, 2) XF(glass_ior, 1.5) XF(glass_spec, 1.4) XI(glass_flip_x, 0)

#define XI_DECL(n, d) int n;
#define XF_DECL(n, d) float n;
typedef struct { CFG_LIST(XI_DECL, XF_DECL) } Config;
static Config C;

static void cfg_defaults(void) {
#define XI_SET(n, d) C.n = d;
#define XF_SET(n, d) C.n = (float)(d);
    CFG_LIST(XI_SET, XF_SET)
}

/* applies "key = value" (key name = field name) */
static void cfg_set(const char* k, const char* v) {
#define XI_GET(n, d) if (!strcmp(k, #n)) C.n = atoi(v);
#define XF_GET(n, d) if (!strcmp(k, #n)) C.n = (float)atof(v);
    CFG_LIST(XI_GET, XF_GET)
#undef XI_GET
#undef XF_GET
}

/* Sections [map1] [map2] [map3]: settings specific to each map, re-applied at every map change
 * (on top of the map's factory settings). Lines before any section are global. */
typedef struct { char k[40]; char v[40]; } CfgKV;
#define CFG_MAPS 4
static CfgKV g_mapkv[CFG_MAPS][64]; static int g_mapkvn[CFG_MAPS];

static void cfg_load(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256]; int sec = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char* c = strpbrk(line, ";#"); char *eq, *k, *e, *v;
        if (c) *c = 0;
        k = line; while (*k == ' ' || *k == '\t') k++;
        if (*k == '[') {                                   /* section header */
            sec = (!strncmp(k + 1, "map", 3) && k[4] >= '1' && k[4] < '0' + CFG_MAPS) ? k[4] - '0' : -1;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; k = line;
        while (*k == ' ' || *k == '\t') k++;
        e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        if (sec == 0) cfg_set(k, v);
        else if (sec > 0 && g_mapkvn[sec] < 64 && strlen(k) < 40 && strlen(v) < 40) {
            strcpy(g_mapkv[sec][g_mapkvn[sec]].k, k); strcpy(g_mapkv[sec][g_mapkvn[sec]].v, v); g_mapkvn[sec]++;
        }
    }
    fclose(f);
}
static void cfg_apply_map(int id) {
    int i;
    if (id < 1 || id >= CFG_MAPS) return;
    for (i = 0; i < g_mapkvn[id]; i++) cfg_set(g_mapkv[id][i].k, g_mapkv[id][i].v);
}
