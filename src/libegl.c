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
extern volatile int host_ov_mode __attribute__((weak));      /* 0 off, 1 big FPS, 2 full stats */
extern char host_ov_lines[8][48] __attribute__((weak));      /* stats lines from the host sampler */
static void (*r_glGetProgramiv)(unsigned, unsigned, int *);
extern volatile int host_hide_idx __attribute__((weak));     /* draw-bisect: -1 off, else index into the programs drawn last frame */
static char ov_hide_txt[48];
static int sc_pct(void);
extern volatile int host_vsync_req __attribute__((weak));   /* -1 none, 0/1 = set swap interval on the render thread */
static int g_vsync = -1;
static char *hb_src[4096];
static void hb_frame_end(void);
static int fps_value;
static double ov_logic_ms, ov_logic_cpu, ov_swap_ms;         /* 0.5 s averages, filled in eglSwapBuffers */
static double ov_frame_avg, ov_frame_worst;
static int ov_shc_hits, ov_shc_miss; static double ov_shc_ms;
/* 5x7 font (rows top->bottom, 5 bits each), 6x8 cells */
static const unsigned char font57[][7] = {
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},
 {17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31} };
static const unsigned char *glyph(char c) {
    static const unsigned char blank[7] = {0,0,0,0,0,0,0}, dot[7] = {0,0,0,0,0,12,12}, colon[7] = {0,12,12,0,12,12,0}, pct[7] = {25,26,2,4,8,11,19}, slash[7] = {1,2,2,4,8,8,16}, dash[7] = {0,0,0,31,0,0,0}, plus[7] = {0,4,4,31,4,4,0};
    if (c >= '0' && c <= '9') return font57[c - '0'];
    if (c >= 'A' && c <= 'Z') return font57[10 + c - 'A'];
    if (c >= 'a' && c <= 'z') return font57[10 + c - 'a'];
    switch (c) { case '.': return dot; case ':': return colon; case '%': return pct; case '/': return slash; case '-': return dash; case '+': return plus; default: return blank; }
}
#define OVW 320
#define OVH 128
static unsigned char ov_buf[OVW * OVH * 4];
static unsigned ov_tex, ov_prog; static int ov_lines_used, ov_dirty = 1, ov_failed, ov_fps_chars;
static void ov_px(int x, int y, unsigned rgb, int a) { if (x < 0 || y < 0 || x >= OVW || y >= OVH) return; unsigned char *p = ov_buf + (y * OVW + x) * 4; p[0] = rgb >> 16; p[1] = rgb >> 8; p[2] = rgb; p[3] = a; }
static void ov_text(int line, const char *s, unsigned rgb) {
    int y0 = line * 8;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; s[i] && i < 52; i++) {
            const unsigned char *g = glyph(s[i]);
            for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (g[r] & (16 >> c)) {
                if (pass == 0) ov_px(i * 6 + c + 1, y0 + r + 1, 0, 230); else ov_px(i * 6 + c, y0 + r, rgb, 255); }
        }
}
static void ov_build(int mode) {
    memset(ov_buf, 0, sizeof ov_buf);
    char t[48]; snprintf(t, sizeof t, "FPS %d", fps_value);
    ov_text(0, t, fps_value >= 50 ? 0x60FF60 : fps_value >= 30 ? 0xFFE040 : 0xFF5050);
    ov_fps_chars = (int)strlen(t); ov_lines_used = 0;
    if (ov_hide_txt[0]) ov_text(1, ov_hide_txt, 0xFF8080);
    if (mode >= 2 && &host_ov_lines[0][0]) {
        int n = ov_hide_txt[0] ? 2 : 1; char l[64];
        snprintf(l, sizeof l, "FRAME %.1fMS WORST %.0f", ov_frame_avg, ov_frame_worst); ov_text(n++, l, 0xFFFFFF);
        snprintf(l, sizeof l, "LOGIC %.1f SWAP %.1f SCALE %d%%", ov_logic_ms, ov_swap_ms, sc_pct()); ov_text(n++, l, 0xFFFFFF);
        for (int i = 0; i < 8 && n < 15; i++) if (host_ov_lines[i][0]) { unsigned c = i < 2 ? 0x80FF80 : i < 4 ? 0x80C0FF : i < 6 ? 0xFFD080 : 0xD0D0D0; ov_text(n++, host_ov_lines[i], c); }
        snprintf(l, sizeof l, "SHADERS %d NEW %d", ov_shc_hits, ov_shc_miss); ov_text(n++, l, 0xD0D0D0);
        ov_lines_used = n - 1;
    }
    ov_dirty = 1;
}
static void draw_overlay(void) {
    static void (*gi)(unsigned, int *), (*en)(unsigned), (*dis)(unsigned), (*cm)(unsigned char, unsigned char, unsigned char, unsigned char), (*actv)(unsigned);
    static unsigned char (*ise)(unsigned);
    static void (*gen_t)(int, unsigned *), (*bind_t)(unsigned, unsigned), (*teximg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*texsub)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*texpar)(unsigned, unsigned, int);
    static unsigned (*mk_sh)(unsigned), (*mk_pr)(void); static void (*sh_src)(unsigned, int, const char *const *, const int *), (*sh_comp)(unsigned), (*pr_att)(unsigned, unsigned), (*pr_bind)(unsigned, unsigned, const char *), (*pr_link)(unsigned), (*use)(unsigned), (*u1i)(int, int);
    static int (*uloc)(unsigned, const char *);
    static void (*en_a)(unsigned), (*dis_a)(unsigned), (*vap)(unsigned, int, unsigned, unsigned char, int, const void *), (*draw)(unsigned, int, int), (*bfs)(unsigned, unsigned, unsigned, unsigned), (*bindb)(unsigned, unsigned), (*vp)(int, int, int, int), (*gvai)(unsigned, unsigned, int *), (*gvap)(unsigned, unsigned, void **);
    static unsigned t0, frames; static int last_mode = -1; static struct timespec last_ts; static double fsum, fworst; static int fcount;
    frames++; unsigned now = SDL_GetTicks();
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); if (last_ts.tv_sec) { double dt = (ts.tv_sec - last_ts.tv_sec) * 1e3 + (ts.tv_nsec - last_ts.tv_nsec) / 1e6; fsum += dt; fcount++; if (dt > fworst) fworst = dt; } last_ts = ts; }
    int mode = (&host_ov_mode) ? host_ov_mode : 1;
    if (now - t0 >= 500) { fps_value = (int)(frames * 1000.0f / (now - t0) + 0.5f); frames = 0; t0 = now;
        if (fcount) { ov_frame_avg = fsum / fcount; ov_frame_worst = fworst; } fsum = fworst = 0; fcount = 0; if (mode) ov_build(mode); }
    if (mode != last_mode) { last_mode = mode; if (mode) ov_build(mode); }
    if (!mode || ov_failed) return;
    if (!gi) { gi = SDL_GL_GetProcAddress("glGetIntegerv"); en = SDL_GL_GetProcAddress("glEnable"); dis = SDL_GL_GetProcAddress("glDisable"); cm = SDL_GL_GetProcAddress("glColorMask"); ise = SDL_GL_GetProcAddress("glIsEnabled");
        gen_t = SDL_GL_GetProcAddress("glGenTextures"); bind_t = SDL_GL_GetProcAddress("glBindTexture"); teximg = SDL_GL_GetProcAddress("glTexImage2D"); texsub = SDL_GL_GetProcAddress("glTexSubImage2D");
        texpar = SDL_GL_GetProcAddress("glTexParameteri"); actv = SDL_GL_GetProcAddress("glActiveTexture");
        mk_sh = SDL_GL_GetProcAddress("glCreateShader"); mk_pr = SDL_GL_GetProcAddress("glCreateProgram"); sh_src = SDL_GL_GetProcAddress("glShaderSource"); sh_comp = SDL_GL_GetProcAddress("glCompileShader");
        pr_att = SDL_GL_GetProcAddress("glAttachShader"); pr_bind = SDL_GL_GetProcAddress("glBindAttribLocation"); pr_link = SDL_GL_GetProcAddress("glLinkProgram"); use = SDL_GL_GetProcAddress("glUseProgram");
        uloc = SDL_GL_GetProcAddress("glGetUniformLocation"); u1i = SDL_GL_GetProcAddress("glUniform1i"); en_a = SDL_GL_GetProcAddress("glEnableVertexAttribArray"); dis_a = SDL_GL_GetProcAddress("glDisableVertexAttribArray");
        vap = SDL_GL_GetProcAddress("glVertexAttribPointer"); draw = SDL_GL_GetProcAddress("glDrawArrays"); bfs = SDL_GL_GetProcAddress("glBlendFuncSeparate"); bindb = SDL_GL_GetProcAddress("glBindBuffer");
        vp = SDL_GL_GetProcAddress("glViewport"); gvai = SDL_GL_GetProcAddress("glGetVertexAttribiv"); gvap = SDL_GL_GetProcAddress("glGetVertexAttribPointerv");
 }
    if (!gi || !en || !dis || !cm || !ise || !gen_t || !bind_t || !teximg || !texsub || !texpar || !actv || !mk_sh || !mk_pr || !sh_src || !sh_comp || !pr_att || !pr_bind || !pr_link || !use || !uloc || !u1i || !en_a || !dis_a || !vap || !draw || !bfs || !bindb || !vp || !gvai || !gvap) { ov_failed = 1; return; }
    int fb = 0; gi(0x8CA6, &fb); if (fb) return;           /* only draw into the window framebuffer */
    if (!ov_prog) {
        static const char *vs = "attribute vec2 p; attribute vec2 t; varying vec2 v; void main() { gl_Position = vec4(p, 0.0, 1.0); v = t; }";
        static const char *fs = "precision mediump float; uniform sampler2D s; varying vec2 v; void main() { gl_FragColor = texture2D(s, v); }";
        unsigned v = mk_sh(0x8B31), f = mk_sh(0x8B30); sh_src(v, 1, &vs, NULL); sh_comp(v); sh_src(f, 1, &fs, NULL); sh_comp(f);
        unsigned pr = mk_pr(); pr_att(pr, v); pr_att(pr, f); pr_bind(pr, 0, "p"); pr_bind(pr, 1, "t"); pr_link(pr);
        int ok = 0; ld(); r_glGetProgramiv(pr, 0x8B82, &ok);
        if (!ok) { fprintf(stderr, "[overlay] shader link failed, overlay disabled\n"); ov_failed = 1; return; }
        ov_prog = pr; }
    /* ---- save state we touch ---- */
    int prog = 0, abuf = 0, unit = 0, tex = 0, vpv[4], bsr = 0, bdr = 0, bsa = 0, bda = 0, sc_was = ise(0x0C11), bl_was = ise(0x0BE2), dt_was = ise(0x0B71), cf_was = ise(0x0B44), st_was = ise(0x0B90);
    unsigned char mask[4]; { static void (*gb)(unsigned, unsigned char *); if (!gb) gb = SDL_GL_GetProcAddress("glGetBooleanv"); if (!gb) return; gb(0x0C23, mask); }
    gi(0x8B8D, &prog); gi(0x8894, &abuf); gi(0x84E0, &unit); gi(0x0BA2, vpv); gi(0x80C9, &bsr); gi(0x80C8, &bdr); gi(0x80CB, &bsa); gi(0x80CA, &bda);
    struct { int en, size, type, norm, stride, buf; void *ptr; } at[2];
    for (int i = 0; i < 2; i++) { gvai(i, 0x8622, &at[i].en); gvai(i, 0x8623, &at[i].size); gvai(i, 0x8625, &at[i].type); gvai(i, 0x886A, &at[i].norm); gvai(i, 0x8624, &at[i].stride); gvai(i, 0x889F, &at[i].buf); gvap(i, 0x8645, &at[i].ptr); }
    actv(0x84C0); gi(0x8069, &tex);
    if (ov_dirty || !ov_tex) {
        if (!ov_tex) { gen_t(1, &ov_tex); bind_t(0x0DE1, ov_tex); texpar(0x0DE1, 0x2801, 0x2600); texpar(0x0DE1, 0x2800, 0x2600); texpar(0x0DE1, 0x2802, 0x812F); texpar(0x0DE1, 0x2803, 0x812F);
            teximg(0x0DE1, 0, 0x1908, OVW, OVH, 0, 0x1908, 0x1401, ov_buf); }
        else { bind_t(0x0DE1, ov_tex); texsub(0x0DE1, 0, 0, 0, OVW, OVH, 0x1908, 0x1401, ov_buf); }
        ov_dirty = 0;
    } else bind_t(0x0DE1, ov_tex);
    int W = 640, H = 480; SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &W, &H);
    vp(0, 0, W, H); if (sc_was) dis(0x0C11); if (dt_was) dis(0x0B71); if (cf_was) dis(0x0B44); if (st_was) dis(0x0B90); en(0x0BE2); cm(1, 1, 1, 1);
    bfs(0x0302, 0x0303, 1, 0x0303);
    use(ov_prog); u1i(uloc(ov_prog, "s"), 0); bindb(0x8892, 0); en_a(0); en_a(1);
    #define QUAD(px, py, pw, ph, u0, v0, u1, v1) do { float x0 = 2.0f * (px) / W - 1, x1 = 2.0f * ((px) + (pw)) / W - 1, y0 = 1 - 2.0f * (py) / H, y1 = 1 - 2.0f * ((py) + (ph)) / H; \
        float q[16] = { x0, y0, u0, v0,  x1, y0, u1, v0,  x0, y1, u0, v1,  x1, y1, u1, v1 }; vap(0, 2, 0x1406, 0, 16, q); vap(1, 2, 0x1406, 0, 16, q + 2); draw(0x0005, 0, 4); } while (0)
    { const int BS = 4; int cw = ov_fps_chars * 6; QUAD(8, 6, cw * BS, 8 * BS, 0.0f, 0.0f, (float)cw / OVW, 8.0f / OVH); }
    if (ov_lines_used > 0) { const int S = 2; int rows = ov_lines_used * 8; QUAD(8, 6 + 8 * 4 + 4, OVW * S, rows * S, 0.0f, 8.0f / OVH, 1.0f, (8.0f + rows) / OVH); }
    /* ---- restore ---- */
    use(prog);
    for (int i = 0; i < 2; i++) { bindb(0x8892, at[i].buf); vap(i, at[i].size, at[i].type, (unsigned char)at[i].norm, at[i].stride, at[i].ptr); if (at[i].en) en_a(i); else dis_a(i); }
    bindb(0x8892, abuf);
    bfs(bsr, bdr, bsa, bda);
    bind_t(0x0DE1, tex); actv(unit);
    vp(vpv[0], vpv[1], vpv[2], vpv[3]); cm(mask[0], mask[1], mask[2], mask[3]);
    if (!bl_was) dis(0x0BE2); if (dt_was) en(0x0B71); if (cf_was) en(0x0B44); if (st_was) en(0x0B90); if (sc_was) en(0x0C11);
}

