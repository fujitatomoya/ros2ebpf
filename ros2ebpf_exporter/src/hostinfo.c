// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "ros2ebpf/exporter.h"

static void read_first_line(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(out, (int)n, f)) {
        size_t l = strlen(out);
        while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' '))
            out[--l] = '\0';
    }
    fclose(f);
}

static void os_release_value(const char *key, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/os-release", "r");
    if (!f)
        return;
    char line[256];
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            char *v = line + kl + 1;
            size_t l = strlen(v);
            while (l && (v[l - 1] == '\n' || v[l - 1] == '"'))
                v[--l] = '\0';
            if (*v == '"')
                v++;
            snprintf(out, n, "%s", v);
            break;
        }
    }
    fclose(f);
}

void ros2ebpf_host_info_collect(struct ros2ebpf_host_info *hi)
{
    read_first_line("/proc/sys/kernel/hostname", hi->host, sizeof(hi->host));
    if (!hi->host[0] && gethostname(hi->host, sizeof(hi->host)) != 0)
        snprintf(hi->host, sizeof(hi->host), "unknown");
    read_first_line("/etc/machine-id", hi->machine_id, sizeof(hi->machine_id));

    char id[32], ver[32];
    os_release_value("ID", id, sizeof(id));
    os_release_value("VERSION_ID", ver, sizeof(ver));
    if (id[0])
        snprintf(hi->distro, sizeof(hi->distro), "%.31s%s%.31s", id, ver[0] ? "-" : "", ver);
    else
        snprintf(hi->distro, sizeof(hi->distro), "unknown");

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(hi->arch, sizeof(hi->arch), "%.31s", u.machine);
        snprintf(hi->kernel, sizeof(hi->kernel), "%.127s", u.release);
    }
}
