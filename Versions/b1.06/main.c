/*
 * D3D9 3D engine: baked lighting (lightmap + reflections + caustics), refractive glass, LoD, culling, FXAA, disk cache.
 * Demo maps: keys 1 / 2 / 3 (numpad or top row); G = global lighting on/off; right-click the lamp = color.
 * Camera: W A S D (QWERTY, default) or Z Q S D (AZERTY, selectable in the menu: Camera tab / key_layout in engine.ini).
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
#include <stdarg.h>
#include <ctype.h>

#include "vecmath.h"
#include "config.h"
#include "sm2asm.h"
#include "scene.h"
#include "maps.h"
#include "bake.h"
#include "render.h"
#include "gpubake.h"
#include "gpugi.h"
#include "glass.h"
#include "gui.h"
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
    if (f) { fprintf(f, "%s: bake %.2f s (GPU shadows %.2f | CPU direct %.2f | indirect %.2f | blur+compose %.2f | reflections %.2f) gi_gpu=%d gpu_direct=%d K=%d taps=%d res=%d rt_shadows=%d rt_texels=%ld\n", tag, secs, g_ph[0], g_ph[1], g_ph[2], g_ph[3], g_ph[4], g_gi_gpu, g_gpu_direct, g_gpu_K, g_gpu_taps, g_gpu_res, C.rt_shadows, (long)g_rt_refined); fclose(f); }
}
static double bench_now(void) { LARGE_INTEGER c, fq; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&fq); return (double)c.QuadPart / (double)fq.QuadPart; }

static float g_ci_pos[3], g_ci_yaw, g_ci_pitch;      /* initial camera of the map (Reset camera button) */

static void toggle_gui(void) {
    g_gui_on ^= 1; if (!g_gui_on) g_hint_t = GetTickCount() - 5000; g_cam_have_init = 0; g_mdown = 0; g_gactive = 0;
    if (g_gui_on) { RECT rc; POINT p; GetClientRect(g_hwnd, &rc); p.x = rc.right / 2; p.y = rc.bottom / 2; ClientToScreen(g_hwnd, &p); SetCursorPos(p.x + 160, p.y); }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ACTIVATE: g_win_active = LOWORD(w) != WA_INACTIVE; g_rdx = g_rdy = 0; if (!g_win_active) { ClipCursor(NULL); g_clip_on = 0; } break;
    case WM_INPUT: {                                                    /* raw mouse deltas (mouse look, no cursor warping) */
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_rdx += ri.data.mouse.lLastX; g_rdy += ri.data.mouse.lLastY;
        }
        break; }
    case WM_LBUTTONDOWN: g_mdown = 1; g_mclick = 1; g_mx = (short)LOWORD(l); g_my = (short)HIWORD(l); SetCapture(h); return 0;
    case WM_LBUTTONUP:   g_mdown = 0; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:   g_mx = (short)LOWORD(l); g_my = (short)HIWORD(l); return 0;
    case WM_SETCURSOR:
        if (!g_gui_on && LOWORD(l) == HTCLIENT && g_map) { SetCursor(NULL); return TRUE; }      /* mouse look: no cursor */
        break;
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(w) / 120;
        if (g_gui_on) g_mwheel += d;
        else { C.cam_speed = clampf(C.cam_speed * powf(1.15f, (float)d), 0.5f, 120.0f); cfg_touch(); }   /* wheel = camera speed */
        return 0; }
    case WM_KEYDOWN:
        if (l & (1 << 30)) return 0;                                    /* ignore auto-repeat */
        if (w == VK_ESCAPE || w == VK_TAB) toggle_gui();
        else if (w >= VK_NUMPAD1 && w < VK_NUMPAD1 + NMAPS) g_want_map = (int)(w - VK_NUMPAD0);
        else if (w >= '1' && w < '1' + NMAPS) g_want_map = (int)(w - '0');
        else if (w == 'G') { g_gi_on ^= 1; g_need_refresh = 1; }          /* demo: bounces + caustics on/off, without re-baking */
        return 0;
    case WM_DESTROY: g_running = 0; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* fullscreen / windowed, vsync, FXAA: Reset of the device (everything that lives in D3DPOOL_DEFAULT is released first) */
