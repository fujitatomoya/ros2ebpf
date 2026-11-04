// SPDX-License-Identifier: Apache-2.0
/*
 * Per-(distro, arch) struct offsets. The public rmw handles have had the
 * layout { implementation_identifier, data, <name> } since Foxy and
 * rmw_message_info_t has carried publication_sequence_number at offset 16
 * since Galactic, so every entry is identical today; the table exists so a
 * future ABI change on one distro does not silently misread the others.
 * ros2ebpf_daemon/offsets/gen.sh regenerates the values with pahole/gcc.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <elf.h>
#include "offsets.h"

static const struct offset_entry table[] = {
    { "humble",  "x86_64",  16, 16 },
    { "humble",  "aarch64", 16, 16 },
    { "iron",    "x86_64",  16, 16 },
    { "iron",    "aarch64", 16, 16 },
    { "jazzy",   "x86_64",  16, 16 },
    { "jazzy",   "aarch64", 16, 16 },
    { "kilted",  "x86_64",  16, 16 },
    { "kilted",  "aarch64", 16, 16 },
    { "rolling", "x86_64",  16, 16 },
    { "rolling", "aarch64", 16, 16 },
    { "lp64",    "",        16, 16 },   /* generic fallback for any LP64 target */
};

const struct offset_entry *offsets_lookup(const char *distro, const char *arch)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const struct offset_entry *e = &table[i];
        if ((!distro[0] || !strcmp(e->distro, distro)) && (!arch[0] || !e->arch[0] || !strcmp(e->arch, arch)))
            return e;
    }
    return NULL;
}

int offsets_distro_from_path(const char *path, char *out, size_t n)
{
    const char *p = strstr(path, "/opt/ros/");
    if (!p)
        return -1;
    p += strlen("/opt/ros/");
    const char *end = strchr(p, '/');
    if (!end || end == p)
        return -1;
    snprintf(out, n, "%.*s", (int)(end - p), p);
    return 0;
}

void offsets_apply(const struct offset_entry *e, struct ros2ebpf_config *cfg)
{
    cfg->name_off_pub = cfg->name_off_sub = cfg->name_off_client = cfg->name_off_service = e->name_off;
    cfg->msginfo_off_pubseq = e->msginfo_pubseq;
}

static const char *required_syms[] = {
    "rmw_publish", "rmw_take_with_info", "rmw_send_request", "rmw_take_response",
    "rmw_take_request", "rmw_send_response", "rmw_destroy_publisher",
    "rmw_destroy_subscription", "rmw_destroy_client", "rmw_destroy_service",
    "rmw_get_implementation_identifier",
};

/* Minimal ELF64 .dynsym scan; enough to tell a shim from a random library. */
int offsets_check_shim(const char *path, char *missing, size_t n)
{
    missing[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(missing, n, "open: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        snprintf(missing, n, "not an ELF file");
        return -1;
    }
    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        snprintf(missing, n, "mmap: %s", strerror(errno));
        return -1;
    }
    const unsigned char *b = base;
    const Elf64_Ehdr *eh = base;
    int rc = -1;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64) {
        snprintf(missing, n, "not ELF64");
        goto out;
    }
    if (eh->e_shoff == 0 || eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > (size_t)st.st_size) {
        snprintf(missing, n, "no section headers");
        goto out;
    }
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(b + eh->e_shoff);
    const Elf64_Shdr *dynsym = NULL, *dynstr = NULL;
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_DYNSYM) {
            dynsym = &sh[i];
            if (sh[i].sh_link < eh->e_shnum)
                dynstr = &sh[sh[i].sh_link];
        }
    if (!dynsym || !dynstr) {
        snprintf(missing, n, "no .dynsym");
        goto out;
    }
    const Elf64_Sym *syms = (const Elf64_Sym *)(b + dynsym->sh_offset);
    size_t nsyms = dynsym->sh_size / sizeof(Elf64_Sym);
    const char *strtab = (const char *)(b + dynstr->sh_offset);
    size_t found_mask = 0, want = sizeof(required_syms) / sizeof(required_syms[0]);
    for (size_t i = 0; i < nsyms; i++) {
        if (syms[i].st_shndx == SHN_UNDEF || syms[i].st_name >= dynstr->sh_size)
            continue;
        const char *name = strtab + syms[i].st_name;
        for (size_t k = 0; k < want; k++)
            if (!strcmp(name, required_syms[k]))
                found_mask |= 1u << k;
    }
    rc = 0;
    for (size_t k = 0; k < want; k++)
        if (!(found_mask & (1u << k))) {
            rc = -1;
            if (missing[0])
                strncat(missing, ",", n - strlen(missing) - 1);
            strncat(missing, required_syms[k], n - strlen(missing) - 1);
        }
out:
    munmap(base, (size_t)st.st_size);
    return rc;
}
