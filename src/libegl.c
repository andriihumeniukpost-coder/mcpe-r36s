/* libEGL.so replacement built on SDL2. The host (main.c) creates the window
 * and GLES2 context on the main thread and hands them over through
 * shim_egl_window / shim_egl_ctx; the game's render thread then "creates" and
 * binds them via the EGL calls it makes.
 *
 * eglGetProcAddress also returns soft-float wrappers for the few GL entry
 * points that take float arguments (the game is soft-float, Mesa is hard-float). */
#define _GNU_SOURCE
#include "android_min.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

EXPORT void *shim_egl_window;
EXPORT void *shim_egl_ctx;

typedef void *EGLhandle;
typedef int EGLint;
#define EGL_TRUE 1

EXPORT EGLhandle eglGetDisplay(EGLhandle d) { (void)d; return (EGLhandle)1; }
EXPORT int eglInitialize(EGLhandle d, EGLint *maj, EGLint *min) { (void)d; if (maj) *maj = 1; if (min) *min = 4; return EGL_TRUE; }
EXPORT int eglTerminate(EGLhandle d) { (void)d; return EGL_TRUE; }
EXPORT const char *eglQueryString(EGLhandle d, EGLint n) { (void)d; (void)n; return ""; }
EXPORT int eglChooseConfig(EGLhandle d, const EGLint *attr, EGLhandle *cfgs, EGLint size, EGLint *num) {
    (void)d; (void)attr;
    if (cfgs && size > 0) cfgs[0] = (EGLhandle)1;
    if (num) *num = 1;
    return EGL_TRUE;
}
EXPORT int eglGetConfigAttrib(EGLhandle d, EGLhandle c, EGLint a, EGLint *v) {
    (void)d; (void)c;
    switch (a) {
        case 0x302E: *v = 1; break;     /* EGL_NATIVE_VISUAL_ID */
        case 0x3024: *v = 8; break;     /* RED   */
        case 0x3023: *v = 8; break;     /* GREEN */
        case 0x3022: *v = 8; break;     /* BLUE  */
        case 0x3025: *v = 16; break;    /* DEPTH */
        default: *v = 0;
    }
    return EGL_TRUE;
}
EXPORT EGLhandle eglCreateWindowSurface(EGLhandle d, EGLhandle c, EGLhandle w, const EGLint *a) { (void)d; (void)c; (void)w; (void)a; return (EGLhandle)2; }
EXPORT EGLhandle eglCreateContext(EGLhandle d, EGLhandle c, EGLhandle share, const EGLint *a) { (void)d; (void)c; (void)share; (void)a; return shim_egl_ctx ? shim_egl_ctx : (EGLhandle)3; }
EXPORT int eglDestroyContext(EGLhandle d, EGLhandle c) { (void)d; (void)c; return EGL_TRUE; }
EXPORT int eglDestroySurface(EGLhandle d, EGLhandle s) { (void)d; (void)s; return EGL_TRUE; }
EXPORT int eglMakeCurrent(EGLhandle d, EGLhandle draw, EGLhandle read, EGLhandle ctx) {
    (void)d; (void)draw; (void)read;
    if (!ctx) return SDL_GL_MakeCurrent((SDL_Window *)shim_egl_window, NULL) == 0;
    return SDL_GL_MakeCurrent((SDL_Window *)shim_egl_window, (SDL_GLContext)shim_egl_ctx) == 0;
}
static unsigned (*r_glGetError)(void);
static const unsigned char *(*r_glGetString)(unsigned);
static void ld(void);
static void probe_pixels(void);
extern volatile int host_cursor[3] __attribute__((weak));
extern volatile int host_ov_mode __attribute__((weak));      /* 0 off, 1 FPS, 2 full stats */
extern char host_ov_lines[4][40] __attribute__((weak));      /* CPU / GPU / RAM / GAME lines from the host sampler */
static int fps_value;
/* 3x5 font: digits, A-Z, a few symbols (rows top->bottom, 3 bits each) */
static const unsigned char font_digits[10][5] = { {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7} };
static const unsigned char font_alpha[26][5] = {
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},
    {6,5,5,5,5},{2,5,5,5,2},{6,5,6,4,4},{2,5,5,7,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7} };