static void apply_display(void) {
    D3DDISPLAYMODE dm; RECT r; DWORD style; int w, h;
    release_rt();
    g_pp.PresentationInterval = C.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (C.fullscreen) {
        IDirect3D9_GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT, &dm);
        w = (int)dm.Width; h = (int)dm.Height;
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_pp.Windowed = FALSE; g_pp.BackBufferWidth = (UINT)w; g_pp.BackBufferHeight = (UINT)h; g_pp.FullScreen_RefreshRateInHz = dm.RefreshRate;
    } else {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        r.left = r.top = 0; r.right = C.width; r.bottom = C.height; AdjustWindowRect(&r, style, FALSE);
        g_pp.Windowed = TRUE; g_pp.BackBufferWidth = (UINT)C.width; g_pp.BackBufferHeight = (UINT)C.height; g_pp.FullScreen_RefreshRateInHz = 0;
        SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)style);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 100, 60, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_fxaa_on = C.fxaa && (g_fxaa_ps != NULL || load_fxaa());
    if (FAILED(IDirect3DDevice9_Reset(g_dev, &g_pp))) {
        ui_msg("Display mode refused: back to windowed"); C.fullscreen = 0; cfg_touch();
        g_pp.Windowed = TRUE; g_pp.BackBufferWidth = (UINT)C.width; g_pp.BackBufferHeight = (UINT)C.height; g_pp.FullScreen_RefreshRateInHz = 0;
        if (FAILED(IDirect3DDevice9_Reset(g_dev, &g_pp))) fatal("Reset failed");
    }
    if (g_fxaa_on) create_rt();
    frame_states(); g_cam_valid = 0; g_fr_valid = 0; g_mdown = 0; g_clip_on = 0;
}

/* Unloads the current map, applies the new settings, builds, bakes (or loads the cache) and displays it.
 * Returns 0 if the user closed the window during the bake. */
