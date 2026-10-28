#ifndef ELF64_LOADER_H
#define ELF64_LOADER_H
#include <stdint.h>
#include <stddef.h>

typedef struct Elf64Lib Elf64Lib;

/* Symbols that replace glibc's for the game libraries (Bionic ABI differences). NULL-terminated. */
struct host_sym { const char *name; void *addr; };
extern const struct host_sym bionic_syms[];

/* Phase 1: reserve address space and map the PT_LOAD segments (file-backed, copy-on-write: nothing is read into RAM
 * until it is touched). Register the library so later libraries can resolve against it. */
Elf64Lib *e64_map(const char *path, const char *name);
/* Phase 2 (after ALL libraries are mapped): apply RELA relocations, resolve symbols, set final page protections. */
int e64_relocate(Elf64Lib *l);
void e64_run_init(Elf64Lib *l, int argc, char **argv, char **envp);
void *e64_sym(Elf64Lib *l, const char *name);
const char *e64_where(uintptr_t addr, uintptr_t *off);
/* Print every symbol that could not be resolved (they were replaced with trapping stubs). Returns the count. */
int e64_report_unresolved(void);
extern int e64_stub_weak;
size_t e64_size(Elf64Lib *l);
const char *e64_name(Elf64Lib *l);
#endif
