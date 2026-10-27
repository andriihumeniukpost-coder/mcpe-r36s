/* libGLESv1_CM.so replacement: OpenGL ES 1.1 fixed-function pipeline emulated on top of the host's
 * GLES2 context (old Minecraft PE builds such as 0.8.1 import glMatrixMode/glVertexPointer/... directly).
 *
 * What is emulated: the three matrix stacks, client-array pointers (VBO or client memory), texturing
 * (MODULATE only), linear/exp/exp2 fog, alpha test, up to 4 directional/positional lights with
 * COLOR_MATERIAL.  Everything is done with a handful of generated GLSL ES 1.00 program variants
 * (one per combination of the states that matter), so the GPU never runs code it does not need.
 *
 * The game is soft-float: every exported function that takes a float is declared SOFT (base AAPCS).
 * All real GL calls go through function pointers fetched from the host's SDL/EGL context. */
#define _GNU_SOURCE
#include "android_min.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#ifdef GL1_TEST
extern void *SDL_GL_GetProcAddress(const char *);
#else
#include <SDL2/SDL.h>
#endif

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei, GLfixed;
typedef unsigned char GLboolean;
typedef float GLfloat;

/* ---- the few enums we need ---- */
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_TEXTURE_2D 0x0DE1
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FIXED 0x140C
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

enum { A_POS, A_COL, A_NRM, A_TEX, A_N };      /* attribute slots == generic attribute locations */

/* ------------------------------------------------------------------------- real GL entry points */
#define RGL(ret, name, args) static ret (*r_##name) args;
RGL(void, glEnable, (GLenum)) RGL(void, glDisable, (GLenum))
RGL(void, glBindBuffer, (GLenum, GLuint)) RGL(void, glBufferData, (GLenum, long, const void *, GLenum))
RGL(void, glDeleteBuffers, (GLsizei, const GLuint *)) RGL(void, glGenBuffers, (GLsizei, GLuint *))
RGL(void, glBindTexture, (GLenum, GLuint)) RGL(void, glDeleteTextures, (GLsizei, const GLuint *)) RGL(void, glGenTextures, (GLsizei, GLuint *))
RGL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
RGL(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
RGL(void, glTexParameteri, (GLenum, GLenum, GLint)) RGL(void, glGenerateMipmap, (GLenum))
RGL(void, glBlendFunc, (GLenum, GLenum)) RGL(void, glClear, (GLbitfield)) RGL(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
RGL(void, glClearDepthf, (GLfloat))
RGL(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) RGL(void, glCullFace, (GLenum))
RGL(void, glDepthFunc, (GLenum)) RGL(void, glDepthMask, (GLboolean)) RGL(void, glDepthRangef, (GLfloat, GLfloat))
RGL(GLenum, glGetError, (void)) RGL(const unsigned char *, glGetString, (GLenum)) RGL(void, glGetFloatv, (GLenum, GLfloat *))
RGL(void, glGetIntegerv, (GLenum, GLint *))
RGL(void, glLineWidth, (GLfloat)) RGL(void, glPolygonOffset, (GLfloat, GLfloat))
RGL(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
RGL(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) RGL(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
RGL(void, glStencilFunc, (GLenum, GLint, GLuint)) RGL(void, glStencilMask, (GLuint)) RGL(void, glStencilOp, (GLenum, GLenum, GLenum))
RGL(void, glActiveTexture, (GLenum)) RGL(void, glFinish, (void)) RGL(void, glFlush, (void))
RGL(void, glDrawArrays, (GLenum, GLint, GLsizei)) RGL(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))
RGL(GLuint, glCreateShader, (GLenum)) RGL(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))
RGL(void, glCompileShader, (GLuint)) RGL(void, glGetShaderiv, (GLuint, GLenum, GLint *)) RGL(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *))
RGL(void, glDeleteShader, (GLuint))
RGL(GLuint, glCreateProgram, (void)) RGL(void, glAttachShader, (GLuint, GLuint)) RGL(void, glBindAttribLocation, (GLuint, GLuint, const char *))
RGL(void, glLinkProgram, (GLuint)) RGL(void, glGetProgramiv, (GLuint, GLenum, GLint *)) RGL(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))
RGL(void, glUseProgram, (GLuint)) RGL(GLint, glGetUniformLocation, (GLuint, const char *))
RGL(void, glUniform1i, (GLint, GLint)) RGL(void, glUniform1f, (GLint, GLfloat)) RGL(void, glUniform1fv, (GLint, GLsizei, const GLfloat *))
RGL(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) RGL(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))
RGL(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) RGL(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))
RGL(void, glEnableVertexAttribArray, (GLuint)) RGL(void, glDisableVertexAttribArray, (GLuint))
RGL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) RGL(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))

