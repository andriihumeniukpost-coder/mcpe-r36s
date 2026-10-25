/* "libc.so" shim: translates the Android Bionic ABI used by libminecraftpe.so
 * to glibc. Loaded with soname libc.so; the game is dlopen()ed with
 * RTLD_DEEPBIND so these definitions win over glibc's for the game only. */
#define _GNU_SOURCE
#include "android_min.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static void *glibc_h[5];
static void *R(const char *n) {
    /* glibc < 2.34 keeps pthread_* in libpthread.so.0 */
    static const char *names[5] = { "libc.so.6", "libpthread.so.0", "libdl.so.2", "libm.so.6", "librt.so.1" };
    void *p = NULL;
    for (int i = 0; i < 5 && !p; i++) {
        if (!glibc_h[i]) glibc_h[i] = dlopen(names[i], RTLD_NOW);
        if (glibc_h[i]) p = dlsym(glibc_h[i], n);
    }
    if (!p) { fprintf(stderr, "[compat] missing glibc symbol %s\n", n); _exit(134); }
    return p;
}

static void report(const char *what, void *ra) {
    void (*f)(const char *, void *) = dlsym(RTLD_DEFAULT, "host_report");
    if (f) f(what, ra);
}
EXPORT int bc_raise(int sig) {
    char b[32]; snprintf(b, sizeof b, "raise(%d)", sig); report(b, __builtin_return_address(0));
    return ((int (*)(int))R("raise"))(sig);
}
EXPORT int bc_kill(int pid, int sig) {
    char b[48]; snprintf(b, sizeof b, "kill(%d,%d)", pid, sig); report(b, __builtin_return_address(0));
    return ((int (*)(int, int))R("kill"))(pid, sig);
}
EXPORT int bc_pthread_kill(unsigned long t, int sig) {
    char b[48]; snprintf(b, sizeof b, "pthread_kill(%d)", sig); report(b, __builtin_return_address(0));
    return ((int (*)(unsigned long, int))R("pthread_kill"))(t, sig);
}
EXPORT void bc_abort(void) {
    report("abort()", __builtin_return_address(0));
    ((void (*)(void))R("abort"))();
    for (;;) {}
}

/* ---------------------------------------------------- basic data */
EXPORT int *__errno(void) { return __errno_location(); }
EXPORT uintptr_t __stack_chk_guard = 0x5ca1ab1e;
EXPORT void __stack_chk_fail(void) { fprintf(stderr, "[compat] stack smashing detected\n"); _exit(134); }
EXPORT void __google_potentially_blocking_region_begin(void) {}
EXPORT void __google_potentially_blocking_region_end(void) {}
EXPORT void (*bsd_signal(int sig, void (*h)(int)))(int) { return signal(sig, h); }

