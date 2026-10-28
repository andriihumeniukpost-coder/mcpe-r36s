/* Minimal AArch64 ELF loader for the Android libraries (libminecraftpe.so and friends).
 * glibc's dlopen() cannot be used: the libraries import Bionic-only symbols and are linked against libc.so/libm.so/
 * libdl.so/liblog.so names that do not exist on a glibc system. So we map and relocate them ourselves.
 *   - PT_LOAD segments are mmap()ed from the file MAP_PRIVATE (the 326 MB libminecraftpe.so is NOT copied into RAM),
 *   - RELA relocations: RELATIVE / ABS64 / GLOB_DAT / JUMP_SLOT / IRELATIVE are applied eagerly,
 *   - symbol lookup: all loaded libs -> bionic_syms[] -> glibc via dlsym(RTLD_DEFAULT),
 *   - unresolved symbols become stubs that print the symbol name when called (instead of crashing at address 0),
 *   - dl_iterate_phdr() is interposed so the C++ unwinder (.eh_frame_hdr) can see our libraries. */
#define _GNU_SOURCE
#include "elf64_loader.h"
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif
#define PAGE 4096ul
#define ALIGN16K 0x4000ul
#define PGDOWN(x) ((x) & ~(PAGE - 1))
#define PGUP(x)   (((x) + PAGE - 1) & ~(PAGE - 1))

struct Elf64Lib {
    char name[64];
    uint8_t *base; size_t size;
    Elf64_Phdr *phdr; int phnum;
    int fd;
    const Elf64_Sym *symtab; const char *strtab;
    const uint32_t *gnuhash; uint32_t nsyms;
    const Elf64_Rela *rela; size_t relasz, jmprelsz; const Elf64_Rela *jmprel;
    uintptr_t *cache;
    void (*init)(void); uintptr_t *init_array; size_t init_arraysz;
    int relocated;
    Elf64Lib *next;
};

static Elf64Lib *libs_head, *libs_tail;

/* ---------------------------------------------------------------- unwinder support */
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
    static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    if (!real) real = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
    for (Elf64Lib *l = libs_head; l; l = l->next) {
        struct dl_phdr_info info; memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))l->base; info.dlpi_name = l->name;
        info.dlpi_phdr = l->phdr; info.dlpi_phnum = l->phnum;
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return real ? real(cb, data) : 0;
}

/* ---------------------------------------------------------------- symbols */
static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    for (unsigned char c; (c = *s++);) h = h * 33 + c;
    return h;
}

void *e64_sym(Elf64Lib *l, const char *name) {
    if (!l->gnuhash) return NULL;
    const uint32_t *h = l->gnuhash;
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2], bloom_shift = h[3];
    const uint64_t *bloom = (const uint64_t *)(h + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t hv = gnu_hash(name);
    uint64_t w = bloom[(hv / 64) % bloom_size];
    if (!((w >> (hv % 64)) & (w >> ((hv >> bloom_shift) % 64)) & 1)) return NULL;
    uint32_t i = buckets[hv % nbuckets];
    if (i < symoffset) return NULL;
    for (;; i++) {
        uint32_t ch = chain[i - symoffset];
        if ((ch | 1) == (hv | 1)) {
            const Elf64_Sym *s = &l->symtab[i];
            if (s->st_shndx != SHN_UNDEF && !strcmp(l->strtab + s->st_name, name)) {
                unsigned t = ELF64_ST_TYPE(s->st_info);
                if (t == STT_NOTYPE || t == STT_FUNC || t == STT_OBJECT || t == STT_GNU_IFUNC)
                    return l->base + s->st_value;
            }
        }
        if (ch & 1) break;
    }
    return NULL;
}

