/* Minimal ELF loader for the two Android libraries (libfmod.so, libminecraftpe.so).
 * glibc's dlopen() rejects them silently on the target device, so we map and relocate them ourselves:
 *   - segments are read into anonymous memory (works even if the SD card is mounted noexec),
 *   - REL relocations (RELATIVE / ABS32 / GLOB_DAT / JUMP_SLOT) are applied eagerly,
 *   - symbols are resolved: own definitions -> other custom libs -> shim libs (dlsym handles) -> glibc/libgcc,
 *   - unresolved symbols get a stub that prints the symbol name when called,
 *   - init_array is run, PT_ARM_EXIDX is exposed to the C++ unwinder through dl_iterate_phdr / dl_unwind_find_exidx. */
#define _GNU_SOURCE
#include "elf_loader.h"
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/socket.h>

#define PAGE 4096u
#define PGDOWN(x) ((x) & ~(PAGE - 1))
#define PGUP(x)   (((x) + PAGE - 1) & ~(PAGE - 1))

struct ElfLib {
    char name[64];
    uint8_t *base;              /* load bias */
    size_t size;
    Elf32_Phdr *phdr; int phnum;
    const Elf32_Sym *symtab; const char *strtab;
    const uint32_t *hash; uint32_t nsyms;
    const Elf32_Rel *rel; size_t relsz, jmprelsz; const Elf32_Rel *jmprel;
    uintptr_t *cache;           /* symbol index -> resolved address (0 = not yet) */
    void (*init)(void); uintptr_t *init_array; size_t init_arraysz;
    ElfLib *next;
};

static ElfLib *libs_head;
static struct { void *h; char path[1300]; } resolvers[16]; static int nresolve;
static const char *stub_names[4096]; static int nstubs_used;

void elf_add_resolver(void *dl_handle, const char *path) {
    if (nresolve < 16) { resolvers[nresolve].h = dl_handle; snprintf(resolvers[nresolve].path, sizeof resolvers[0].path, "%s", path); nresolve++; }
}

/* ---------------------------------------------------------------- host-provided symbols */
extern int __cxa_atexit(void (*)(void *), void *, void *);
static int h_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) { return __cxa_atexit(dtor, obj, dso); }

/* Bionic's addrinfo has ai_canonname before ai_addr; glibc has them the other way round. */
struct bionic_addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; socklen_t ai_addrlen;
                         char *ai_canonname; struct sockaddr *ai_addr; struct bionic_addrinfo *ai_next; };
static void swap_ai(struct addrinfo *a) {
    for (; a; a = a->ai_next) { void *t = a->ai_addr; a->ai_addr = (void *)a->ai_canonname; a->ai_canonname = t; }
}
static int h_getaddrinfo(const char *n, const char *s, const struct addrinfo *hints, struct addrinfo **res) {
    struct addrinfo h2; const struct addrinfo *hp = NULL;
    if (hints) { memset(&h2, 0, sizeof h2); h2.ai_flags = hints->ai_flags; h2.ai_family = hints->ai_family;
                 h2.ai_socktype = hints->ai_socktype; h2.ai_protocol = hints->ai_protocol; hp = &h2; }
    int r = getaddrinfo(n, s, hp, res);
    if (!r) swap_ai(*res);
    return r;
}
static void h_freeaddrinfo(struct addrinfo *a) { swap_ai(a); freeaddrinfo(a); }

__asm__(".text\n.arm\n.globl host_sigsetjmp\n.type host_sigsetjmp,%function\n"
        "host_sigsetjmp:\n mov r1, #0\n b __sigsetjmp\n");
extern void host_sigsetjmp(void);