/* Bionic FILE is 84 bytes; stdin/stdout/stderr are &__sF[0..2]. */
#define BFILE_SZ 84
EXPORT char __sF[3 * BFILE_SZ];
static FILE *mapf(FILE *f) {
    char *p = (char *)f;
    if (p >= __sF && p < __sF + sizeof __sF) {
        switch ((p - __sF) / BFILE_SZ) { case 0: return stdin; case 1: return stdout; default: return stderr; }
    }
    return f;
}
EXPORT int bc_vfprintf(FILE *f, const char *fmt, va_list ap) {
    int (*r)(FILE *, const char *, va_list) = R("vfprintf");
    return r(mapf(f), fmt, ap);
}
EXPORT int bc_fprintf(FILE *f, const char *fmt, ...) {
    int (*r)(FILE *, const char *, va_list) = R("vfprintf");
    va_list ap; va_start(ap, fmt);
    int n = r(mapf(f), fmt, ap);
    va_end(ap);
    return n;
}
EXPORT int bc_fputs(const char *s, FILE *f) { int (*r)(const char *, FILE *) = R("fputs"); return r(s, mapf(f)); }
EXPORT int bc_fputc(int c, FILE *f) { int (*r)(int, FILE *) = R("fputc"); return r(c, mapf(f)); }
EXPORT int bc_putc(int c, FILE *f) { int (*r)(int, FILE *) = R("fputc"); return r(c, mapf(f)); }
EXPORT int bc_fgetc(FILE *f) { int (*r)(FILE *) = R("fgetc"); return r(mapf(f)); }
EXPORT int bc_getc(FILE *f) { int (*r)(FILE *) = R("fgetc"); return r(mapf(f)); }
EXPORT int bc_ungetc(int c, FILE *f) { int (*r)(int, FILE *) = R("ungetc"); return r(c, mapf(f)); }
EXPORT char *bc_fgets(char *s, int n, FILE *f) { char *(*r)(char *, int, FILE *) = R("fgets"); return r(s, n, mapf(f)); }
EXPORT size_t bc_fwrite(const void *p, size_t s, size_t n, FILE *f) { size_t (*r)(const void *, size_t, size_t, FILE *) = R("fwrite"); return r(p, s, n, mapf(f)); }
EXPORT size_t bc_fread(void *p, size_t s, size_t n, FILE *f) { size_t (*r)(void *, size_t, size_t, FILE *) = R("fread"); return r(p, s, n, mapf(f)); }
EXPORT int bc_fflush(FILE *f) { int (*r)(FILE *) = R("fflush"); return f ? r(mapf(f)) : r(NULL); }
EXPORT int bc_setvbuf(FILE *f, char *b, int m, size_t s) { int (*r)(FILE *, char *, int, size_t) = R("setvbuf"); return r(mapf(f), b, m, s); }
EXPORT int bc_feof(FILE *f) { int (*r)(FILE *) = R("feof"); return r(mapf(f)); }
EXPORT int bc_fscanf(FILE *f, const char *fmt, ...) {
    int (*r)(FILE *, const char *, va_list) = R("vfscanf");
    va_list ap; va_start(ap, fmt);
    int n = r(mapf(f), fmt, ap);
    va_end(ap);
    return n;
}

/* ctype tables used by old NDK macros */
#define B_U 0x01
#define B_L 0x02
#define B_N 0x04
#define B_S 0x08
#define B_P 0x10
#define B_C 0x20
#define B_X 0x40
#define B_B 0x80
EXPORT char _ctype_[257];
EXPORT short _tolower_tab_[257];
EXPORT short _toupper_tab_[257];
__attribute__((constructor)) static void init_ctype(void) {
    for (int c = 0; c < 256; c++) {
        int f = 0;
        if (c < 128) {
            if (isupper(c)) f |= B_U;
            if (islower(c)) f |= B_L;
            if (isdigit(c)) f |= B_N;
            if (isspace(c)) f |= B_S;
            if (ispunct(c)) f |= B_P;
            if (iscntrl(c)) f |= B_C;
            if (isxdigit(c)) f |= B_X;
            if (c == ' ') f |= B_B;
        }
        _ctype_[c + 1] = (char)f;
        _tolower_tab_[c + 1] = (short)(c < 128 ? tolower(c) : c);
        _toupper_tab_[c + 1] = (short)(c < 128 ? toupper(c) : c);
    }
    _tolower_tab_[0] = _toupper_tab_[0] = -1;
}

/* ------------------------------------------------------- sysconf */
EXPORT long bc_sysconf(int name) {
    long (*r)(int) = R("sysconf");
    switch (name) {
        case 0x27: case 0x28: return r(_SC_PAGESIZE);
        case 0x60: return r(_SC_NPROCESSORS_CONF);
        case 0x61: return r(_SC_NPROCESSORS_ONLN);
        case 0x62: return r(_SC_PHYS_PAGES);
        case 0x63: return r(_SC_AVPHYS_PAGES);
        case 0x02: return r(_SC_CLK_TCK);
        case 0x04: return r(_SC_OPEN_MAX);
        default: return -1;
    }
}