static int load_map(int id) {
    int oi;
    if (g_map) scene_unload();
    g_refl_n = g_refl_pos = 0;
    map_config(id); sanitize_cfg();            /* scene properties only: the user settings in C are general */
    pow_lut_init(); g_gi_on = 1; g_abort = 0;
    switch (id) { case 1: build_map1(); break; case 2: build_map2(); break; default: build_map3(); break; }
    scene_finalize();
    for (oi = 0; oi < g_nobj; oi++) upload_object(&g_obj[oi]);
    g_map = id;
    if (id == 3) { g_lamp_lim[0] = -9.5f; g_lamp_lim[1] = 9.5f; g_lamp_lim[2] = 1.5f; g_lamp_lim[3] = 14.0f; g_lamp_lim[4] = -6.3f; g_lamp_lim[5] = 6.3f;
                   cam_from_orbit(0.6f, -0.7f, 30.0f); }
    else           cam_from_orbit(0.0f, 0.0f, 16.5f);
    g_ci_pos[0] = g_cpos.x; g_ci_pos[1] = g_cpos.y; g_ci_pos[2] = g_cpos.z; g_ci_yaw = g_yaw; g_ci_pitch = g_pitch;
    g_cam_valid = 0; g_fr_valid = 0; g_lsel = 0; g_pending_rebake = 0; g_need_refresh = 0; g_gdirty_rebake = 0; g_cam_have_init = 0;
    if (!bake_all()) return 0;
    refresh_lighting();
    return 1;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    WNDCLASSA wc; RECT r; DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    MSG msg; int oi, frames = 0, drawn = 0; DWORD tlast, tprev, tref = 0; char title[200];
    (void)hp; (void)cmd; (void)show;

    cfg_defaults();
    {   int lr = cfg_load("engine.ini");              /* 1 = loaded, 0 = missing, -1 = old format (safe keys only) */
        if (lr == -1) CopyFileA("engine.ini", "engine_old.ini", TRUE);     /* keep the old file once */
        if (lr != 1) cfg_touch();                       /* (re)write engine.ini in the current format */
    }
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
    if (C.auto_low_end && g_vram_total > 0 && g_vram_total < (double)C.low_end_vram_mb * 1048576.0 && C.anisotropy > 1) {
        g_aniso_user = C.anisotropy; C.anisotropy = 1;          /* runtime override only: engine.ini keeps the user's value */
    }

    {   const char* bt = getenv("ENGINE_BENCH"); double t0 = bench_now(); const char* bm = getenv("ENGINE_MAP");
        g_want_map = 0;
        if (!load_map(bm ? atoi(bm) : 1)) return 0;   /* disk cache, otherwise a full bake with a loading screen */
        if (bt) { bench_dump(bt, bench_now() - t0); return 0; }
    }
    gui_init();
    { RAWINPUTDEVICE rid; rid.usUsagePage = 0x01; rid.usUsage = 0x02; rid.dwFlags = 0; rid.hwndTarget = g_hwnd; RegisterRawInputDevices(&rid, 1, sizeof rid); }
    g_hint_t = GetTickCount();

    tlast = tprev = GetTickCount();
    while (g_running) {
        DWORD now; float dt;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) g_running = 0; TranslateMessage(&msg); DispatchMessage(&msg); }
        if (!g_running) break;
        now = GetTickCount(); dt = (now - tprev) * 0.001f; if (dt > 0.1f) dt = 0.1f; tprev = now;

        if (g_want_map && g_want_map != g_map) {        /* keys 1 / 2 / 3: map change */
            int m = g_want_map; g_want_map = 0;
            if (!load_map(m)) break;
            tlast = tprev = GetTickCount(); frames = 0; g_mdown = 0; ReleaseCapture();
            continue;
        }
        g_want_map = 0;
        if (g_req_reload) {                              /* menu: apply the changed settings (full reload of the map) */
            int m = g_map; g_req_reload = 0;
            if (!load_map(m)) break;
            tlast = tprev = GetTickCount(); frames = 0; g_mdown = 0; ReleaseCapture();
            continue;
        }
        if (g_req_display) { g_req_display = 0; apply_display(); }
        cfg_autosave(0);                                 /* settings changed in the menu: engine.ini rewritten automatically */
        if (g_req_cam_reset) { g_req_cam_reset = 0; g_cpos = v3(g_ci_pos[0], g_ci_pos[1], g_ci_pos[2]); g_yaw = g_ci_yaw; g_pitch = g_ci_pitch; }
        ui_camera(dt, !g_gui_on);
        if (ui_move_lamp(dt)) { g_pending_rebake = 1; g_pending_t = now; }
        if (g_pending_rebake && !g_mdown && now - g_pending_t >= (DWORD)(C.lamp_rebake_delay * 1000.0f)) {
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
            g_fps = (float)(frames * 1000.0 / (now - tlast));
            sprintf(title, "D3D9 Engine [map %d: %s] - %.0f FPS | %d drawn objects | GI %s%s%s%s | VRAM %.0f/%.0f MB", g_map, MAP_NAME[g_map],
                    frames * 1000.0 / (now - tlast), drawn, g_gi_on ? "on" : "off",
                    g_fxaa_on ? " | FXAA" : "", g_pending_rebake ? " | lighting pending..." : "",
                    reflections_pending() ? " | reflections in progress..." : "",
                    vram_used() / 1048576.0, g_vram_total / 1048576.0);
            SetWindowTextA(g_hwnd, title); frames = 0; tlast = now;
        }
    }
    cfg_autosave(1);                                     /* pending changes are never lost on exit */
    IDirect3DDevice9_Release(g_dev); IDirect3D9_Release(g_d3d);
    return 0;
}
