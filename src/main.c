/* Host launcher: loads the unmodified Android libminecraftpe.so on a glibc
 * ARM handheld (R36S / ArkOS). Creates the SDL2 window + GLES2 context, builds
 * a fake ANativeActivity, starts the game, and turns gamepad input into
 * Android key/motion events. */
#define _GNU_SOURCE
#include "android_min.h"
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <execinfo.h>

extern const char *g_assets_root;
JNIEnv *fake_env(void);
JavaVM *fake_vm(void);
jobject fake_activity_obj(void);

static void crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    void *bt[48];
    fprintf(stderr, "\n[host] CRASH signal %d at address %p\n", sig, si->si_addr);
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

static char base[1024], libdir[1100], datadir[1100], assetdir[1100];

/* Android libs carry PT_INTERP=/system/bin/linker; neutralise it (PT_NULL) so glibc's loader ignores it. */
static void patch_interp(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    unsigned char eh[52];
    if (fread(eh, 1, 52, f) == 52 && eh[0] == 0x7f && eh[1] == 'E' && eh[4] == 1) {
        uint32_t phoff = eh[28] | eh[29] << 8 | eh[30] << 16 | (uint32_t)eh[31] << 24;
        unsigned phentsize = eh[42] | eh[43] << 8, phnum = eh[44] | eh[45] << 8;
        for (unsigned i = 0; i < phnum; i++) {
            long off = (long)phoff + (long)i * phentsize;
            unsigned char t[4];
            fseek(f, off, SEEK_SET);
            if (fread(t, 1, 4, f) != 4) break;
            if (t[0] == 3 && !t[1] && !t[2] && !t[3]) {   /* PT_INTERP */
                unsigned char z[4] = {0, 0, 0, 0};
                fseek(f, off, SEEK_SET);
                fwrite(z, 1, 4, f);
                fprintf(stderr, "[host] patched PT_INTERP in %s\n", path);
            }
        }
    }
    fclose(f);
}

static void *open_lib(const char *name, int flags) {
    char p[1300];
    snprintf(p, sizeof p, "%s/%s", libdir, name);
    if (!strcmp(name, "libfmod.so")) patch_interp(p);
    void *h = dlopen(p, flags);
    if (!h) { fprintf(stderr, "[host] dlopen(%s) failed: %s\n", p, dlerror()); exit(1); }
    return h;
}
static void *need(void *h, const char *sym) {
    void *p = dlsym(h, sym);
    if (!p) { fprintf(stderr, "[host] missing symbol %s\n", sym); exit(1); }
    return p;
}

/* ------------------------------------------------ input mapping */
static AInputQueue *queue;
static void (*queue_push)(AInputQueue *, const AInputEvent *);
static float axes[MAX_AXES];