static uint32_t count_dynsyms(const uint32_t *h) {
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint32_t *buckets = (const uint32_t *)((const uint64_t *)(h + 4) + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t max = 0;
    for (uint32_t b = 0; b < nbuckets; b++) if (buckets[b] > max) max = buckets[b];
    if (max < symoffset) return symoffset;
    while (!(chain[max - symoffset] & 1)) max++;
    return max + 1;
}

/* ---------------------------------------------------------------- stubs for unresolved symbols */
static struct { char lib[64]; const char *sym; } unresolved[8192], weakunres[8192];
static int nunresolved, nweakunres;
int e64_stub_weak;   /* 1: give unresolved WEAK symbols a trapping stub too (default: leave NULL so "if (fn)" checks work) */

static void unresolved_called(const char *name) {
    fprintf(stderr, "[loader] CALLED UNRESOLVED SYMBOL: %s\n", name);
    fflush(stderr);
    abort();
}

/* 32-byte stub:  ldr x0,[pc,#16] ; ldr x16,[pc,#20] ; br x16 ; <pad> ; .quad name ; .quad handler */
static void *make_stub(const char *name) {
    static uint32_t *page; static int used;
    if (!page || used >= (int)(PAGE / 32)) {
        page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) { page = NULL; return NULL; }
        used = 0;
    }
    uint32_t *p = page + used * 8; used++;
    p[0] = 0x58000080; p[1] = 0x580000b0; p[2] = 0xd61f0200; p[3] = 0xd503201f;
    *(uint64_t *)(p + 4) = (uint64_t)(uintptr_t)name;
    *(uint64_t *)(p + 6) = (uint64_t)(uintptr_t)unresolved_called;
    __builtin___clear_cache((char *)p, (char *)(p + 8));
    return p;
}

static void *lookup_external(const char *name) {
    void *a;
    for (Elf64Lib *l = libs_head; l; l = l->next)
        if ((a = e64_sym(l, name))) return a;
    for (const struct host_sym *h = bionic_syms; h->name; h++)
        if (!strcmp(h->name, name)) return h->addr;
    if ((a = dlsym(RTLD_DEFAULT, name))) return a;
    /* system libraries the host executable may not have pulled in (libm is dropped by --as-needed, libz is optional) */
    static void *extra[3]; static int tried;
    if (!tried) { tried = 1; extra[0] = dlopen("libm.so.6", RTLD_NOW); extra[1] = dlopen("libz.so.1", RTLD_NOW); extra[2] = dlopen("libgcc_s.so.1", RTLD_NOW); }
    for (int i = 0; i < 3; i++) if (extra[i] && (a = dlsym(extra[i], name))) return a;
    return NULL;
}

static uintptr_t resolve_sym(Elf64Lib *l, uint32_t idx, int *weak_unres) {
    if (idx >= l->nsyms) {
        fprintf(stderr, "[loader] %s: relocation references symbol index %u but nsyms=%u (hash table undercount?)\n", l->name, idx, l->nsyms);
        *weak_unres = 1; return 0;
    }
    if (l->cache[idx]) return l->cache[idx];
    const Elf64_Sym *s = &l->symtab[idx];
    const char *nm = l->strtab + s->st_name;
    uintptr_t a = 0;
    if (s->st_shndx != SHN_UNDEF) a = (uintptr_t)l->base + s->st_value;
    else if (!(a = (uintptr_t)lookup_external(nm))) {
        if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
            if (nweakunres < 8192) { snprintf(weakunres[nweakunres].lib, 64, "%s", l->name); weakunres[nweakunres++].sym = nm; }
            if (!e64_stub_weak) { *weak_unres = 1; return 0; }
            a = (uintptr_t)make_stub(nm); l->cache[idx] = a; return a;
        }
        if (nunresolved < 8192) { snprintf(unresolved[nunresolved].lib, 64, "%s", l->name); unresolved[nunresolved++].sym = nm; }
        a = (uintptr_t)make_stub(nm);
    }
    l->cache[idx] = a;
    return a;
}

int e64_report_unresolved(void) {
    for (int i = 0; i < nunresolved; i++) printf("UNRESOLVED %s: %s\n", unresolved[i].lib, unresolved[i].sym);
    for (int i = 0; i < nweakunres; i++) printf("WEAK-UNRESOLVED %s: %s\n", weakunres[i].lib, weakunres[i].sym);
    return nunresolved;
}

/* ---------------------------------------------------------------- phase 1: map */
static int fail(const char *msg, const char *name) {
    fprintf(stderr, "[loader] %s: %s (%s)\n", name, msg, strerror(errno));
    return 0;
}

