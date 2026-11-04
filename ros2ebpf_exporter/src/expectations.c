// SPDX-License-Identifier: Apache-2.0
/*
 * expectations.yaml: a flat mapping of topic -> expected rate, with an
 * optional taker count. Accepted forms:
 *   /odom: 50
 *   /lidar/points: {hz: 10, takers: 2}
 *   /imu/data:
 *     hz: 200
 *     takers: 1
 * Kept deliberately simple so the daemon needs no YAML library for it.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ros2ebpf/exporter.h"

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1]))
        s[--l] = '\0';
    return s;
}

static void parse_inline(char *v, struct ros2ebpf_expectation *e)
{
    /* v is inside the braces: "hz: 10, takers: 2" */
    char *save = NULL;
    for (char *tok = strtok_r(v, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *k = trim(tok), *val = trim(colon + 1);
        if (strcmp(k, "hz") == 0)
            e->hz = atof(val);
        else if (strcmp(k, "takers") == 0)
            e->takers = atoi(val);
    }
}

int ros2ebpf_expectations_load(const char *path, struct ros2ebpf_expectation **out, size_t *n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    struct ros2ebpf_expectation *arr = NULL;
    size_t cnt = 0, cap = 0;
    char line[512];
    struct ros2ebpf_expectation *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        int indented = line[0] == ' ' || line[0] == '\t';
        char *s = trim(line);
        if (!*s)
            continue;
        char *colon = strchr(s, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *key = trim(s), *val = trim(colon + 1);

        if (indented && cur) {
            if (strcmp(key, "hz") == 0)
                cur->hz = atof(val);
            else if (strcmp(key, "takers") == 0)
                cur->takers = atoi(val);
            continue;
        }
        if (cnt == cap) {
            cap = cap ? cap * 2 : 16;
            struct ros2ebpf_expectation *p = realloc(arr, cap * sizeof(*arr));
            if (!p) {
                free(arr);
                fclose(f);
                return -ENOMEM;
            }
            arr = p;
        }
        cur = &arr[cnt++];
        memset(cur, 0, sizeof(*cur));
        cur->hz = -1;
        cur->takers = -1;
        if (*key == '"' || *key == '\'') {
            key++;
            size_t l = strlen(key);
            if (l && (key[l - 1] == '"' || key[l - 1] == '\''))
                key[l - 1] = '\0';
        }
        snprintf(cur->topic, sizeof(cur->topic), "%s", key);
        if (*val == '{') {
            char *end = strrchr(val, '}');
            if (end)
                *end = '\0';
            parse_inline(val + 1, cur);
        } else if (*val) {
            cur->hz = atof(val);
        }
    }
    fclose(f);
    *out = arr;
    *n = cnt;
    return (int)cnt;
}