static const unsigned char *glyph(char c) {
    static const unsigned char blank[5] = {0,0,0,0,0}, dot[5] = {0,0,0,0,2}, colon[5] = {0,2,0,2,0}, pct[5] = {5,1,2,4,5}, slash[5] = {1,1,2,4,4}, dash[5] = {0,0,7,0,0};
    if (c >= '0' && c <= '9') return font_digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return font_alpha[c - 'A'];
    if (c >= 'a' && c <= 'z') return font_alpha[c - 'a'];
    switch (c) { case '.': return dot; case ':': return colon; case '%': return pct; case '/': return slash; case '-': return dash; default: return blank; }
}
#define OVW 128
#define OVH 32
static unsigned char ov_buf[OVW * OVH * 4];
static unsigned ov_tex, ov_fbo; static int ov_rows, ov_dirty = 1, ov_failed;
static void ov_text(int line, const char *s, unsigned rgb) {
    int y0 = line * 6;
    for (int i = 0; s[i] && i < 31; i++) {
        const unsigned char *g = glyph(s[i]);
        for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++) if (g[r] & (4 >> c)) {
            unsigned char *p = ov_buf + ((y0 + r) * OVW + i * 4 + c) * 4; p[0] = rgb >> 16; p[1] = rgb >> 8; p[2] = rgb; p[3] = 255; }
    }
}
static void ov_build(int mode) {
    for (int i = 0; i < OVW * OVH; i++) { ov_buf[i * 4] = ov_buf[i * 4 + 1] = ov_buf[i * 4 + 2] = 0; ov_buf[i * 4 + 3] = 255; }
    char t[48]; snprintf(t, sizeof t, "FPS %d", fps_value); ov_text(0, t, 0xFFFF00);
    ov_rows = 1;
    if (mode >= 2 && &host_ov_lines[0][0]) { for (int i = 0; i < 4; i++) { ov_text(1 + i, host_ov_lines[i], i == 0 ? 0x80FF80 : i == 1 ? 0x80C0FF : 0xFFFFFF); } ov_rows = 5; }
    ov_dirty = 1;
}
static void draw_overlay(void) {
    static void (*gi)(unsigned, int *), (*en)(unsigned), (*dis)(unsigned), (*sc)(int, int, int, int), (*cc)(float, float, float, float), (*cl)(unsigned), (*gf)(unsigned, float *), (*cm)(unsigned char, unsigned char, unsigned char, unsigned char), (*gb)(unsigned, unsigned char *);
    static unsigned char (*ise)(unsigned);
    static void (*gen_t)(int, unsigned *), (*bind_t)(unsigned, unsigned), (*teximg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*texsub)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*texpar)(unsigned, unsigned, int), (*actv)(unsigned);
    static void (*gen_f)(int, unsigned *), (*bind_f)(unsigned, unsigned), (*fbtex)(unsigned, unsigned, unsigned, unsigned, int), (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    static unsigned t0, frames; static int last_mode = -1;
    frames++; unsigned now = SDL_GetTicks();
    int mode = (&host_ov_mode) ? host_ov_mode : 1;
    int cur = (&host_cursor[0] && host_cursor[2]);
    if (now - t0 >= 500) { fps_value = (int)(frames * 1000.0f / (now - t0) + 0.5f); frames = 0; t0 = now; if (mode) ov_build(mode); }
    if (mode != last_mode) { last_mode = mode; if (mode) ov_build(mode); }
    if (!mode && !cur) return;
    if (!gi) { gi = SDL_GL_GetProcAddress("glGetIntegerv"); en = SDL_GL_GetProcAddress("glEnable"); dis = SDL_GL_GetProcAddress("glDisable"); sc = SDL_GL_GetProcAddress("glScissor");
        cc = SDL_GL_GetProcAddress("glClearColor"); cl = SDL_GL_GetProcAddress("glClear"); gf = SDL_GL_GetProcAddress("glGetFloatv"); cm = SDL_GL_GetProcAddress("glColorMask");
        gb = SDL_GL_GetProcAddress("glGetBooleanv"); ise = SDL_GL_GetProcAddress("glIsEnabled");
        gen_t = SDL_GL_GetProcAddress("glGenTextures"); bind_t = SDL_GL_GetProcAddress("glBindTexture"); teximg = SDL_GL_GetProcAddress("glTexImage2D"); texsub = SDL_GL_GetProcAddress("glTexSubImage2D");
        texpar = SDL_GL_GetProcAddress("glTexParameteri"); actv = SDL_GL_GetProcAddress("glActiveTexture");
        gen_f = SDL_GL_GetProcAddress("glGenFramebuffers"); bind_f = SDL_GL_GetProcAddress("glBindFramebuffer"); fbtex = SDL_GL_GetProcAddress("glFramebufferTexture2D"); blit = SDL_GL_GetProcAddress("glBlitFramebuffer"); }
    if (!gi || !sc || !cc || !cl || !cm || !gb || !ise) return;
    int box[4]; float col[4]; unsigned char mask[4]; int fb = 0;
    gi(0x8CA6, &fb); if (fb) return;
    gi(0x0C10, box); gf(0x0C22, col); gb(0x0C23, mask); unsigned char was = ise(0x0C11);
    int W = 640, H = 480; SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &W, &H);
    cm(1, 1, 1, 1);
    if (mode) {
        int ok = blit && gen_t && bind_t && teximg && texsub && texpar && actv && gen_f && bind_f && fbtex && !ov_failed;
        if (ok) {
            int prev_unit = 0, prev_tex = 0, rfb = 0; gi(0x84E0, &prev_unit); gi(0x8CAA, &rfb);
            if (ov_dirty || !ov_tex) {
                actv(0x84C0); gi(0x8069, &prev_tex);
                if (!ov_tex) { gen_t(1, &ov_tex); bind_t(0x0DE1, ov_tex); texpar(0x0DE1, 0x2801, 0x2600); texpar(0x0DE1, 0x2800, 0x2600);
                    texpar(0x0DE1, 0x2802, 0x812F); texpar(0x0DE1, 0x2803, 0x812F); teximg(0x0DE1, 0, 0x1908, OVW, OVH, 0, 0x1908, 0x1401, ov_buf);
                    gen_f(1, &ov_fbo); bind_f(0x8D40, ov_fbo); fbtex(0x8D40, 0x8CE0, 0x0DE1, ov_tex, 0); bind_f(0x8D40, 0); }
                else { bind_t(0x0DE1, ov_tex); texsub(0x0DE1, 0, 0, 0, OVW, OVH, 0x1908, 0x1401, ov_buf); }
                bind_t(0x0DE1, prev_tex); actv(prev_unit); ov_dirty = 0;
            }
            if (was) dis(0x0C11);
            const int S = 2; int w = OVW, h = ov_rows * 6;
            { static int w_used; if (!w_used) { w_used = 1; } }
            bind_f(0x8CA8, ov_fbo); bind_f(0x8CA9, 0);
            blit(0, 0, 31 * 4, h, 4, H - 4, 4 + 31 * 4 * S, H - 4 - h * S, 0x4000, 0x2600); (void)w;
            bind_f(0x8CA8, rfb); bind_f(0x8CA9, fb);
            if (was) en(0x0C11);
        } else { /* fallback: FPS digits only via scissored clears */
            en(0x0C11);
            char txt[8]; int n = snprintf(txt, sizeof txt, "%d", fps_value); const int S = 3, X0 = 4, Y0 = H - 4 - 5 * S;
            sc(X0 - 2, Y0 - 2, n * 4 * S + 3, 5 * S + 4); cc(0, 0, 0, 1); cl(0x4000); cc(1, 1, 0, 1);
            for (int i = 0; i < n; i++) { const unsigned char *g = glyph(txt[i]);
                for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++) if (g[r] & (4 >> c)) { sc(X0 + (i * 4 + c) * S, Y0 + (4 - r) * S, S, S); cl(0x4000); } }
            sc(box[0], box[1], box[2], box[3]); cc(col[0], col[1], col[2], col[3]); if (!was) dis(0x0C11);
        }
    }
    if (cur) { en(0x0C11); int x = host_cursor[0], y = H - host_cursor[1];
        sc(x - 7, y - 7, 15, 15); cc(0, 0, 0, 1); cl(0x4000); sc(x - 5, y - 5, 11, 11); cc(1, 1, 1, 1); cl(0x4000); }
    sc(box[0], box[1], box[2], box[3]); cc(col[0], col[1], col[2], col[3]); cm(mask[0], mask[1], mask[2], mask[3]);
    if (was) en(0x0C11); else dis(0x0C11);
}
EXPORT int eglSwapBuffers(EGLhandle d, EGLhandle s) {
    static int n; (void)d; (void)s;
    ld();
    if (n == 0 && r_glGetString) fprintf(stderr, "[gl] VENDOR=%s RENDERER=%s VERSION=%s\n", (const char *)r_glGetString(0x1F00), (const char *)r_glGetString(0x1F01), (const char *)r_glGetString(0x1F02));
    if (n < 3 || n % 120 == 0) { unsigned e = r_glGetError ? r_glGetError() : 0; fprintf(stderr, "[egl] swap #%d glError=0x%x\n", n, e); }
    if (n == 200 || n == 1500) probe_pixels();
    n++;
    draw_overlay();
    { /* frame timing: wall + this thread's CPU time in the game vs. inside swap */
        static struct timespec w_end, c_end; static double g_wall, g_cpu, s_wall, s_cpu; static int frames, init; static struct timespec rep;
        struct timespec w0, c0, w1, c1; clock_gettime(CLOCK_MONOTONIC, &w0); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
        if (init) { g_wall += (w0.tv_sec - w_end.tv_sec) * 1e3 + (w0.tv_nsec - w_end.tv_nsec) / 1e6; g_cpu += (c0.tv_sec - c_end.tv_sec) * 1e3 + (c0.tv_nsec - c_end.tv_nsec) / 1e6; }
        { static int fin = -1; static void (*r_glFinish)(void); if (fin < 0) { fin = getenv("MCPE_FINISH") != NULL; r_glFinish = SDL_GL_GetProcAddress("glFinish"); }
          if (fin && r_glFinish) { r_glFinish(); struct timespec wf; clock_gettime(CLOCK_MONOTONIC, &wf); static double f_wall; f_wall += (wf.tv_sec - w0.tv_sec) * 1e3 + (wf.tv_nsec - w0.tv_nsec) / 1e6; static int fc; if (++fc % 300 == 0) { fprintf(stderr, "[finish] avg glFinish %.1f ms\n", f_wall / 300); f_wall = 0; } w0 = wf; } }
        SDL_GL_SwapWindow((SDL_Window *)shim_egl_window);
        clock_gettime(CLOCK_MONOTONIC, &w1); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
        s_wall += (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6; s_cpu += (c1.tv_sec - c0.tv_sec) * 1e3 + (c1.tv_nsec - c0.tv_nsec) / 1e6;
        w_end = w1; c_end = c1; init = 1; frames++;
        if (!rep.tv_sec) rep = w1;
        if (w1.tv_sec - rep.tv_sec >= 5) {
            fprintf(stderr, "[frame] %d frames/5s: game wall %.1f ms cpu %.1f ms | swap wall %.1f ms cpu %.1f ms (per frame)\n", frames, g_wall / frames, g_cpu / frames, s_wall / frames, s_cpu / frames);
            frames = 0; g_wall = g_cpu = s_wall = s_cpu = 0; rep = w1;
        }
    }
    return EGL_TRUE;
}
EXPORT int eglSwapInterval(EGLhandle d, EGLint i) { (void)d; const char *o = getenv("MCPE_SWAPINT"); int v = o ? atoi(o) : i; int r = SDL_GL_SetSwapInterval(v); fprintf(stderr, "[egl] swapInterval game=%d used=%d -> %d (now %d)\n", i, v, r, SDL_GL_GetSwapInterval()); return EGL_TRUE; }
EXPORT int eglQuerySurface(EGLhandle d, EGLhandle s, EGLint a, EGLint *v) {
    (void)d; (void)s;
    int w = 640, h = 480;
    SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &w, &h);
    if (a == 0x3057) *v = w;          /* EGL_WIDTH  */
    else if (a == 0x3056) *v = h;     /* EGL_HEIGHT */
    else *v = 0;
    return EGL_TRUE;
}

