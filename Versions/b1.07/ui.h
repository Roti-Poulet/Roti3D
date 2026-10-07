/* ui.h - free camera (W A S D / Z Q S D + mouse), lamp movement, in-window menu (gui.h) */
#pragma once

static int g_req_open_glb;                              /* menu / key O: open the .glb file dialog (handled by the main loop) */
static int g_need_refresh, g_pending_rebake; static DWORD g_pending_t;
static float g_lamp_lim[6] = { -13.0f, 13.0f, 1.2f, 16.0f, -6.5f, 6.5f };   /* x0,x1,y0,y1,z0,z1: set per map */
static int g_req_reload, g_req_display, g_req_cam_reset; static float g_fps; static float g_panel_h = 400.0f; static int g_ui_saved_msg;
static DWORD g_ui_msg_t; static char g_ui_msg[96];

static void ui_msg(const char* s) { strncpy(g_ui_msg, s, 95); g_ui_msg[95] = 0; g_ui_msg_t = GetTickCount(); }

/* ---------- free camera ---------- */
static void cam_basis(V3* R, V3* U, V3* L) {
    float cp = cosf(g_pitch);
    *L = v3(sinf(g_yaw) * cp, sinf(g_pitch), cosf(g_yaw) * cp);
    *R = v3(cosf(g_yaw), 0.0f, -sinf(g_yaw));
    *U = vcross(*L, *R);
}
/* starting position: same viewpoint as the old orbit camera (rotation yaw/pitch around the origin at distance dist) */
static void cam_from_orbit(float yaw, float pitch, float dist) {
    M4 w = m_mul(m_roty(yaw), m_rotx(pitch)); V3 f = m_vec_inv(v3(0, 0, 1), w);
    g_cpos = m_vec_inv(v3(0, 0, -dist), w);
    g_yaw = atan2f(f.x, f.z); g_pitch = asinf(clampf(f.y, -1, 1));
}
static float g_cam_init[3]; static int g_cam_have_init;
static int g_win_active = 1, g_clip_on, g_rdx, g_rdy; static BYTE g_keys[256]; static DWORD g_hint_t;
#define KD(k) (g_keys[k] & 0x80)

/* camera keys: QWERTY (W A S D) by default, AZERTY (Z Q S D) via C.key_layout. Virtual-key codes follow the
 * layout of the OS keyboard, so the letters printed on the keycaps are the ones that move the camera. */
static int key_fwd(void)   { return C.key_layout ? 'Z' : 'W'; }
static int key_back(void)  { return 'S'; }
static int key_left(void)  { return C.key_layout ? 'Q' : 'A'; }
static int key_right(void) { return 'D'; }
static const char* key_help(void) { return C.key_layout ? "Z Q S D" : "W A S D"; }

/* mouse look without raw input (Windows 2000 compatible: no GetRawInputData / RegisterRawInputDevices).
 * Event driven: WM_MOUSEMOVE gives the client position; the delta is (position - previous position), accumulated in
 * g_rdx/g_rdy. The cursor is confined to the client area (ClipCursor) and only re-centered (SetCursorPos) when it
 * leaves a central zone, so there is still no per-frame syscall. The WM_MOUSEMOVE caused by the re-centering is
 * recognized and never counted as movement. */
static int g_ml_cx, g_ml_cy, g_ml_zx, g_ml_zy, g_ml_lx, g_ml_ly, g_ml_skip; static POINT g_ml_scr; static DWORD g_ml_skip_t;

static void ui_ml_recenter(void) {
    SetCursorPos(g_ml_scr.x, g_ml_scr.y);
    g_ml_lx = g_ml_cx; g_ml_ly = g_ml_cy; g_ml_skip = 1; g_ml_skip_t = GetTickCount();
}

static void ui_cursor_state(int want) {
    if (want && !g_clip_on) {
        RECT rc; POINT tl = { 0, 0 }; GetClientRect(g_hwnd, &rc); ClientToScreen(g_hwnd, &tl);
        g_ml_cx = rc.right / 2; g_ml_cy = rc.bottom / 2;
        g_ml_zx = rc.right / 4;  if (g_ml_zx < 8) g_ml_zx = 8;          /* re-center beyond +/- 1/4 of the window */
        g_ml_zy = rc.bottom / 4; if (g_ml_zy < 8) g_ml_zy = 8;
        g_ml_scr.x = tl.x + g_ml_cx; g_ml_scr.y = tl.y + g_ml_cy;
        rc.left += tl.x; rc.right += tl.x; rc.top += tl.y; rc.bottom += tl.y; ClipCursor(&rc); g_clip_on = 1;
        ui_ml_recenter(); g_rdx = g_rdy = 0;
    } else if (!want && g_clip_on) { ClipCursor(NULL); g_clip_on = 0; g_ml_skip = 0; }
}

