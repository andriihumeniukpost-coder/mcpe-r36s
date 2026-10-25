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
EXPORT int eglSwapBuffers(EGLhandle d, EGLhandle s) { (void)d; (void)s; SDL_GL_SwapWindow((SDL_Window *)shim_egl_window); return EGL_TRUE; }
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

#define MAP(n) { #n, (void **)&r_##n, (void *)w_##n }
static struct { const char *name; void **real; void *wrap; } wraps[] = {
    MAP(glUniform1f), MAP(glUniform2f), MAP(glUniform3f), MAP(glUniform4f),
    MAP(glClearColor), MAP(glClearDepthf), MAP(glDepthRangef), MAP(glLineWidth),
    MAP(glPolygonOffset), MAP(glBlendColor), MAP(glSampleCoverage), MAP(glTexParameterf),
    MAP(glVertexAttrib1f), MAP(glVertexAttrib2f), MAP(glVertexAttrib3f), MAP(glVertexAttrib4f),
};

EXPORT void *eglGetProcAddress(const char *name) {
    for (unsigned i = 0; i < sizeof wraps / sizeof *wraps; i++) {
        if (!strcmp(name, wraps[i].name)) {
            if (!*wraps[i].real) *wraps[i].real = SDL_GL_GetProcAddress(name);
            return *wraps[i].real ? wraps[i].wrap : NULL;
        }
    }
    void *p = SDL_GL_GetProcAddress(name);
    if (!p) fprintf(stderr, "[egl] GL symbol not found: %s\n", name);
    return p;
}