static void (*shim_gl_viewport)(int, int, int, int), (*shim_gl_scissor)(int, int, int, int);
static int gl_loaded;
static void gl_load(void) {
#define L(n) r_##n = SDL_GL_GetProcAddress(#n)
    L(glEnable); L(glDisable); L(glBindBuffer); L(glBufferData); L(glDeleteBuffers); L(glGenBuffers);
    L(glBindTexture); L(glDeleteTextures); L(glGenTextures); L(glTexImage2D); L(glTexSubImage2D); L(glTexParameteri); L(glGenerateMipmap);
    L(glBlendFunc); L(glClear); L(glClearColor); L(glClearDepthf); L(glColorMask); L(glCullFace); L(glDepthFunc); L(glDepthMask); L(glDepthRangef);
    L(glGetError); L(glGetString); L(glGetFloatv); L(glGetIntegerv); L(glLineWidth); L(glPolygonOffset); L(glReadPixels);
    L(glScissor); L(glViewport); L(glStencilFunc); L(glStencilMask); L(glStencilOp); L(glActiveTexture); L(glFinish); L(glFlush);
    L(glDrawArrays); L(glDrawElements);
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv); L(glGetShaderInfoLog); L(glDeleteShader);
    L(glCreateProgram); L(glAttachShader); L(glBindAttribLocation); L(glLinkProgram); L(glGetProgramiv); L(glGetProgramInfoLog);
    L(glUseProgram); L(glGetUniformLocation); L(glUniform1i); L(glUniform1f); L(glUniform1fv); L(glUniform3fv); L(glUniform4fv);
    L(glUniformMatrix3fv); L(glUniformMatrix4fv);
    L(glEnableVertexAttribArray); L(glDisableVertexAttribArray); L(glVertexAttribPointer); L(glVertexAttrib4f);
#undef L
    { void *e = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);       /* our shim EGL (loaded RTLD_LOCAL by the host) */
      if (e) { shim_gl_viewport = dlsym(e, "shim_gl_viewport"); shim_gl_scissor = dlsym(e, "shim_gl_scissor"); } }
    gl_loaded = 1;
    fprintf(stderr, "[gles1] fixed-function emulation active (GLES1 -> GLES2)\n");
}
#define NEED() do { if (!gl_loaded) gl_load(); } while (0)

/* host render-scale hooks (libEGL.so): viewport/scissor must be scaled while rendering into the small target */


/* ------------------------------------------------------------------------- matrices (column-major) */
static void m_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void m_mul(float *o, const float *a, const float *b) {      /* o = a * b */
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    memcpy(o, t, 64);
}
static void m_vec(const float *m, const float *v, float *o) {       /* o = m * v */
    float t[4];
    for (int r = 0; r < 4; r++) t[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    memcpy(o, t, 16);
}
/* inverse-transpose of the upper-left 3x3 (normal matrix); falls back to the plain 3x3 if singular */
static void m_normal(const float *m, float *o) {
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-12f) { o[0] = a; o[1] = d; o[2] = g; o[3] = b; o[4] = e; o[5] = h; o[6] = c; o[7] = f; o[8] = i; return; }
    float id = 1.0f / det;
    /* inverse-transpose == cofactor matrix / det (column-major output) */
    o[0] = A * id;                   o[3] = B * id;                   o[6] = C * id;
    o[1] = (c * h - b * i) * id;     o[4] = (a * i - c * g) * id;     o[7] = (b * g - a * h) * id;
    o[2] = (b * f - c * e) * id;     o[5] = (c * d - a * f) * id;     o[8] = (a * e - b * d) * id;
}

/* ------------------------------------------------------------------------- state */
#define STK 32
static struct {
    float mv[STK][16], pr[STK][16], tx[STK][16]; int mvd, prd, txd; int mode;      /* mode 0 = MODELVIEW, 1 = PROJECTION, 2 = TEXTURE */
    float color[4], normal[3];
    int tex2d, fog, lighting, colmat, atest, lon[4];
    float lpos[4][4], lamb[4][4], ldif[4][4], scene_amb[4], mat_amb[4], mat_dif[4];
    int fog_mode; float fog_start, fog_end, fog_density, fog_color[4];
    int afunc; float aref;
    struct { int en, size, type, stride; const void *ptr; GLuint buf; } va[A_N];
    GLuint abuf_app, abuf_cur, cur_tex, cur_prog;
    int attr_en[A_N]; float cattr[A_N][4]; int cattr_valid[A_N];
    unsigned v_mv, v_pr, v_tx, v_fog, v_alpha, v_light;
    unsigned char gen_mip[8192];
    int inited;
} S;

