/* Bionic (Android, LP64/AArch64) -> glibc compatibility for libminecraftpe.so and its sibling libraries.
 * Only the places where the two ABIs actually differ need code; everything else resolves straight to glibc
 * (see lookup_external() in elf64_loader.c). Entries in bionic_syms[] take priority over glibc.
 *
 * Status: written from the Bionic headers' layouts, verified only up to "libraries load and relocate under qemu".
 * Anything marked TODO is untested on real hardware. */
#define _GNU_SOURCE
#include "elf64_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/sysinfo.h>
#include <fcntl.h>

/* ---------------------------------------------------------------- errno, logging, asserts */
static int *b_errno(void) { return __errno_location(); }

static int b_log_write(int prio, const char *tag, const char *msg) { fprintf(stderr, "[android:%d] %s: %s\n", prio, tag ? tag : "?", msg); return 1; }
static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    return b_log_write(prio, tag, buf);
}
static int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    char buf[1024]; vsnprintf(buf, sizeof buf, fmt, ap); return b_log_write(prio, tag, buf);
}
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    char buf[1024] = ""; if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); }
    fprintf(stderr, "[android:ASSERT] %s: %s %s\n", tag ? tag : "?", cond ? cond : "", buf); abort();
}
static void b_assert2(const char *file, int line, const char *func, const char *expr) {
    fprintf(stderr, "[game] ASSERT FAILED %s:%d (%s): %s\n", file, line, func, expr); fflush(stderr); abort();
}

/* ---------------------------------------------------------------- _FORTIFY helpers */
static size_t b_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char *b_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static char *b_strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) { (void)dl; (void)sl; return strncpy(d, s, n); }
static char *b_strrchr_chk(const char *s, int c, size_t n) { (void)n; return strrchr(s, c); }
static void b_set_abort_message(const char *m) { fprintf(stderr, "[android] abort message: %s\n", m ? m : "(null)"); }
static void b_fd_set_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_SET(fd, s); }
static void b_fd_clr_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_CLR(fd, s); }
static int  b_fd_isset_chk(int fd, const fd_set *s, size_t sz) { (void)sz; return FD_ISSET(fd, s); }

/* ---------------------------------------------------------------- bionic-only functions */
static int b_system_property_get(const char *name, char *value) { (void)name; value[0] = 0; return 0; }
static int b_pthread_gettid_np(pthread_t t) { (void)t; return (int)syscall(SYS_gettid); }  /* TODO: map arbitrary pthread_t */

struct b_cleanup { struct b_cleanup *prev; void (*fn)(void *); void *arg; };
static void b_cleanup_push(struct b_cleanup *c, void (*fn)(void *), void *arg) { c->fn = fn; c->arg = arg; }
static void b_cleanup_pop(struct b_cleanup *c, int execute) { if (execute && c->fn) c->fn(c->arg); }

/* ctype table: Bionic's isalpha() etc. index _ctype_[c+1]. Flags: U=1 L=2 N=4 S=8 P=0x10 C=0x20 X=0x40 B=0x80 */
static char b_ctype[257];
static void init_ctype(void) {
    for (int c = 0; c < 256; c++) {
        char f = 0;
        if (c >= 'A' && c <= 'Z') f |= 0x01;
        if (c >= 'a' && c <= 'z') f |= 0x02;
        if (c >= '0' && c <= '9') f |= 0x04;
        if (c == ' ') f |= 0x80;
        if ((c >= 9 && c <= 13) || c == ' ') f |= 0x08;
        if (c > 32 && c < 127 && !((f & 0x07))) f |= 0x10;
        if (c < 32 || c == 127) f |= 0x20;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x40;
        b_ctype[c + 1] = f;
    }
}

/* ---------------------------------------------------------------- stdio: __sF and FILE* mapping */
#define B_FILE_SZ 152                      /* sizeof(FILE) in LP64 Bionic */
static char b_sF[3 * B_FILE_SZ];
static FILE *mapf(FILE *f) {
    char *p = (char *)f;
    if (p >= b_sF && p < b_sF + sizeof b_sF) {
        switch ((p - b_sF) / B_FILE_SZ) { case 0: return stdin; case 1: return stdout; default: return stderr; }
    }
    return f;
}
static int    b_fclose(FILE *f) { return fclose(mapf(f)); }
static int    b_fflush(FILE *f) { return fflush(f ? mapf(f) : NULL); }
static char  *b_fgets(char *s, int n, FILE *f) { return fgets(s, n, mapf(f)); }
static int    b_fputs(const char *s, FILE *f) { return fputs(s, mapf(f)); }
static size_t b_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, mapf(f)); }
static size_t b_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, mapf(f)); }
static int    b_fseek(FILE *f, long o, int w) { return fseek(mapf(f), o, w); }
static long   b_ftell(FILE *f) { return ftell(mapf(f)); }
static int    b_fileno(FILE *f) { return fileno(mapf(f)); }
static int    b_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(mapf(f), fmt, ap); }
static int    b_fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(mapf(f), fmt, ap); va_end(ap); return n; }