static void send_key(int action, int keycode) {
    AInputEvent e; memset(&e, 0, sizeof e);
    e.type = EV_KEY; e.source = AINPUT_SOURCE_GAMEPAD; e.device = 1;
    e.action = action; e.keycode = keycode;
    queue_push(queue, &e);
}
static void send_axes(void) {
    AInputEvent e; memset(&e, 0, sizeof e);
    e.type = EV_MOTION; e.source = AINPUT_SOURCE_JOYSTICK; e.device = 1;
    e.action = 2; /* ACTION_MOVE */
    e.pointer_count = 1;
    memcpy(e.axis, axes, sizeof axes);
    queue_push(queue, &e);
}
static int map_button(int b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return 96;
        case SDL_CONTROLLER_BUTTON_B: return 97;
        case SDL_CONTROLLER_BUTTON_X: return 99;
        case SDL_CONTROLLER_BUTTON_Y: return 100;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 102;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 103;
        case SDL_CONTROLLER_BUTTON_START: return 108;
        case SDL_CONTROLLER_BUTTON_BACK: return 109;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 106;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 107;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return 19;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 20;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 21;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 22;
        default: return 0;
    }
}
static int map_axis(int a) {
    switch (a) {
        case SDL_CONTROLLER_AXIS_LEFTX: return AXIS_X;
        case SDL_CONTROLLER_AXIS_LEFTY: return AXIS_Y;
        case SDL_CONTROLLER_AXIS_RIGHTX: return AXIS_Z;
        case SDL_CONTROLLER_AXIS_RIGHTY: return AXIS_RZ;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return AXIS_LTRIGGER;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return AXIS_RTRIGGER;
        default: return -1;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    char exe[1024]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) { perror("readlink"); return 1; }
    exe[n] = 0;
    snprintf(base, sizeof base, "%s", dirname(exe));
    snprintf(libdir, sizeof libdir, "%s/libs", base);
    snprintf(datadir, sizeof datadir, "%s/data", base);
    snprintf(assetdir, sizeof assetdir, "%s/assets", base);
    mkdir(datadir, 0755);
    g_assets_root = assetdir;
    setvbuf(stderr, NULL, _IONBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
    fprintf(stderr, "[host] start, base=%s\n", base);
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[host] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    char *gcdb = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
    if (gcdb) SDL_GameControllerAddMappingsFromFile(gcdb);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_Window *win = SDL_CreateWindow("Minecraft PE", 0, 0, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "[host] window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "[host] GLES2 context: %s\n", SDL_GetError()); return 1; }
    fprintf(stderr, "[host] GL_RENDERER: %s\n", "(see glGetString in game log)");
    SDL_GL_MakeCurrent(win, NULL); /* the game's render thread will bind it */

    /* Shim libraries first, in the order the game lists them */
    open_lib("libc.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libm.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libdl.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("liblog.so", RTLD_NOW | RTLD_LOCAL);
    void *hand = open_lib("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    void *hegl = open_lib("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libGLESv1_CM.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libOpenSLES.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libz.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libstdc++.so", RTLD_NOW | RTLD_LOCAL);   /* separate step so a failure names the culprit */
    void *hfmod = open_lib("libfmod.so", RTLD_LAZY | RTLD_LOCAL);

    *(void **)need(hegl, "shim_egl_window") = win;
    *(void **)need(hegl, "shim_egl_ctx") = ctx;
    AAssetManager *(*mk_assets)(const char *) = need(hand, "shim_asset_manager_create");
    AInputQueue *(*mk_queue)(void) = need(hand, "shim_queue_create");
    queue_push = need(hand, "shim_queue_push");
    volatile int *finish = need(hand, "shim_finish_requested");

    char gamepath[1300];
    snprintf(gamepath, sizeof gamepath, "%s/libminecraftpe.so", libdir);
    patch_interp(gamepath);
    void *game = dlopen(gamepath, RTLD_LAZY | RTLD_DEEPBIND);
    if (!game) { fprintf(stderr, "[host] cannot load game: %s\n", dlerror()); return 1; }
    fprintf(stderr, "[host] libminecraftpe.so loaded\n");

    jint (*JNI_OnLoad_)(JavaVM *, void *) = dlsym(game, "JNI_OnLoad");
    void (*ANativeActivity_onCreate_)(ANativeActivity *, void *, size_t) = need(game, "ANativeActivity_onCreate");
    /* Android calls JNI_OnLoad on System.loadLibrary; glibc does not, so do it ourselves */
    jint (*fmod_onload)(JavaVM *, void *) = dlsym(hfmod, "JNI_OnLoad");
    if (fmod_onload) fmod_onload(fake_vm(), NULL);
    if (JNI_OnLoad_) JNI_OnLoad_(fake_vm(), NULL);

    static ANativeActivity act;
    act.callbacks = calloc(1, sizeof *act.callbacks);
    act.vm = fake_vm(); act.env = fake_env(); act.clazz = fake_activity_obj();
    act.internalDataPath = datadir; act.externalDataPath = datadir; act.obbPath = datadir;
    act.sdkVersion = 21;
    act.assetManager = mk_assets(assetdir);
    ANativeActivity_onCreate_(&act, NULL, 0);

    queue = mk_queue();
    ANativeActivityCallbacks *cb = act.callbacks;
    if (cb->onStart) cb->onStart(&act);
    if (cb->onResume) cb->onResume(&act);
    if (cb->onNativeWindowCreated) cb->onNativeWindowCreated(&act, (ANativeWindow *)win);
    if (cb->onInputQueueCreated) cb->onInputQueueCreated(&act, queue);
    if (cb->onWindowFocusChanged) cb->onWindowFocusChanged(&act, 1);
    fprintf(stderr, "[host] activity started\n");

    SDL_GameController *pad = NULL;
    int running = 1;
    while (running && !*finish) {
        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 100)) continue;
        do {
            switch (ev.type) {
                case SDL_QUIT: running = 0; break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (!pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    int kc = map_button(ev.cbutton.button);
                    if (kc) send_key(ev.type == SDL_CONTROLLERBUTTONDOWN ? 0 : 1, kc);
                    break;
                }
                case SDL_CONTROLLERAXISMOTION: {
                    int a = map_axis(ev.caxis.axis);
                    if (a >= 0) {
                        float v = ev.caxis.value / 32767.0f;
                        if (v > -0.15f && v < 0.15f) v = 0;
                        axes[a] = v;
                        send_axes();
                    }
                    break;
                }
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    if (ev.key.keysym.sym == SDLK_ESCAPE) send_key(ev.type == SDL_KEYDOWN ? 0 : 1, 4); /* BACK */
                    break;
            }
        } while (SDL_PollEvent(&ev));
    }

    if (cb->onPause) cb->onPause(&act);
    if (cb->onStop) cb->onStop(&act);
    if (cb->onDestroy) cb->onDestroy(&act);
    SDL_Quit();
    return 0;
}