/* ---- render scale: draw the game into a smaller offscreen target and upscale it at swap (less GPU work per frame) ---- */
extern volatile int host_scale_req __attribute__((weak));   /* -1 lower, +1 higher */
static const int sc_levels[] = {100, 85, 75, 67, 50};
static int sc_idx, sc_active, sc_w, sc_h, sc_fbo, sc_tex, sc_rb, sc_game_fb, sc_inited;
static void (*s_genfb)(int, unsigned *), (*s_bindfb)(unsigned, unsigned), (*s_fbtex)(unsigned, unsigned, unsigned, unsigned, int), (*s_fbrb)(unsigned, unsigned, unsigned, unsigned), (*s_genrb)(int, unsigned *), (*s_bindrb)(unsigned, unsigned), (*s_rbst)(unsigned, unsigned, int, int);
static void (*s_gent)(int, unsigned *), (*s_bindt)(unsigned, unsigned), (*s_teximg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*s_texpar)(unsigned, unsigned, int), (*s_blit)(int, int, int, int, int, int, int, int, unsigned, unsigned), (*s_vp)(int, int, int, int), (*s_gi)(unsigned, int *), (*s_en)(unsigned), (*s_dis)(unsigned);
static unsigned (*s_chk)(unsigned); static unsigned char (*s_ise)(unsigned);
static void (*r_glBindFramebuffer)(unsigned, unsigned), (*r_glViewport)(int, int, int, int), (*r_glScissor)(int, int, int, int);
static void sc_load(void) {
    if (s_blit) return;
    s_genfb = SDL_GL_GetProcAddress("glGenFramebuffers"); s_bindfb = SDL_GL_GetProcAddress("glBindFramebuffer"); s_fbtex = SDL_GL_GetProcAddress("glFramebufferTexture2D"); s_fbrb = SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    s_genrb = SDL_GL_GetProcAddress("glGenRenderbuffers"); s_bindrb = SDL_GL_GetProcAddress("glBindRenderbuffer"); s_rbst = SDL_GL_GetProcAddress("glRenderbufferStorage");
    s_gent = SDL_GL_GetProcAddress("glGenTextures"); s_bindt = SDL_GL_GetProcAddress("glBindTexture"); s_teximg = SDL_GL_GetProcAddress("glTexImage2D"); s_texpar = SDL_GL_GetProcAddress("glTexParameteri");
    s_blit = SDL_GL_GetProcAddress("glBlitFramebuffer"); s_vp = SDL_GL_GetProcAddress("glViewport"); s_gi = SDL_GL_GetProcAddress("glGetIntegerv"); s_en = SDL_GL_GetProcAddress("glEnable"); s_dis = SDL_GL_GetProcAddress("glDisable");
    s_chk = SDL_GL_GetProcAddress("glCheckFramebufferStatus"); s_ise = SDL_GL_GetProcAddress("glIsEnabled");
    if (!sc_inited) { sc_inited = 1; const char *d = getenv("MCPE_DATADIR"); if (d) { char pth[1200]; snprintf(pth, sizeof pth, "%s/scale.cfg", d); FILE *f = fopen(pth, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } if (v >= 0 && v < 5) sc_idx = v; } }
}
static int sc_ready(void) { return s_blit && s_genfb && s_bindfb && s_fbtex && s_fbrb && s_genrb && s_bindrb && s_rbst && s_gent && s_bindt && s_teximg && s_texpar && s_vp && s_gi && s_chk && s_en && s_dis && s_ise; }
static void sc_build(int W, int H) {
    sc_load(); if (!sc_ready()) { sc_active = 0; return; }
    int pct = sc_levels[sc_idx];
    if (pct >= 100) { sc_active = 0; return; }
    int w = (W * pct / 100) & ~1, h = (H * pct / 100) & ~1; if (w < 64) w = 64; if (h < 48) h = 48;
    int pf = 0, pt = 0, prb = 0; s_gi(0x8CA6, &pf); s_gi(0x8069, &pt); s_gi(0x8CA7, &prb);
    if (!sc_fbo) { s_genfb(1, (unsigned *)&sc_fbo); s_gent(1, (unsigned *)&sc_tex); s_genrb(1, (unsigned *)&sc_rb); }
    s_bindt(0x0DE1, sc_tex); s_teximg(0x0DE1, 0, 0x1908, w, h, 0, 0x1908, 0x1401, NULL);
    s_texpar(0x0DE1, 0x2801, 0x2601); s_texpar(0x0DE1, 0x2800, 0x2601); s_texpar(0x0DE1, 0x2802, 0x812F); s_texpar(0x0DE1, 0x2803, 0x812F);
    s_bindrb(0x8D41, sc_rb); s_rbst(0x8D41, 0x88F0, w, h);
    s_bindfb(0x8D40, sc_fbo); s_fbtex(0x8D40, 0x8CE0, 0x0DE1, sc_tex, 0); s_fbrb(0x8D40, 0x821A, 0x8D41, sc_rb);
    unsigned st = s_chk(0x8D40);
    s_bindfb(0x8D40, pf); s_bindt(0x0DE1, pt); s_bindrb(0x8D41, prb);
    if (st != 0x8CD5) { fprintf(stderr, "[scale] FBO %dx%d incomplete (0x%x), scaling disabled\n", w, h, st); sc_active = 0; return; }
    sc_w = w; sc_h = h; sc_active = 1; fprintf(stderr, "[scale] %d%% -> %dx%d offscreen target\n", pct, w, h);
}
static void sc_set_index(int idx, int W, int H) {
    sc_load(); if (idx < 0) idx = 0; if (idx > 4) idx = 4; sc_idx = idx;
    const char *d = getenv("MCPE_DATADIR"); if (d) { char pth[1200]; snprintf(pth, sizeof pth, "%s/scale.cfg", d); FILE *f = fopen(pth, "w"); if (f) { fprintf(f, "%d\n", sc_idx); fclose(f); } }
    sc_build(W, H);
}
SOFT static void w_glBindFramebuffer(unsigned t, unsigned f) { sc_game_fb = f; if (f == 0 && sc_active) f = sc_fbo; r_glBindFramebuffer(t, f); }
SOFT static void w_glViewport(int x, int y, int w, int h) { if (sc_active && sc_game_fb == 0) { int W = 640, H = 480; SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &W, &H); x = x * sc_w / W; w = w * sc_w / W; y = y * sc_h / H; h = h * sc_h / H; } r_glViewport(x, y, w, h); }
SOFT static void w_glScissor(int x, int y, int w, int h) { if (sc_active && sc_game_fb == 0) { int W = 640, H = 480; SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &W, &H); x = x * sc_w / W; w = w * sc_w / W; y = y * sc_h / H; h = h * sc_h / H; } r_glScissor(x, y, w, h); }
static int sc_vp_save[4], sc_sc_was;
static void sc_resolve(void) {  /* before the overlay: copy the small frame onto the window framebuffer */
    int W = 640, H = 480; SDL_GL_GetDrawableSize((SDL_Window *)shim_egl_window, &W, &H);
    if (&host_scale_req && host_scale_req != 0) { int rq = host_scale_req; host_scale_req = 0; sc_set_index(sc_idx + (rq > 0 ? -1 : 1), W, H); /* "higher" = more percent = lower index */ }
    if (!sc_inited) { sc_load(); if (sc_levels[sc_idx] < 100) sc_build(W, H); }
    if (!sc_active || sc_game_fb != 0) return;
    s_gi(0x0BA2, sc_vp_save); sc_sc_was = s_ise(0x0C11); if (sc_sc_was) s_dis(0x0C11);
    s_bindfb(0x8CA8, sc_fbo); s_bindfb(0x8CA9, 0);
    s_blit(0, 0, sc_w, sc_h, 0, 0, W, H, 0x4000, 0x2601);
    s_bindfb(0x8D40, 0); s_vp(0, 0, W, H);
}
static void sc_restore(void) {  /* after the swap: draw into the small target again */
    if (sc_active && sc_game_fb == 0) { s_bindfb(0x8D40, sc_fbo); s_vp(sc_vp_save[0], sc_vp_save[1], sc_vp_save[2], sc_vp_save[3]); if (sc_sc_was) s_en(0x0C11); }
    else if (!sc_active) { s_bindfb(0x8D40, sc_game_fb); }
}
static int sc_pct(void) { return sc_levels[sc_idx]; }
EXPORT int eglSwapBuffers(EGLhandle d, EGLhandle s) {
    static int n; (void)d; (void)s;
    ld();
    sc_resolve();
    if (n == 0) { int d = 0, st = 0; SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d); SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &st); fprintf(stderr, "[gl] depth bits %d stencil bits %d\n", d, st); }
    if (n == 0 && r_glGetString) fprintf(stderr, "[gl] VENDOR=%s RENDERER=%s VERSION=%s\n", (const char *)r_glGetString(0x1F00), (const char *)r_glGetString(0x1F01), (const char *)r_glGetString(0x1F02));
    if (n < 3 || n % 120 == 0) { unsigned e = r_glGetError ? r_glGetError() : 0; fprintf(stderr, "[egl] swap #%d glError=0x%x\n", n, e); }
    if (n == 200 || n == 1500) probe_pixels();
    n++;
    hb_frame_end();
    draw_overlay();
    { /* frame timing: wall + this thread's CPU time in the game vs. inside swap */
        static struct timespec w_end, c_end; static double g_wall, g_cpu, s_wall, s_cpu; static int frames, init; static struct timespec rep;
        struct timespec w0, c0, w1, c1; clock_gettime(CLOCK_MONOTONIC, &w0); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
        if (init) { double fw = (w0.tv_sec - w_end.tv_sec) * 1e3 + (w0.tv_nsec - w_end.tv_nsec) / 1e6; if (fw > 60) fprintf(stderr, "[slow] frame %.0f ms (cpu %.0f ms)\n", fw, (c0.tv_sec - c_end.tv_sec) * 1e3 + (c0.tv_nsec - c_end.tv_nsec) / 1e6); }
        if (init) { g_wall += (w0.tv_sec - w_end.tv_sec) * 1e3 + (w0.tv_nsec - w_end.tv_nsec) / 1e6; g_cpu += (c0.tv_sec - c_end.tv_sec) * 1e3 + (c0.tv_nsec - c_end.tv_nsec) / 1e6; }
        { static int fin = -1; static void (*r_glFinish)(void); if (fin < 0) { fin = getenv("MCPE_FINISH") != NULL; r_glFinish = SDL_GL_GetProcAddress("glFinish"); }
          if (fin && r_glFinish) { r_glFinish(); struct timespec wf; clock_gettime(CLOCK_MONOTONIC, &wf); static double f_wall; f_wall += (wf.tv_sec - w0.tv_sec) * 1e3 + (wf.tv_nsec - w0.tv_nsec) / 1e6; static int fc; if (++fc % 300 == 0) { fprintf(stderr, "[finish] avg glFinish %.1f ms\n", f_wall / 300); f_wall = 0; } w0 = wf; } }
        if (&host_vsync_req && host_vsync_req >= 0) { int v = host_vsync_req; host_vsync_req = -1; int r = SDL_GL_SetSwapInterval(v); g_vsync = v; fprintf(stderr, "[egl] vsync toggled -> %d (r=%d, now %d)\n", v, r, SDL_GL_GetSwapInterval()); }
        SDL_GL_SwapWindow((SDL_Window *)shim_egl_window);
        sc_restore();
        clock_gettime(CLOCK_MONOTONIC, &w1); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
        s_wall += (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6; s_cpu += (c1.tv_sec - c0.tv_sec) * 1e3 + (c1.tv_nsec - c0.tv_nsec) / 1e6;
        { static double a_g, a_c, a_s; static int a_n; static struct timespec a_t; double gw = 0, gc = 0;
          if (init) { gw = (w0.tv_sec - w_end.tv_sec) * 1e3 + (w0.tv_nsec - w_end.tv_nsec) / 1e6; gc = (c0.tv_sec - c_end.tv_sec) * 1e3 + (c0.tv_nsec - c_end.tv_nsec) / 1e6; }
          a_g += gw; a_c += gc; a_s += (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6; a_n++;
          if (!a_t.tv_sec) a_t = w1;
          if ((w1.tv_sec - a_t.tv_sec) * 1000 + (w1.tv_nsec - a_t.tv_nsec) / 1000000 >= 500 && a_n) { ov_logic_ms = a_g / a_n; ov_logic_cpu = a_c / a_n; ov_swap_ms = a_s / a_n; a_g = a_c = a_s = 0; a_n = 0; a_t = w1; } }
        w_end = w1; c_end = c1; init = 1; frames++;
        if (!rep.tv_sec) rep = w1;
        if (w1.tv_sec - rep.tv_sec >= 5) {
            fprintf(stderr, "[frame] %d frames/5s: game wall %.1f ms cpu %.1f ms | swap wall %.1f ms cpu %.1f ms (per frame)\n", frames, g_wall / frames, g_cpu / frames, s_wall / frames, s_cpu / frames);
            frames = 0; g_wall = g_cpu = s_wall = s_cpu = 0; rep = w1;
        }
    }
    return EGL_TRUE;
}
EXPORT int eglSwapInterval(EGLhandle d, EGLint i) { (void)d; const char *o = getenv("MCPE_SWAPINT"); int v = o ? atoi(o) : i; int r = SDL_GL_SetSwapInterval(v); g_vsync = v; fprintf(stderr, "[egl] swapInterval game=%d used=%d -> %d (now %d)\n", i, v, r, SDL_GL_GetSwapInterval()); return EGL_TRUE; }
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

static unsigned ft_cur; static int ft_rd[4096];
SOFT static void w_glUniform1f(int l, float a) { if (ft_cur < 4096 && l >= 0 && l == ft_rd[ft_cur]) { static int c; c++; if (c < 30 || c % 600 == 0) fprintf(stderr, "[fog] prog %u RENDER_DISTANCE = %.3f\n", ft_cur, a); } r_glUniform1f(l, a); }
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
    ld(); struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); r_glCompileShader(sh);
    int ok = 0; r_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    clock_gettime(CLOCK_MONOTONIC, &t1); { static int nc; static double tot; double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6; tot += ms; nc++; if (ms > 5) fprintf(stderr, "[slow] compile shader %u %.1f ms (total %d shaders %.0f ms)\n", sh, ms, nc, tot); }
    if (!ok) { char b[1024] = ""; r_glGetShaderInfoLog(sh, sizeof b - 1, NULL, b); fprintf(stderr, "[gl] SHADER %u COMPILE FAILED: %s\n", sh, b); }
}
/* ---- program binary cache: the game links ~700 programs lazily (15-70 ms each on Mali = frame hitches) ---- */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define PC_N 4096
static unsigned long long pc_shash[PC_N], pc_phash[PC_N]; static unsigned pc_att[PC_N][8]; static int pc_natt[PC_N];
static void (*r_glAttachShader)(unsigned, unsigned);
static void (*r_glBindAttribLocation)(unsigned, unsigned, const char *);
static void (*r_glGetProgramBinary)(unsigned, int, int *, unsigned *, void *);
static void (*r_glProgramBinary)(unsigned, unsigned, const void *, int);
static void (*r_glProgramParameteri)(unsigned, unsigned, int);
static unsigned long long fnv(unsigned long long h, const void *d, size_t n) { const unsigned char *p = d; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
SOFT static void w_glAttachShader(unsigned pr, unsigned sh) {
    if (pr < PC_N && pc_natt[pr] < 8) pc_att[pr][pc_natt[pr]++] = sh;
    r_glAttachShader(pr, sh);
}
SOFT static void w_glBindAttribLocation(unsigned pr, unsigned i, const char *name) {
    if (pr < PC_N) { pc_phash[pr] = fnv(pc_phash[pr] ? pc_phash[pr] : 1469598103934665603ULL, name, strlen(name)); pc_phash[pr] = fnv(pc_phash[pr], &i, sizeof i); }
    r_glBindAttribLocation(pr, i, name);
}
SOFT static void w_glShaderSource(unsigned sh, int n, const char *const *src, const int *len) {
    if (sh < PC_N) { unsigned long long h = 1469598103934665603ULL; size_t tot = 1; for (int i = 0; i < n; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]); h = fnv(h, src[i], l); tot += l; } pc_shash[sh] = h;
        free(hb_src[sh]); hb_src[sh] = malloc(tot); if (hb_src[sh]) { size_t o = 0; for (int i = 0; i < n; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]); memcpy(hb_src[sh] + o, src[i], l); o += l; } hb_src[sh][o] = 0; } }
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