/* ---- soft-float wrappers for GL calls with float parameters ---- */
#define GL_R(name, ...) static void (*r_##name)(__VA_ARGS__);
GL_R(glUniform1f, int, float)
GL_R(glUniform2f, int, float, float)
GL_R(glUniform3f, int, float, float, float)
GL_R(glUniform4f, int, float, float, float, float)
GL_R(glClearColor, float, float, float, float)
GL_R(glClearDepthf, float)
GL_R(glDepthRangef, float, float)
GL_R(glLineWidth, float)
GL_R(glPolygonOffset, float, float)
GL_R(glBlendColor, float, float, float, float)
GL_R(glSampleCoverage, float, unsigned char)
GL_R(glTexParameterf, unsigned, unsigned, float)
GL_R(glVertexAttrib1f, unsigned, float)
GL_R(glVertexAttrib2f, unsigned, float, float)
GL_R(glVertexAttrib3f, unsigned, float, float, float)
GL_R(glVertexAttrib4f, unsigned, float, float, float, float)

SOFT static void w_glUniform1f(int l, float a) { r_glUniform1f(l, a); }
SOFT static void w_glUniform2f(int l, float a, float b) { r_glUniform2f(l, a, b); }
SOFT static void w_glUniform3f(int l, float a, float b, float c) { r_glUniform3f(l, a, b, c); }
SOFT static void w_glUniform4f(int l, float a, float b, float c, float d) { r_glUniform4f(l, a, b, c, d); }
SOFT static void w_glClearColor(float a, float b, float c, float d) { r_glClearColor(a, b, c, d); }
SOFT static void w_glClearDepthf(float a) { r_glClearDepthf(a); }
SOFT static void w_glDepthRangef(float a, float b) { r_glDepthRangef(a, b); }
SOFT static void w_glLineWidth(float a) { r_glLineWidth(a); }
SOFT static void w_glPolygonOffset(float a, float b) { r_glPolygonOffset(a, b); }
SOFT static void w_glBlendColor(float a, float b, float c, float d) { r_glBlendColor(a, b, c, d); }
SOFT static void w_glSampleCoverage(float a, unsigned char b) { r_glSampleCoverage(a, b); }
SOFT static void w_glTexParameterf(unsigned t, unsigned p, float v) { r_glTexParameterf(t, p, v); }
SOFT static void w_glVertexAttrib1f(unsigned i, float a) { r_glVertexAttrib1f(i, a); }
SOFT static void w_glVertexAttrib2f(unsigned i, float a, float b) { r_glVertexAttrib2f(i, a, b); }
SOFT static void w_glVertexAttrib3f(unsigned i, float a, float b, float c) { r_glVertexAttrib3f(i, a, b, c); }
SOFT static void w_glVertexAttrib4f(unsigned i, float a, float b, float c, float d) { r_glVertexAttrib4f(i, a, b, c, d); }