/* unwinder lookup, also used for code in our custom libs */
static void *find_exidx(uintptr_t pc, int *count) {
    for (ElfLib *l = libs_head; l; l = l->next) {
        for (int i = 0; i < l->phnum; i++) {
            if (l->phdr[i].p_type != PT_LOAD) continue;
            uintptr_t lo = (uintptr_t)l->base + l->phdr[i].p_vaddr;
            if (pc >= lo && pc < lo + l->phdr[i].p_memsz) {
                for (int j = 0; j < l->phnum; j++)
                    if (l->phdr[j].p_type == 0x70000001) {   /* PT_ARM_EXIDX */
                        *count = l->phdr[j].p_memsz / 8;
                        return l->base + l->phdr[j].p_vaddr;
                    }
            }
        }
    }
    *count = 0;
    return NULL;
}
void *__gnu_Unwind_Find_exidx(uintptr_t pc, int *count) { return find_exidx(pc, count); }
struct S { uintptr_t pc; void *r; int n; };
static int exidx_cb(struct dl_phdr_info *i, size_t sz, void *d) {
    struct S *s = d; (void)sz; int in = 0;
    for (int k = 0; k < i->dlpi_phnum; k++) {
        const Elf32_Phdr *p = &i->dlpi_phdr[k];
        if (p->p_type == PT_LOAD && s->pc >= i->dlpi_addr + p->p_vaddr && s->pc < i->dlpi_addr + p->p_vaddr + p->p_memsz) in = 1;
    }
    if (!in) return 0;
    for (int k = 0; k < i->dlpi_phnum; k++)
        if (i->dlpi_phdr[k].p_type == 0x70000001) { s->r = (void *)(i->dlpi_addr + i->dlpi_phdr[k].p_vaddr); s->n = i->dlpi_phdr[k].p_memsz / 8; return 1; }
    return 0;
}
void *dl_unwind_find_exidx(uintptr_t pc, int *count) {
    void *r = find_exidx(pc, count);
    if (r) return r;
    /* system libraries: walk their program headers */
    struct S s = { pc, NULL, 0 };
    dl_iterate_phdr(exidx_cb, &s);
    *count = s.n;
    return s.r;
}

/* Let libgcc_s's unwinder (which walks dl_iterate_phdr) also see our libraries. */
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
    static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    if (!real) real = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
    int r = real ? real(cb, data) : 0;
    if (r) return r;
    for (ElfLib *l = libs_head; l; l = l->next) {
        struct dl_phdr_info info; memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))l->base; info.dlpi_name = l->name;
        info.dlpi_phdr = l->phdr; info.dlpi_phnum = l->phnum;
        r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}

static const struct { const char *name; void *addr; } host_syms[] = {
    { "__aeabi_atexit", (void *)h_aeabi_atexit },
    { "sigsetjmp", (void *)host_sigsetjmp },
    { "getaddrinfo", (void *)h_getaddrinfo },
    { "freeaddrinfo", (void *)h_freeaddrinfo },
    { "__gnu_Unwind_Find_exidx", (void *)__gnu_Unwind_Find_exidx },
    { "dl_unwind_find_exidx", (void *)dl_unwind_find_exidx },
};

/* ---------------------------------------------------------------- symbol tables */
static uint32_t sysv_hash(const char *s) {
    uint32_t h = 0, g;
    while (*s) { h = (h << 4) + (unsigned char)*s++; g = h & 0xf0000000; if (g) h ^= g >> 24; h &= ~g; }
    return h;
}

void *elf_sym(ElfLib *l, const char *name) {
    uint32_t nb = l->hash[0], h = sysv_hash(name);
    const uint32_t *bucket = l->hash + 2, *chain = bucket + nb;
    for (uint32_t i = bucket[h % nb]; i; i = chain[i]) {
        const Elf32_Sym *s = &l->symtab[i];
        if (s->st_shndx != SHN_UNDEF && !strcmp(l->strtab + s->st_name, name)) {
            unsigned t = ELF32_ST_TYPE(s->st_info);
            if (t == STT_NOTYPE || t == STT_FUNC || t == STT_OBJECT || t == STT_TLS || t == 0)
                return l->base + s->st_value;
        }
    }
    return NULL;
}

static void unresolved_called(const char *name) {
    fprintf(stderr, "[loader] CALLED UNRESOLVED SYMBOL: %s\n", name);
    fflush(stderr);
    abort();
}

static void *make_stub(const char *name) {
    static uint32_t *page; static int used;
    if (!page || used >= (int)(PAGE / 32)) {
        page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) { page = NULL; return NULL; }
        used = 0;
    }
    uint32_t *p = page + used * 8; used++;
    p[0] = 0xe59f0004;   /* ldr r0, [pc, #4]  -> p[3] */
    p[1] = 0xe59ff004;   /* ldr pc, [pc, #4]  -> p[4] */
    p[3] = (uint32_t)(uintptr_t)name;
    p[4] = (uint32_t)(uintptr_t)unresolved_called;
    __builtin___clear_cache((char *)p, (char *)(p + 8));
    nstubs_used++;
    return p;
}