static void st_init(void) {
    if (S.inited) return;
    S.inited = 1;
    m_ident(S.mv[0]); m_ident(S.pr[0]); m_ident(S.tx[0]);
    S.color[0] = S.color[1] = S.color[2] = S.color[3] = 1; S.normal[2] = 1;
    S.fog_mode = 0x0800; S.fog_start = 0; S.fog_end = 1; S.fog_density = 1;       /* GL_EXP default */
    S.afunc = 0x207; S.aref = 0;
    for (int i = 0; i < 4; i++) { S.lpos[i][2] = 1; S.lamb[i][3] = 1; S.ldif[i][3] = 1; }
    S.ldif[0][0] = S.ldif[0][1] = S.ldif[0][2] = 1;
    S.scene_amb[0] = S.scene_amb[1] = S.scene_amb[2] = 0.2f; S.scene_amb[3] = 1;
    S.mat_amb[0] = S.mat_amb[1] = S.mat_amb[2] = 0.2f; S.mat_amb[3] = 1;
    S.mat_dif[0] = S.mat_dif[1] = S.mat_dif[2] = 0.8f; S.mat_dif[3] = 1;
    S.va[A_NRM].size = 3; S.va[A_TEX].size = 2; S.va[A_POS].size = 4; S.va[A_COL].size = 4;
    S.v_mv = S.v_pr = S.v_tx = S.v_fog = S.v_alpha = S.v_light = 1;
}
static float *cur_m(void) { return S.mode == 0 ? S.mv[S.mvd] : S.mode == 1 ? S.pr[S.prd] : S.tx[S.txd]; }
static void cur_changed(void) { if (S.mode == 0) S.v_mv++; else if (S.mode == 1) S.v_pr++; else S.v_tx++; }

/* ------------------------------------------------------------------------- shader variants */
static const char *VS_BODY =
    "attribute vec4 aPos;\nattribute vec4 aCol;\nattribute vec3 aNrm;\nattribute vec4 aTex;\n"
    "uniform mat4 uMVP;\nuniform mat4 uMV;\nuniform mat4 uTexM;\nuniform mat3 uNM;\n"
    "varying vec4 vCol;\nvarying vec2 vTex;\nvarying float vFog;\n"
    "#ifdef LIGHT\n"
    "uniform vec4 uLPos[4];\nuniform vec4 uLAmb[4];\nuniform vec4 uLDif[4];\nuniform float uLOn[4];\n"
    "uniform vec4 uSceneAmb;\nuniform vec4 uMatAmb;\nuniform vec4 uMatDif;\n"
    "#endif\n"
    "uniform vec3 uFogP;\n"
    "void main() {\n"
    "  gl_Position = uMVP * aPos;\n"
    "#ifdef TEX\n  vTex = (uTexM * aTex).xy;\n#else\n  vTex = vec2(0.0);\n#endif\n"
    "#if defined(LIGHT) || FOGMODE != 0\n  vec4 ep = uMV * aPos;\n#endif\n"
    "#ifdef LIGHT\n"
    "  vec3 n = normalize(uNM * aNrm);\n"
    "#ifdef COLMAT\n  vec4 mA = aCol; vec4 mD = aCol;\n#else\n  vec4 mA = uMatAmb; vec4 mD = uMatDif;\n#endif\n"
    "  vec3 c = uSceneAmb.rgb * mA.rgb;\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    if (uLOn[i] > 0.5) {\n"
    "      vec3 L = (uLPos[i].w == 0.0) ? normalize(uLPos[i].xyz) : normalize(uLPos[i].xyz - ep.xyz);\n"
    "      float d = max(dot(n, L), 0.0);\n"
    "      c += uLAmb[i].rgb * mA.rgb + uLDif[i].rgb * mD.rgb * d;\n"
    "    }\n"
    "  }\n"
    "  vCol = vec4(clamp(c, 0.0, 1.0), mD.a);\n"
    "#else\n  vCol = aCol;\n#endif\n"
    "#if FOGMODE == 1\n  vFog = clamp((uFogP.y - abs(ep.z)) / (uFogP.y - uFogP.x), 0.0, 1.0);\n"
    "#elif FOGMODE == 2\n  vFog = clamp(exp(-uFogP.z * abs(ep.z)), 0.0, 1.0);\n"
    "#elif FOGMODE == 3\n  float ft = uFogP.z * abs(ep.z); vFog = clamp(exp(-ft * ft), 0.0, 1.0);\n"
    "#else\n  vFog = 1.0;\n#endif\n"
    "}\n";
static const char *FS_BODY =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec4 vCol;\nvarying vec2 vTex;\nvarying float vFog;\n"
    "uniform sampler2D uSamp;\nuniform vec4 uFogCol;\nuniform float uARef;\n"
    "void main() {\n"
    "  vec4 c = vCol;\n"
    "#ifdef TEX\n  c *= texture2D(uSamp, vTex);\n#endif\n"
    "#if FOGMODE != 0\n  c.rgb = mix(uFogCol.rgb, c.rgb, vFog);\n#endif\n"
    "#if AFUNC == 0\n  discard;\n"
    "#elif AFUNC == 1\n  if (!(c.a < uARef)) discard;\n"
    "#elif AFUNC == 2\n  if (abs(c.a - uARef) > 0.002) discard;\n"
    "#elif AFUNC == 3\n  if (!(c.a <= uARef)) discard;\n"
    "#elif AFUNC == 4\n  if (!(c.a > uARef)) discard;\n"
    "#elif AFUNC == 5\n  if (abs(c.a - uARef) <= 0.002) discard;\n"
    "#elif AFUNC == 6\n  if (!(c.a >= uARef)) discard;\n"
    "#endif\n"
    "  gl_FragColor = c;\n"
    "}\n";

