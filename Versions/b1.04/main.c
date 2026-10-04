/*
 * D3D9 3D engine: baked lighting (lightmap + reflections + caustics), refractive glass, LoD, culling, FXAA, disk cache.
 * Demo maps: keys 1 / 2 / 3 (numpad or top row); G = global lighting on/off; right-click the lamp = color.
 * Build: see build.bat.  Settings: engine.ini
 *
 * Real-time rendering: 1 draw call per object, fixed-function multitexture pipeline
 *   stage 0: texture x vertex color | stage 1: x lightmap (shadows, AO, color bounces) | stage 2: reflections (cubemap)
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "vecmath.h"
#include "config.h"
#include "scene.h"
#include "maps.h"
#include "bake.h"
#include "render.h"
#include "gpubake.h"
#include "glass.h"
#include "ui.h"


/* ---------- benchmark (environment variable ENGINE_BENCH=label): measures the bake, then exports the lightmaps ---------- */
static void bench_dump(const char* tag, double secs) {
    char fn[96]; FILE* f; int oi;
    sprintf(fn, "bench_%s.bin", tag); f = fopen(fn, "wb");
    if (f) {
        for (oi = 0; oi < g_nobj; oi++) {
            Obj* o = &g_obj[oi]; size_t n = (size_t)o->aw * o->ah, k; float* t; int hd[2];
            if (!o->lightmap) continue;
            hd[0] = o->aw; hd[1] = o->ah; fwrite(hd, sizeof hd, 1, f);
            t = (float*)malloc(n * 9 * sizeof(float));
            for (k = 0; k < n; k++) {
                V3 T = rgbe_unpack(o->T[k]), A = rgbe_unpack(o->A[k]), B = rgbe_unpack(o->Bo[k]);
                t[k * 9] = T.x; t[k * 9 + 1] = T.y; t[k * 9 + 2] = T.z; t[k * 9 + 3] = A.x; t[k * 9 + 4] = A.y; t[k * 9 + 5] = A.z;
                t[k * 9 + 6] = B.x; t[k * 9 + 7] = B.y; t[k * 9 + 8] = B.z;
            }
            fwrite(t, sizeof(float), n * 9, f); free(t);
        }
        fclose(f);
    }
    sprintf(fn, "bench_%s.gb", tag); f = fopen(fn, "wb");
    if (f) { for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].gbuf) { int hd[2]; hd[0] = oi; hd[1] = g_obj[oi].ngbuf; fwrite(hd, sizeof hd, 1, f); fwrite(g_obj[oi].gbuf, 4, (size_t)g_obj[oi].ngbuf, f); } fclose(f); }
    f = fopen("bench.log", "a");
    if (f) { fprintf(f, "%s: bake %.2f s (ombres GPU %.2f | direct CPU %.2f | indirect %.2f | flou+compose %.2f | reflets %.2f) gpu_direct=%d K=%d taps=%d res=%d rt_shadows=%d rt_texels=%ld\n", tag, secs, g_ph[0], g_ph[1], g_ph[2], g_ph[3], g_ph[4], g_gpu_direct, g_gpu_K, g_gpu_taps, g_gpu_res, C.rt_shadows, (long)g_rt_refined); fclose(f); }
}
static double bench_now(void) { LARGE_INTEGER c, fq; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&fq); return (double)c.QuadPart / (double)fq.QuadPart; }

static int g_drag, g_lastx, g_lasty;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN: g_drag = 1; g_lastx = (short)LOWORD(l); g_lasty = (short)HIWORD(l); SetCapture(h); return 0;
    case WM_LBUTTONUP:   g_drag = 0; ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN:
        if (g_menu && ui_pick_lamp((short)LOWORD(l), (short)HIWORD(l))) ui_show_menu((short)LOWORD(l), (short)HIWORD(l));
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag) {
            int x = (short)LOWORD(l), y = (short)HIWORD(l);
            g_yaw -= (x - g_lastx) * 0.008f;
            g_pitch -= (y - g_lasty) * 0.008f;
            g_pitch = C.allow_underside ? clampf(g_pitch, -1.55f, 1.55f) : clampf(g_pitch, -1.55f, -0.03f);   /* without underside: bottom faces are never seen */
            g_lastx = x; g_lasty = y;
        }
        return 0;
    case WM_MOUSEWHEEL:
        g_dist = clampf(g_dist * powf(0.9f, (short)HIWORD(w) / 120.0f), 4.0f, 120.0f);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) g_running = 0;
        else if (w >= VK_NUMPAD1 && w < VK_NUMPAD1 + NMAPS) g_want_map = (int)(w - VK_NUMPAD0);
        else if (w >= '1' && w < '1' + NMAPS) g_want_map = (int)(w - '0');
        else if (w == 'G') { g_gi_on ^= 1; g_need_refresh = 1; }          /* demo: bounces + caustics on/off, without re-baking */
        return 0;
    case WM_DESTROY: g_running = 0; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}


/* Unloads the current map, applies the new settings, builds, bakes (or loads the cache) and displays it.
 * Returns 0 if the user closed the window during the bake. */