/* called by WM_MOUSEMOVE (client coordinates) while mouse look is active */
static void ui_mouse_move(int x, int y) {
    int far_ = abs(x - g_ml_cx) > g_ml_zx || abs(y - g_ml_cy) > g_ml_zy;
    if (g_ml_skip) {
        if (x == g_ml_cx && y == g_ml_cy) { g_ml_skip = 0; return; }       /* the message of our own SetCursorPos */
        if (far_ && GetTickCount() - g_ml_skip_t < 100) return;             /* stale message from before the re-centering */
        g_ml_skip = 0;                                                      /* real movement right after the warp: count it from the center */
    }
    g_rdx += x - g_ml_lx; g_rdy += y - g_ml_ly; g_ml_lx = x; g_ml_ly = y;
    if (far_) ui_ml_recenter();
}

static void ui_camera(float dt, int mouse_look) {
    V3 R, U, L, mv = { 0, 0, 0 }; float sp;
    GetKeyboardState(g_keys);                                   /* one call for all the keys of the frame */
    ui_cursor_state(mouse_look && g_win_active);
    if (!g_win_active) { g_rdx = g_rdy = 0; return; }
    if (mouse_look) {
        g_yaw += (float)g_rdx * C.mouse_sens; g_pitch -= (float)g_rdy * C.mouse_sens;
        g_pitch = clampf(g_pitch, -1.55f, 1.55f);
        if (g_yaw > PI) g_yaw -= 2 * PI; else if (g_yaw < -PI) g_yaw += 2 * PI;
    }
    g_rdx = g_rdy = 0;
    cam_basis(&R, &U, &L);
    if (KD(key_fwd()))   mv = vadd(mv, L);
    if (KD(key_back()))  mv = vsub(mv, L);
    if (KD(key_right())) mv = vadd(mv, R);
    if (KD(key_left()))  mv = vsub(mv, R);
    if (KD(VK_SPACE)) mv.y += 1.0f;
    if (KD(VK_CONTROL)) mv.y -= 1.0f;
    if (vlen(mv) > 1e-4f) {
        sp = C.cam_speed * (KD(VK_SHIFT) ? 3.0f : 1.0f) * dt;
        g_cpos = vadd(g_cpos, vmul(vnorm(mv), sp));
    }
}

/* arrows: move the selected lamp in the plane of the platform, relative to the view; PgUp/PgDn: height */
static int ui_move_lamp(float dt) {
    V3 R, U, L, fwd, mv = { 0, 0, 0 }; float step = C.lamp_speed * dt;
    if (!g_win_active) return 0;
    cam_basis(&R, &U, &L); fwd = vnorm(v3(L.x, 0, L.z));
    if (vlen(fwd) < 0.2f) fwd = v3(sinf(g_yaw), 0, cosf(g_yaw));
    if (KD(VK_RIGHT)) mv = vadd(mv, R);
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) mv = vsub(mv, R);
    if (GetAsyncKeyState(VK_UP)    & 0x8000) mv = vadd(mv, fwd);
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) mv = vsub(mv, fwd);
    if (KD(VK_PRIOR)) mv.y += 1;
    if (GetAsyncKeyState(VK_NEXT)  & 0x8000) mv.y -= 1;
    if (vlen(mv) < 1e-4f) return 0;
    if (!g_lightset[g_lsel].fixed) {                            /* movement: re-bake (shadows) */
        V3 np = vadd(g_lightset[g_lsel].pos, vmul(mv, step));
        np.x = clampf(np.x, g_lamp_lim[0], g_lamp_lim[1]); np.y = clampf(np.y, g_lamp_lim[2], g_lamp_lim[3]); np.z = clampf(np.z, g_lamp_lim[4], g_lamp_lim[5]);
        g_lightset[g_lsel].pos = np;
        if (g_lsel == 0) { g_light = np; C.light_x = np.x; C.light_y = np.y; C.light_z = np.z; }
        return 1;
    }
    return 0;
}

/* ---------- panel ---------- */
static const char* TAB_NAME[6] = { "Light", "Camera", "Display", "Bake", "Perf.", "System" };

