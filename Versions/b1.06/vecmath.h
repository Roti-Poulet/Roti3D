/* vecmath.h - 3D math (D3D matrices: row-major, row vectors) */
#pragma once
#define PI 3.14159265358979f

typedef struct { float x, y, z; } V3;
static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vmul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 vmulv(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static inline V3 vnorm(V3 a) { float l = vlen(a); return l > 0 ? vmul(a, 1.0f / l) : a; }
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

typedef struct { float m[4][4]; } M4;
static M4 m_ident(void) { M4 r; memset(&r, 0, sizeof r); r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r; }
static M4 m_mul(M4 a, M4 b) {
    M4 r; int i, j, k;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) {
        float s = 0; for (k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
        r.m[i][j] = s;
    }
    return r;
}
static M4 m_rotx(float a) { M4 r = m_ident(); float c = cosf(a), s = sinf(a); r.m[1][1] = c; r.m[1][2] = s; r.m[2][1] = -s; r.m[2][2] = c; return r; }
static M4 m_roty(float a) { M4 r = m_ident(); float c = cosf(a), s = sinf(a); r.m[0][0] = c; r.m[0][2] = -s; r.m[2][0] = s; r.m[2][2] = c; return r; }
static M4 m_trans(float x, float y, float z) { M4 r = m_ident(); r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z; return r; }
static M4 m_persp(float fovy, float aspect, float zn, float zf) {
    M4 r; float ys = 1.0f / tanf(fovy * 0.5f);
    memset(&r, 0, sizeof r);
    r.m[0][0] = ys / aspect; r.m[1][1] = ys;
    r.m[2][2] = zf / (zf - zn); r.m[2][3] = 1; r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}
/* point (with translation) */
static V3 m_pt(V3 p, M4 m) {
    return v3(p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
              p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
              p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]);
}
/* vector (rotation only): v * M3 */
static V3 m_vec(V3 p, M4 m) {
    return v3(p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0],
              p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1],
              p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2]);
}
/* vector * transpose(M3) = inverse for a rotation */
static V3 m_vec_inv(V3 p, M4 m) {
    return v3(p.x * m.m[0][0] + p.y * m.m[0][1] + p.z * m.m[0][2],
              p.x * m.m[1][0] + p.y * m.m[1][1] + p.z * m.m[1][2],
              p.x * m.m[2][0] + p.y * m.m[2][1] + p.z * m.m[2][2]);
}