/* ------------------------------------------------ stat / dirent */
struct bionic_stat {
    unsigned long long st_dev; unsigned char pad0[4]; unsigned long ino32;
    unsigned int st_mode, st_nlink; unsigned long st_uid, st_gid;
    unsigned long long st_rdev; unsigned char pad3[4];
    long long st_size; unsigned long st_blksize; unsigned long long st_blocks;
    unsigned long atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    unsigned long long st_ino;
};
#ifdef __arm__
_Static_assert(sizeof(struct bionic_stat) == 104, "bionic stat layout");
#endif
static void conv_stat(struct bionic_stat *o, const struct stat64 *s) {
    memset(o, 0, sizeof *o);
    o->st_dev = s->st_dev; o->ino32 = (unsigned long)s->st_ino; o->st_ino = s->st_ino;
    o->st_mode = s->st_mode; o->st_nlink = s->st_nlink;
    o->st_uid = s->st_uid; o->st_gid = s->st_gid; o->st_rdev = s->st_rdev;
    o->st_size = s->st_size; o->st_blksize = s->st_blksize; o->st_blocks = s->st_blocks;
    o->atime = s->st_atim.tv_sec; o->atime_ns = s->st_atim.tv_nsec;
    o->mtime = s->st_mtim.tv_sec; o->mtime_ns = s->st_mtim.tv_nsec;
    o->ctime = s->st_ctim.tv_sec; o->ctime_ns = s->st_ctim.tv_nsec;
}
EXPORT int bc_stat(const char *p, struct bionic_stat *o) {
    int (*r)(const char *, struct stat64 *) = R("stat64");
    struct stat64 s; int rc = r(p, &s); if (rc == 0) conv_stat(o, &s); return rc;
}
EXPORT int bc_lstat(const char *p, struct bionic_stat *o) {
    int (*r)(const char *, struct stat64 *) = R("lstat64");
    struct stat64 s; int rc = r(p, &s); if (rc == 0) conv_stat(o, &s); return rc;
}
EXPORT int bc_fstat(int fd, struct bionic_stat *o) {
    int (*r)(int, struct stat64 *) = R("fstat64");
    struct stat64 s; int rc = r(fd, &s); if (rc == 0) conv_stat(o, &s); return rc;
}

struct bionic_dirent {
    unsigned long long d_ino; long long d_off;
    unsigned short d_reclen; unsigned char d_type; char d_name[256];
};
EXPORT struct bionic_dirent *bc_readdir(DIR *d) {
    static __thread struct bionic_dirent out;
    struct dirent64 *(*r)(DIR *) = R("readdir64");
    struct dirent64 *e = r(d);
    if (!e) return NULL;
    out.d_ino = e->d_ino; out.d_off = e->d_off; out.d_type = e->d_type;
    out.d_reclen = sizeof out;
    strncpy(out.d_name, e->d_name, 255); out.d_name[255] = 0;
    return &out;
}

/* ---------------------------------------------------- signals */
struct bionic_sigaction {
    void (*handler)(int, siginfo_t *, void *);
    unsigned long mask; int flags; void (*restorer)(void);
};
EXPORT int bc_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old) {
    int (*r)(int, const struct sigaction *, struct sigaction *) = R("sigaction");
    struct sigaction a, o, *pa = NULL;
    if (act) {
        memset(&a, 0, sizeof a);
        a.sa_sigaction = act->handler;
        for (int i = 1; i < 32; i++) if (act->mask & (1UL << (i - 1))) sigaddset(&a.sa_mask, i);
        a.sa_flags = act->flags;
        pa = &a;
    }
    int rc = r(sig, pa, old ? &o : NULL);
    if (rc == 0 && old) {
        memset(old, 0, sizeof *old);
        old->handler = (void *)o.sa_sigaction; old->flags = o.sa_flags;
    }
    return rc;
}
EXPORT int bc_sigprocmask(int how, const unsigned long *set, unsigned long *old) {
    int (*r)(int, const sigset_t *, sigset_t *) = R("sigprocmask");
    sigset_t s, o, *ps = NULL;
    if (set) { sigemptyset(&s); for (int i = 1; i < 32; i++) if (*set & (1UL << (i - 1))) sigaddset(&s, i); ps = &s; }
    int rc = r(how, ps, old ? &o : NULL);
    if (rc == 0 && old) { *old = 0; for (int i = 1; i < 32; i++) if (sigismember(&o, i) == 1) *old |= 1UL << (i - 1); }
    return rc;
}
/* NOTE: sigsetjmp/siglongjmp are NOT wrapped: the Bionic jmp_buf (260 bytes) is
 * smaller than glibc's. If the game calls them the log below will tell us. */