/* ---- fog uniform tracing (diagnose black horizon band) ---- */
static void (*r_glUseProgram)(unsigned);
static int (*r_glGetUniformLocation)(unsigned, const char *);
static void (*r_glUniform4fv)(int, int, const float *);
static void (*r_glUniform1fv)(int, int, const float *);
static void (*r_glUniform3fv)(int, int, const float *);
static void (*r_glUniform2fv)(int, int, const float *);
static int ft_fc[PC_N], ft_fk[PC_N];
SOFT static void w_glUseProgram(unsigned p) { ft_cur = p; r_glUseProgram(p); }
SOFT static int w_glGetUniformLocation(unsigned p, const char *n) {
    int l = r_glGetUniformLocation(p, n);
    if (p < PC_N) { if (!ft_fc[p] && !ft_fk[p] && !ft_rd[p]) ft_fc[p] = ft_fk[p] = ft_rd[p] = -1;
        if (!strcmp(n, "FOG_COLOR")) ft_fc[p] = l; else if (!strcmp(n, "FOG_CONTROL")) ft_fk[p] = l; else if (!strcmp(n, "RENDER_DISTANCE")) ft_rd[p] = l; }
    return l;
}
static void ft_log(const char *what, const float *v, int n) {
    static int c, rdc; if (what[0] == 'R') { if (++rdc > 30 && rdc % 300) return; } c++; if (what[0] == 'R' || c < 40 || c % 600 == 0) { fprintf(stderr, "[fog] prog %u %s =", ft_cur, what); for (int i = 0; i < n; i++) fprintf(stderr, " %.3f", v[i]); fprintf(stderr, "\n"); }
}
SOFT static void w_glUniform4fv(int l, int c, const float *v) { if (ft_cur < PC_N && l >= 0 && l == ft_fc[ft_cur]) ft_log("FOG_COLOR", v, 4); r_glUniform4fv(l, c, v); }
SOFT static void w_glUniform1fv(int l, int c, const float *v) { if (ft_cur < PC_N && l >= 0 && l == ft_rd[ft_cur]) ft_log("RENDER_DISTANCE", v, 1); r_glUniform1fv(l, c, v); }
SOFT static void w_glUniform3fv(int l, int c, const float *v) { if (ft_cur < PC_N && l >= 0 && l == ft_fc[ft_cur]) ft_log("FOG_COLOR3", v, 3); r_glUniform3fv(l, c, v); }
SOFT static void w_glUniform2fv(int l, int c, const float *v) { if (ft_cur < PC_N && l >= 0 && l == ft_fk[ft_cur]) ft_log("FOG_CONTROL", v, 2); r_glUniform2fv(l, c, v); }