/* ---- diagnostics: shader compile/link logs, texture uploads, GL errors ---- */
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
static void (*r_glCompileShader)(unsigned);
static void (*r_glLinkProgram)(unsigned);
static void (*r_glGetShaderiv)(unsigned, unsigned, int *);
static void (*r_glGetProgramiv)(unsigned, unsigned, int *);
static void (*r_glGetShaderInfoLog)(unsigned, int, int *, char *);
static void (*r_glGetProgramInfoLog)(unsigned, int, int *, char *);
static void (*r_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*r_glShaderSource)(unsigned, int, const char *const *, const int *);
static void ld(void) {
    if (r_glGetError) return;
#define L(n) r_##n = SDL_GL_GetProcAddress(#n)
    L(glGetShaderiv); L(glGetProgramiv); L(glGetShaderInfoLog); L(glGetProgramInfoLog); L(glGetError); L(glGetString);
}
SOFT static void w_glCompileShader(unsigned sh) {
    ld(); r_glCompileShader(sh);
    int ok = 0; r_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024] = ""; r_glGetShaderInfoLog(sh, sizeof b - 1, NULL, b); fprintf(stderr, "[gl] SHADER %u COMPILE FAILED: %s\n", sh, b); }
}
SOFT static void w_glLinkProgram(unsigned pr) {
    ld(); r_glLinkProgram(pr);
    int ok = 0; r_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) { char b[1024] = ""; r_glGetProgramInfoLog(pr, sizeof b - 1, NULL, b); fprintf(stderr, "[gl] PROGRAM %u LINK FAILED: %s\n", pr, b); }
}
SOFT static void w_glShaderSource(unsigned sh, int n, const char *const *src, const int *len) {
    static int c; if (c < 8) { c++; fprintf(stderr, "[gl] ==== shader %u source (%d parts)\n", sh, n);
        for (int i = 0; i < n; i++) fprintf(stderr, "%.3000s\n", src[i]); fprintf(stderr, "[gl] ==== end\n"); }
    char *cp[8]; const char *np[8]; int k = n < 8 ? n : 8;
    for (int i = 0; i < k; i++) {
        size_t L = len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]);
        char *o = malloc(L * 2 + 64); size_t oi = 0;
        static const char *prec; if (!prec) { prec = getenv("MCPE_PREC"); if (!prec) prec = "highp"; }
        size_t pl = strlen(prec);
        for (size_t j = 0; j < L; ) {
            if (j + 4 <= L && !memcmp(src[i] + j, "lowp", 4)) { memcpy(o + oi, prec, pl); oi += pl; j += 4; }
            else if (j + 7 <= L && !memcmp(src[i] + j, "mediump", 7)) { memcpy(o + oi, prec, pl); oi += pl; j += 7; }
            else o[oi++] = src[i][j++]; }
        o[oi] = 0; cp[i] = o; np[i] = o;
    }
    r_glShaderSource(sh, k, np, NULL);
    for (int i = 0; i < k; i++) free(cp[i]);
}
SOFT static void w_glTexImage2D(unsigned t, int l, int i, int w, int h, int b, unsigned f, unsigned ty, const void *p) {
    static int c; if (c < 40) { c++; fprintf(stderr, "[gl] glTexImage2D tgt=%x lvl=%d ifmt=%x %dx%d fmt=%x type=%x data=%s\n", t, l, i, w, h, f, ty, p ? "yes" : "null"); }
    r_glTexImage2D(t, l, i, w, h, b, f, ty, p);
}

