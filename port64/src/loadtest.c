/* Load test: maps and relocates the Android arm64 libraries of Minecraft Bedrock on a glibc/AArch64 system
 * (run under qemu-aarch64 on a PC, or natively on the R36S), then lists every symbol that could not be resolved.
 *   usage: loadtest <dir-with-APK-libs> [--init]
 * --init additionally runs the static constructors (2373 for libminecraftpe.so) as a smoke test. */
#define _GNU_SOURCE
#include "elf64_loader.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>

static void on_segv(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_; uintptr_t off, pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    const char *w = e64_where(pc, &off);
    fprintf(stderr, "[crash] signal %d addr=%p pc=%p (%s+0x%lx)", sig, si->si_addr, (void *)pc, w ? w : "?", (unsigned long)off);
    w = e64_where(lr, &off);
    fprintf(stderr, " lr=%p (%s+0x%lx)\n", (void *)lr, w ? w : "?", (unsigned long)off);
    _exit(139);
}

int main(int argc, char **argv, char **envp) {
    if (argc < 2) { fprintf(stderr, "usage: %s <libdir> [--init]\n", argv[0]); return 2; }
    int do_init = argc > 2 && !strcmp(argv[2], "--init");
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);

    /* order = dependency order; every library is mapped before any is relocated */
    static const char *order[] = { "libc++_shared.so", "libfmod.so", "libHttpClient.Android.so", "libPlayFabMultiplayer.so",
                                   "libmaesdk.so", "libminecraftpe.so", NULL };
    Elf64Lib *libs[16]; int n = 0;
    for (int i = 0; order[i]; i++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], order[i]);
        if (!(libs[n] = e64_map(path, order[i]))) { fprintf(stderr, "FAILED to map %s\n", order[i]); return 1; }
        n++;
    }
    int ok = 1;
    for (int i = 0; i < n; i++) ok &= e64_relocate(libs[i]);
    int unres = e64_report_unresolved();
    fprintf(stderr, "[loadtest] %d libraries relocated, %d unresolved symbols, relocation errors: %s\n", n, unres, ok ? "none" : "YES");
    if (do_init) for (int i = 0; i < n; i++) e64_run_init(libs[i], argc, argv, envp);
    return ok ? 0 : 1;
}
