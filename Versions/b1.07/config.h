/* config.h - engine configuration.
 *
 *   CFG_LIST : GENERAL settings, identical for every map. Stored in engine.ini and saved automatically
 *              whenever the in-game menu changes something. Key name = field name.
 *              XG(title, is_bake) opens a group; groups flagged is_bake are restored by the "Reset" button
 *              of the Bake tab (cfg_reset_bake).
 *   SCN_LIST : SCENE properties (light position, glass, caustics, ambience...). Set by each map in maps.h
 *              (map_config), never stored in engine.ini.
 */
#pragma once

#define CFG_VERSION 2

#define CFG_LIST(XI, XF, XG) \
    XG("Display", 0) \
    XI(width, 1280, "window width (windowed mode)") \
    XI(height, 720, "window height (windowed mode)") \
    XI(fullscreen, 0, "1 = fullscreen (the lamp menu is windowed only)") \
    XI(vsync, 1, "1 = vertical sync") \
    XI(msaa, 0, "0 = off ; 2/4/8 if the GPU supports it (ignored if fxaa = 1)") \
    XI(fxaa, 0, "1 = FXAA post-process (ps_2_0)") \
    XF(render_scale, 1.0, "internal resolution 0.35-1 : scene drawn smaller then bilinearly upscaled (pixel-bound GPUs; ignored with msaa)") \
    XI(anisotropy, 8, "1 = off, up to 16") \
    XI(auto_low_end, 1, "1 = force anisotropy = 1 when the detected VRAM is below low_end_vram_mb") \
    XI(low_end_vram_mb, 256, "VRAM threshold (MB) under which the GPU is considered old") \
    XI(gamma_correct, 1, "1 = lighting in linear space (needs a re-bake) ; 0 = faster on very old GPUs") \
    XF(fov, 50, "vertical field of view (degrees)") \
    XF(exposure, 1.1, "tone mapping curve (no re-bake needed)") \
    XG("Camera and HUD", 0) \
    XF(cam_speed, 7.0, "camera speed (units/s), also changed with the mouse wheel") \
    XF(mouse_sens, 0.0022, "mouse sensitivity") \
    XI(key_layout, 0, "camera keys: 0 = QWERTY (W A S D), 1 = AZERTY (Z Q S D); also changeable in the menu (Camera tab)") \
    XI(hud, 1, "0 = no FPS counter / help text (zero overhead)") \
    XG("Lamp", 0) \
    XF(light_r, 1.0, "lamp color, red (0..1)") \
    XF(light_g, 0.97, "lamp color, green (0..1)") \
    XF(light_b, 0.92, "lamp color, blue (0..1)") \
    XF(light_intensity, 50, "lamp intensity") \
    XF(lamp_size, 0.35, "size of the movable lamp of map 3 (larger = softer shadows, needs a re-bake)") \
    XF(lamp_speed, 5.0, "lamp movement speed (units/s)") \
    XF(lamp_rebake_delay, 1.5, "seconds of inactivity before re-baking after the lamp moved") \
    XG("Bake: quality (computed once, then cached)", 1) \
    XF(texels_per_unit, 24, "lightmap resolution: texels per world unit") \
    XI(lightmap_max_size, 2048, "max atlas size (power of 2)") \
    XI(shadow_samples, 64, "shadow rays per texel (CPU shadows)") \
    XI(ao_samples, 64, "paths per node of the indirect grid (CPU path tracer)") \
    XI(blur_passes, 1, "3x3 noise smoothing passes (0 = none)") \
    XI(bake_threads, 0, "0 = automatic (number of cores)") \
    XI(indirect_step, 3, "indirect light computed 1 texel out of N, then interpolated (CPU path tracer)") \
    XI(occluder_lod, 1, "LoD level used by the bake rays (0 = full mesh)") \
    XG("Bake: indirect lighting", 1) \
    XI(bounce_count, 3, "number of light bounces (1 = single bounce)") \
    XF(bounce_strength, 1.5, "bounce gain (1.0 = physical)") \
    XF(bounce_distance, 10, "bounce range (units)") \
    XF(ao_distance, 3, "ambient occlusion range") \
    XF(ao_strength, 0.5, "ambient occlusion strength") \
    XG("Bake: ray-traced shadows", 1) \
    XI(rt_shadows, 1, "0 = GPU shadow maps only ; 1 = hybrid GPU + CPU rays in the penumbra ; 2 = all CPU") \
    XI(rt_shadow_samples, 64, "rays per texel on the extended lamp") \
    XI(rt_shadow_margin, 2, "mode 1: radius (texels) of the re-traced zone around shadow edges (0-8)") \
    XG("Bake: GPU (Shader Model 2.0)", 1) \
    XI(bake_gpu, 1, "1 = soft shadows computed by the GPU (automatic CPU fallback)") \
    XI(gpu_shadow_res, 256, "shadow cubemap resolution (power of 2, 64..1024)") \
    XI(gpu_taps, 4, "lamp points per pass (1-4)") \
    XF(gpu_shadow_bias, 1.0, "anti-acne offset of GPU shadows") \
    XI(bake_gpu_gi, 1, "1 = AO + bounces on the GPU (virtual point lights), 0 = CPU path tracer") \
    XI(gpu_gi_vpl, 512, "number of virtual lights (more = smoother, slower)") \
    XI(gpu_gi_res, 64, "depth cubemap resolution of each virtual light") \
    XF(gpu_gi_bias, 1.0, "occlusion tolerance") \
    XF(gpu_gi_ao_gain, 1.8, "ambient occlusion strength of the GPU path") \
    XI(bake_gpu_reflections, 1, "1 = reflections by GPU rasterization, 0 = CPU tracer (needs bake_gpu = 1)") \
    XI(reflection_upscale, 2, "bilinear upscale of the reflection cubemaps (1 = off)") \
    XI(refl_filter, 1, "1 = only objects flagged in_refl appear in reflections") \
    XG("Models (.glb, per-vertex lighting)", 1) \
    XF(model_max_edge, 0.3, "triangles with an edge longer than this (units) are subdivided so that per-vertex lighting can show shadows (needs a re-bake)") \
    XI(model_ao_samples, 32, "paths per vertex for the ambient occlusion / bounces of models (needs a re-bake)") \
    XI(model_smooth_passes, 5, "smoothing passes of the baked ambient occlusion / bounces between neighbouring vertices: removes the blotchy noise in dark areas (needs a re-bake)") \
    XI(model_reflections, 1, "1 = reflection layer on the models (metallic / roughness of the glTF material: iron vs plastic) (needs a re-bake)") \
    XF(model_refl_min, 0.10, "materials whose reflectivity stays below this (matte plastic, wood...) get no reflection layer (needs a re-bake)") \
    XI(model_tex_max, 2048, "model textures larger than this are reduced at load time") \
    XG("Reflections (real time)", 0) \
    XI(reflection_update_interval, 3, "frames to wait between two reflection refresh slices") \
    XF(reflection_budget_ms, 2.0, "time budget per refresh slice (ms); 1000+ = everything in one frame") \
    XF(reflection_lod_bias, 0.0, "mip bias on reflection cubemaps (0 = sharp, 1-2 = blurrier and cheaper)") \
    XF(reflection_distance, 70, "beyond this distance: no reflections") \
    XG("Performance", 0) \
    XF(lod_start, 30, "distance of the 1st LoD level (map 3)") \
    XF(lod_step, 25, "distance between LoD levels") \
    XF(lod_reduction, 0.5, "fraction of polygons removed at each LoD level") \
    XI(frustum_culling, 1, "1 = frustum culling") \
    XI(depth_prepass, 0, "1 = depth-only pass before the color pass (measured slower on this scene)") \
    XI(lean_stages, 1, "1 = untextured objects skip the 1x1 white texture stage (identical image, one fewer stage per pixel)") \
    XI(state_cache, 1, "1 = skip redundant state / texture / buffer binding calls (identical image)") \
    XI(pure_device, 0, "1 = D3DCREATE_PUREDEVICE: less driver-side checking (restart needed; automatic fallback)") \
    XI(show_timing, 1, "1 = window title: draw submission time vs Present/wait time (CPU or GPU bound?)") \
    XF(overlay_distance, 90, "beyond this distance the lightmap is no longer sampled") \
    XF(far_brightness, 0.55, "brightness used beyond overlay_distance") \
    XI(vcache_opt, 1, "1 = triangles reordered for the GPU vertex cache (same image, helps old GPUs)") \
    XI(adaptive_fps, 0, "target FPS (0 = off): below it the engine shortens lightmap/reflection/LoD distances (and the resolution if adaptive_res = 1), above it restores them") \
    XF(adaptive_min, 0.2, "adaptive quality: lowest distance scale (0.1 .. 1)") \
    XI(adaptive_res, 0, "adaptive quality also lowers the internal resolution (render_scale), down to adaptive_res_min; ignored with msaa") \
    XF(adaptive_res_min, 0.6, "adaptive quality: lowest internal resolution scale (0.35 .. 1)") \
    XG("Cache and loading", 0) \
    XI(cache_enabled, 1, "1 = bake cache in %TEMP%\\d3d9engine_cache") \
    XI(cache_max_files, 8, "old bakes kept (one lamp position = one file)") \
    XI(loading_hz, 5, "loading screen refreshes per second")