static void ui_tab_light(void) {
    Light* lp; float m, r, g, b, in, rad; int ch = 0, i, k;
    gui_separator("Maps");
    for (k = 1; k <= NMAPS; k++) {
        char t[24]; sprintf(t, "Map %d", k);
        if (gui_button_w(t, (g_gw - 8) / 3, g_map == k)) g_want_map = k;
        g_gx += (g_gw - 8) / 3 + 4;
    }
    g_gx -= 3 * ((g_gw - 8) / 3 + 4); g_gy += GUI_ROW;
    if (gui_button_w("Open GLB file...  (O)", g_gw, g_map == MAP_MODEL)) g_req_open_glb = 1;
    g_gy += GUI_ROW;
    gui_label(g_map == MAP_MODEL ? model_file_name() : MAP_NAME[g_map], COL_DIM);
    if (g_map != MAP_MODEL) gui_label("or drop a .glb file on the window", COL_DIM);
    gui_separator("Lamp");
    if (g_nlight > 1) {
        float bw = (g_gw - 4.0f * (g_nlight - 1)) / g_nlight;
        for (i = 0; i < g_nlight && i < MAXLIGHT; i++) { char t[8]; sprintf(t, "%d", i + 1); if (gui_button_w(t, bw, g_lsel == i)) g_lsel = i; g_gx += bw + 4; }
        g_gx = g_gx - g_nlight * (bw + 4); g_gy += GUI_ROW;
    }
    lp = &g_lightset[g_lsel < g_nlight ? g_lsel : 0];
    m = fmaxf(lp->col.x, fmaxf(lp->col.y, lp->col.z)); if (m < 1e-4f) m = 1.0f;
    r = lp->col.x / m; g = lp->col.y / m; b = lp->col.z / m; in = m; rad = C.light_radius;
    ch |= gui_slider("Red", &r, NULL, 0, 1, 0); ch |= gui_slider("Green", &g, NULL, 0, 1, 0); ch |= gui_slider("Blue", &b, NULL, 0, 1, 0);
    ch |= gui_slider("Intensity", &in, NULL, 0, 400, 0);
    if (ch) {
        lp->col = v3(r * in, g * in, b * in); g_need_refresh = 1;
        if (g_lsel == 0) { C.light_r = r; C.light_g = g; C.light_b = b; C.light_intensity = in; cfg_touch(); }     /* general settings */
    }
    if (lp->shape) gui_label("Size: fixed by the light panel", COL_DIM);
    else if (gui_slider("Size (shadows)", &rad, NULL, 0.05f, 2.0f, 0)) {
        int l; C.lamp_size = C.light_radius = rad; for (l = 0; l < g_nlight; l++) g_lightset[l].radius = rad;
        g_pending_rebake = 1; g_pending_t = GetTickCount(); cfg_touch();
    }
    gui_check("Global illumination (G)", &g_gi_on, 0); if (g_mclick == 0 && 0) {}
    {   static int last = -1; if (last != g_gi_on) { if (last != -1) g_need_refresh = 1; last = g_gi_on; } }
    gui_label("Arrows / PgUp / PgDn: move the lamp", COL_DIM);
}

static void ui_tab_camera(void) {
    gui_separator("Movement");
    gui_cf("Speed", &C.cam_speed, 0.5f, 60.0f, 0);
    gui_cf("Mouse sensitivity", &C.mouse_sens, 0.0004f, 0.008f, 0);
    if (gui_cf("Field of view", &C.fov, 30.0f, 110.0f, 0)) { g_cam_valid = 0; }
    if (gui_button("Reset camera")) g_req_cam_reset = 1;
    gui_space(6);
    {   /* keyboard layout selector: QWERTY | AZERTY (saved in engine.ini as key_layout) */
        float bw = (g_gw - 4.0f) * 0.5f;
        gui_separator("Keyboard layout");
        if (gui_button_w("QWERTY (W A S D)", bw, C.key_layout == 0)) { C.key_layout = 0; cfg_touch(); }
        g_gx += bw + 4;
        if (gui_button_w("AZERTY (Z Q S D)", bw, C.key_layout == 1)) { C.key_layout = 1; cfg_touch(); }
        g_gx -= bw + 4; g_gy += GUI_ROW;
    }
    gui_space(6);
    {   char t[64]; sprintf(t, "%s: forward / left / back / right", key_help()); gui_label(t, COL_DIM); }
    gui_label("Space / Ctrl: up / down", COL_DIM);
    gui_label("Shift: sprint     Mouse wheel: speed", COL_DIM);
}