typedef struct {
    GLuint id;
    int uMVP, uMV, uTexM, uNM, uSamp, uFogP, uFogCol, uARef, uLPos, uLAmb, uLDif, uLOn, uSceneAmb, uMatAmb, uMatDif;
    unsigned v_mv, v_pr, v_tx, v_fog, v_alpha, v_light;
} Prog;
#define NVAR 1024
static Prog *progs[NVAR];

static GLuint compile(GLenum type, const char *defs, const char *body) {
    GLuint s = r_glCreateShader(type);
    const char *src[2] = { defs, body };
    r_glShaderSource(s, 2, src, NULL);
    r_glCompileShader(s);
    GLint ok = 0; r_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024] = ""; r_glGetShaderInfoLog(s, sizeof b - 1, NULL, b); fprintf(stderr, "[gles1] SHADER COMPILE FAILED (%s): %s\n--- defs:\n%s\n", type == GL_VERTEX_SHADER ? "vs" : "fs", b, defs); }
    return s;
}
/* key: bit0 TEX, bits1-2 fog mode (0 none, 1 lin, 2 exp, 3 exp2), bit3 LIGHT, bit4 COLMAT, bits5-8 alpha func+1 (0 = no alpha test) */
static Prog *get_prog(unsigned key) {
    if (progs[key]) return progs[key];
    char defs[256];
    int tex = key & 1, fog = (key >> 1) & 3, light = (key >> 3) & 1, colmat = (key >> 4) & 1, af = (int)((key >> 5) & 15) - 1;
    snprintf(defs, sizeof defs, "%s%s%s#define FOGMODE %d\n#define AFUNC %d\n", tex ? "#define TEX\n" : "", light ? "#define LIGHT\n" : "", colmat ? "#define COLMAT\n" : "", fog, af);
    Prog *p = calloc(1, sizeof *p);
    GLuint vs = compile(GL_VERTEX_SHADER, defs, VS_BODY), fs = compile(GL_FRAGMENT_SHADER, defs, FS_BODY);
    p->id = r_glCreateProgram();
    r_glAttachShader(p->id, vs); r_glAttachShader(p->id, fs);
    r_glBindAttribLocation(p->id, A_POS, "aPos"); r_glBindAttribLocation(p->id, A_COL, "aCol");
    r_glBindAttribLocation(p->id, A_NRM, "aNrm"); r_glBindAttribLocation(p->id, A_TEX, "aTex");
    r_glLinkProgram(p->id);
    GLint ok = 0; r_glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) { char b[1024] = ""; r_glGetProgramInfoLog(p->id, sizeof b - 1, NULL, b); fprintf(stderr, "[gles1] PROGRAM LINK FAILED: %s\n", b); }
    r_glDeleteShader(vs); r_glDeleteShader(fs);
#define U(f, n) p->f = r_glGetUniformLocation(p->id, n)
    U(uMVP, "uMVP"); U(uMV, "uMV"); U(uTexM, "uTexM"); U(uNM, "uNM"); U(uSamp, "uSamp"); U(uFogP, "uFogP"); U(uFogCol, "uFogCol"); U(uARef, "uARef");
    U(uLPos, "uLPos"); U(uLAmb, "uLAmb"); U(uLDif, "uLDif"); U(uLOn, "uLOn"); U(uSceneAmb, "uSceneAmb"); U(uMatAmb, "uMatAmb"); U(uMatDif, "uMatDif");
#undef U
    r_glUseProgram(p->id); S.cur_prog = p->id;
    if (p->uSamp >= 0) r_glUniform1i(p->uSamp, 0);
    progs[key] = p;
    { static int n; if (++n <= 40) fprintf(stderr, "[gles1] program #%d key=0x%x tex=%d fog=%d light=%d colmat=%d afunc=%d link=%d\n", n, key, tex, fog, light, colmat, af, ok); }
    return p;
}