/* scene properties: set by map_config() in maps.h, not stored in engine.ini */
#define SCN_LIST(XI, XF) \
    XF(light_x, 0) XF(light_y, 0) XF(light_z, 0) XF(light_radius, 0.5) \
    XI(object_count, 200) XI(block_seed, 1234) \
    XF(ambient_r, 0.02) XF(ambient_g, 0.02) XF(ambient_b, 0.02) \
    XF(background_r, 0) XF(background_g, 0) XF(background_b, 0) \
    XI(lod_levels, 0) XI(reflection_size, 64) XI(reflection_samples, 4) \
    XI(caustics, 0) XI(caustic_photons, 1200000) XI(caustic_blur, 2) XF(glass_ior, 1.5) XF(glass_spec, 1.0) XI(glass_flip_x, 0)

#define XI_DECL(n, d, doc) int n;
#define XF_DECL(n, d, doc) float n;
#define XG_NOP(t, b)
#define SI_DECL(n, d) int n;
#define SF_DECL(n, d) float n;
typedef struct { CFG_LIST(XI_DECL, XF_DECL, XG_NOP) SCN_LIST(SI_DECL, SF_DECL) } Config;
static Config C;
static Config g_Cdef;                         /* factory defaults (used by Reset) */

static void cfg_defaults(void) {
#define XI_SET(n, d, doc) C.n = d;
#define XF_SET(n, d, doc) C.n = (float)(d);
#define SI_SET(n, d) C.n = d;
#define SF_SET(n, d) C.n = (float)(d);
    CFG_LIST(XI_SET, XF_SET, XG_NOP) SCN_LIST(SI_SET, SF_SET)
#undef XI_SET
#undef XF_SET
#undef SI_SET
#undef SF_SET
    g_Cdef = C;
}