/* --------------------------------------------- float ABI helpers */
EXPORT SOFT double bc_strtod(const char *s, char **e) { double (*r)(const char *, char **) = R("strtod"); return r(s, e); }
EXPORT SOFT double bc_difftime(time_t a, time_t b) { double (*r)(time_t, time_t) = R("difftime"); return r(a, b); }

/* ----------------------------------------------------- pthreads */
/* Bionic: mutex/cond are 4-byte words (0 = static init). We keep a pointer to
 * a lazily-allocated glibc object inside that word. Values <= 0xffff are
 * "uninitialised" (0 normal, 0x4000 recursive, 0x8000 errorcheck). */
static int (*r_mutex_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
static int (*r_cond_init)(pthread_cond_t *, const pthread_condattr_t *);
static pthread_mutex_t *hmutex(void *g, int forced_type) {
    volatile uintptr_t *w = g;
    uintptr_t v = *w;
    if (v > 0xffff) return (pthread_mutex_t *)v;
    if (!r_mutex_init) r_mutex_init = R("pthread_mutex_init");
    pthread_mutex_t *m = malloc(sizeof *m);
    int (*a_init)(pthread_mutexattr_t *) = R("pthread_mutexattr_init");
    int (*a_type)(pthread_mutexattr_t *, int) = R("pthread_mutexattr_settype");
    int (*a_dest)(pthread_mutexattr_t *) = R("pthread_mutexattr_destroy");
    int (*m_dest)(pthread_mutex_t *) = R("pthread_mutex_destroy");
    pthread_mutexattr_t a; a_init(&a);
    int t = forced_type >= 0 ? forced_type : v == 0x4000 ? PTHREAD_MUTEX_RECURSIVE : v == 0x8000 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
    a_type(&a, t);
    r_mutex_init(m, &a);
    a_dest(&a);
    if (__sync_bool_compare_and_swap(w, v, (uintptr_t)m)) return m;
    m_dest(m); free(m);
    return (pthread_mutex_t *)*w;
}
static pthread_cond_t *hcond(void *g) {
    volatile uintptr_t *w = g;
    uintptr_t v = *w;
    if (v > 0xffff) return (pthread_cond_t *)v;
    if (!r_cond_init) r_cond_init = R("pthread_cond_init");
    pthread_cond_t *c = malloc(sizeof *c);
    r_cond_init(c, NULL);
    if (__sync_bool_compare_and_swap(w, v, (uintptr_t)c)) return c;
    { int (*cd)(pthread_cond_t *) = R("pthread_cond_destroy"); cd(c); }
    free(c);
    return (pthread_cond_t *)*w;
}

EXPORT int bc_pthread_mutexattr_init(int *a) { *a = 0; return 0; }
EXPORT int bc_pthread_mutexattr_destroy(int *a) { (void)a; return 0; }
EXPORT int bc_pthread_mutexattr_settype(int *a, int t) { *a = t; return 0; }
EXPORT int bc_pthread_mutexattr_gettype(const int *a, int *t) { *t = *a; return 0; }
EXPORT int bc_pthread_mutex_init(void *m, const int *a) {
    *(volatile uintptr_t *)m = 0;
    hmutex(m, a ? *a : 0);
    return 0;
}
EXPORT int bc_pthread_mutex_destroy(void *m) {
    uintptr_t v = *(uintptr_t *)m;
    if (v > 0xffff) { int (*d)(pthread_mutex_t *) = R("pthread_mutex_destroy"); d((pthread_mutex_t *)v); free((void *)v); }
    *(uintptr_t *)m = 0xdead;
    return 0;
}
/* The glibc symbols are shadowed by these names, so reach them via R(). */
#define REAL_MX(fn) ({ static int (*p)(pthread_mutex_t *); if (!p) p = R(fn); p; })
EXPORT int bc_pthread_mutex_lock(void *m) { return REAL_MX("pthread_mutex_lock")(hmutex(m, -1)); }
EXPORT int bc_pthread_mutex_unlock(void *m) { return REAL_MX("pthread_mutex_unlock")(hmutex(m, -1)); }
EXPORT int bc_pthread_mutex_trylock(void *m) { return REAL_MX("pthread_mutex_trylock")(hmutex(m, -1)); }

EXPORT int bc_pthread_cond_init(void *c, const void *a) { (void)a; *(volatile uintptr_t *)c = 0; hcond(c); return 0; }
EXPORT int bc_pthread_cond_destroy(void *c) {
    uintptr_t v = *(uintptr_t *)c;
    if (v > 0xffff) { int (*d)(pthread_cond_t *) = R("pthread_cond_destroy"); d((pthread_cond_t *)v); free((void *)v); }
    *(uintptr_t *)c = 0xdead;
    return 0;
}
EXPORT int bc_pthread_cond_signal(void *c) { int (*r)(pthread_cond_t *) = R("pthread_cond_signal"); return r(hcond(c)); }
EXPORT int bc_pthread_cond_broadcast(void *c) { int (*r)(pthread_cond_t *) = R("pthread_cond_broadcast"); return r(hcond(c)); }
EXPORT int bc_pthread_cond_wait(void *c, void *m) { int (*r)(pthread_cond_t *, pthread_mutex_t *) = R("pthread_cond_wait"); return r(hcond(c), hmutex(m, -1)); }
EXPORT int bc_pthread_cond_timedwait(void *c, void *m, const struct timespec *t) {
    int (*r)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *) = R("pthread_cond_timedwait");
    return r(hcond(c), hmutex(m, -1), t);
}

