/* gui.h - immediate-mode GUI (ImGui style) drawn INSIDE the D3D9 frame: works in windowed and fullscreen.
 *
 * - no dependency: the font atlas is generated at startup with GDI (Segoe UI / Tahoma), then everything is
 *   batched in a single vertex array and drawn with the fixed-function pipeline (DrawPrimitiveUP);
 * - ImGui-like API: gui_begin() / gui_button() / gui_slider_f() / gui_check() ... called EVERY frame, no widget object;
 * - the settings are general (same for every map): a change made in the GUI is applied to C directly and
 *   engine.ini is rewritten automatically shortly afterwards (cfg_touch / cfg_autosave).
 *
 * Tab = toggles menu mode (free cursor) / game mode (mouse look). */
#pragma once

typedef struct { float x, y, z, rhw; DWORD c; float u, v; } GV;
#define FVF_GUI (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define GUI_MAXV (6 * 6000)
#define GUI_CW 16          /* atlas cell */
#define GUI_CH 20
static GV g_gv[GUI_MAXV]; static int g_gvn;
static IDirect3DTexture9* g_gtex; static int g_gtw = 256, g_gth = 384;
static float g_gadv[256]; static int g_gui_ok;

/* ---- input (filled by the wndproc) ---- */
static int g_gui_on;                        /* 1 = menu displayed + free cursor */
static int g_mx, g_my, g_mdown, g_mclick, g_mwheel;
static unsigned g_gid, g_gactive, g_ghot;   /* widget under construction / dragged / hovered */
static float g_gx, g_gy, g_gw;              /* layout cursor */
static int g_gtab, g_gdirty_rebake;         /* g_gdirty_rebake: a setting that needs a re-bake has changed */

/* ---------------------------------------------------------------- font atlas */
static void gui_init(void) {
    BITMAPINFO bi; void* bits = NULL; HDC dc; HBITMAP bm, obm; HFONT font, ofont; int ch, i; uint32_t* px; D3DLOCKED_RECT lr; int y;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = g_gtw; bi.bmiHeader.biHeight = -g_gth;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { DeleteDC(dc); return; }
    obm = (HBITMAP)SelectObject(dc, bm);
    font = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    ofont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(0, 0, 0)); SetTextColor(dc, RGB(255, 255, 255));
    memset(bits, 0, (size_t)g_gtw * g_gth * 4);
    for (ch = 32; ch < 256; ch++) {
        char c = (char)ch; SIZE sz; int cx = (ch - 32) % 16, cy = (ch - 32) / 16;
        GetTextExtentPoint32A(dc, &c, 1, &sz);
        g_gadv[ch] = (float)(sz.cx < GUI_CW - 2 ? sz.cx : GUI_CW - 2);
        TextOutA(dc, cx * GUI_CW + 1, cy * GUI_CH + 1, &c, 1);
    }
    GdiFlush();
    px = (uint32_t*)bits;
    for (i = 0; i < g_gtw * g_gth; i++) { uint32_t a = (px[i] >> 8) & 255; px[i] = 0x00FFFFFFu | (a << 24); }   /* alpha = coverage (green channel) */
    for (y = g_gth - 8; y < g_gth; y++) for (i = 0; i < 8; i++) px[y * g_gtw + i] = 0xFFFFFFFFu;                      /* white block: solid rectangles */
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(g_dev, g_gtw, g_gth, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_gtex, NULL))) {
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_gtex, 0, &lr, NULL, 0))) {
            for (y = 0; y < g_gth; y++) memcpy((char*)lr.pBits + (size_t)y * lr.Pitch, px + (size_t)y * g_gtw, (size_t)g_gtw * 4);
            IDirect3DTexture9_UnlockRect(g_gtex, 0); g_gui_ok = 1;
        }
    }
    SelectObject(dc, ofont); DeleteObject(font); SelectObject(dc, obm); DeleteObject(bm); DeleteDC(dc);
}