/* ------------------------------------------------------------------------- draw preparation */
static void bind_abuf(GLuint b) { if (S.abuf_cur != b) { r_glBindBuffer(GL_ARRAY_BUFFER, b); S.abuf_cur = b; } }
static int is_norm(int slot, int type) {
    if (slot == A_COL) return type != GL_FLOAT && type != GL_FIXED;
    if (slot == A_NRM) return type == GL_BYTE || type == GL_SHORT;
    return 0;
}
static void prepare(void) {
    NEED(); st_init();
    unsigned key = S.tex2d ? 1u : 0u;
    if (S.fog) key |= (S.fog_mode == 0x2601 ? 1u : S.fog_mode == 0x0800 ? 2u : 3u) << 1;
    if (S.lighting) { key |= 1u << 3; if (S.colmat) key |= 1u << 4; }
    if (S.atest && S.afunc != 0x207) key |= (unsigned)((S.afunc - 0x200) + 1) << 5;
    Prog *p = get_prog(key);
    if (S.cur_prog != p->id) { r_glUseProgram(p->id); S.cur_prog = p->id; }

    /* uniforms (re-sent only when the state or the program changed) */
    if (p->v_mv != S.v_mv || p->v_pr != S.v_pr) {
        float mvp[16]; m_mul(mvp, S.pr[S.prd], S.mv[S.mvd]);
        r_glUniformMatrix4fv(p->uMVP, 1, GL_FALSE, mvp);
        if (p->uMV >= 0) r_glUniformMatrix4fv(p->uMV, 1, GL_FALSE, S.mv[S.mvd]);
        if (p->uNM >= 0) { float nm[9]; m_normal(S.mv[S.mvd], nm); r_glUniformMatrix3fv(p->uNM, 1, GL_FALSE, nm); }
        p->v_mv = S.v_mv; p->v_pr = S.v_pr;
    }
    if (p->uTexM >= 0 && p->v_tx != S.v_tx) { r_glUniformMatrix4fv(p->uTexM, 1, GL_FALSE, S.tx[S.txd]); p->v_tx = S.v_tx; }
    if (p->v_fog != S.v_fog) {
        if (p->uFogP >= 0) { float f[3] = { S.fog_start, S.fog_end, S.fog_density }; r_glUniform3fv(p->uFogP, 1, f); }
        if (p->uFogCol >= 0) r_glUniform4fv(p->uFogCol, 1, S.fog_color);
        p->v_fog = S.v_fog;
    }
    if (p->uARef >= 0 && p->v_alpha != S.v_alpha) { r_glUniform1f(p->uARef, S.aref); p->v_alpha = S.v_alpha; }
    if (p->uLPos >= 0 && p->v_light != S.v_light) {
        float on[4]; for (int i = 0; i < 4; i++) on[i] = S.lon[i] ? 1.0f : 0.0f;
        r_glUniform4fv(p->uLPos, 4, &S.lpos[0][0]); r_glUniform4fv(p->uLAmb, 4, &S.lamb[0][0]); r_glUniform4fv(p->uLDif, 4, &S.ldif[0][0]);
        r_glUniform1fv(p->uLOn, 4, on); r_glUniform4fv(p->uSceneAmb, 1, S.scene_amb);
        if (p->uMatAmb >= 0) r_glUniform4fv(p->uMatAmb, 1, S.mat_amb);
        if (p->uMatDif >= 0) r_glUniform4fv(p->uMatDif, 1, S.mat_dif);
        p->v_light = S.v_light;
    }

    /* vertex attributes */
    for (int a = 0; a < A_N; a++) {
        if (S.va[a].en && S.va[a].ptr == NULL && S.va[a].buf == 0) { /* enabled with no data: treat as disabled */ }
        if (S.va[a].en) {
            bind_abuf(S.va[a].buf);
            int type = S.va[a].type;
            r_glVertexAttribPointer(a, S.va[a].size, type, is_norm(a, type) ? GL_TRUE : GL_FALSE, S.va[a].stride, S.va[a].ptr);
            if (!S.attr_en[a]) { r_glEnableVertexAttribArray(a); S.attr_en[a] = 1; }
        } else {
            if (S.attr_en[a]) { r_glDisableVertexAttribArray(a); S.attr_en[a] = 0; }
            float v[4];
            switch (a) {
                case A_COL: memcpy(v, S.color, 16); break;
                case A_NRM: v[0] = S.normal[0]; v[1] = S.normal[1]; v[2] = S.normal[2]; v[3] = 0; break;
                case A_TEX: v[0] = v[1] = v[2] = 0; v[3] = 1; break;
                default: v[0] = v[1] = v[2] = 0; v[3] = 1;
            }
            if (!S.cattr_valid[a] || memcmp(S.cattr[a], v, 16)) { r_glVertexAttrib4f(a, v[0], v[1], v[2], v[3]); memcpy(S.cattr[a], v, 16); S.cattr_valid[a] = 1; }
        }
    }
    bind_abuf(S.abuf_app);
}