static void *lookup_external(const char *name, ElfLib *self) {
    void *a;
    for (ElfLib *l = libs_head; l; l = l->next)
        if (l != self && (a = elf_sym(l, name))) return a;
    for (size_t i = 0; i < sizeof host_syms / sizeof *host_syms; i++)
        if (!strcmp(host_syms[i].name, name)) return host_syms[i].addr;
    /* dlsym(handle) also searches the handle's dependencies (glibc!), so accept only symbols the shim itself defines */
    for (int i = 0; i < nresolve; i++) {
        Dl_info di;
        if ((a = dlsym(resolvers[i].h, name)) && dladdr(a, &di) && di.dli_fname && !strcmp(di.dli_fname, resolvers[i].path)) return a;
    }
    if ((a = dlsym(RTLD_DEFAULT, name))) return a;
    return NULL;
}

/* ---------------------------------------------------------------- loading */
static int fail(const char *msg, const char *name) {
    fprintf(stderr, "[loader] %s: %s (%s)\n", name, msg, strerror(errno));
    return 0;
}

static uintptr_t resolve_sym(ElfLib *l, uint32_t idx, int *weak_unres) {
    if (l->cache[idx]) return l->cache[idx];
    const Elf32_Sym *s = &l->symtab[idx];
    const char *nm = l->strtab + s->st_name;
    uintptr_t a = 0;
    if (s->st_shndx != SHN_UNDEF) a = (uintptr_t)l->base + s->st_value;     /* own definition first */
    else if (!(a = (uintptr_t)lookup_external(nm, l))) {
        if (ELF32_ST_BIND(s->st_info) == STB_WEAK) { *weak_unres = 1; return 0; }
        fprintf(stderr, "[loader] %s: unresolved symbol %s (stubbed)\n", l->name, nm);
        a = (uintptr_t)make_stub(nm);
    }
    l->cache[idx] = a;
    return a;
}