/* ---------------------------------------------------------------- stat: kernel struct stat is identical on AArch64,
 * and old glibc (2.30 on ArkOS) has no exported stat/fstat/lstat, so go straight to the kernel. */
static int b_fstat(int fd, void *st) { return (int)syscall(SYS_fstat, fd, st); }
static int b_stat(const char *p, void *st) { return (int)syscall(SYS_newfstatat, AT_FDCWD, p, st, 0); }
static int b_lstat(const char *p, void *st) { return (int)syscall(SYS_newfstatat, AT_FDCWD, p, st, AT_SYMLINK_NOFOLLOW); }

/* ---------------------------------------------------------------- sysconf: Bionic's _SC_* numbers differ from glibc's */
static long b_sysconf(int name) {
    switch (name) {
        case 0x0002: return sysconf(_SC_CLK_TCK);
        case 0x0004: return sysconf(_SC_OPEN_MAX);
        case 0x0027: case 0x0028: return sysconf(_SC_PAGESIZE);
        case 0x0060: return sysconf(_SC_NPROCESSORS_CONF);
        case 0x0061: return sysconf(_SC_NPROCESSORS_ONLN);
        case 0x0062: return sysconf(_SC_PHYS_PAGES);
        case 0x0063: return sysconf(_SC_AVPHYS_PAGES);
        default: fprintf(stderr, "[compat] sysconf(0x%x) unsupported\n", name); errno = EINVAL; return -1;
    }
}

/* ---------------------------------------------------------------- strerror_r: Bionic returns int (XSI), glibc's default is GNU */
extern int __xpg_strerror_r(int, char *, size_t);
static int b_strerror_r(int e, char *buf, size_t n) { return __xpg_strerror_r(e, buf, n); }

/* ---------------------------------------------------------------- getaddrinfo: ai_addr / ai_canonname are swapped */
static void swap_ai(struct addrinfo *a) { for (; a; a = a->ai_next) { void *t = a->ai_addr; a->ai_addr = (void *)a->ai_canonname; a->ai_canonname = t; } }
static int b_getaddrinfo(const char *n, const char *s, const struct addrinfo *hints, struct addrinfo **res) {
    struct addrinfo h2; const struct addrinfo *hp = NULL;
    if (hints) { memset(&h2, 0, sizeof h2); h2.ai_flags = hints->ai_flags; h2.ai_family = hints->ai_family;
                 h2.ai_socktype = hints->ai_socktype; h2.ai_protocol = hints->ai_protocol; hp = &h2; }
    int r = getaddrinfo(n, s, hp, res);
    if (!r) swap_ai(*res);
    return r;
}
static void b_freeaddrinfo(struct addrinfo *a) { swap_ai(a); freeaddrinfo(a); }

/* ---------------------------------------------------------------- sigaction: different struct layout, 64-bit sigset */
struct b_sigaction { int sa_flags; union { void (*h)(int); void (*sa)(int, siginfo_t *, void *); } u; unsigned long sa_mask; void (*sa_restorer)(void); };
static int b_sigaction(int sig, const struct b_sigaction *act, struct b_sigaction *old) {
    if (act && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT)) {
        fprintf(stderr, "[compat] game sigaction(%d) ignored (host crash handler kept)\n", sig);
        if (old) memset(old, 0, sizeof *old);
        return 0;
    }
    struct sigaction ga, go;
    if (act) {
        memset(&ga, 0, sizeof ga);
        ga.sa_flags = act->sa_flags; ga.sa_handler = act->u.h;
        sigemptyset(&ga.sa_mask);
        for (int s = 1; s < 64; s++) if (act->sa_mask & (1ul << (s - 1))) sigaddset(&ga.sa_mask, s);
    }
    int r = sigaction(sig, act ? &ga : NULL, old ? &go : NULL);
    if (!r && old) {
        memset(old, 0, sizeof *old);
        old->sa_flags = go.sa_flags; old->u.h = go.sa_handler;
        for (int s = 1; s < 64; s++) if (sigismember(&go.sa_mask, s) == 1) old->sa_mask |= 1ul << (s - 1);
    }
    return r;
}
static void (*b_signal(int sig, void (*h)(int)))(int) {
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT) return SIG_DFL;
    return signal(sig, h);
}