/* ------------------------------------------------------------------------- exported GLES 1.1 API */
EXPORT void glMatrixMode(GLenum m) { st_init(); S.mode = m == 0x1700 ? 0 : m == 0x1701 ? 1 : 2; }
EXPORT void glLoadIdentity(void) { st_init(); m_ident(cur_m()); cur_changed(); }
EXPORT void glPushMatrix(void) {
    st_init();
    int *d = S.mode == 0 ? &S.mvd : S.mode == 1 ? &S.prd : &S.txd;
    if (*d >= STK - 1) return;
    float (*st)[16] = S.mode == 0 ? S.mv : S.mode == 1 ? S.pr : S.tx;
    memcpy(st[*d + 1], st[*d], 64); (*d)++;
}
EXPORT void glPopMatrix(void) {
    st_init();
    int *d = S.mode == 0 ? &S.mvd : S.mode == 1 ? &S.prd : &S.txd;
    if (*d > 0) { (*d)--; cur_changed(); }
}
EXPORT void glMultMatrixf(const GLfloat *m) { st_init(); if (!m) return; m_mul(cur_m(), cur_m(), m); cur_changed(); }
EXPORT void glLoadMatrixf(const GLfloat *m) { st_init(); if (!m) return; memcpy(cur_m(), m, 64); cur_changed(); }
EXPORT SOFT void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    st_init(); float *m = cur_m();
    for (int r = 0; r < 4; r++) m[12 + r] += m[r] * x + m[4 + r] * y + m[8 + r] * z;
    cur_changed();
}
EXPORT SOFT void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    st_init(); float *m = cur_m();
    for (int r = 0; r < 4; r++) { m[r] *= x; m[4 + r] *= y; m[8 + r] *= z; }
    cur_changed();
}
EXPORT SOFT void glRotatef(GLfloat ang, GLfloat x, GLfloat y, GLfloat z) {
    st_init();
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-9f) return;
    x /= len; y /= len; z /= len;
    float a = ang * (float)(M_PI / 180.0), c = cosf(a), s = sinf(a), t = 1 - c, r[16];
    r[0] = t * x * x + c;     r[1] = t * x * y + s * z; r[2] = t * x * z - s * y; r[3] = 0;
    r[4] = t * x * y - s * z; r[5] = t * y * y + c;     r[6] = t * y * z + s * x; r[7] = 0;
    r[8] = t * x * z + s * y; r[9] = t * y * z - s * x; r[10] = t * z * z + c;    r[11] = 0;
    r[12] = r[13] = r[14] = 0; r[15] = 1;
    m_mul(cur_m(), cur_m(), r); cur_changed();
}
EXPORT SOFT void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    st_init(); float o[16]; m_ident(o);
    o[0] = 2 / (r - l); o[5] = 2 / (t - b); o[10] = -2 / (f - n);
    o[12] = -(r + l) / (r - l); o[13] = -(t + b) / (t - b); o[14] = -(f + n) / (f - n);
    m_mul(cur_m(), cur_m(), o); cur_changed();
}
EXPORT SOFT void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    st_init(); float o[16]; memset(o, 0, 64);
    o[0] = 2 * n / (r - l); o[5] = 2 * n / (t - b); o[8] = (r + l) / (r - l); o[9] = (t + b) / (t - b);
    o[10] = -(f + n) / (f - n); o[11] = -1; o[14] = -2 * f * n / (f - n);
    m_mul(cur_m(), cur_m(), o); cur_changed();
}
EXPORT void glGetFloatv(GLenum pn, GLfloat *p) {
    NEED(); st_init();
    if (pn == 0x0BA6) memcpy(p, S.mv[S.mvd], 64);
    else if (pn == 0x0BA7) memcpy(p, S.pr[S.prd], 64);
    else if (pn == 0x0BA8) memcpy(p, S.tx[S.txd], 64);
    else r_glGetFloatv(pn, p);
}

EXPORT SOFT void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { st_init(); S.color[0] = r; S.color[1] = g; S.color[2] = b; S.color[3] = a; }
EXPORT void glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { st_init(); S.color[0] = r / 255.0f; S.color[1] = g / 255.0f; S.color[2] = b / 255.0f; S.color[3] = a / 255.0f; }
EXPORT SOFT void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { st_init(); S.normal[0] = x; S.normal[1] = y; S.normal[2] = z; }

EXPORT void glEnable(GLenum c) {
    NEED(); st_init();
    switch (c) {
        case 0x0DE1: if (!S.tex2d) S.tex2d = 1; return;
        case 0x0B60: S.fog = 1; return;
        case 0x0B50: S.lighting = 1; return;
        case 0x0B57: S.colmat = 1; return;
        case 0x0BC0: S.atest = 1; return;
        case 0x4000: case 0x4001: case 0x4002: case 0x4003: S.lon[c - 0x4000] = 1; S.v_light++; return;
        case 0x0BE2: case 0x0B44: case 0x0B71: case 0x0BD0: case 0x8037: case 0x809E: case 0x80A0: case 0x0C11: case 0x0B90:
            r_glEnable(c); return;
        default: return;                      /* ES1-only capability: nothing to do in ES2 */
    }
}
EXPORT void glDisable(GLenum c) {
    NEED(); st_init();
    switch (c) {
        case 0x0DE1: S.tex2d = 0; return;
        case 0x0B60: S.fog = 0; return;
        case 0x0B50: S.lighting = 0; return;
        case 0x0B57: S.colmat = 0; return;
        case 0x0BC0: S.atest = 0; return;
        case 0x4000: case 0x4001: case 0x4002: case 0x4003: S.lon[c - 0x4000] = 0; S.v_light++; return;
        case 0x0BE2: case 0x0B44: case 0x0B71: case 0x0BD0: case 0x8037: case 0x809E: case 0x80A0: case 0x0C11: case 0x0B90:
            r_glDisable(c); return;
        default: return;
    }
}
EXPORT void glEnableClientState(GLenum a) {
    st_init();
    switch (a) { case 0x8074: S.va[A_POS].en = 1; break; case 0x8076: S.va[A_COL].en = 1; break; case 0x8075: S.va[A_NRM].en = 1; break; case 0x8078: S.va[A_TEX].en = 1; break; }
}
EXPORT void glDisableClientState(GLenum a) {
    st_init();
    switch (a) { case 0x8074: S.va[A_POS].en = 0; break; case 0x8076: S.va[A_COL].en = 0; break; case 0x8075: S.va[A_NRM].en = 0; break; case 0x8078: S.va[A_TEX].en = 0; break; }
}
static void set_ptr(int slot, GLint size, GLenum type, GLsizei stride, const void *p) {
    st_init();
    S.va[slot].size = size; S.va[slot].type = type; S.va[slot].stride = stride; S.va[slot].ptr = p; S.va[slot].buf = S.abuf_app;
}
EXPORT void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p) { set_ptr(A_POS, size, type, stride, p); }
EXPORT void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p) { set_ptr(A_COL, size, type, stride, p); }
EXPORT void glNormalPointer(GLenum type, GLsizei stride, const void *p) { set_ptr(A_NRM, 3, type, stride, p); }
EXPORT void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p) { set_ptr(A_TEX, size, type, stride, p); }
EXPORT void glClientActiveTexture(GLenum u) { (void)u; }

EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count) { prepare(); r_glDrawArrays(mode, first, count); }
EXPORT void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) { prepare(); r_glDrawElements(mode, count, type, idx); }

/* ---- fog / alpha / lights ---- */
static void fog_set(GLenum pn, const float *v) {
    st_init();
    switch (pn) {
        case 0x0B65: S.fog_mode = (int)v[0]; break;
        case 0x0B62: S.fog_density = v[0]; break;
        case 0x0B63: S.fog_start = v[0]; break;
        case 0x0B64: S.fog_end = v[0]; break;
        case 0x0B66: memcpy(S.fog_color, v, 16); break;
        default: return;
    }
    S.v_fog++;
}
EXPORT SOFT void glFogf(GLenum pn, GLfloat v) { float a[4] = { v, 0, 0, 0 }; fog_set(pn, a); }
EXPORT void glFogfv(GLenum pn, const GLfloat *v) { fog_set(pn, v); }
EXPORT void glFogx(GLenum pn, GLfixed v) { float a[4] = { pn == 0x0B65 ? (float)v : v / 65536.0f, 0, 0, 0 }; fog_set(pn, a); }
EXPORT void glFogxv(GLenum pn, const GLfixed *v) {
    float a[4]; if (pn == 0x0B65) a[0] = (float)v[0]; else for (int i = 0; i < 4; i++) a[i] = v[i] / 65536.0f;
    fog_set(pn, a);
}
EXPORT SOFT void glAlphaFunc(GLenum f, GLfloat ref) { st_init(); S.afunc = (int)f; S.aref = ref < 0 ? 0 : ref > 1 ? 1 : ref; S.v_alpha++; }
EXPORT void glLightfv(GLenum light, GLenum pn, const GLfloat *v) {
    st_init(); int i = (int)light - 0x4000;
    if (i < 0 || i > 3 || !v) return;
    switch (pn) {
        case 0x1200: memcpy(S.lamb[i], v, 16); break;
        case 0x1201: memcpy(S.ldif[i], v, 16); break;
        case 0x1203: m_vec(S.mv[S.mvd], v, S.lpos[i]); break;     /* transformed by the modelview matrix at call time */
        default: return;
    }
    S.v_light++;
}
EXPORT void glLightModelfv(GLenum pn, const GLfloat *v) { st_init(); if (pn == 0x0B53) { memcpy(S.scene_amb, v, 16); S.v_light++; } }
EXPORT SOFT void glLightModelf(GLenum pn, GLfloat v) { (void)pn; (void)v; }        /* GL_LIGHT_MODEL_TWO_SIDE: not emulated */
EXPORT SOFT void glMaterialf(GLenum f, GLenum pn, GLfloat v) { (void)f; (void)pn; (void)v; }
EXPORT void glMaterialfv(GLenum f, GLenum pn, const GLfloat *v) {
    (void)f; st_init();
    if (pn == 0x1200 || pn == 0x1602) memcpy(S.mat_amb, v, 16);
    if (pn == 0x1201 || pn == 0x1602) memcpy(S.mat_dif, v, 16);
    S.v_light++;
}
EXPORT void glShadeModel(GLenum m) { (void)m; }
EXPORT void glHint(GLenum t, GLenum m) { (void)t; (void)m; }
EXPORT SOFT void glTexEnvf(GLenum t, GLenum pn, GLfloat v) { (void)t; (void)pn; (void)v; }
EXPORT void glTexEnvi(GLenum t, GLenum pn, GLint v) { (void)t; (void)pn; (void)v; }
EXPORT void glTexEnvfv(GLenum t, GLenum pn, const GLfloat *v) { (void)t; (void)pn; (void)v; }

