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
EXPORT int eglSwapBuffers(EGLhandle d, EGLhandle s) {
    static int n; (void)d; (void)s;
    ld();
    if (n == 0 && r_glGetString) fprintf(stderr, "[gl] VENDOR=%s RENDERER=%s VERSION=%s\n", (const char *)r_glGetString(0x1F00), (const char *)r_glGetString(0x1F01), (const char *)r_glGetString(0x1F02));
    if (n < 3 || n % 120 == 0) { unsigned e = r_glGetError ? r_glGetError() : 0; fprintf(stderr, "[egl] swap #%d glError=0x%x\n", n, e); }
    if (n == 200 || n == 600) probe_pixels();
    n++;
    SDL_GL_SwapWindow((SDL_Window *)shim_egl_window); return EGL_TRUE;
}
EXPORT int eglSwapInterval(EGLhandle d, EGLint i) { (void)d; SDL_GL_SetSwapInterval(i); return EGL_TRUE; }
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
    r_glShaderSource(sh, n, src, len);
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
    r_glTexStorage2D(t, lv, f, w, h);
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
static void probe_pixels(void) {
    if (!r_glReadPixels) r_glReadPixels = SDL_GL_GetProcAddress("glReadPixels");
    if (!r_glReadPixels) return;
    static const int pts[][2] = { {10, 10}, {320, 240}, {320, 400}, {100, 300}, {500, 100} };
    for (int i = 0; i < 5; i++) {
        unsigned char px[4] = {0};
        r_glReadPixels(pts[i][0], pts[i][1], 1, 1, 0x1908, 0x1401, px);
        fprintf(stderr, "[gl] px(%d,%d) = %02x %02x %02x %02x\n", pts[i][0], pts[i][1], px[0], px[1], px[2], px[3]);
    }
}

#define MAP(n) { #n, (void **)&r_##n, (void *)w_##n }
static struct { const char *name; void **real; void *wrap; } wraps[] = {
    MAP(glUniform1f), MAP(glUniform2f), MAP(glUniform3f), MAP(glUniform4f),
    MAP(glClearColor), MAP(glClearDepthf), MAP(glDepthRangef), MAP(glLineWidth),
    MAP(glPolygonOffset), MAP(glBlendColor), MAP(glSampleCoverage), MAP(glTexParameterf),
    MAP(glVertexAttrib1f), MAP(glVertexAttrib2f), MAP(glVertexAttrib3f), MAP(glVertexAttrib4f),
    MAP(glCompileShader), MAP(glLinkProgram), MAP(glShaderSource), MAP(glTexImage2D), MAP(glTexStorage2DEXT), MAP(glTexStorage2D), MAP(glUniform1i), MAP(glActiveTexture), MAP(glGenerateMipmap), MAP(glTexSubImage2D), MAP(glTexParameteri), MAP(glCompressedTexImage2D),
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