/* applies "key = value" (key name = field name). Only CFG_LIST keys are accepted. */
static void cfg_set(const char* k, const char* v) {
#define XI_GET(n, d, doc) if (!strcmp(k, #n)) C.n = atoi(v);
#define XF_GET(n, d, doc) if (!strcmp(k, #n)) C.n = (float)atof(v);
    CFG_LIST(XI_GET, XF_GET, XG_NOP)
#undef XI_GET
#undef XF_GET
}

/* "Reset" of the Bake tab: every key of the groups flagged is_bake goes back to its factory default.
 * Returns the number of values that actually changed. */
static int cfg_reset_bake(void) {
    int in = 0, changed = 0;
#define XG_R(t, b) in = (b);
#define XI_R(n, d, doc) if (in && C.n != g_Cdef.n) { C.n = g_Cdef.n; changed++; }
#define XF_R(n, d, doc) if (in && C.n != g_Cdef.n) { C.n = g_Cdef.n; changed++; }
    CFG_LIST(XI_R, XF_R, XG_R)
#undef XG_R
#undef XI_R
#undef XF_R
    return changed;
}

/* ---------- engine.ini: parsing ---------- */
/* splits a line into key / value (comments ; and # removed). Returns 1 = key/value, 0 = nothing, -1 = [section] header */
static int cfg_parse_line(char* line, char** key, char** val) {
    char* c = strpbrk(line, ";#"); char *eq, *k, *e, *v;
    if (c) *c = 0;
    k = line; while (*k == ' ' || *k == '\t') k++;
    if (*k == '[') return -1;
    eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = 0; k = line; while (*k == ' ' || *k == '\t') k++;
    e = k + strlen(k); while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    v = eq + 1; while (*v == ' ' || *v == '\t') v++;
    e = v + strlen(v); while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (!*k) return 0;
    *key = k; *val = v; return 1;
}