Elf64Lib *e64_map(const char *path, const char *name) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fail("open", path); return NULL; }
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, 4) ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_AARCH64) {
        fprintf(stderr, "[loader] %s: not an AArch64 ELF64\n", path); close(fd); return NULL;
    }
    Elf64Lib *l = calloc(1, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name);
    l->fd = fd; l->phnum = eh.e_phnum;
    size_t phsz = eh.e_phnum * sizeof(Elf64_Phdr);
    l->phdr = malloc(phsz);
    if (pread(fd, l->phdr, phsz, eh.e_phoff) != (ssize_t)phsz) { fail("read phdrs", path); return NULL; }

    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < l->phnum; i++) {
        Elf64_Phdr *p = &l->phdr[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_vaddr < lo) lo = p->p_vaddr;
        if (p->p_vaddr + p->p_memsz > hi) hi = p->p_vaddr + p->p_memsz;
    }
    lo = PGDOWN(lo); hi = PGUP(hi);
    l->size = hi - lo;
    uint8_t *res = mmap(NULL, l->size + ALIGN16K, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { fail("reserve address space", path); return NULL; }
    res = (uint8_t *)(((uintptr_t)res + ALIGN16K - 1) & ~(ALIGN16K - 1));
    l->base = res - lo;

    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < l->phnum; i++) {
        Elf64_Phdr *p = &l->phdr[i];
        if (p->p_type == PT_DYNAMIC) { /* resolved below, after mapping */ }
        if (p->p_type != PT_LOAD) continue;
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_X ? PROT_EXEC : 0) | PROT_WRITE;
        uint64_t va = PGDOWN(p->p_vaddr), off = PGDOWN(p->p_offset);
        uint64_t fend = PGUP(p->p_vaddr + p->p_filesz), mend = PGUP(p->p_vaddr + p->p_memsz);
        if (fend > va) {
            void *m = mmap(l->base + va, fend - va, prot, MAP_PRIVATE | MAP_FIXED, fd, off);
            if (m == MAP_FAILED) {            /* e.g. noexec mount: fall back to a RAM copy */
                m = mmap(l->base + va, fend - va, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
                if (m == MAP_FAILED) { fail("map segment", path); return NULL; }
                size_t done = 0, want = p->p_filesz + (p->p_vaddr - va);
                while (done < want) { ssize_t n = pread(fd, l->base + va + done, want - done, off + done); if (n <= 0) break; done += n; }
                fprintf(stderr, "[loader] %s: segment %d copied into RAM (file mmap failed)\n", name, i);
            }
        }
        if (p->p_flags & PF_W) {              /* zero .bss tail of the last file page, then anonymous pages for the rest */
            uint64_t z0 = p->p_vaddr + p->p_filesz;
            if (p->p_memsz > p->p_filesz && PGUP(z0) > z0) memset(l->base + z0, 0, PGUP(z0) - z0 > p->p_memsz - p->p_filesz ? p->p_memsz - p->p_filesz : PGUP(z0) - z0);
        }
        if (mend > fend && mmap(l->base + fend, mend - fend, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
            fail("map bss", path); return NULL;
        }
    }
    for (int i = 0; i < l->phnum; i++) if (l->phdr[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn *)(l->base + l->phdr[i].p_vaddr);
    if (!dyn) { fprintf(stderr, "[loader] %s: no PT_DYNAMIC\n", name); return NULL; }

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        uintptr_t v = d->d_un.d_ptr;
        switch (d->d_tag) {
            case DT_SYMTAB: l->symtab = (void *)(l->base + v); break;
            case DT_STRTAB: l->strtab = (void *)(l->base + v); break;
            case DT_GNU_HASH: l->gnuhash = (void *)(l->base + v); break;
            case DT_RELA:   l->rela = (void *)(l->base + v); break;
            case DT_RELASZ: l->relasz = v; break;
            case DT_JMPREL: l->jmprel = (void *)(l->base + v); break;
            case DT_PLTRELSZ: l->jmprelsz = v; break;
            case DT_INIT:   l->init = (void *)(l->base + v); break;
            case DT_INIT_ARRAY: l->init_array = (void *)(l->base + v); break;
            case DT_INIT_ARRAYSZ: l->init_arraysz = v; break;
            case 0x24: /* DT_RELR */ fprintf(stderr, "[loader] %s: DT_RELR not supported yet\n", name); break;
        }
    }
    if (!l->symtab || !l->strtab || !l->gnuhash) { fprintf(stderr, "[loader] %s: missing dynamic tables\n", name); return NULL; }
    l->nsyms = count_dynsyms(l->gnuhash);
    /* some libraries list imports after the hashed symbols, so also cover every index the relocations mention */
    for (int pass = 0; pass < 2; pass++) {
        const Elf64_Rela *r = pass ? l->jmprel : l->rela;
        size_t n = (pass ? l->jmprelsz : l->relasz) / sizeof(Elf64_Rela);
        for (size_t i = 0; i < n; i++) { uint32_t si = ELF64_R_SYM(r[i].r_info); if (si >= l->nsyms) l->nsyms = si + 1; }
    }
    l->cache = calloc(l->nsyms + 1, sizeof(uintptr_t));
    if (libs_tail) libs_tail->next = l; else libs_head = l;
    libs_tail = l;
    return l;
}