static int load_map(int id) {
    int oi;
    if (g_map) scene_unload();
    g_refl_n = g_refl_pos = 0;
    C = g_Cbase; map_config(id); cfg_apply_map(id); sanitize_cfg();
    pow_lut_init(); g_gi_on = 1; g_abort = 0;
    switch (id) { case 1: build_map1(); break; case 2: build_map2(); break; default: build_map3(); break; }
    scene_finalize();
    for (oi = 0; oi < g_nobj; oi++) upload_object(&g_obj[oi]);
    g_map = id;
    if (id == 3) { g_lamp_lim[0] = -9.5f; g_lamp_lim[1] = 9.5f; g_lamp_lim[2] = 1.5f; g_lamp_lim[3] = 14.0f; g_lamp_lim[4] = -6.3f; g_lamp_lim[5] = 6.3f;
                   g_yaw = 0.6f; g_pitch = -0.7f; g_dist = 30.0f; }
    else           { g_yaw = 0.0f; g_pitch = 0.0f; g_dist = 16.5f; }
    g_cam_valid = 0; g_fr_valid = 0; g_lsel = 0; g_pending_rebake = 0; g_need_refresh = 0;
    if (!bake_all()) return 0;
    refresh_lighting();
    if (g_menu) { ShowWindow(g_menu, SW_HIDE); ui_sync_sliders(); }
    return 1;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    WNDCLASSA wc; RECT r; DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    MSG msg; int oi, frames = 0, drawn = 0; DWORD tlast, tprev, tref = 0; char title[200];
    (void)hp; (void)cmd; (void)show;

    cfg_defaults(); cfg_load("engine.ini");
    sanitize_cfg();
    pow_lut_init();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "D3D9Engine";
    RegisterClassA(&wc);
    r.left = r.top = 0; r.right = C.width; r.bottom = C.height; AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowA("D3D9Engine", "D3D9 Engine", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    if (!g_hwnd) fatal("CreateWindow failed");

    init_d3d();
    rgbe_init();
    init_common_textures();
    vram_query_total();
    /* Old integrated GPU (reduced VRAM): anisotropic filtering is done in software there and costs
       a lot per pixel. It is disabled automatically (unless already disabled). */
    if (C.auto_low_end && g_vram_total > 0 && g_vram_total < (double)C.low_end_vram_mb * 1048576.0 && C.anisotropy > 1)
        C.anisotropy = 1;

    g_Cbase = C;                                     /* base settings for all maps (anisotropy / GPU limits already applied) */
    {   const char* bt = getenv("ENGINE_BENCH"); double t0 = bench_now(); const char* bm = getenv("ENGINE_MAP");
        g_want_map = 0;
        if (!load_map(bm ? atoi(bm) : 1)) return 0;   /* disk cache, otherwise a full bake with a loading screen */
        if (bt) { bench_dump(bt, bench_now() - t0); return 0; }
    }
    ui_create_menu(hi);
    ui_sync_sliders();

    tlast = tprev = GetTickCount();
    while (g_running) {
        DWORD now; float dt;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) g_running = 0; TranslateMessage(&msg); DispatchMessage(&msg); }
        if (!g_running) break;
        now = GetTickCount(); dt = (now - tprev) * 0.001f; if (dt > 0.1f) dt = 0.1f; tprev = now;

        if (g_want_map && g_want_map != g_map) {        /* keys 1 / 2 / 3: map change */
            int m = g_want_map; g_want_map = 0;
            if (!load_map(m)) break;
            tlast = tprev = GetTickCount(); frames = 0; g_drag = 0; ReleaseCapture();
            continue;
        }
        g_want_map = 0;
        if (ui_move_lamp(dt)) { g_pending_rebake = 1; g_pending_t = now; }
        if (g_pending_rebake && !g_drag && now - g_pending_t >= (DWORD)(C.lamp_rebake_delay * 1000.0f)) {
            g_pending_rebake = 0;                       /* the lamp position/size changed: new bake (or cache) */
            if (!bake_all()) break;
            refresh_lightmaps(); reflections_mark_dirty();   /* reflections resumed frame by frame */
            tlast = tprev = GetTickCount(); frames = 0;
            continue;
        }
        if (g_need_refresh && now - tref >= 200) {
            /* color / intensity: T = sum (color x mask), no ray re-traced */
            { int oi; for (oi = 0; oi < g_nobj; oi++) if (g_obj[oi].lightmap) compose_light(&g_obj[oi]); }
            refresh_lightmaps(); reflections_mark_dirty(); g_need_refresh = 0; tref = now;
        }
        reflections_step();       /* advances by one object (or more) if the budget allows */

        render(); frames++; drawn = g_drawn;
        now = GetTickCount();
        if (now - tlast >= 500) {
            sprintf(title, "D3D9 Engine [map %d : %s] - %.0f FPS | %d drawn objects | GI %s%s%s%s | VRAM %.0f/%.0f Mo", g_map, MAP_NAME[g_map],
                    frames * 1000.0 / (now - tlast), drawn, g_gi_on ? "on" : "off",
                    g_fxaa_on ? " | FXAA" : "", g_pending_rebake ? " | lighting pending..." : "",
                    reflections_pending() ? " | reflections in progress..." : "",
                    vram_used() / 1048576.0, g_vram_total / 1048576.0);
            SetWindowTextA(g_hwnd, title); frames = 0; tlast = now;
        }
    }
    IDirect3DDevice9_Release(g_dev); IDirect3D9_Release(g_d3d);
    return 0;
}