/* ---- buffers / textures ---- */
EXPORT void glBindBuffer(GLenum t, GLuint b) {
    NEED(); st_init();
    if (t == GL_ARRAY_BUFFER) { S.abuf_app = b; S.abuf_cur = b; }
    r_glBindBuffer(t, b);
}
EXPORT void glDeleteBuffers(GLsizei n, const GLuint *b) {
    NEED(); st_init();
    for (int i = 0; i < n; i++) if (b[i] == S.abuf_app) { S.abuf_app = 0; S.abuf_cur = 0; }
    r_glDeleteBuffers(n, b);
}
EXPORT void glGenBuffers(GLsizei n, GLuint *b) { NEED(); r_glGenBuffers(n, b); }
EXPORT void glBufferData(GLenum t, long sz, const void *d, GLenum u) { NEED(); r_glBufferData(t, sz, d, u); }
EXPORT void glBindTexture(GLenum t, GLuint id) { NEED(); st_init(); if (t == GL_TEXTURE_2D) S.cur_tex = id; r_glBindTexture(t, id); }
EXPORT void glGenTextures(GLsizei n, GLuint *ids) { NEED(); r_glGenTextures(n, ids); }
EXPORT void glDeleteTextures(GLsizei n, const GLuint *ids) { NEED(); st_init(); for (int i = 0; i < n; i++) if (ids[i] < sizeof S.gen_mip) S.gen_mip[ids[i]] = 0; r_glDeleteTextures(n, ids); }
EXPORT void glTexImage2D(GLenum t, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p) {
    NEED(); st_init();
    r_glTexImage2D(t, lv, ifmt, w, h, b, f, ty, p);
    if (t == GL_TEXTURE_2D && lv == 0 && p && S.cur_tex < sizeof S.gen_mip && S.gen_mip[S.cur_tex]) r_glGenerateMipmap(t);
}
EXPORT void glTexSubImage2D(GLenum t, GLint lv, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p) {
    NEED(); st_init();
    r_glTexSubImage2D(t, lv, x, y, w, h, f, ty, p);
    if (t == GL_TEXTURE_2D && lv == 0 && p && S.cur_tex < sizeof S.gen_mip && S.gen_mip[S.cur_tex]) r_glGenerateMipmap(t);
}
EXPORT void glTexParameteri(GLenum t, GLenum pn, GLint v) {
    NEED(); st_init();
    if (pn == 0x8191) { if (S.cur_tex < sizeof S.gen_mip) S.gen_mip[S.cur_tex] = v ? 1 : 0; return; }   /* GL_GENERATE_MIPMAP (ES1 only) */
    if ((pn == 0x2802 || pn == 0x2803) && v == 0x2900) v = 0x812F;                                      /* GL_CLAMP -> CLAMP_TO_EDGE */
    r_glTexParameteri(t, pn, v);
}
EXPORT void glActiveTexture(GLenum u) { NEED(); r_glActiveTexture(u); }

/* ---- plain pass-through (hard-float side called with the host convention) ---- */
EXPORT void glBlendFunc(GLenum s, GLenum d) { NEED(); r_glBlendFunc(s, d); }
EXPORT void glClear(GLbitfield m) { NEED(); r_glClear(m); }
EXPORT SOFT void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { NEED(); r_glClearColor(r, g, b, a); }
EXPORT SOFT void glClearDepthf(GLfloat d) { NEED(); r_glClearDepthf(d); }
EXPORT void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { NEED(); r_glColorMask(r, g, b, a); }
EXPORT void glCullFace(GLenum m) { NEED(); r_glCullFace(m); }
EXPORT void glDepthFunc(GLenum f) { NEED(); r_glDepthFunc(f); }
EXPORT void glDepthMask(GLboolean m) { NEED(); r_glDepthMask(m); }
EXPORT SOFT void glDepthRangef(GLfloat n, GLfloat f) { NEED(); r_glDepthRangef(n, f); }
EXPORT GLenum glGetError(void) { NEED(); return r_glGetError(); }
EXPORT const unsigned char *glGetString(GLenum n) { NEED(); return r_glGetString(n); }
EXPORT void glGetIntegerv(GLenum n, GLint *p) { NEED(); r_glGetIntegerv(n, p); }
EXPORT SOFT void glLineWidth(GLfloat w) { NEED(); r_glLineWidth(w); }
EXPORT SOFT void glPolygonOffset(GLfloat f, GLfloat u) { NEED(); r_glPolygonOffset(f, u); }
EXPORT void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p) { NEED(); r_glReadPixels(x, y, w, h, f, t, p); }
EXPORT void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { NEED(); if (shim_gl_scissor) shim_gl_scissor(x, y, w, h); else r_glScissor(x, y, w, h); }
EXPORT void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { NEED(); if (shim_gl_viewport) shim_gl_viewport(x, y, w, h); else r_glViewport(x, y, w, h); }
EXPORT void glStencilFunc(GLenum f, GLint r, GLuint m) { NEED(); r_glStencilFunc(f, r, m); }
EXPORT void glStencilMask(GLuint m) { NEED(); r_glStencilMask(m); }
EXPORT void glStencilOp(GLenum a, GLenum b, GLenum c) { NEED(); r_glStencilOp(a, b, c); }
EXPORT void glFinish(void) { NEED(); r_glFinish(); }
EXPORT void glFlush(void) { NEED(); r_glFlush(); }