/* ---------------------------------------------------------------- phase 2: relocate */
int e64_relocate(Elf64Lib *l) {
    size_t nrel = 0, nweak = 0, nbad = 0;
    for (int pass = 0; pass < 2; pass++) {
        const Elf64_Rela *r = pass ? l->jmprel : l->rela;
        size_t n = (pass ? l->jmprelsz : l->relasz) / sizeof(Elf64_Rela);
        for (size_t i = 0; i < n; i++, nrel++) {
            uint64_t *where = (uint64_t *)(l->base + r[i].r_offset);
            uint32_t type = ELF64_R_TYPE(r[i].r_info), sym = ELF64_R_SYM(r[i].r_info);
            int weak = 0;
            uintptr_t S = (sym && type != R_AARCH64_RELATIVE) ? resolve_sym(l, sym, &weak) : 0;
            nweak += weak;
            switch (type) {
                case R_AARCH64_RELATIVE: *where = (uint64_t)(uintptr_t)l->base + r[i].r_addend; break;
                case R_AARCH64_ABS64:    *where = S + r[i].r_addend; break;
                case R_AARCH64_GLOB_DAT:
                case R_AARCH64_JUMP_SLOT: *where = S + r[i].r_addend; break;
                case R_AARCH64_IRELATIVE: *where = ((uint64_t (*)(void))(l->base + r[i].r_addend))(); break;
                case R_AARCH64_NONE: break;
                default:
                    if (nbad++ < 10) fprintf(stderr, "[loader] %s: unsupported relocation type %u at 0x%lx\n", l->name, type, (unsigned long)r[i].r_offset);
            }
        }
    }
    for (int i = 0; i < l->phnum; i++) {          /* final page protections */
        Elf64_Phdr *p = &l->phdr[i];
        if (p->p_type != PT_LOAD) continue;
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) | (p->p_flags & PF_X ? PROT_EXEC : 0);
        uintptr_t a = PGDOWN((uintptr_t)l->base + p->p_vaddr), e = PGUP((uintptr_t)l->base + p->p_vaddr + p->p_memsz);
        if (mprotect((void *)a, e - a, prot)) {
            fprintf(stderr, "[loader] %s: base=%p p_vaddr=0x%lx memsz=0x%lx: mprotect(seg %d, vaddr 0x%lx..0x%lx, prot %d) failed: %s\n", l->name, l->base, (unsigned long)p->p_vaddr, (unsigned long)p->p_memsz, i,
                    (unsigned long)(a - (uintptr_t)l->base), (unsigned long)(e - (uintptr_t)l->base), prot, strerror(errno));
        }
        if (p->p_flags & PF_X) __builtin___clear_cache((char *)a, (char *)e);
    }
    for (int i = 0; i < l->phnum; i++) {          /* PT_GNU_RELRO */
        Elf64_Phdr *p = &l->phdr[i];
        if (p->p_type == PT_GNU_RELRO) {
            uintptr_t a = PGDOWN((uintptr_t)l->base + p->p_vaddr), e = PGDOWN((uintptr_t)l->base + p->p_vaddr + p->p_memsz);
            if (e > a) mprotect((void *)a, e - a, PROT_READ);
        }
    }
    l->relocated = 1;
    fprintf(stderr, "[loader] %-28s base=%p size=0x%zx  %zu relocs, %zu weak-unresolved, %zu unsupported\n",
            l->name, l->base, l->size, nrel, nweak, nbad);
    return nbad == 0;
}

void e64_run_init(Elf64Lib *l, int argc, char **argv, char **envp) {
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

const char *e64_where(uintptr_t addr, uintptr_t *off) {
    for (Elf64Lib *l = libs_head; l; l = l->next)
        if (addr >= (uintptr_t)l->base && addr < (uintptr_t)l->base + l->size + 0x200000000ull) {
            for (int i = 0; i < l->phnum; i++) {
                const Elf64_Phdr *p = &l->phdr[i];
                if (p->p_type == PT_LOAD && addr >= (uintptr_t)l->base + p->p_vaddr && addr < (uintptr_t)l->base + p->p_vaddr + p->p_memsz) {
                    *off = addr - (uintptr_t)l->base; return l->name;
                }
            }
        }
    return NULL;
}
size_t e64_size(Elf64Lib *l) { return l->size; }
const char *e64_name(Elf64Lib *l) { return l->name; }