/* Keys of the old format (before config_version 2) that were really effective: the others were silently
 * overridden by the per-map factory settings, so their old values must NOT be applied now. */
static int cfg_legacy_keep(const char* k) {
    static const char* const keep[] = { "width", "height", "fullscreen", "vsync", "msaa", "fxaa", "anisotropy", "auto_low_end", "low_end_vram_mb",
        "fov", "cam_speed", "mouse_sens", "hud", "reflection_update_interval", "reflection_budget_ms", "reflection_lod_bias",
        "cache_enabled", "cache_max_files", "loading_hz", "bake_threads", "frustum_culling", "lamp_speed", "lamp_rebake_delay", NULL };
    int i; for (i = 0; keep[i]; i++) if (!strcmp(k, keep[i])) return 1;
    return 0;
}

/* returns 1 = file loaded, 0 = no file, -1 = old format file (only the safe keys were loaded; rewrite it) */
static int cfg_load(const char* path) {
    FILE* f = fopen(path, "r"); char line[256], *k, *v; int ver = 0, skip = 0, r;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) { if (cfg_parse_line(line, &k, &v) == 1 && !strcmp(k, "config_version")) ver = atoi(v); }
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        r = cfg_parse_line(line, &k, &v);
        if (r < 0) { skip = 1; continue; }                 /* old [mapN] sections are ignored: no more per-map settings */
        if (r == 0 || skip || !strcmp(k, "config_version")) continue;
        if (ver >= CFG_VERSION || cfg_legacy_keep(k)) cfg_set(k, v);
    }
    fclose(f);
    return ver >= CFG_VERSION ? 1 : -1;
}

/* ---------- engine.ini: writing (whole file, documented, atomic: written to .tmp then renamed) ---------- */
static int g_aniso_user;                 /* >0: anisotropy requested by the user, forced to 1 at runtime by auto_low_end: save THIS value */

static int cfg_save(const char* path) {
    FILE* f; char tmp[300], val[48];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w"); if (!f) return 0;
    fprintf(f, "; engine.ini - GENERAL settings, the same for every map.\n"
               "; Written automatically by the in-game menu (Tab): every change made there is saved here.\n"
               "; You can also edit this file by hand (restart the engine). Delete it to get the factory defaults.\n"
               "; Map-specific scene properties (lamp position, glass, caustics, ambience) are defined in maps.h.\n\n"
               "config_version = %d\n", CFG_VERSION);
#define XG_W(t, b) fprintf(f, "\n; ---- %s ----\n", t);
#define XI_W(n, d, doc) sprintf(val, "%d", (!strcmp(#n, "anisotropy") && g_aniso_user > 0) ? g_aniso_user : C.n); fprintf(f, "%-26s = %-9s ; %s\n", #n, val, doc);
#define XF_W(n, d, doc) sprintf(val, "%g", C.n); fprintf(f, "%-26s = %-9s ; %s\n", #n, val, doc);
    CFG_LIST(XI_W, XF_W, XG_W)
#undef XG_W
#undef XI_W
#undef XF_W
    if (fclose(f) != 0) return 0;
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}