/* ---------------------------------------------------------------- pthread_attr_t: 56 bytes in Bionic, 64 in glibc.
 * The game allocates the Bionic-sized struct itself, so we must never write glibc's layout into it. */
struct b_attr { uint32_t flags; void *stack_base; size_t stack_size; size_t guard_size; int32_t policy; int32_t prio; char reserved[16]; };
#define B_ATTR_DETACHED 1u
static int b_attr_init(struct b_attr *a) { memset(a, 0, sizeof *a); a->stack_size = 1024 * 1024; a->guard_size = 4096; return 0; }
static int b_attr_destroy(struct b_attr *a) { (void)a; return 0; }
static int b_attr_setdetachstate(struct b_attr *a, int s) { if (s) a->flags |= B_ATTR_DETACHED; else a->flags &= ~B_ATTR_DETACHED; return 0; }
static int b_attr_getdetachstate(const struct b_attr *a, int *s) { *s = (a->flags & B_ATTR_DETACHED) ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE; return 0; }
static int b_attr_setstacksize(struct b_attr *a, size_t s) { a->stack_size = s; return 0; }
static int b_attr_getstacksize(const struct b_attr *a, size_t *s) { *s = a->stack_size; return 0; }
static int b_attr_setschedparam(struct b_attr *a, const struct sched_param *p) { a->prio = p->sched_priority; return 0; }
static int b_pthread_create(pthread_t *t, const struct b_attr *a, void *(*fn)(void *), void *arg) {
    pthread_attr_t ga; pthread_attr_init(&ga);
    if (a) {
        if (a->flags & B_ATTR_DETACHED) pthread_attr_setdetachstate(&ga, PTHREAD_CREATE_DETACHED);
        if (a->stack_size >= 16384) pthread_attr_setstacksize(&ga, a->stack_size);
    }
    int r = pthread_create(t, &ga, fn, arg);
    pthread_attr_destroy(&ga);
    return r;
}

/* ---------------------------------------------------------------- table */
#define E(n, f) { n, (void *)(f) }
const struct host_sym bionic_syms[] = {
    E("__errno", b_errno),
    E("__android_log_print", b_log_print), E("__android_log_write", b_log_write),
    E("__android_log_vprint", b_log_vprint), E("__android_log_assert", b_log_assert),
    E("__assert2", b_assert2),
    E("__strlen_chk", b_strlen_chk), E("__strchr_chk", b_strchr_chk), E("__strrchr_chk", b_strrchr_chk), E("android_set_abort_message", b_set_abort_message), E("__strncpy_chk2", b_strncpy_chk2),
    E("__FD_SET_chk", b_fd_set_chk), E("__FD_CLR_chk", b_fd_clr_chk), E("__FD_ISSET_chk", b_fd_isset_chk),
    E("__system_property_get", b_system_property_get), E("pthread_gettid_np", b_pthread_gettid_np),
    E("__pthread_cleanup_push", b_cleanup_push), E("__pthread_cleanup_pop", b_cleanup_pop),
    E("_ctype_", b_ctype), E("__sF", b_sF),
    E("fclose", b_fclose), E("fflush", b_fflush), E("fgets", b_fgets), E("fputs", b_fputs), E("fread", b_fread),
    E("fwrite", b_fwrite), E("fseek", b_fseek), E("ftell", b_ftell), E("fileno", b_fileno),
    E("vfprintf", b_vfprintf), E("fprintf", b_fprintf),
    E("fstat", b_fstat), E("stat", b_stat), E("lstat", b_lstat),
    E("sysconf", b_sysconf), E("strerror_r", b_strerror_r),
    E("getaddrinfo", b_getaddrinfo), E("freeaddrinfo", b_freeaddrinfo),
    E("sigaction", b_sigaction), E("signal", b_signal), E("bsd_signal", b_signal),
    E("pthread_attr_init", b_attr_init), E("pthread_attr_destroy", b_attr_destroy),
    E("pthread_attr_setdetachstate", b_attr_setdetachstate), E("pthread_attr_getdetachstate", b_attr_getdetachstate),
    E("pthread_attr_setstacksize", b_attr_setstacksize), E("pthread_attr_getstacksize", b_attr_getstacksize),
    E("pthread_attr_setschedparam", b_attr_setschedparam), E("pthread_create", b_pthread_create),
    { NULL, NULL }
};

__attribute__((constructor)) static void bionic64_init(void) { init_ctype(); }