/* ---- draw bisect: hide all draws of one program (chosen with SELECT + d-pad left/right) to find what draws the horizon band ---- */
static void (*r_glDrawArrays)(unsigned, int, int);
static void (*r_glDrawElements)(unsigned, int, unsigned, const void *);
static unsigned hb_cur[64], hb_last[64]; static int hb_ncur, hb_nlast, hb_dr[64], hb_vt[64], hb_dr_l[64], hb_vt_l[64]; static unsigned hb_hide;
static unsigned hb_psh[PC_N][8]; static int hb_npsh[PC_N];
static unsigned char hb_plain[PC_N];
static int hb_compute_plain(unsigned pr) {
    for (int a = 0; a < hb_npsh[pr]; a++) { const char *t = hb_src[hb_psh[pr][a] < PC_N ? hb_psh[pr][a] : 0]; while (t && (t = strstr(t, "#define "))) { t += 8; const char *e = t; while (*e && *e != '\n' && *e != ' ' && *e != '(') e++;
        int n = (int)(e - t); if (!((n == 4 && !strncmp(t, "MAT4", 4)) || (n == 4 && !strncmp(t, "POS4", 4)) || (n == 4 && !strncmp(t, "POS3", 4)) || (n == 9 && !strncmp(t, "attribute", 9)) || (n == 7 && !strncmp(t, "varying", 7)) || (n == 9 && !strncmp(t, "texture2D", 9)) || (n == 12 && !strncmp(t, "gl_FragColor", 12)) || (n == 9 && !strncmp(t, "_centroid", 9)))) return 0; t = e; } }
    return hb_npsh[pr] > 0;
}
static int hb_void_skip = 1;
static void hb_state(const char *tag) {
    static unsigned char (*ise)(unsigned); static void (*gi)(unsigned, int *), (*gb)(unsigned, unsigned char *); if (!ise) { ise = SDL_GL_GetProcAddress("glIsEnabled"); gi = SDL_GL_GetProcAddress("glGetIntegerv"); gb = SDL_GL_GetProcAddress("glGetBooleanv"); }
    if (!ise || !gi || !gb) return; int df = 0, vpv[4]; unsigned char dm = 0; gi(0x0B74, &df); gb(0x0B72, &dm); gi(0x0BA2, vpv);
    fprintf(stderr, "[hide-state] %s depthTest=%d depthMask=%d depthFunc=0x%x blend=%d cull=%d viewport=%d,%d,%d,%d prog=%u\n", tag, ise(0x0B71), dm, df, ise(0x0BE2), ise(0x0B44), vpv[0], vpv[1], vpv[2], vpv[3], ft_cur);
}
static void hb_state_log(void) { static unsigned last; static int n; if (last != hb_hide) { last = hb_hide; n = 0; } if (n++ < 2) hb_state("hidden draw"); }
static void hb_state_log_v(void) { static int n; if (n++ < 3) hb_state("void plane"); }
static int hb_slot(void) { for (int i = 0; i < hb_ncur; i++) if (hb_cur[i] == ft_cur) return i; if (hb_ncur < 64) { hb_cur[hb_ncur] = ft_cur; hb_dr[hb_ncur] = hb_vt[hb_ncur] = 0; return hb_ncur++; } return -1; }
SOFT static void w_glDrawArrays(unsigned m, int f, int c) { int k = hb_slot(); if (k >= 0) { hb_dr[k]++; hb_vt[k] += c; } if (hb_hide && ft_cur == hb_hide) { hb_state_log(); return; } if (hb_void_skip && c == 30 && ft_cur < PC_N && hb_plain[ft_cur]) { static int n; if (n++ < 3) fprintf(stderr, "[void] skipping dark horizon plane draw (prog %u)\n", ft_cur); hb_state_log_v(); return; } r_glDrawArrays(m, f, c); }
SOFT static void w_glDrawElements(unsigned m, int c, unsigned t, const void *p) { int k = hb_slot(); if (k >= 0) { hb_dr[k]++; hb_vt[k] += c; } if (hb_hide && ft_cur == hb_hide) { hb_state_log(); return; } if (hb_void_skip && c == 30 && ft_cur < PC_N && hb_plain[ft_cur]) { static int n; if (n++ < 3) fprintf(stderr, "[void] skipping dark horizon plane draw (prog %u)\n", ft_cur); hb_state_log_v(); return; } r_glDrawElements(m, c, t, p); }
static void hb_frame_end(void) {
    memcpy(hb_last, hb_cur, sizeof hb_cur); memcpy(hb_dr_l, hb_dr, sizeof hb_dr); memcpy(hb_vt_l, hb_vt, sizeof hb_vt); hb_nlast = hb_ncur; hb_ncur = 0;
    static int last_idx = -2; int idx = (&host_hide_idx) ? host_hide_idx : -1;
    if (idx < 0) { hb_hide = 0; ov_hide_txt[0] = 0; }
    else if (hb_nlast > 0) { int k = idx % hb_nlast; hb_hide = hb_last[k]; snprintf(ov_hide_txt, sizeof ov_hide_txt, "HIDE %d/%d PROG %u DRAWS %d", k + 1, hb_nlast, hb_hide, hb_dr_l[k]);
        if (idx != last_idx) { fprintf(stderr, "[hide] %d/%d prog %u draws %d verts %d defines:", k + 1, hb_nlast, hb_hide, hb_dr_l[k], hb_vt_l[k]);
            for (int a = 0; a < hb_npsh[hb_hide]; a++) { const char *t = hb_src[hb_psh[hb_hide][a] < PC_N ? hb_psh[hb_hide][a] : 0]; while (t && (t = strstr(t, "#define "))) { t += 8; const char *e = t; while (*e && *e != '\n' && *e != ' ' && *e != '(') e++; fprintf(stderr, " %.*s", (int)(e - t), t); t = e; } }
            fprintf(stderr, "\n"); } }
    last_idx = idx;
}
static int pc_try_load(unsigned pr, const char *path) {
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat st; if (fstat(fd, &st) || st.st_size < 8) { close(fd); return 0; }
    char *buf = malloc(st.st_size); int ok = 0;
    if (buf && read(fd, buf, st.st_size) == st.st_size) { unsigned fmt = *(unsigned *)buf; r_glProgramBinary(pr, fmt, buf + 4, (int)st.st_size - 4); int ls = 0; r_glGetProgramiv(pr, GL_LINK_STATUS, &ls); ok = ls; }
    free(buf); close(fd); return ok;
}
static void pc_save(unsigned pr, const char *path) {
    int len = 0; r_glGetProgramiv(pr, 0x8741, &len); if (len <= 0 || len > (8 << 20)) return;
    char *buf = malloc(len + 4); if (!buf) return; int got = 0; unsigned fmt = 0;
    r_glGetProgramBinary(pr, len, &got, &fmt, buf + 4);
    if (got > 0) { *(unsigned *)buf = fmt; char tmp[1400]; snprintf(tmp, sizeof tmp, "%s.tmp", path); int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { int okw = write(fd, buf, got + 4) == got + 4; close(fd); if (okw) rename(tmp, path); else unlink(tmp); } }
    free(buf);
}
SOFT static void w_glLinkProgram(unsigned pr) {
    ld(); struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    static int init, usable; static char dir[1100]; static int hits, misses;
    if (!init) { init = 1; const char *d = getenv("MCPE_DATADIR");
        r_glGetProgramBinary = SDL_GL_GetProcAddress("glGetProgramBinary"); r_glProgramBinary = SDL_GL_GetProcAddress("glProgramBinary"); r_glProgramParameteri = SDL_GL_GetProcAddress("glProgramParameteri");
        if (d && r_glGetProgramBinary && r_glProgramBinary && !getenv("MCPE_NOSHCACHE")) { snprintf(dir, sizeof dir, "%s/shadercache", d); mkdir(dir, 0755); usable = 1; }
        fprintf(stderr, "[shcache] %s (dir %s)\n", usable ? "enabled" : "disabled", dir); }
    int cached = 0; char path[1300] = "";
    if (usable && pr < PC_N && pc_natt[pr] > 0) {
        unsigned long long h = pc_phash[pr] ? pc_phash[pr] : 1469598103934665603ULL;
        h = fnv(h, getenv("MCPE_PREC") ? getenv("MCPE_PREC") : "highp", 5);
        const char *rn = (const char *)r_glGetString(0x1F01), *vn = (const char *)r_glGetString(0x1F02);
        if (rn) h = fnv(h, rn, strlen(rn)); if (vn) h = fnv(h, vn, strlen(vn));
        for (int i = 0; i < pc_natt[pr]; i++) { unsigned sh = pc_att[pr][i]; unsigned long long sv = sh < PC_N ? pc_shash[sh] : 0; h = fnv(h, &sv, sizeof sv); }
        snprintf(path, sizeof path, "%s/%016llx.bin", dir, h);
        cached = pc_try_load(pr, path);
        if (cached) hits++;
        ov_shc_hits = hits;
    }
    if (!cached) { if (r_glProgramParameteri) r_glProgramParameteri(pr, 0x8257, 1); r_glLinkProgram(pr); }
    int ok = 0; r_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!cached && ok && path[0]) { misses++; ov_shc_miss = misses; pc_save(pr, path); }
    if (pr < PC_N) { hb_npsh[pr] = pc_natt[pr]; memcpy(hb_psh[pr], pc_att[pr], sizeof hb_psh[pr]); hb_plain[pr] = (unsigned char)hb_compute_plain(pr); pc_natt[pr] = 0; pc_phash[pr] = 0; }
    clock_gettime(CLOCK_MONOTONIC, &t1); { static int nl; static double tot; double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6; tot += ms; nl++; if (ms > 5 || nl % 50 == 0) fprintf(stderr, "[slow] link program %u %.1f ms %s (total %d programs %.0f ms; cache hits %d misses %d)\n", pr, ms, cached ? "(cached)" : "", nl, tot, hits, misses); }
    if (!ok) { char b[1024] = ""; r_glGetProgramInfoLog(pr, sizeof b - 1, NULL, b); fprintf(stderr, "[gl] PROGRAM %u LINK FAILED: %s\n", pr, b); }
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
    MAP(glBindFramebuffer), MAP(glViewport), MAP(glScissor), MAP(glDrawArrays), MAP(glDrawElements), MAP(glUseProgram), MAP(glGetUniformLocation), MAP(glUniform4fv), MAP(glUniform1fv), MAP(glUniform3fv), MAP(glUniform2fv), MAP(glCompileShader), MAP(glAttachShader), MAP(glBindAttribLocation), MAP(glLinkProgram), MAP(glShaderSource), MAP(glTexImage2D), MAP(glTexStorage2DEXT), MAP(glGenTextures), MAP(glBindTexture), MAP(glDeleteTextures), MAP(glTexStorage2D), MAP(glUniform1i), MAP(glActiveTexture), MAP(glGenerateMipmap), MAP(glTexSubImage2D), MAP(glTexParameteri), MAP(glCompressedTexImage2D),
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
