/* sm2asm.h - minimal vs_2_0 / ps_2_0 assembler (text -> D3D9 bytecode), without d3dx9.
 *
 * The GPU bake shaders are written in assembly and assembled HERE, at startup: nothing depends on
 * d3dx9_xx.dll (absent on many machines) and the instruction count is exactly the one written
 * (the HLSL compilers differ a lot: 4-tap shadows = ~40 instructions by hand, 110 with some compilers).
 *
 * Supported: dcl / dcl_position|normal|texcoordN|color|cube|2d, def, mov add sub mul mad dp3 dp4 rcp rsq
 *            frc min max abs nrm texld, modifiers _sat, source -r, swizzles/masks, registers r v c t s oPos oT oD oC.
 * Rule respected by the shaders written for it: ONE constant register read per instruction. */
#pragma once

typedef struct { const char* name; int op, nsrc, dst; } SmOp;
static const SmOp SM_OPS[] = {
    { "mov", 1, 1, 1 }, { "add", 2, 2, 1 }, { "sub", 3, 2, 1 }, { "mad", 4, 3, 1 }, { "mul", 5, 2, 1 }, { "rcp", 6, 1, 1 }, { "rsq", 7, 1, 1 },
    { "dp3", 8, 2, 1 }, { "dp4", 9, 2, 1 }, { "min", 10, 2, 1 }, { "max", 11, 2, 1 }, { "frc", 19, 1, 1 }, { "abs", 35, 1, 1 }, { "nrm", 36, 1, 1 },
    { "texld", 66, 2, 1 }, { NULL, 0, 0, 0 }
};

/* register: type (D3DSPR_*), number, flags */
typedef struct { int type, num, mask, swz, neg; } SmReg;

static int sm_parse_reg(const char* s, int is_vs, SmReg* r) {
    char name[16]; int n = 0, k;
    r->neg = 0; r->mask = 0xF; r->swz = 0xE4; r->type = 0; r->num = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { r->neg = 1; s++; }
    while (isalpha((unsigned char)*s) && n < 15) name[n++] = *s++;
    name[n] = 0;
    if (!strcmp(name, "oPos")) { r->type = 4; r->num = 0; }
    else if (!strcmp(name, "oFog")) { r->type = 4; r->num = 1; }
    else if (!strcmp(name, "oDepth")) { r->type = 9; r->num = 0; }
    else {
        int ty;
        if (!strcmp(name, "r")) ty = 0; else if (!strcmp(name, "v")) ty = 1; else if (!strcmp(name, "c")) ty = 2;
        else if (!strcmp(name, "t")) ty = 3; else if (!strcmp(name, "s")) ty = 10;
        else if (!strcmp(name, "oD")) ty = 5; else if (!strcmp(name, "oT")) ty = 6; else if (!strcmp(name, "oC")) ty = 8;
        else return 0;
        (void)is_vs;
        if (!isdigit((unsigned char)*s)) return 0;
        r->type = ty; r->num = atoi(s);
        while (isdigit((unsigned char)*s)) s++;
    }
    if (*s == '.') {                                   /* mask (destination) or swizzle (source): both are parsed, the caller picks */
        int comp[4], nc = 0; s++;
        while (*s && nc < 4 && strchr("xyzwrgba", *s)) {
            int c = *s == 'x' || *s == 'r' ? 0 : (*s == 'y' || *s == 'g' ? 1 : (*s == 'z' || *s == 'b' ? 2 : 3)); comp[nc++] = c; s++;
        }
        if (nc == 0) return 0;
        r->mask = 0; for (k = 0; k < nc; k++) r->mask |= 1 << comp[k];
        for (k = nc; k < 4; k++) comp[k] = comp[nc - 1];
        r->swz = comp[0] | (comp[1] << 2) | (comp[2] << 4) | (comp[3] << 6);
    }
    return 1;
}
static DWORD sm_regtok(const SmReg* r) { return 0x80000000u | ((DWORD)(r->type & 7) << 28) | ((DWORD)((r->type >> 3) & 3) << 11) | (DWORD)r->num; }
static DWORD sm_dsttok(const SmReg* r, int sat) { return sm_regtok(r) | ((DWORD)r->mask << 16) | (sat ? (1u << 20) : 0u); }
static DWORD sm_srctok(const SmReg* r) { return sm_regtok(r) | ((DWORD)r->swz << 16) | (r->neg ? (1u << 24) : 0u); }