/* Bionic pthread_attr_t is 24 bytes */
struct battr { uint32_t flags; void *stack_base; size_t stack_size, guard; int32_t policy, prio; };
EXPORT int bc_pthread_attr_init(struct battr *a) { memset(a, 0, sizeof *a); a->stack_size = 1024 * 1024; return 0; }
EXPORT int bc_pthread_attr_destroy(struct battr *a) { (void)a; return 0; }
EXPORT int bc_pthread_attr_setdetachstate(struct battr *a, int s) { a->flags = (a->flags & ~1u) | (s ? 1u : 0u); return 0; }
EXPORT int bc_pthread_attr_getdetachstate(const struct battr *a, int *s) { *s = a->flags & 1; return 0; }
EXPORT int bc_pthread_attr_setstacksize(struct battr *a, size_t s) { a->stack_size = s; return 0; }
EXPORT int bc_pthread_attr_getstacksize(const struct battr *a, size_t *s) { *s = a->stack_size; return 0; }
EXPORT int bc_pthread_attr_setschedparam(struct battr *a, const struct sched_param *p) { a->prio = p ? p->sched_priority : 0; return 0; }
EXPORT int bc_pthread_create(pthread_t *t, const struct battr *a, void *(*fn)(void *), void *arg) {
    int (*r)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = R("pthread_create");
    pthread_attr_t ha;
    int (*ainit)(pthread_attr_t *) = R("pthread_attr_init");
    ainit(&ha);
    if (a) {
        size_t ss = a->stack_size < 64 * 1024 ? 64 * 1024 : a->stack_size;
        int (*sst)(pthread_attr_t *, size_t) = R("pthread_attr_setstacksize");
        sst(&ha, ss);
        int (*sds)(pthread_attr_t *, int) = R("pthread_attr_setdetachstate");
        sds(&ha, (a->flags & 1) ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
    }
    return r(t, &ha, fn, arg);
}

/* Cleanup handlers (no cancellation support: just run on pop(execute)) */
struct bcleanup { struct bcleanup *prev; void (*fn)(void *); void *arg; };
EXPORT void __pthread_cleanup_push(struct bcleanup *c, void (*fn)(void *), void *arg) { c->prev = NULL; c->fn = fn; c->arg = arg; }
EXPORT void __pthread_cleanup_pop(struct bcleanup *c, int execute) { if (execute && c->fn) c->fn(c->arg); }
