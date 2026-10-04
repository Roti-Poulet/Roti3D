/* ui.h - interaction: right-click on the lamp (color/intensity menu), keyboard movement */
#pragma once
#include <commctrl.h>

static HWND g_menu, g_sl[5], g_lb[5];
static int g_need_refresh, g_pending_rebake; static DWORD g_pending_t;
static float g_lamp_lim[6] = { -13.0f, 13.0f, 1.2f, 16.0f, -6.5f, 6.5f };   /* x0,x1,y0,y1,z0,z1: set per map */

static const char* SL_NAME[5] = { "Red", "Green", "Blue", "Intensity", "Size (shadows)" };
static const int   SL_MAX[5]  = { 100, 100, 100, 400, 200 };

static void ui_labels(void) {
    char t[64]; int i; int v[5];
    for (i = 0; i < 5; i++) v[i] = (int)SendMessage(g_sl[i], TBM_GETPOS, 0, 0);
    for (i = 0; i < 5; i++) { sprintf(t, "%s : %d", SL_NAME[i], v[i]); SetWindowTextA(g_lb[i], t); }
    if (g_menu) { char n[80]; sprintf(n, "Light %d/%d  (keys 1-%d)", g_lsel + 1, g_nlight, g_nlight); SetWindowTextA(g_menu, n); }
}

/* The sliders edit the selected light (keys 1..6). */
static void ui_read_sliders(void) {
    float r = (float)SendMessage(g_sl[0], TBM_GETPOS, 0, 0) / 100.0f, g = (float)SendMessage(g_sl[1], TBM_GETPOS, 0, 0) / 100.0f,
          b = (float)SendMessage(g_sl[2], TBM_GETPOS, 0, 0) / 100.0f, in = (float)SendMessage(g_sl[3], TBM_GETPOS, 0, 0),
          rad = (float)SendMessage(g_sl[4], TBM_GETPOS, 0, 0) / 100.0f;
    Light* lp = &g_lightset[g_lsel];
    if (g_lsel == 0) { C.light_r = r; C.light_g = g; C.light_b = b; C.light_intensity = in; }
    lp->col = v3(r * in, g * in, b * in);
    if (fabsf(rad - C.light_radius) > 1e-4f) {                     /* the radius changes the bake duration */
        C.light_radius = rad;
        { int l; for (l = 0; l < g_nlight; l++) g_lightset[l].radius = rad; }
        g_pending_rebake = 1; g_pending_t = GetTickCount();
    }
    /* color / intensity: recomposed from the masks, no re-bake */
    g_need_refresh = 1;
    ui_labels();
}

static void ui_sync_sliders(void) {
    Light* lp = &g_lightset[g_lsel < g_nlight ? g_lsel : 0];
    float m = fmaxf(lp->col.x, fmaxf(lp->col.y, lp->col.z));
    if (m < 1e-4f) m = 1.0f;
    SendMessage(g_sl[0], TBM_SETPOS, TRUE, (LPARAM)(lp->col.x / m * 100 + 0.5f));
    SendMessage(g_sl[1], TBM_SETPOS, TRUE, (LPARAM)(lp->col.y / m * 100 + 0.5f));
    SendMessage(g_sl[2], TBM_SETPOS, TRUE, (LPARAM)(lp->col.z / m * 100 + 0.5f));
    SendMessage(g_sl[3], TBM_SETPOS, TRUE, (LPARAM)(m + 0.5f));
    SendMessage(g_sl[4], TBM_SETPOS, TRUE, (LPARAM)(C.light_radius * 100 + 0.5f));
    EnableWindow(g_sl[4], lp->shape ? FALSE : TRUE);          /* ceiling light: the size is that of the visible panel */
    ui_labels();
}

static LRESULT CALLBACK menu_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HSCROLL) { ui_read_sliders(); return 0; }
    if (m == WM_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void ui_create_menu(HINSTANCE hi) {
    INITCOMMONCONTROLSEX ic; WNDCLASSA wc; int i; HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT); RECT r = { 0, 0, 330, 230 };
    ic.dwSize = sizeof ic; ic.dwICC = ICC_BAR_CLASSES; InitCommonControlsEx(&ic);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = menu_proc; wc.hInstance = hi; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "D3D9LampMenu"; RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    g_menu = CreateWindowExA(WS_EX_TOOLWINDOW, "D3D9LampMenu", "Lamp", WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0,
                             r.right - r.left, r.bottom - r.top, g_hwnd, NULL, hi, NULL);
    for (i = 0; i < 5; i++) {
        g_lb[i] = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 10, 14 + i * 42, 130, 20, g_menu, NULL, hi, NULL);
        g_sl[i] = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 145, 8 + i * 42, 175, 30, g_menu, NULL, hi, NULL);
        SendMessage(g_sl[i], TBM_SETRANGE, TRUE, MAKELONG(0, SL_MAX[i]));
        SendMessage(g_lb[i], WM_SETFONT, (WPARAM)font, TRUE);
    }
    ui_sync_sliders();
}

static void ui_show_menu(int cx, int cy) {
    POINT p; p.x = cx; p.y = cy; ClientToScreen(g_hwnd, &p);
    ui_sync_sliders();
    SetWindowPos(g_menu, HWND_TOP, p.x + 12, p.y + 12, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
}

/* is the mouse over the lamp? (screen ray -> selection sphere, in camera space) */
static int ui_pick_lamp(int mx, int my) {
    float W = (float)g_pp.BackBufferWidth, H = (float)g_pp.BackBufferHeight;
    float ty = tanf(C.fov * PI / 360.0f), tx = ty * W / H;
    float nx = ((float)mx / W) * 2 - 1, ny = 1 - ((float)my / H) * 2;
    V3 d = vnorm(v3(nx * tx, ny * ty, 1.0f)), c = m_pt(m_pt(g_light, g_world), g_view);
    float t = vdot(c, d), d2;
    if (t < 0) return 0;
    d2 = vdot(c, c) - t * t;
    return d2 < 0.7f * 0.7f;
}

/* arrows: move in the plane of the platform, relative to the screen; PgUp/PgDn: height */
static V3 flat_norm(V3 v) { v.y = 0; return vnorm(v); }
static int ui_move_lamp(float dt) {
    V3 right, up, mv = { 0, 0, 0 }; float step = C.lamp_speed * dt;
    if (GetForegroundWindow() != g_hwnd) return 0;
    right = flat_norm(m_vec_inv(v3(1, 0, 0), g_world));
    up = m_vec_inv(v3(0, 1, 0), g_world); up.y = 0;
    if (vlen(up) < 0.25f) up = m_vec_inv(v3(0, 0, 1), g_world);
    up = flat_norm(up);
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) mv = vadd(mv, right);
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) mv = vsub(mv, right);
    if (GetAsyncKeyState(VK_UP)    & 0x8000) mv = vadd(mv, up);
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) mv = vsub(mv, up);
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) mv.y += 1;
    if (GetAsyncKeyState(VK_NEXT)  & 0x8000) mv.y -= 1;
    {   int l, ch = 0;                                        /* keys 1..6: edited light */
        for (l = 0; l < g_nlight && l < 9; l++) if (GetAsyncKeyState('1' + l) & 1) ch = l + 1;
        if (ch && ch - 1 != g_lsel) { g_lsel = ch - 1; ui_sync_sliders(); }
    }
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