/* returns malloc'd bytecode (DWORD tokens, *ntok) or NULL (message in err) */
static DWORD* sm_assemble(const char* text, int* ntok, char* err, size_t errsz) {
    DWORD* out = (DWORD*)malloc(4096 * sizeof(DWORD)); int n = 0, is_vs = 0, line = 0; const char* p = text;
    #define SM_FAIL(...) do { if (err) _snprintf(err, errsz, __VA_ARGS__); free(out); return NULL; } while (0)
    while (*p) {
        char ln[256], *c, *tok[8]; int nt = 0, len = 0, i;
        while (*p && *p != '\n' && len < 255) ln[len++] = *p++;
        if (*p == '\n') p++;
        ln[len] = 0; line++;
        if ((c = strchr(ln, ';')) != NULL) *c = 0;
        if ((c = strstr(ln, "//")) != NULL) *c = 0;
        c = ln; while (*c == ' ' || *c == '\t' || *c == '\r') c++;
        if (!*c) continue;
        /* opcode = first word; operands separated by commas (spaces inside an operand are not allowed) */
        { char* q = c; while (*q && *q != ' ' && *q != '\t') q++; tok[nt++] = c; if (*q) { *q++ = 0; } c = q; }
        while (*c) {
            char* q; while (*c == ' ' || *c == '\t' || *c == '\r' || *c == ',') c++;
            if (!*c) break;
            q = c; while (*q && *q != ',' && *q != ' ' && *q != '\t' && *q != '\r') q++;
            if (nt < 8) tok[nt++] = c;
            if (*q) *q++ = 0;
            c = q;
        }
        if (!strcmp(tok[0], "vs_2_0") || !strcmp(tok[0], "ps_2_0")) { is_vs = tok[0][0] == 'v'; out[n++] = is_vs ? 0xFFFE0200u : 0xFFFF0200u; continue; }
        if (n == 0) SM_FAIL("line %d: version (vs_2_0 / ps_2_0) expected first", line);
        if (!strncmp(tok[0], "dcl", 3)) {
            SmReg d; DWORD usage = 0x80000000u; const char* u = tok[0] + 3; int idx = 0;
            if (nt < 2 || !sm_parse_reg(tok[1], is_vs, &d)) SM_FAIL("line %d: dcl: bad register", line);
            if (*u == '_') {
                u++;
                if (!strncmp(u, "position", 8)) { usage |= 0; u += 8; }
                else if (!strncmp(u, "normal", 6)) { usage |= 3; u += 6; }
                else if (!strncmp(u, "texcoord", 8)) { usage |= 5; u += 8; }
                else if (!strncmp(u, "color", 5)) { usage |= 10; u += 5; }
                else if (!strncmp(u, "cube", 4)) { usage |= (3u << 27); u += 4; d.mask = 0xF; }
                else if (!strncmp(u, "2d", 2)) { usage |= (2u << 27); u += 2; d.mask = 0xF; }
                else SM_FAIL("line %d: dcl usage unknown", line);
                if (isdigit((unsigned char)*u)) idx = atoi(u);
                usage |= (DWORD)idx << 16;
            }
            out[n++] = 31u | (2u << 24); out[n++] = usage; out[n++] = sm_dsttok(&d, 0);
            continue;
        }
        if (!strcmp(tok[0], "def")) {
            SmReg d; int k;
            if (nt != 6 || !sm_parse_reg(tok[1], is_vs, &d) || d.type != 2) SM_FAIL("line %d: def cN, x, y, z, w", line);
            out[n++] = 81u | (5u << 24); out[n++] = sm_regtok(&d) | (0xFu << 16);
            for (k = 0; k < 4; k++) { float f = (float)atof(tok[2 + k]); memcpy(&out[n++], &f, 4); }
            continue;
        }
        {   char mn[24]; const SmOp* op = NULL; int sat = 0; SmReg r[4]; DWORD* hdr;
            strncpy(mn, tok[0], 23); mn[23] = 0;
            { char* u = strstr(mn, "_sat"); if (u) { *u = 0; sat = 1; } }
            for (i = 0; SM_OPS[i].name; i++) if (!strcmp(SM_OPS[i].name, mn)) { op = &SM_OPS[i]; break; }
            if (!op) SM_FAIL("line %d: unknown instruction '%s'", line, tok[0]);
            if (nt != 2 + op->nsrc) SM_FAIL("line %d: '%s' expects %d operands", line, mn, 1 + op->nsrc);
            for (i = 0; i < 1 + op->nsrc; i++) if (!sm_parse_reg(tok[1 + i], is_vs, &r[i])) SM_FAIL("line %d: bad operand '%s'", line, tok[1 + i]);
            /* SM2+: D3DSIO_SUB does not exist for the runtime (fxc emits add + negated source) */
            if (op->op == 3) r[2].neg ^= 1;
            hdr = &out[n++]; *hdr = (DWORD)(op->op == 3 ? 2 : op->op) | ((DWORD)(1 + op->nsrc) << 24);
            out[n++] = sm_dsttok(&r[0], sat);
            for (i = 1; i <= op->nsrc; i++) out[n++] = sm_srctok(&r[i]);
        }
        if (n > 4000) SM_FAIL("shader too long");
    }
    out[n++] = 0x0000FFFFu;
    *ntok = n;
    return out;
    #undef SM_FAIL
}