ElfLib *elf_load(const char *path, const char *name) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fail("open", path); return NULL; }
    Elf32_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, 4) || eh.e_machine != EM_ARM) {
        fail("not an ARM ELF", path); close(fd); return NULL;
    }
    ElfLib *l = calloc(1, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name);
    l->phnum = eh.e_phnum;
    l->phdr = malloc(eh.e_phnum * sizeof(Elf32_Phdr));
    if (pread(fd, l->phdr, eh.e_phnum * sizeof(Elf32_Phdr), eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof(Elf32_Phdr))) {
        fail("read phdrs", path); close(fd); return NULL;
    }
    uint32_t lo = 0xffffffff, hi = 0;
    for (int i = 0; i < l->phnum; i++) {
        Elf32_Phdr *p = &l->phdr[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_vaddr < lo) lo = p->p_vaddr;
        if (p->p_vaddr + p->p_memsz > hi) hi = p->p_vaddr + p->p_memsz;
    }
    lo = PGDOWN(lo); hi = PGUP(hi);
    l->size = hi - lo;
    uint8_t *mem = mmap(NULL, l->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { fail("mmap reserve", path); close(fd); return NULL; }
    l->base = mem - lo;
    Elf32_Dyn *dyn = NULL;
    for (int i = 0; i < l->phnum; i++) {
        Elf32_Phdr *p = &l->phdr[i];
        if (p->p_type == PT_LOAD) {
            size_t done = 0;
            while (done < p->p_filesz) {
                ssize_t n = pread(fd, l->base + p->p_vaddr + done, p->p_filesz - done, p->p_offset + done);
                if (n <= 0) { fail("read segment", path); close(fd); return NULL; }
                done += n;
            }
        } else if (p->p_type == PT_DYNAMIC) dyn = (Elf32_Dyn *)(l->base + p->p_vaddr);
    }
    close(fd);
    if (!dyn) { fprintf(stderr, "[loader] %s: no PT_DYNAMIC\n", name); return NULL; }

    for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        uintptr_t v = d->d_un.d_ptr;
        switch (d->d_tag) {
            case DT_SYMTAB: l->symtab = (void *)(l->base + v); break;
            case DT_STRTAB: l->strtab = (void *)(l->base + v); break;
            case DT_HASH:   l->hash = (void *)(l->base + v); break;
            case DT_REL:    l->rel = (void *)(l->base + v); break;
            case DT_RELSZ:  l->relsz = v; break;
            case DT_JMPREL: l->jmprel = (void *)(l->base + v); break;
            case DT_PLTRELSZ: l->jmprelsz = v; break;
            case DT_INIT:   l->init = (void *)(l->base + v); break;
            case DT_INIT_ARRAY: l->init_array = (void *)(l->base + v); break;
            case DT_INIT_ARRAYSZ: l->init_arraysz = v; break;
        }
    }
    if (!l->symtab || !l->strtab || !l->hash) { fprintf(stderr, "[loader] %s: missing dynamic tables\n", name); return NULL; }
    l->nsyms = l->hash[1];
    l->cache = calloc(l->nsyms + 1, sizeof(uintptr_t));

    /* register before relocating so the unwinder and later libs can see it */
    l->next = libs_head; libs_head = l;

    size_t nrel = 0, nweak = 0;
    for (int pass = 0; pass < 2; pass++) {
        const Elf32_Rel *r = pass ? l->jmprel : l->rel;
        size_t n = (pass ? l->jmprelsz : l->relsz) / sizeof(Elf32_Rel);
        for (size_t i = 0; i < n; i++, nrel++) {
            uint32_t *where = (uint32_t *)(l->base + r[i].r_offset);
            uint32_t type = ELF32_R_TYPE(r[i].r_info), sym = ELF32_R_SYM(r[i].r_info);
            int weak = 0;
            uintptr_t S = sym ? resolve_sym(l, sym, &weak) : 0;
            nweak += weak;
            switch (type) {
                case R_ARM_RELATIVE: *where += (uint32_t)(uintptr_t)l->base; break;
                case R_ARM_ABS32:    *where += (uint32_t)S; break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT: *where = (uint32_t)S; break;
                case R_ARM_NONE: break;
                default:
                    fprintf(stderr, "[loader] %s: unsupported relocation type %u at 0x%x\n", name, type, r[i].r_offset);
            }
        }
    }

    /* final page protections */
    for (int i = 0; i < l->phnum; i++) {
        Elf32_Phdr *p = &l->phdr[i];
        if (p->p_type != PT_LOAD) continue;
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) | (p->p_flags & PF_X ? PROT_EXEC : 0);
        uintptr_t a = PGDOWN((uintptr_t)l->base + p->p_vaddr), e = PGUP((uintptr_t)l->base + p->p_vaddr + p->p_memsz);
        if (mprotect((void *)a, e - a, prot)) fail("mprotect", name);
        if (p->p_flags & PF_X) __builtin___clear_cache((char *)a, (char *)e);
    }
    fprintf(stderr, "[loader] %s mapped at %p size 0x%zx, %zu relocations (%zu weak-unresolved), %d stubbed symbols total\n",
            name, l->base, l->size, nrel, nweak, nstubs_used);
    return l;
}

void elf_run_init(ElfLib *l, int argc, char **argv, char **envp) {
    typedef void (*initfn)(int, char **, char **);
    if (l->init) ((initfn)l->init)(argc, argv, envp);
    size_t n = l->init_arraysz / sizeof(uintptr_t);
    fprintf(stderr, "[loader] %s: running %zu init_array entries\n", l->name, n);
    for (size_t i = 0; i < n; i++) {
        uintptr_t f = l->init_array[i];
        if (f && f != (uintptr_t)-1) ((initfn)f)(argc, argv, envp);
    }
    fprintf(stderr, "[loader] %s: init done\n", l->name);
}

/* Describe an address as "lib+0xoffset" for crash reports. */
const char *elf_where(uintptr_t addr, uintptr_t *off) {
    for (ElfLib *l = libs_head; l; l = l->next)
        if (addr >= (uintptr_t)l->base && addr < (uintptr_t)l->base + l->size + 0x10000000u) {
            for (int i = 0; i < l->phnum; i++) {
                const Elf32_Phdr *p = &l->phdr[i];
                if (p->p_type == PT_LOAD && addr >= (uintptr_t)l->base + p->p_vaddr && addr < (uintptr_t)l->base + p->p_vaddr + p->p_memsz) {
                    *off = addr - (uintptr_t)l->base; return l->name;
                }
            }
        }
    return NULL;
}
