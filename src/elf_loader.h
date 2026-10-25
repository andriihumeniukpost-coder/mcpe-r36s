#ifndef ELF_LOADER_H
#define ELF_LOADER_H
#include <stdint.h>
typedef struct ElfLib ElfLib;
/* dlopen() handle of a shim library whose symbols take priority over glibc (call in the order of priority); path = the string given to dlopen */
void elf_add_resolver(void *dl_handle, const char *path);
ElfLib *elf_load(const char *path, const char *name);
void *elf_sym(ElfLib *l, const char *name);
void elf_run_init(ElfLib *l, int argc, char **argv, char **envp);
const char *elf_where(uintptr_t addr, uintptr_t *off);
#endif