static void ui_tab_display(void) {
    int ch = 0;
    gui_separator("Image");
    ch |= gui_cb("Vertical sync", &C.vsync, 0);
    ch |= gui_cb("FXAA (anti-aliasing)", &C.fxaa, 0);
    if (ch) g_req_display = 1;
    gui_cf("Render scale", &C.render_scale, 0.35f, 1.0f, 0);
    if (gui_ci("Anisotropy", &C.anisotropy, 1, 16, 0)) g_aniso_user = 0;      /* the user's choice replaces the low-end override */
    gui_cb("Gamma correction", &C.gamma_correct, 1);
    if (gui_cf("Exposure", &C.exposure, 0.3f, 4.0f, 0)) g_need_refresh = 1;
    gui_separator("Reflections");
    gui_cf("Reflection distance", &C.reflection_distance, 5, 300, 0);
    gui_cf("Reflection blur", &C.reflection_lod_bias, 0, 3, 0);
}

static void ui_tab_bake(void) {
    gui_label("Orange = re-bake required", COL_WARN);
    gui_separator("Quality");
    gui_cf("Texels / unit", &C.texels_per_unit, 4, 64, 1);
    gui_ci("Shadow rays", &C.shadow_samples, 8, 256, 1);
    gui_ci("Blur passes", &C.blur_passes, 0, 3, 1);
    gui_separator("Indirect light");
    gui_ci("Bounces", &C.bounce_count, 1, 6, 1);
    gui_cf("Bounce strength", &C.bounce_strength, 0, 4, 1);
    gui_cf("Bounce range", &C.bounce_distance, 1, 60, 1);
    gui_cf("AO strength", &C.ao_strength, 0, 1, 1);
    gui_cf("AO range", &C.ao_distance, 0.5f, 10, 1);
    gui_separator("3D models");
    gui_cb("Reflection layer (metal / rough)", &C.model_reflections, 1);
    gui_cf("Min. reflectivity", &C.model_refl_min, 0.0f, 0.6f, 1);
    gui_ci("Smoothing (dark-area noise)", &C.model_smooth_passes, 0, 12, 1);
    gui_cf("Model edge (shadow detail)", &C.model_max_edge, 0.05f, 1.0f, 1);
    gui_separator("Compute");
    gui_cb("GPU shadows", &C.bake_gpu, 1);
    gui_cb("GPU indirect light", &C.bake_gpu_gi, 1);
    gui_ci("Virtual lights", &C.gpu_gi_vpl, 64, 4096, 1);
    gui_cf("Gain AO (GPU)", &C.gpu_gi_ao_gain, 0.5f, 4, 1);
    gui_space(2);
    {   /* Re-bake | Reset (side by side) */
        float bw = (g_gw - 4.0f) * 0.5f;
        if (gui_button_w(g_gdirty_rebake ? "Apply and re-bake  *" : "Re-bake", bw, 0)) g_req_reload = 1;
        g_gx += bw + 4;
        if (gui_button_w("Reset", bw, 0)) {
            if (cfg_reset_bake()) { g_gdirty_rebake = 1; cfg_touch(); ui_msg("Bake settings reset: click Re-bake"); }
            else ui_msg("Bake settings already at default values");
        }
        g_gx -= bw + 4; g_gy += GUI_ROW;
    }
}

static void ui_tab_perf(void) {
    gui_separator("Distances");
    gui_cf("Lightmaps up to", &C.overlay_distance, 10, 300, 0);
    gui_cf("LoD start", &C.lod_start, 10, 200, 0);
    gui_cf("LoD step", &C.lod_step, 5, 100, 0);
    gui_separator("Options");
    gui_cb("Frustum culling", &C.frustum_culling, 0);
    gui_cb("Depth pre-pass", &C.depth_prepass, 0);
    gui_cb("State cache (fewer API calls)", &C.state_cache, 0);
    gui_cb("Lean stages (skip white stage)", &C.lean_stages, 0);
    gui_cb("Show timing in title bar", &C.show_timing, 0);
    gui_cb("Pure device (restart)", &C.pure_device, 0);
    if (gui_cb("Vertex cache optimization", &C.vcache_opt, 0)) g_req_idx = 1;
    gui_separator("Adaptive quality");
    gui_ci("Target FPS (0 = off)", &C.adaptive_fps, 0, 240, 0);
    gui_cf("Minimum distance scale", &C.adaptive_min, 0.1f, 1.0f, 0);
    gui_cb("Also lower the resolution", &C.adaptive_res, 0);
    gui_cf("Minimum resolution scale", &C.adaptive_res_min, 0.35f, 1.0f, 0);
    { char t[96]; sprintf(t, "Current: distances %.0f%%, resolution %.0f%%", g_adapt * 100.0f, g_adapt_rs * C.render_scale * 100.0f); gui_label(t, COL_DIM); }
}

