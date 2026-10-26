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
#include <errno.h>
#include <fcntl.h>
#include <execinfo.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <glob.h>
#include <pthread.h>
#include "elf_loader.h"

extern const char *g_assets_root;
JNIEnv *fake_env(void);
JavaVM *fake_vm(void);
jobject fake_activity_obj(void);

static void scan_stack(uintptr_t *sp, int words) {
    int shown = 0;
    for (int i = 0; i < words && shown < 60; i++) {
        if ((((uintptr_t)(sp + i)) & 4095) == 0 || i == 0) {      /* never read past the end of the stack */
            unsigned char vec; if (mincore((void *)((uintptr_t)(sp + i) & ~4095u), 1, &vec)) break;
        }
        uintptr_t off, w = sp[i]; const char *n = elf_where(w, &off);
        if (n) { fprintf(stderr, "[host]   stack[%d] %s+0x%lx\n", i, n, (unsigned long)off); shown++; continue; }
        Dl_info di;
        if (w > 0x10000 && i < 600 && dladdr((void *)w, &di) && di.dli_sname && w - (uintptr_t)di.dli_saddr < 0x4000) {
            const char *f = strrchr(di.dli_fname, '/');
            fprintf(stderr, "[host]   stack[%d] %s:%s+0x%lx\n", i, f ? f + 1 : di.dli_fname, di.dli_sname, (unsigned long)(w - (uintptr_t)di.dli_saddr));
            shown++;
        }
    }
}
/* lock tracing for mutexes that live inside the Android libs' data (static mutexes) */
void host_lockwatch(const char *what, void *word, void *ra) {
    static volatile int n;
    uintptr_t off, roff; const char *w = elf_where((uintptr_t)word, &off);
    if (!w || n > 120) return;
    n++;
    const char *rw = elf_where((uintptr_t)ra, &roff);
    fprintf(stderr, "[lock] tid=%d %s word=%s+0x%lx from %s+0x%lx\n", (int)syscall(SYS_gettid), what, w, (unsigned long)off, rw ? rw : "?", (unsigned long)roff);
}
#include <unwind.h>
struct bt_ctx { int n; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *uc, void *arg) {
    struct bt_ctx *c = arg; uintptr_t pc = _Unwind_GetIP(uc), off; const char *w = elf_where(pc, &off); Dl_info di;
    if (w) fprintf(stderr, "[bt] #%d %s+0x%lx\n", c->n, w, (unsigned long)off);
    else if (dladdr((void *)pc, &di)) fprintf(stderr, "[bt] #%d %s:%s+0x%lx\n", c->n, di.dli_fname ? di.dli_fname : "?", di.dli_sname ? di.dli_sname : "?", (unsigned long)(pc - (uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase)));
    else fprintf(stderr, "[bt] #%d %p\n", c->n, (void *)pc);
    return ++c->n > 40 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
/* called from the bionic-compat shim (raise/abort/kill...) to show who asked for it */
void host_report(const char *what, void *ra) {
    uintptr_t off; const char *w = elf_where((uintptr_t)ra, &off); if (!w) off = 0;
    fprintf(stderr, "[trace] tid=%d %s called from %s+0x%lx (ra=%p)\n", (int)syscall(SYS_gettid), what, w ? w : "?", (unsigned long)off, ra);
    volatile uintptr_t here = 0;
    { Dl_info di; if (dladdr(ra, &di)) fprintf(stderr, "[trace]   ra in %s (%s) +0x%lx\n", di.dli_fname ? di.dli_fname : "?", di.dli_sname ? di.dli_sname : "?", (unsigned long)((uintptr_t)ra - (uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase))); }
    { struct bt_ctx c = { 0 }; _Unwind_Backtrace(bt_cb, &c); fprintf(stderr, "[bt] %d frames\n", c.n); }
    scan_stack((uintptr_t *)&here, 300);
    fflush(stderr);
}


/* Interposers: calls from ld.so-loaded libs (and the shim) that end the process or a thread get reported */
int raise(int sig) {
    static int (*real)(int); char b[32]; snprintf(b, sizeof b, "raise(%d)", sig);
    host_report(b, __builtin_return_address(0));
    if (!real) real = dlsym(RTLD_NEXT, "raise");
    return real(sig);
}
int pthread_kill(pthread_t t, int sig) {
    static int (*real)(pthread_t, int); char b[48]; snprintf(b, sizeof b, "pthread_kill(%d)", sig);
    if (sig != SIGUSR2) host_report(b, __builtin_return_address(0));
    if (!real) real = dlsym(RTLD_NEXT, "pthread_kill");
    return real(t, sig);
}
void pthread_exit(void *r) {
    static void (*real)(void *) __attribute__((noreturn));
    host_report("pthread_exit", __builtin_return_address(0));
    if (!real) real = dlsym(RTLD_NEXT, "pthread_exit");
    real(r);
    for (;;) {}
}
void exit(int code) {
    static void (*real)(int) __attribute__((noreturn)); char b[32]; snprintf(b, sizeof b, "exit(%d)", code);
    host_report(b, __builtin_return_address(0));
    if (!real) real = dlsym(RTLD_NEXT, "exit");
    real(code);
    for (;;) {}
}

static void crash(int sig, siginfo_t *si, void *uc) {
    void *bt[48];
    fprintf(stderr, "\n[host] CRASH signal %d at address %p\n", sig, si->si_addr);
    {
        ucontext_t *u = uc; uintptr_t regs[3] = { u->uc_mcontext.arm_pc, u->uc_mcontext.arm_lr, u->uc_mcontext.arm_sp };
        const char *rn[3] = { "pc", "lr", "sp" };
        for (int i = 0; i < 3; i++) {
            uintptr_t off; const char *w = elf_where(regs[i], &off);
            fprintf(stderr, "[host]   %s = 0x%lx%s%s", rn[i], (unsigned long)regs[i], w ? "  = " : "", w ? w : "");
            if (w) fprintf(stderr, "+0x%lx", (unsigned long)off);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[host]   si_code=%d fault_address=0x%lx\n", si->si_code, (unsigned long)u->uc_mcontext.fault_address);
        fprintf(stderr, "[host]   r0=%lx r1=%lx r2=%lx r3=%lx r4=%lx r5=%lx r6=%lx r7=%lx r8=%lx r9=%lx r10=%lx fp=%lx ip=%lx\n",
                u->uc_mcontext.arm_r0, u->uc_mcontext.arm_r1, u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3, u->uc_mcontext.arm_r4,
                u->uc_mcontext.arm_r5, u->uc_mcontext.arm_r6, u->uc_mcontext.arm_r7, u->uc_mcontext.arm_r8, u->uc_mcontext.arm_r9,
                u->uc_mcontext.arm_r10, u->uc_mcontext.arm_fp, u->uc_mcontext.arm_ip);
        Dl_info di;
        for (int i = 0; i < 2; i++) {
            if (dladdr((void *)regs[i], &di))
                fprintf(stderr, "[host]   %s in %s (%s) %s+0x%lx\n", rn[i], di.dli_fname ? di.dli_fname : "?", di.dli_sname ? di.dli_sname : "?",
                        di.dli_sname ? "sym" : "base", (unsigned long)(regs[i] - (uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase)));
        }
        scan_stack((uintptr_t *)u->uc_mcontext.arm_sp, 700);
    }
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

static char base[1024], libdir[1100], datadir[1100], assetdir[1100];

/* Android libs carry PT_INTERP=/system/bin/linker and a soft-float ELF flag. glibc's loader rejects such
 * files silently (reported as "No such file or directory"), so neutralise both in our copy:
 *  - PT_INTERP -> PT_NULL
 *  - e_flags: EF_ARM_ABI_FLOAT_SOFT -> EF_ARM_ABI_FLOAT_HARD (metadata only; the code still uses the soft ABI,
 *    which is why every call across the boundary goes through our SOFT-annotated wrappers). */
static void patch_android_elf(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    unsigned char eh[52];
    if (fread(eh, 1, 52, f) == 52 && eh[0] == 0x7f && eh[1] == 'E' && eh[4] == 1) {
        uint32_t flags = eh[36] | eh[37] << 8 | eh[38] << 16 | (uint32_t)eh[39] << 24;
        if (!(flags & 0x400)) {           /* no EF_ARM_ABI_FLOAT_HARD: soft (0x200) or unmarked */
            flags = (flags & ~0x600u) | 0x400u;
            unsigned char fb[4] = { flags & 0xff, (flags >> 8) & 0xff, (flags >> 16) & 0xff, (flags >> 24) & 0xff };
            fseek(f, 36, SEEK_SET);
            fwrite(fb, 1, 4, f);
            fprintf(stderr, "[host] patched e_flags (set hard-float marker) in %s\n", path);
        }
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

static void diag_file(const char *p) {
    struct stat st;
    if (stat(p, &st)) { fprintf(stderr, "[diag] stat(%s) failed: %s\n", p, strerror(errno)); return; }
    int fd = open(p, O_RDONLY);
    unsigned char h[52] = {0};
    ssize_t n = fd >= 0 ? read(fd, h, sizeof h) : -1;
    fprintf(stderr, "[diag] %s size=%lld open=%s read=%zd class=%d data=%d osabi=%d abiver=%d type=%d machine=%d "
            "version=%u flags=0x%x phentsize=%d phnum=%d\n", p, (long long)st.st_size, fd >= 0 ? "ok" : strerror(errno), n,
            h[4], h[5], h[7], h[8], h[16] | h[17] << 8, h[18] | h[19] << 8,
            h[20] | h[21] << 8 | h[22] << 16 | (unsigned)h[23] << 24,
            h[36] | h[37] << 8 | h[38] << 16 | (unsigned)h[39] << 24, h[42] | h[43] << 8, h[44] | h[45] << 8);
    if (fd >= 0) close(fd);
}

/* Fallback: copy a library to tmpfs and dlopen it from there (SD-card filesystems can be odd). */
static void *open_via_tmp(const char *path, const char *name, int flags) {
    char dst[256];
    snprintf(dst, sizeof dst, "/tmp/mcpe_%s", name);
    FILE *in = fopen(path, "rb"), *out = fopen(dst, "wb");
    if (!in || !out) { fprintf(stderr, "[host] tmp copy failed (%s)\n", strerror(errno)); if (in) fclose(in); if (out) fclose(out); return NULL; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    chmod(dst, 0755);
    void *h = dlopen(dst, flags);
    fprintf(stderr, "[host] dlopen(%s) from tmp: %s\n", dst, h ? "OK" : dlerror());
    return h;
}

static void *open_lib(const char *name, int flags) {
    char p[1300];
    snprintf(p, sizeof p, "%s/%s", libdir, name);
    void *h = dlopen(p, flags);
    if (!h) {
        fprintf(stderr, "[host] dlopen(%s) failed: %s\n", p, dlerror());
        diag_file(p);
        { char q[1300]; snprintf(q, sizeof q, "%s/libOpenSLES.so", libdir); diag_file(q); }
        h = open_via_tmp(p, name, flags);
        if (!h) exit(1);
    }
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
volatile int host_cursor[3] = {320, 240, 0}; /* x, y, visible (read by libegl) */
volatile int host_hide_idx = -1;              /* draw bisect (SELECT + d-pad left/right) */
volatile int host_ov_mode = 1;                /* overlay: 0 off, 1 FPS, 2 FPS+CPU/GPU/RAM; cycled with L3+R3 */
char host_ov_lines[8][48];                    /* filled by stats_thread, drawn by libegl */
static int slurp(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t r = fread(buf, 1, n - 1, f); fclose(f); buf[r] = 0; return (int)r;
}
static int find_gpu_dir(char *out, size_t n) {
    static const char *c[] = { "/sys/class/devfreq/ff400000.gpu", "/sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu", NULL };
    for (int i = 0; c[i]; i++) { char t[300]; snprintf(t, sizeof t, "%s/load", c[i]); if (access(t, R_OK) == 0) { snprintf(out, n, "%s", c[i]); return 1; } }
    glob_t g; if (glob("/sys/class/devfreq/*gpu*", 0, NULL, &g) == 0 && g.gl_pathc) { snprintf(out, n, "%s", g.gl_pathv[0]); globfree(&g); return 1; }
    return 0;
}
static void *stats_thread(void *arg) {
    (void)arg; char gpu[300] = ""; int have_gpu = find_gpu_dir(gpu, sizeof gpu);
    unsigned long long pt = 0, pi = 0; unsigned long long pp = 0, pw = 0; char b[4096];
    for (;;) {
        usleep(500000);
        /* CPU load from /proc/stat, process usage from /proc/self/stat */
        int cpu = 0, proc = 0;
        if (slurp("/proc/stat", b, sizeof b)) { unsigned long long u, n, sy, id, io, ir, so, st; if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &ir, &so, &st) == 8) {
            unsigned long long tot = u + n + sy + id + io + ir + so + st, idle = id + io; if (pt && tot > pt) cpu = (int)(100 - 100 * (idle - pi) / (tot - pt)); 
            unsigned long long ps = 0; char sb[1024]; if (slurp("/proc/self/stat", sb, sizeof sb)) { char *q = strrchr(sb, ')'); unsigned long long ut = 0, stt = 0; if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &stt) == 2) ps = ut + stt; }
            if (pw && tot > pt) proc = (int)(100 * (ps - pp) / (tot - pt));
            pp = ps; pw = 1; pt = tot; pi = idle; } }
        char t[64]; int mhz = 0, temp = 0;
        if (slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", t, sizeof t)) mhz = atoi(t) / 1000;
        if (slurp("/sys/class/thermal/thermal_zone0/temp", t, sizeof t)) temp = atoi(t) / 1000;
        snprintf(host_ov_lines[0], 48, "CPU %d%% %dMHZ %dC", cpu, mhz, temp);
        if (have_gpu) { int gl = 0, gm = 0; char p[340]; snprintf(p, sizeof p, "%s/load", gpu); if (slurp(p, t, sizeof t)) gl = atoi(t); snprintf(p, sizeof p, "%s/cur_freq", gpu); if (slurp(p, t, sizeof t)) gm = (int)(atoll(t) / 1000000); snprintf(host_ov_lines[2], 48, "GPU %d%% %dMHZ", gl, gm); }
        else snprintf(host_ov_lines[2], 48, "GPU N/A");
        long tot = 0, av = 0; if (slurp("/proc/meminfo", b, sizeof b)) { char *q; if ((q = strstr(b, "MemTotal:"))) tot = atol(q + 9); if ((q = strstr(b, "MemAvailable:"))) av = atol(q + 13); }
        snprintf(host_ov_lines[3], 48, "RAM %ld/%ld MB FREE %ld", (tot - av) / 1024, tot / 1024, av / 1024);
        long swp = 0, cch = 0; { char *q; if ((q = strstr(b, "SwapTotal:"))) swp = atol(q + 10); if ((q = strstr(b, "SwapFree:"))) swp -= atol(q + 9); if ((q = strstr(b, "\nCached:"))) cch = atol(q + 8); }
        snprintf(host_ov_lines[6], 48, "CACHE %ld MB SWAP %ld MB", cch / 1024, swp / 1024);
        long rss = 0; if (slurp("/proc/self/statm", t, sizeof t)) { long a, r2; if (sscanf(t, "%ld %ld", &a, &r2) == 2) rss = r2 * 4096 / 1048576; }
        long vm = 0; int nthr = 0; if (slurp("/proc/self/statm", t, sizeof t)) { long a; if (sscanf(t, "%ld", &a) == 1) vm = a * 4096 / 1048576; }
        { char sb[1024]; if (slurp("/proc/self/status", sb, sizeof sb)) { char *q = strstr(sb, "Threads:"); if (q) nthr = atoi(q + 8); } }
        snprintf(host_ov_lines[4], 48, "GAME %ld MB VM %ld CPU %d%%", rss, vm, proc);
        { char lb[64] = ""; double l1 = 0; if (slurp("/proc/loadavg", lb, sizeof lb)) l1 = atof(lb); snprintf(host_ov_lines[5], 48, "THREADS %d LOAD %.2f", nthr, l1); }
        { static unsigned long long pt4[4], pi4[4]; char cb[2048]; int pc[4] = {0, 0, 0, 0};
          if (slurp("/proc/stat", cb, sizeof cb)) { for (int k = 0; k < 4; k++) { char key[16]; snprintf(key, sizeof key, "\ncpu%d ", k); char *q = strstr(cb, key); unsigned long long u, n, sy, id, io, ir, so, st;
              if (q && sscanf(q + 6, "%llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &ir, &so, &st) == 8) { unsigned long long tt = u + n + sy + id + io + ir + so + st, ii = id + io; if (pt4[k] && tt > pt4[k]) pc[k] = (int)(100 - 100 * (ii - pi4[k]) / (tt - pt4[k])); pt4[k] = tt; pi4[k] = ii; } } }
          snprintf(host_ov_lines[1], 48, "CORES %d %d %d %d", pc[0], pc[1], pc[2], pc[3]); }
    }
    return NULL;
}
static int touch_down;
static void send_axes(void);
static void dpad_axes(const int *d) { /* d-pad = menu navigation: hat axes + left stick axes */
    float x = (float)(d[3] - d[2]), y = (float)(d[1] - d[0]);
    axes[AXIS_HAT_X] = x; axes[AXIS_HAT_Y] = y; axes[AXIS_X] = x; axes[AXIS_Y] = y; send_axes();
}
static void send_touch(int action) {
    AInputEvent e; memset(&e, 0, sizeof e);
    e.type = EV_MOTION; e.source = 0x00001002; /* AINPUT_SOURCE_TOUCHSCREEN */ e.device = 2;
    e.action = action; e.pointer_count = 1; e.pointer_id[0] = 0;
    e.x[0] = (float)host_cursor[0]; e.y[0] = (float)host_cursor[1];
    queue_push(queue, &e);
}

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

/* ---- hang diagnostics: after a few seconds every thread prints where it is */
#include <sys/syscall.h>
#include <dirent.h>
#include <pthread.h>
static volatile int dump_lock;
static void usr2(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    while (__sync_lock_test_and_set(&dump_lock, 1)) {}
    ucontext_t *u = uc; int tid = (int)syscall(SYS_gettid);
    char path[64], comm[32] = "?";
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(path, "r"); if (f) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
    comm[strcspn(comm, "\n")] = 0;
    uintptr_t pc = u->uc_mcontext.arm_pc, lr = u->uc_mcontext.arm_lr, off;
    const char *w = elf_where(pc, &off); Dl_info di;
    fprintf(stderr, "[watchdog] thread %d (%s): pc=0x%lx", tid, comm, (unsigned long)pc);
    if (w) fprintf(stderr, " %s+0x%lx", w, (unsigned long)off);
    else if (dladdr((void *)pc, &di) && di.dli_fname) fprintf(stderr, " %s:%s+0x%lx", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
             di.dli_sname ? di.dli_sname : "?", (unsigned long)(pc - (uintptr_t)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)));
    w = elf_where(lr, &off);
    fprintf(stderr, "  lr=0x%lx", (unsigned long)lr);
    if (w) fprintf(stderr, " %s+0x%lx", w, (unsigned long)off);
    fprintf(stderr, "\n");
    scan_stack((uintptr_t *)u->uc_mcontext.arm_sp, 700);
    fflush(stderr);
    __sync_lock_release(&dump_lock);
}
static void *watchdog(void *arg) {
    (void)arg;
    static const int when[] = { 8, 12, 25 };
    int slept = 0;
    for (unsigned k = 0; k < sizeof when / sizeof *when; k++) {
        sleep(when[k] - slept); slept = when[k];
        fprintf(stderr, "[watchdog] %ds after start - thread dump\n", when[k]);
        DIR *d = opendir("/proc/self/task");
        if (!d) continue;
        struct dirent *e; int me = (int)syscall(SYS_gettid);
        while ((e = readdir(d))) {
            int tid = atoi(e->d_name);
            if (tid <= 0 || tid == me) continue;
            { char cp[64], cm[32] = "?"; snprintf(cp, sizeof cp, "/proc/self/task/%d/comm", tid); FILE *cf = fopen(cp, "r"); if (cf) { if (!fgets(cm, sizeof cm, cf)) cm[0] = 0; fclose(cf); } cm[strcspn(cm, "\n")] = 0;
              fprintf(stderr, "[watchdog] -> tid %d (%s)\n", tid, cm); fflush(stderr); }
            syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
            usleep(400000);
        }
        closedir(d);
    }
    return NULL;
}

/* Anything that is not the host (SDL, pulse, the Android libs) must not replace the crash handler */
static int host_allow;
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    static int (*real)(int, const struct sigaction *, struct sigaction *);
    if (!real) real = dlsym(RTLD_NEXT, "sigaction");
    if (!host_allow && act && (sig == SIGSEGV || sig == SIGBUS)) {
        uintptr_t off; void *ra = __builtin_return_address(0); const char *w = elf_where((uintptr_t)ra, &off); Dl_info di;
        fprintf(stderr, "[host] sigaction(%d) ignored, caller %p %s+0x%lx\n", sig, ra, w ? w : (dladdr(ra, &di) && di.dli_fname ? di.dli_fname : "?"), w ? (unsigned long)off : 0UL);
        act = NULL;
    }
    return real(sig, act, old);
}
void (*signal(int sig, void (*h)(int)))(int) {
    if (sig == SIGSEGV || sig == SIGBUS) { fprintf(stderr, "[host] signal(%d) ignored\n", sig); return SIG_DFL; }
    static void (*(*real)(int, void (*)(int)))(int);
    if (!real) real = dlsym(RTLD_NEXT, "signal");
    return real(sig, h);
}

/* ---- tiny sampling profiler: SIGPROF to every thread, aggregated in the log ([prof] lines) */
#define PROF_N 12000
static struct { int tid; uintptr_t pc, caller; } prof_buf[PROF_N];
static volatile unsigned prof_i;
static void sigprof(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    ucontext_t *u = uc; unsigned i = __sync_fetch_and_add(&prof_i, 1);
    if (i >= PROF_N) return;
    uintptr_t pc = u->uc_mcontext.arm_pc, caller = 0, off; uintptr_t *sp = (uintptr_t *)u->uc_mcontext.arm_sp;
    if (!elf_where(pc, &off)) {
        for (int k = 0; k < 160; k++) {
            if ((((uintptr_t)(sp + k)) & 4095) == 0 || k == 0) { unsigned char vec; if (mincore((void *)((uintptr_t)(sp + k) & ~4095u), 1, &vec)) break; }
            if (elf_where(sp[k], &off)) { caller = sp[k]; break; }
        }
    }
    prof_buf[i].tid = (int)syscall(SYS_gettid); prof_buf[i].pc = pc; prof_buf[i].caller = caller;
}
static void prof_site(uintptr_t pc, char *out, size_t n) {
    uintptr_t off; const char *w = elf_where(pc, &off); Dl_info di;
    if (!pc) snprintf(out, n, "-");
    else if (w) snprintf(out, n, "%s+0x%lx", w, (unsigned long)off);
    else if (dladdr((void *)pc, &di) && di.dli_fname) snprintf(out, n, "%s:%s", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?");
    else snprintf(out, n, "0x%lx", (unsigned long)pc);
}
static char prof_keys[PROF_N][150];
static int prof_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void prof_report(int round) {
    unsigned n = prof_i < PROF_N ? prof_i : PROF_N;
    char comms[64][2][24]; int ncomm = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *cm = "?"; int tid = prof_buf[i].tid;
        for (int k = 0; k < ncomm; k++) if (atoi(comms[k][0]) == tid) { cm = comms[k][1]; break; }
        if (cm[0] == '?' && ncomm < 64) {
            char p[64], b[24] = "?"; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid); FILE *f = fopen(p, "r"); if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); } b[strcspn(b, "\n")] = 0;
            snprintf(comms[ncomm][0], 24, "%d", tid); snprintf(comms[ncomm][1], 24, "%s", b); cm = comms[ncomm][1]; ncomm++;
        }
        char s1[60], s2[60]; prof_site(prof_buf[i].pc, s1, sizeof s1); prof_site(prof_buf[i].caller, s2, sizeof s2);
        snprintf(prof_keys[i], sizeof prof_keys[i], "%d|%s|%s|%s", tid, cm, s1, s2);
    }
    qsort(prof_keys, n, sizeof prof_keys[0], prof_cmp);
    static struct { char k[150]; int c; } top[40]; int nt = 0;
    memset(top, 0, sizeof top);
    for (unsigned i = 0; i < n;) {
        unsigned j = i; while (j < n && !strcmp(prof_keys[i], prof_keys[j])) j++;
        int c = (int)(j - i);
        int pos = nt < 40 ? nt++ : 39;
        if (pos == 39 && nt == 40 && top[39].c >= c && top[39].k[0]) { i = j; continue; }
        snprintf(top[pos].k, sizeof top[pos].k, "%s", prof_keys[i]); top[pos].c = c;
        for (int a = pos; a > 0 && top[a].c > top[a - 1].c; a--) { char t[150]; int tc = top[a].c; memcpy(t, top[a].k, 150); memcpy(top[a].k, top[a - 1].k, 150); top[a].c = top[a - 1].c; memcpy(top[a - 1].k, t, 150); top[a - 1].c = tc; }
        i = j;
    }
    fprintf(stderr, "[prof] ===== window %d: %u samples =====  (RUNNING threads only: count | tid | thread | pc | game-caller)\n", round, n);
    for (int i = 0; i < nt; i++) fprintf(stderr, "[prof] %4d | %s\n", top[i].c, top[i].k);
    fflush(stderr);
}
static void *prof_thread(void *arg) {
    (void)arg; sleep(25);
    for (int round = 0; round < 5; round++) {
        prof_i = 0;
        for (int k = 0; k < 500; k++) {
            DIR *d = opendir("/proc/self/task"); if (!d) break;
            struct dirent *e; int me = (int)syscall(SYS_gettid);
            while ((e = readdir(d))) { int tid = atoi(e->d_name); if (tid <= 0 || tid == me) continue;
                char sp[64], sb[256]; snprintf(sp, sizeof sp, "/proc/self/task/%d/stat", tid); int fd = open(sp, O_RDONLY); if (fd < 0) continue;
                ssize_t m = read(fd, sb, sizeof sb - 1); close(fd); if (m <= 0) continue; sb[m] = 0;
                char *q = strrchr(sb, ')'); if (q && q[2] == 'R') syscall(SYS_tgkill, getpid(), tid, SIGPROF); }
            closedir(d); usleep(10000);
        }
        prof_report(round);
    }
    return NULL;
}

int main(int argc, char **argv) {
    char exe[1024]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) { perror("readlink"); return 1; }
    exe[n] = 0;
    snprintf(base, sizeof base, "%s", dirname(exe));
    snprintf(libdir, sizeof libdir, "%s/libs", base);
    snprintf(datadir, sizeof datadir, "%s/data", base);
    snprintf(assetdir, sizeof assetdir, "%s/assets", base);
    mkdir(datadir, 0755);
    setenv("MCPE_DATADIR", datadir, 1);
    { static const char *sub[] = {"/games", "/games/com.mojang", "/games/com.mojang/minecraftWorlds", "/games/com.mojang/resource_packs", "/games/com.mojang/behavior_packs", "/files", "/cache", "/minecraftpe"};
      for (unsigned i = 0; i < sizeof sub / sizeof *sub; i++) { char q[1300]; snprintf(q, sizeof q, "%s%s", datadir, sub[i]); mkdir(q, 0755); } }
    g_assets_root = assetdir;
    setvbuf(stderr, NULL, _IONBF, 0);
    host_allow = 1;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash; sa.sa_flags = SA_SIGINFO;
    { struct sigaction s2; memset(&s2, 0, sizeof s2); s2.sa_sigaction = usr2; s2.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGUSR2, &s2, NULL); }
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
    { struct sigaction s3; memset(&s3, 0, sizeof s3); s3.sa_sigaction = sigprof; s3.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &s3, NULL); }
    host_allow = 0;
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
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_Window *win = SDL_CreateWindow("Minecraft PE", 0, 0, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "[host] window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "[host] GLES2 context: %s\n", SDL_GetError()); return 1; }
    fprintf(stderr, "[host] GL_RENDERER: %s\n", "(see glGetString in game log)");
    SDL_GL_MakeCurrent(win, NULL); /* the game's render thread will bind it */

    /* Shim libraries first, in the order the game lists them */
    void *hc = open_lib("libc.so", RTLD_NOW | RTLD_LOCAL);
    void *hm = open_lib("libm.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libdl.so", RTLD_NOW | RTLD_LOCAL);
    void *hlog = open_lib("liblog.so", RTLD_NOW | RTLD_LOCAL);
    void *hand = open_lib("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    void *hegl = open_lib("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libGLESv1_CM.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    void *hsl = open_lib("libOpenSLES.so", RTLD_NOW | RTLD_LOCAL);
    open_lib("libz.so", RTLD_NOW | RTLD_LOCAL);
    void *hcxx = open_lib("libstdc++.so", RTLD_NOW | RTLD_LOCAL);
    /* symbol lookup order for the Android libs: libm first (float ABI wrappers), then bionic compat, ... */
    {
        struct { void *h; const char *n; } r[] = { { hm, "libm.so" }, { hc, "libc.so" }, { hlog, "liblog.so" },
            { hand, "libandroid.so" }, { hegl, "libEGL.so" }, { hsl, "libOpenSLES.so" }, { hcxx, "libstdc++.so" } };
        for (unsigned i = 0; i < sizeof r / sizeof *r; i++) {
            char p[1300]; snprintf(p, sizeof p, "%s/%s", libdir, r[i].n);
            elf_add_resolver(r[i].h, p);
        }
    }
    char fmodpath[1300];
    snprintf(fmodpath, sizeof fmodpath, "%s/libfmod.so", libdir);
    ElfLib *fmod = elf_load(fmodpath, "libfmod.so");
    if (!fmod) { fprintf(stderr, "[host] cannot load libfmod.so\n"); return 1; }

    *(void **)need(hegl, "shim_egl_window") = win;
    *(void **)need(hegl, "shim_egl_ctx") = ctx;
    AAssetManager *(*mk_assets)(const char *) = need(hand, "shim_asset_manager_create");
    AInputQueue *(*mk_queue)(void) = need(hand, "shim_queue_create");
    queue_push = need(hand, "shim_queue_push");
    volatile int *finish = need(hand, "shim_finish_requested");

    char gamepath[1300];
    snprintf(gamepath, sizeof gamepath, "%s/libminecraftpe.so", libdir);
    ElfLib *game = elf_load(gamepath, "libminecraftpe.so");
    if (!game) { fprintf(stderr, "[host] cannot load game\n"); return 1; }
    fprintf(stderr, "[host] libminecraftpe.so loaded\n");

    jint (*JNI_OnLoad_)(JavaVM *, void *) = elf_sym(game, "JNI_OnLoad");
    void (*ANativeActivity_onCreate_)(ANativeActivity *, void *, size_t) = elf_sym(game, "ANativeActivity_onCreate");
    if (!ANativeActivity_onCreate_) { fprintf(stderr, "[host] missing symbol ANativeActivity_onCreate\n"); return 1; }
    /* Android's linker runs constructors on load; JNI_OnLoad on System.loadLibrary */
    elf_run_init(fmod, argc, argv, environ);
    elf_run_init(game, argc, argv, environ);
    jint (*fmod_onload)(JavaVM *, void *) = elf_sym(fmod, "JNI_OnLoad");
    fprintf(stderr, "[host] calling JNI_OnLoad (fmod=%p game=%p)\n", fmod_onload, JNI_OnLoad_);
    if (fmod_onload) { fprintf(stderr, "[host] fmod JNI_OnLoad...\n"); fmod_onload(fake_vm(), NULL); fprintf(stderr, "[host] fmod JNI_OnLoad returned\n"); }
    if (JNI_OnLoad_) { fprintf(stderr, "[host] game JNI_OnLoad...\n"); JNI_OnLoad_(fake_vm(), NULL); }
    fprintf(stderr, "[host] JNI_OnLoad done\n");

    { pthread_t wt; pthread_create(&wt, NULL, watchdog, NULL); pthread_detach(wt); }
    if (!getenv("MCPE_NOPROF")) { pthread_t pt; pthread_create(&pt, NULL, prof_thread, NULL); pthread_detach(pt); }
    /* MainActivity.onCreate calls nativeRegisterThis(): it stores the activity and releases the mutex that
     * JNI_OnLoad leaves locked; the game thread (android_main) blocks on that mutex until then. */
    {
        void (*reg)(JNIEnv *, jobject) = elf_sym(game, "Java_com_mojang_minecraftpe_MainActivity_nativeRegisterThis");
        fprintf(stderr, "[host] nativeRegisterThis=%p\n", (void *)reg);
        if (reg) reg(fake_env(), fake_activity_obj());
    }
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
    { pthread_t th; if (pthread_create(&th, NULL, stats_thread, NULL) == 0) pthread_detach(th); }
    int running = 1; int dpad[4] = {0}; float stick[2] = {0}; Uint32 last_tick = SDL_GetTicks();
    while (running && !*finish) {
        SDL_Event ev;
        int got = SDL_WaitEventTimeout(&ev, 16);
        if (!got) continue;
        do {
            switch (ev.type) {
                case SDL_QUIT: running = 0; break;
                case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: { static int n; if (n++ < 100) fprintf(stderr, "[joy] button %d %s\n", ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN ? "down" : "up"); break; }
                case SDL_JOYHATMOTION: { static int n; if (n++ < 100) fprintf(stderr, "[joy] hat %d value %d\n", ev.jhat.hat, ev.jhat.value); break; }
                case SDL_CONTROLLERDEVICEADDED:
                    if (!pad) { pad = SDL_GameControllerOpen(ev.cdevice.which); fprintf(stderr, "[pad] opened %s mapping=%s\n", pad ? SDL_GameControllerName(pad) : "?", pad ? SDL_GameControllerMapping(pad) : "?"); }
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    { static int n; if (n++ < 150) fprintf(stderr, "[pad] button %d %s\n", ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up"); }
                    int dn = ev.type == SDL_CONTROLLERBUTTONDOWN, b = ev.cbutton.button;
                    static int l3, r3;
                    if (b == SDL_CONTROLLER_BUTTON_LEFTSTICK) l3 = dn;
                    if (b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) r3 = dn;
                    if ((b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) && dn && l3 && r3) {
                        host_ov_mode = (host_ov_mode + 1) % 3;           /* L3+R3: off -> FPS -> FPS/CPU/GPU/RAM */
                        fprintf(stderr, "[pad] overlay mode %d\n", host_ov_mode); break;
                    }
                    { static int sel; if (b == SDL_CONTROLLER_BUTTON_BACK) sel = dn;
                      if (sel && dn && (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                          host_hide_idx += b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 1 : -1; if (host_hide_idx < -1) host_hide_idx = -1;
                          fprintf(stderr, "[pad] hide index %d\n", host_hide_idx); break; } }
                    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN || b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                        /* d-pad is read by the game as HAT axes (not as key events) */
                        static int dp[4];
                        dp[b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 0 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 1 : b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? 2 : 3] = dn;
                        axes[AXIS_HAT_X] = (float)(dp[3] - dp[2]); axes[AXIS_HAT_Y] = (float)(dp[1] - dp[0]); send_axes();
                        break;
                    }
                    int kc = map_button(b);
                    if (kc) send_key(dn ? 0 : 1, kc);
                    break;
                }
                case SDL_CONTROLLERAXISMOTION: {
                    int a = map_axis(ev.caxis.axis);
                    { static int n; if (n++ < 150 && (ev.caxis.value > 12000 || ev.caxis.value < -12000)) fprintf(stderr, "[pad] axis %d = %d\n", ev.caxis.axis, ev.caxis.value); }
                    if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                        /* triggers: report as axes AND as L2/R2 key presses (the game accepts either) */
                        int right = ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, on = ev.caxis.value > 12000;
                        static int state[2];
                        if (state[right] != on) { state[right] = on; send_key(on ? 0 : 1, right ? 105 : 104); }
                        axes[right ? 18 : 17] = on ? 1.0f : 0.0f; axes[right ? 22 : 23] = axes[right ? 18 : 17]; send_axes();
                        break;
                    }
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
