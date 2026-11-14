// SPDX-License-Identifier: Apache-2.0
/*
 * Struct offsets of the rmw handles. ros2ebpf follows ROS 2 Rolling only
 * (fujitatomoya/ros2ebpf#15): the table has one row per supported
 * architecture, and CI regenerates the values from the rmw headers of mainline
 * on every commit (check_rmw_offsets in scripts/github_workflows.sh) so a
 * layout change on mainline fails the build until the row is updated. The
 * public rmw handles start with { implementation_identifier, data, <name> }
 * and rmw_message_info_t carries publication_sequence_number at offset 16 on
 * both LP64 targets, hence the identical values. The row in use is exported
 * per host as ros2ebpf_host_info{offset_table="rolling/<arch>"}.
 * ros2ebpf_daemon/offsets/gen.sh prints the row for a given Rolling install.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <elf.h>
#include "offsets.h"

static const struct offset_entry table[] = {
    { "rolling", "x86_64",  16, 16 },
    { "rolling", "aarch64", 16, 16 },
};

const struct offset_entry *offsets_lookup(const char *distro, const char *arch)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const struct offset_entry *e = &table[i];
        if (!strcmp(e->distro, distro) && !strcmp(e->arch, arch))
            return e;
    }
    return NULL;
}

const struct offset_entry *offsets_host_default(void)
{
    struct utsname u;
    const struct offset_entry *e = uname(&u) == 0 ? offsets_lookup("rolling", u.machine) : NULL;
    return e ? e : &table[0];
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