static void (*r_glTexStorage2DEXT)(unsigned, int, unsigned, int, int);
static void (*r_glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*r_glTexParameteri)(unsigned, unsigned, int);
static void (*r_glCompressedTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *);
SOFT static void w_glTexStorage2DEXT(unsigned t, int lv, unsigned f, int w, int h) {
    static int c; if (c < 25) { c++; fprintf(stderr, "[gl] glTexStorage2DEXT tgt=%x levels=%d ifmt=%x %dx%d\n", t, lv, f, w, h); }
    r_glTexStorage2DEXT(t, lv, f, w, h);
}
SOFT static void w_glTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned f, unsigned ty, const void *p) {
    static int c; if (c < 40) { c++; const unsigned char *b = p; fprintf(stderr, "[gl] glTexSubImage2D lvl=%d @%d,%d %dx%d fmt=%x type=%x data=%s [%02x %02x %02x %02x %02x %02x %02x %02x]\n", l, x, y, w, h, f, ty, p ? "yes" : "null", p ? b[0] : 0, p ? b[1] : 0, p ? b[2] : 0, p ? b[3] : 0, p ? b[4] : 0, p ? b[5] : 0, p ? b[6] : 0, p ? b[7] : 0); }
    r_glTexSubImage2D(t, l, x, y, w, h, f, ty, p);
    { unsigned e = r_glGetError ? r_glGetError() : 0; if (e) { static int ec; if (ec++ < 30) fprintf(stderr, "[gl] glTexSubImage2D ERROR 0x%x (%dx%d)\n", e, w, h); } }
}
SOFT static void w_glTexParameteri(unsigned t, unsigned pn, int v) {
    static int c; if (c < 30) { c++; fprintf(stderr, "[gl] glTexParameteri tgt=%x pname=%x val=%x\n", t, pn, v); }
    r_glTexParameteri(t, pn, v);
}
SOFT static void w_glCompressedTexImage2D(unsigned t, int l, unsigned f, int w, int h, int b, int sz, const void *p) {
    static int c; if (c < 20) { c++; fprintf(stderr, "[gl] glCompressedTexImage2D lvl=%d fmt=%x %dx%d size=%d\n", l, f, w, h, sz); }
    r_glCompressedTexImage2D(t, l, f, w, h, b, sz, p);
}