/* ---------------------------------------------------------------- drawing */
static void gui_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, DWORD c) {
    GV* v; if (g_gvn + 6 > GUI_MAXV) return;
    v = &g_gv[g_gvn]; g_gvn += 6;
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
#define GVSET(i, X, Y, U, V) v[i].x = X; v[i].y = Y; v[i].z = 0; v[i].rhw = 1; v[i].c = c; v[i].u = U; v[i].v = V;
    GVSET(0, x0, y0, u0, v0) GVSET(1, x1, y0, u1, v0) GVSET(2, x0, y1, u0, v1)
    GVSET(3, x1, y0, u1, v0) GVSET(4, x1, y1, u1, v1) GVSET(5, x0, y1, u0, v1)
#undef GVSET
}
static void gui_rect(float x, float y, float w, float h, DWORD c) {
    float u = 4.0f / g_gtw, v = (g_gth - 4.0f) / g_gth; gui_quad(x, y, x + w, y + h, u, v, u, v, c);
}
static void gui_frame(float x, float y, float w, float h, DWORD c) {
    gui_rect(x, y, w, 1, c); gui_rect(x, y + h - 1, w, 1, c); gui_rect(x, y, 1, h, c); gui_rect(x + w - 1, y, 1, h, c);
}
/* source in UTF-8: U+0080..U+00FF are converted to Latin-1 (accents) */
static int gui_glyph(const unsigned char** p) {
    int c = **p; (*p)++;
    if ((c == 0xC3 || c == 0xC2) && **p) { int c2 = **p; (*p)++; return c == 0xC3 ? c2 + 0x40 : c2; }
    return c;
}
static float gui_textw(const char* s) {
    const unsigned char* p = (const unsigned char*)s; float w = 0;
    while (*p) { int c = gui_glyph(&p); if (c >= 32) w += g_gadv[c]; }
    return w;
}
static void gui_text(float x, float y, const char* s, DWORD c) {
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        int ch = gui_glyph(&p), cx, cy; float w;
        if (ch < 32) continue;
        cx = (ch - 32) % 16; cy = (ch - 32) / 16; w = g_gadv[ch];
        if (ch != ' ') gui_quad(x, y, x + w + 2, y + GUI_CH, (float)(cx * GUI_CW) / g_gtw, (float)(cy * GUI_CH) / g_gth,
                                (float)(cx * GUI_CW + w + 2) / g_gtw, (float)(cy * GUI_CH + GUI_CH) / g_gth, c);
        x += w;
    }
}