static void ui_tab_system(void) {
    char t[96];
    gui_separator("Window");
    if (gui_cb("Fullscreen", &C.fullscreen, 0)) g_req_display = 1;
    gui_cb("Show FPS / help (HUD)", &C.hud, 0);
    gui_separator("Configuration");
    gui_label("Auto-save (engine.ini)", COL_DIM);
    if (gui_button("Reload map")) g_req_reload = 1;
    gui_separator("Info");
    sprintf(t, "%.0f FPS  |  %d objects drawn", g_fps, g_drawn); gui_label(t, COL_DIM);
    sprintf(t, "VRAM: %.0f / %.0f MB", vram_used() / 1048576.0, g_vram_total / 1048576.0); gui_label(t, COL_DIM);
    sprintf(t, "Bake: %s", g_gpu_direct ? (g_gi_gpu ? "GPU (shadows + indirect)" : "GPU shadows + CPU indirect") : "CPU"); gui_label(t, COL_DIM);
    gui_space(6);
    if (gui_button("Quit")) g_running = 0;
}

/* called INSIDE the scene (after the 3D drawing, or after the FXAA quad): no extra BeginScene/EndScene */
static void gui_frame_draw(void) {
    float W = (float)g_pp.BackBufferWidth, px = 12, py = 12, pw = 372; char t[96];
    static GV hc[900]; static int hcn, last_fps = -1, last_hint = -1, last_w = -1;
    if (!g_gui_ok) return;
    if (!g_gui_on) {
        int fps = (int)(g_fps + 0.5f), hint = (GetTickCount() - g_hint_t) < 8000, msg = g_ui_msg[0] && GetTickCount() - g_ui_msg_t < 3000;
        if (!C.hud) return;                                              /* HUD off: zero cost */
        if (!msg && fps == last_fps && hint == last_hint && (int)W == last_w && hcn) {      /* nothing changed: reuse the vertices */
            memcpy(g_gv, hc, (size_t)hcn * sizeof(GV)); g_gvn = hcn; gui_flush(); return;
        }
        gui_begin_frame();
        sprintf(t, "%d FPS", fps); gui_text(W - 12 - gui_textw(t), 8, t, COL_TXT);
        if (hint) { char h[96]; sprintf(h, "Tab: menu    %s: camera    Mouse: look", key_help()); gui_text(12, 8, h, 0xC0FFFFFF); }
        if (msg) gui_text((W - gui_textw(g_ui_msg)) * 0.5f, 40, g_ui_msg, COL_WARN);
        if (!msg && g_gvn <= 900) { memcpy(hc, g_gv, (size_t)g_gvn * sizeof(GV)); hcn = g_gvn; last_fps = fps; last_hint = hint; last_w = (int)W; }
        gui_flush(); gui_end_frame();
        return;
    }
    hcn = 0;
    gui_begin_frame();
    {
        int i; float tw;
        gui_rect(px, py, pw, g_panel_h, COL_PANEL); gui_frame(px, py, pw, g_panel_h, 0xFF3A4458);
        g_gx = px + 12; g_gy = py + 10; g_gw = pw - 24; tw = (g_gw - 5 * 3) / 6;
        for (i = 0; i < 6; i++) { if (gui_button_w(TAB_NAME[i], tw, g_gtab == i)) g_gtab = i; g_gx += tw + 3; }
        g_gx = px + 12; g_gy += GUI_ROW + 4;
        switch (g_gtab) { case 0: ui_tab_light(); break; case 1: ui_tab_camera(); break; case 2: ui_tab_display(); break; case 3: ui_tab_bake(); break; case 4: ui_tab_perf(); break; default: ui_tab_system(); }
        g_panel_h = g_gy - py + 8;
        if (g_mclick && gui_hover(px, py, pw, g_panel_h)) g_mclick = 0;
    }
    sprintf(t, "%.0f FPS", g_fps);
    gui_text(W - 12 - gui_textw(t), 8, t, COL_TXT);
    if (g_gdirty_rebake) gui_text(12, (float)g_pp.BackBufferHeight - 28, "Settings changed: click Re-bake (Bake tab)", COL_WARN);
    if (GetTickCount() - g_ui_msg_t < 3000 && g_ui_msg[0]) gui_text((W - gui_textw(g_ui_msg)) * 0.5f, 40, g_ui_msg, COL_WARN);
    gui_flush();
    gui_end_frame();
}