static void (*r_glTexStorage2D)(unsigned, int, unsigned, int, int);
static void (*r_glUniform1i)(int, int);
static void (*r_glActiveTexture)(unsigned);
static void (*r_glGenerateMipmap)(unsigned);
static void (*r_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
SOFT static void w_glTexStorage2D(unsigned t, int lv, unsigned f, int w, int h) {
    static int c; if (c < 40) { c++; fprintf(stderr, "[gl] glTexStorage2D tgt=%x levels=%d ifmt=%x %dx%d\n", t, lv, f, w, h); }
    if (f >= 0x1906 && f <= 0x190A) { /* unsized format: emulate EXT_texture_storage with mutable levels */
        for (int l = 0, ww = w, hh = h; l < lv; l++, ww = ww > 1 ? ww / 2 : 1, hh = hh > 1 ? hh / 2 : 1)
            r_glTexImage2D(t, l, f, ww, hh, 0, f, 0x1401, NULL);
        r_glTexParameteri(t, 0x813D, lv - 1); /* TEXTURE_MAX_LEVEL: chain is complete with just these levels */
    } else
    r_glTexStorage2D(t, lv, f, w, h);
    { unsigned e = r_glGetError ? r_glGetError() : 0; if (e) fprintf(stderr, "[gl] glTexStorage2D ERROR 0x%x\n", e); }
}
SOFT static void w_glUniform1i(int l, int v) {
    static int c; if (c < 40) { c++; fprintf(stderr, "[gl] glUniform1i loc=%d val=%d\n", l, v); }
    r_glUniform1i(l, v);
}
SOFT static void w_glActiveTexture(unsigned u) {
    static int c; if (c < 20) { c++; fprintf(stderr, "[gl] glActiveTexture %x\n", u); }
    r_glActiveTexture(u);
}
SOFT static void w_glGenerateMipmap(unsigned t) {
    static int c; if (c < 20) { c++; fprintf(stderr, "[gl] glGenerateMipmap %x\n", t); }
    r_glGenerateMipmap(t);
}


static void dump_vbo(int vbo) {
    static void (*bindb)(unsigned, unsigned); static void *(*mapr)(unsigned, long, long, unsigned); static unsigned char (*unmap)(unsigned);
    static void (*getp)(unsigned, unsigned, void **); static void (*geti)(unsigned, int *); static void (*getbp)(unsigned, unsigned, int *);
    if (!bindb) { bindb = SDL_GL_GetProcAddress("glBindBuffer"); mapr = SDL_GL_GetProcAddress("glMapBufferRange"); unmap = SDL_GL_GetProcAddress("glUnmapBuffer");
        getp = SDL_GL_GetProcAddress("glGetVertexAttribPointerv"); geti = SDL_GL_GetProcAddress("glGetIntegerv"); getbp = SDL_GL_GetProcAddress("glGetBufferParameteriv"); }
    if (!bindb || !mapr || !vbo) return;
    int prev = 0; geti(0x8894, &prev);
    for (unsigned a = 0; a < 3; a++) { void *pp = 0; getp(a, 0x8645, &pp); fprintf(stderr, "[gl]   attrib %u pointer/offset=%p\n", a, pp); }
    bindb(0x8892, vbo);
    int size = 0; getbp(0x8892, 0x8764, &size);
    int n = size < 160 ? size : 160;
    fprintf(stderr, "[gl]   vbo %d size=%d first bytes:\n", vbo, size);
    unsigned char *m = mapr(0x8892, 0, n, 1);
    if (m) { for (int i = 0; i < n; i += 20) { fprintf(stderr, "[gl]   v%d:", i / 20); for (int j = 0; j < 20 && i + j < n; j++) fprintf(stderr, " %02x", m[i + j]); fprintf(stderr, "\n"); } unmap(0x8892); }
    else fprintf(stderr, "[gl]   map failed\n");
    bindb(0x8892, prev);
}
static void probe_state(void) {

    typedef void (*fiv_t)(unsigned, int *);
    typedef void (*fv_t)(int, int, float *);
    static fiv_t glGetIntegerv_; static void (*glGetActiveUniform_)(unsigned, unsigned, int, int *, int *, unsigned *, char *);
    static void (*glGetUniformfv_)(unsigned, int, float *); static int (*glGetUniformLocation_)(unsigned, const char *);
    static void (*glGetProgramiv_)(unsigned, unsigned, int *); static void (*glGetVertexAttribiv_)(unsigned, unsigned, int *);
    static void (*glGetVertexAttribfv_)(unsigned, unsigned, float *); static void (*glGetTexParameteriv_)(unsigned, unsigned, int *);
    static void (*glGetActiveAttrib_)(unsigned, unsigned, int, int *, int *, unsigned *, char *);
    static int (*glGetAttribLocation_)(unsigned, const char *);
    if (!glGetIntegerv_) {
        glGetIntegerv_ = SDL_GL_GetProcAddress("glGetIntegerv"); glGetActiveUniform_ = SDL_GL_GetProcAddress("glGetActiveUniform");
        glGetUniformfv_ = SDL_GL_GetProcAddress("glGetUniformfv"); glGetProgramiv_ = SDL_GL_GetProcAddress("glGetProgramiv");
        glGetVertexAttribiv_ = SDL_GL_GetProcAddress("glGetVertexAttribiv"); glGetVertexAttribfv_ = SDL_GL_GetProcAddress("glGetVertexAttribfv");
        glGetTexParameteriv_ = SDL_GL_GetProcAddress("glGetTexParameteriv"); glGetActiveAttrib_ = SDL_GL_GetProcAddress("glGetActiveAttrib");
        glGetAttribLocation_ = SDL_GL_GetProcAddress("glGetAttribLocation");
    }
    int prog = 0, tex = 0, unit = 0, nu = 0, na = 0;
    glGetIntegerv_(0x8B8D, &prog); glGetIntegerv_(0x8069, &tex); glGetIntegerv_(0x84E0, &unit);
    fprintf(stderr, "[gl] STATE program=%d tex2d_binding=%d active_unit=%x\n", prog, tex, unit);
    if (tex) { int v[4] = {0}; for (int i = 0; i < 4; i++) glGetTexParameteriv_(0x0DE1, 0x2800 + i, &v[i]); fprintf(stderr, "[gl]   tex params mag=%x min=%x wrapS=%x wrapT=%x\n", v[0], v[1], v[2], v[3]); }
    if (tex) {
        static void (*gfb)(int, unsigned *), (*bfb)(unsigned, unsigned), (*ftx)(unsigned, unsigned, unsigned, unsigned, int), (*dfb)(int, const unsigned *);
        static unsigned (*cfs)(unsigned); static void (*gtl)(unsigned, int, unsigned, int *);
        if (!gfb) { gfb = SDL_GL_GetProcAddress("glGenFramebuffers"); bfb = SDL_GL_GetProcAddress("glBindFramebuffer"); ftx = SDL_GL_GetProcAddress("glFramebufferTexture2D");
            cfs = SDL_GL_GetProcAddress("glCheckFramebufferStatus"); dfb = SDL_GL_GetProcAddress("glDeleteFramebuffers"); gtl = SDL_GL_GetProcAddress("glGetTexLevelParameteriv"); }
        int tw = 0, th = 0; if (gtl) { gtl(0x0DE1, 0, 0x1000, &tw); gtl(0x0DE1, 0, 0x1001, &th); }
        int prevfb = 0; glGetIntegerv_(0x8CA6, &prevfb);
        unsigned fb = 0; gfb(1, &fb); bfb(0x8D40, fb); ftx(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
        unsigned st = cfs(0x8D40);
        fprintf(stderr, "[gl]   bound tex %d size %dx%d fbo status=%x\n", tex, tw, th, st);
        if (st == 0x8CD5) { static const int tp[][2] = {{0,0},{1,1},{16,16},{70,330},{100,100},{200,200},{8,8},{300,50}};
            for (int i = 0; i < 8; i++) { unsigned char px[4] = {0}; if (!r_glReadPixels) r_glReadPixels = SDL_GL_GetProcAddress("glReadPixels");
                r_glReadPixels(tp[i][0], tp[i][1], 1, 1, 0x1908, 0x1401, px); fprintf(stderr, "[gl]   texel(%d,%d)= %02x %02x %02x %02x\n", tp[i][0], tp[i][1], px[0], px[1], px[2], px[3]); } }
        bfb(0x8D40, prevfb); dfb(1, &fb);
    }
    if (!prog) return;
    glGetProgramiv_(prog, 0x8B86, &nu); glGetProgramiv_(prog, 0x8B89, &na);
    for (int i = 0; i < nu; i++) {
        char nm[64] = ""; int sz = 0; unsigned ty = 0; glGetActiveUniform_(prog, i, 63, NULL, &sz, &ty, nm);
        int loc = glGetUniformLocation_ ? 0 : 0; (void)loc;
        static int (*gl)(unsigned, const char *); if (!gl) gl = SDL_GL_GetProcAddress("glGetUniformLocation");
        int l = gl(prog, nm); float v[16] = {0};
        if (l >= 0 && ty != 0x8B5A && ty != 0x8B5B && ty != 0x8B5C) glGetUniformfv_(prog, l, v);
        fprintf(stderr, "[gl]   uniform %s loc=%d type=%x = %g %g %g %g\n", nm, l, ty, v[0], v[1], v[2], v[3]);
    }
    for (int i = 0; i < na; i++) {
        char nm[64] = ""; int sz = 0; unsigned ty = 0; glGetActiveAttrib_(prog, i, 63, NULL, &sz, &ty, nm);
        static int (*ga)(unsigned, const char *); if (!ga) ga = SDL_GL_GetProcAddress("glGetAttribLocation");
        int l = ga(prog, nm); int en = 0, bb = 0, size = 0, type = 0, norm = 0, stride = 0; float cur[4] = {0};
        glGetVertexAttribiv_(l, 0x8622, &en); glGetVertexAttribiv_(l, 0x889F, &bb); glGetVertexAttribiv_(l, 0x8623, &size);
        glGetVertexAttribiv_(l, 0x8625, &type); glGetVertexAttribiv_(l, 0x886A, &norm); glGetVertexAttribiv_(l, 0x8624, &stride);
        glGetVertexAttribfv_(l, 0x8626, cur);
        if (i == 0) dump_vbo(bb);
        fprintf(stderr, "[gl]   attrib %s loc=%d enabled=%d vbo=%d size=%d type=%x norm=%d stride=%d current=%g %g %g %g\n", nm, l, en, bb, size, type, norm, stride, cur[0], cur[1], cur[2], cur[3]);
    }
}

static void dump_fb(int frame) {
    if (!r_glReadPixels) r_glReadPixels = SDL_GL_GetProcAddress("glReadPixels");
    int W = 640, H = 480;
    SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &W, &H);
    unsigned char *buf = malloc((size_t)W * H * 4);
    if (!buf || !r_glReadPixels) return;
    r_glReadPixels(0, 0, W, H, 0x1908, 0x1401, buf);
    const int GX = 40, GY = 24;
    fprintf(stderr, "[fb] frame %d %dx%d, %dx%d blocks, top row first, hex rgb (4 bits each)\n", frame, W, H, GX, GY);
    for (int by = GY - 1; by >= 0; by--) {
        char line[GX * 4 + 8]; int o = 0;
        for (int bx = 0; bx < GX; bx++) {
            unsigned r = 0, g = 0, b = 0, c = 0;
            for (int y = by * H / GY; y < (by + 1) * H / GY; y += 4)
                for (int x = bx * W / GX; x < (bx + 1) * W / GX; x += 4) { unsigned char *q = buf + ((size_t)y * W + x) * 4; r += q[0]; g += q[1]; b += q[2]; c++; }
            o += sprintf(line + o, "%x%x%x ", c ? r / c >> 4 : 0, c ? g / c >> 4 : 0, c ? b / c >> 4 : 0);
        }
        fprintf(stderr, "[fb] %s\n", line);
    }
    free(buf);
}
static void probe_pixels(void) {
    dump_fb(0);
    probe_state();
    if (!r_glReadPixels) r_glReadPixels = SDL_GL_GetProcAddress("glReadPixels");
    if (!r_glReadPixels) return;
    static const int pts[][2] = { {10, 10}, {320, 240}, {320, 400}, {100, 300}, {500, 100} };
    for (int i = 0; i < 5; i++) {
        unsigned char px[4] = {0};
        r_glReadPixels(pts[i][0], pts[i][1], 1, 1, 0x1908, 0x1401, px);
        fprintf(stderr, "[gl] px(%d,%d) = %02x %02x %02x %02x\n", pts[i][0], pts[i][1], px[0], px[1], px[2], px[3]);
    }
}

static void (*r_glGenTextures)(int, unsigned *);
static void (*r_glBindTexture)(unsigned, unsigned);
static void (*r_glDeleteTextures)(int, const unsigned *);
SOFT static void w_glGenTextures(int n, unsigned *ids) {
    static int c; r_glGenTextures(n, ids);
    if (c < 80) { c++; fprintf(stderr, "[gl] glGenTextures n=%d ->", n); for (int i = 0; i < n && i < 8; i++) fprintf(stderr, " %u", ids[i]); fprintf(stderr, "\n"); }
}
SOFT static void w_glBindTexture(unsigned t, unsigned id) {
    static int c; if (c < 120) { c++; fprintf(stderr, "[gl] glBindTexture %x %u\n", t, id); }
    r_glBindTexture(t, id);
}
SOFT static void w_glDeleteTextures(int n, const unsigned *ids) {
    fprintf(stderr, "[gl] glDeleteTextures n=%d first=%u\n", n, n > 0 ? ids[0] : 0);
    r_glDeleteTextures(n, ids);
}

#define MAP(n) { #n, (void **)&r_##n, (void *)w_##n }
static struct { const char *name; void **real; void *wrap; } wraps[] = {
    MAP(glUniform1f), MAP(glUniform2f), MAP(glUniform3f), MAP(glUniform4f),
    MAP(glClearColor), MAP(glClearDepthf), MAP(glDepthRangef), MAP(glLineWidth),
    MAP(glPolygonOffset), MAP(glBlendColor), MAP(glSampleCoverage), MAP(glTexParameterf),
    MAP(glVertexAttrib1f), MAP(glVertexAttrib2f), MAP(glVertexAttrib3f), MAP(glVertexAttrib4f),
    MAP(glCompileShader), MAP(glLinkProgram), MAP(glShaderSource), MAP(glTexImage2D), MAP(glTexStorage2DEXT), MAP(glGenTextures), MAP(glBindTexture), MAP(glDeleteTextures), MAP(glTexStorage2D), MAP(glUniform1i), MAP(glActiveTexture), MAP(glGenerateMipmap), MAP(glTexSubImage2D), MAP(glTexParameteri), MAP(glCompressedTexImage2D),
};

EXPORT void *eglGetProcAddress(const char *name) {
    for (unsigned i = 0; i < sizeof wraps / sizeof *wraps; i++) {
        if (!strcmp(name, wraps[i].name)) {
            if (!*wraps[i].real) *wraps[i].real = SDL_GL_GetProcAddress(name);
            return *wraps[i].real ? wraps[i].wrap : NULL;
        }
    }
    void *p = SDL_GL_GetProcAddress(name);
    { static int pc; if (pc < 400) { pc++; fprintf(stderr, "[egl] getProc %s %s\n", name, p ? "ok" : "MISSING"); } }
    if (!p) fprintf(stderr, "[egl] GL symbol not found: %s\n", name);
    return p;
}