static void gui_flush(void) {
    if (!g_gvn || !g_gui_ok) { g_gvn = 0; return; }
    IDirect3DDevice9_SetVertexShader(g_dev, NULL); IDirect3DDevice9_SetPixelShader(g_dev, NULL);
    IDirect3DDevice9_SetFVF(g_dev, FVF_GUI);
    RS(D3DRS_ZENABLE, D3DZB_FALSE); RS(D3DRS_ZWRITEENABLE, FALSE); RS(D3DRS_CULLMODE, D3DCULL_NONE); RS(D3DRS_SRGBWRITEENABLE, FALSE);
    RS(D3DRS_LIGHTING, FALSE); RS(D3DRS_ALPHABLENDENABLE, TRUE); RS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); RS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    RS(D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9*)g_gtex);
    ts(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); ts(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    ts(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); ts(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); ts(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    ts(0, D3DTSS_TEXCOORDINDEX, 0); ts(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE); ts(1, D3DTSS_COLOROP, D3DTOP_DISABLE); ts(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    ss(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); ss(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    ss(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(0, D3DSAMP_SRGBTEXTURE, FALSE);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, (UINT)(g_gvn / 3), g_gv, sizeof(GV));
    RS(D3DRS_ALPHABLENDENABLE, FALSE);
    g_gvn = 0;
}

/* ---------------------------------------------------------------- widgets */
#define COL_BG    0xE0161A22u
#define COL_PANEL 0xE61C212Bu
#define COL_BTN   0xFF2C3442u
#define COL_HOT   0xFF3C4A63u
#define COL_ACT   0xFF4D8EF0u
#define COL_TXT   0xFFE6E9EFu
#define COL_DIM   0xFF9AA3B2u
#define COL_WARN  0xFFF0B04Du
#define GUI_ROW 24.0f

static int gui_hover(float x, float y, float w, float h) { return g_mx >= x && g_my >= y && g_mx < x + w && g_my < y + h; }
static unsigned gui_id(void) { return ++g_gid; }

static void gui_begin_frame(void) { g_gid = 0; g_gvn = 0; }
static void gui_end_frame(void) {
    if (!g_mdown) g_gactive = 0;
    g_mclick = 0; g_mwheel = 0;
}

static void gui_label(const char* s, DWORD c) { gui_text(g_gx, g_gy + 2, s, c); g_gy += GUI_ROW; }
static void gui_space(float h) { g_gy += h; }
static void gui_separator(const char* s) {
    float w = gui_textw(s);
    gui_text(g_gx, g_gy + 2, s, COL_ACT); gui_rect(g_gx + w + 8, g_gy + 12, g_gw - w - 8, 1, 0xFF3A4458); g_gy += GUI_ROW;
}

static int gui_button_w(const char* s, float w, int selected) {
    unsigned id = gui_id(); int hov = gui_hover(g_gx, g_gy, w, GUI_ROW - 3), clicked = 0;
    if (hov) g_ghot = id;
    if (hov && g_mclick) { g_gactive = id; clicked = 1; }
    gui_rect(g_gx, g_gy, w, GUI_ROW - 3, selected ? COL_ACT : (hov ? COL_HOT : COL_BTN));
    gui_text(g_gx + (w - gui_textw(s)) * 0.5f, g_gy + 1, s, COL_TXT);
    return clicked;
}
static int gui_button(const char* s) { int r = gui_button_w(s, g_gw, 0); g_gy += GUI_ROW; return r; }

static int gui_check(const char* s, int* v, int needs_bake) {
    unsigned id = gui_id(); int hov = gui_hover(g_gx, g_gy, g_gw, GUI_ROW - 3), ch = 0;
    if (hov && g_mclick) { *v = !*v; ch = 1; if (needs_bake) g_gdirty_rebake = 1; }
    gui_rect(g_gx, g_gy + 2, 16, 16, hov ? COL_HOT : COL_BTN);
    if (*v) gui_rect(g_gx + 4, g_gy + 6, 8, 8, COL_ACT);
    gui_text(g_gx + 24, g_gy + 1, s, needs_bake ? COL_WARN : COL_TXT);
    (void)id; g_gy += GUI_ROW;
    return ch;
}

/* slider: label on the left (42 %), track on the right. Float or int. Returns 1 if the value changed. */
static int gui_slider(const char* s, float* f, int* iv, float lo, float hi, int needs_bake) {
    unsigned id = gui_id(); float lw = g_gw * 0.42f, tx = g_gx + lw, tw = g_gw - lw, v = iv ? (float)*iv : *f, t; int ch = 0; char buf[48];
    int hov = gui_hover(tx, g_gy, tw, GUI_ROW - 3);
    if (hov && g_mclick) g_gactive = id;
    if (g_gactive == id && g_mdown) {
        t = clampf(((float)g_mx - tx) / tw, 0.0f, 1.0f); v = lo + (hi - lo) * t;
        if (iv) { int nv = (int)(v + 0.5f); if (nv != *iv) { *iv = nv; ch = 1; } }
        else if (v != *f) { *f = v; ch = 1; }
    }
    if (hov && g_mwheel) {                                                  /* wheel: fine adjustment */
        float step = iv ? 1.0f : (hi - lo) * 0.01f; v = clampf(v + step * (g_mwheel > 0 ? 1 : -1), lo, hi);
        if (iv) *iv = (int)(v + 0.5f); else *f = v; ch = 1; g_mwheel = 0;
    }
    if (ch && needs_bake) g_gdirty_rebake = 1;
    v = iv ? (float)*iv : *f; t = clampf((v - lo) / (hi - lo), 0.0f, 1.0f);
    gui_text(g_gx, g_gy + 1, s, needs_bake ? COL_WARN : COL_TXT);
    gui_rect(tx, g_gy + 1, tw, GUI_ROW - 5, hov || g_gactive == id ? COL_HOT : COL_BTN);
    gui_rect(tx, g_gy + 1, tw * t, GUI_ROW - 5, 0xB04D8EF0u);
    if (iv) sprintf(buf, "%d", *iv); else sprintf(buf, fabsf(v) < 0.1f && v != 0 ? "%.4f" : (fabsf(v) < 10 ? "%.2f" : "%.1f"), v);
    gui_text(tx + (tw - gui_textw(buf)) * 0.5f, g_gy + 1, buf, COL_TXT);
    g_gy += GUI_ROW;
    return ch;
}

/* ---------------------------------------------------------------- engine.ini auto-save
 * cfg_touch() = "a setting changed". The file is written 0.5 s after the LAST change, and never while the mouse button
 * is held (dragging a slider changes the value every frame: one write at the end, not hundreds). */
static void ui_msg(const char* s);                 /* defined in ui.h */
static int g_cfg_dirty; static DWORD g_cfg_t;
static void cfg_touch(void) { g_cfg_dirty = 1; g_cfg_t = GetTickCount(); }
static void cfg_autosave(int force) {
    if (!g_cfg_dirty) return;
    if (!force && (g_mdown || GetTickCount() - g_cfg_t < 500)) return;
    g_cfg_dirty = 0;
    if (!cfg_save("engine.ini")) ui_msg("Failed to write engine.ini");
}

/* slider / checkbox bound to a field of C. Returns 1 if the value changed (and schedules the auto-save).
 * needs_bake = 1: orange label, the "Recalculer" button of the Bake tab is needed to see the change. */
static int gui_cf(const char* label, float* f, float lo, float hi, int needs_bake) {
    if (gui_slider(label, f, NULL, lo, hi, needs_bake)) { cfg_touch(); return 1; } return 0;
}
static int gui_ci(const char* label, int* v, int lo, int hi, int needs_bake) {
    if (gui_slider(label, NULL, v, (float)lo, (float)hi, needs_bake)) { cfg_touch(); return 1; } return 0;
}
static int gui_cb(const char* label, int* v, int needs_bake) {
    if (gui_check(label, v, needs_bake)) { cfg_touch(); return 1; } return 0;
}
