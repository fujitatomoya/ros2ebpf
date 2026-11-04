// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ros2ebpf/exporter.h"

static int grow(struct ros2ebpf_buf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + need + 1)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p)
        return -ENOMEM;
    b->data = p;
    b->cap = cap;
    return 0;
}

int ros2ebpf_buf_append(struct ros2ebpf_buf *b, const char *s, size_t n)
{
    int rc = grow(b, n);
    if (rc)
        return rc;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int ros2ebpf_buf_printf(struct ros2ebpf_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return -EINVAL;
    }
    int rc = grow(b, (size_t)n);
    if (rc) {
        va_end(ap2);
        return rc;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 0;
}

int ros2ebpf_buf_escape(struct ros2ebpf_buf *b, const char *s)
{
    if (!s)
        return 0;
    for (; *s; s++) {
        int rc;
        switch (*s) {
        case '\\': rc = ros2ebpf_buf_append(b, "\\\\", 2); break;
        case '"':  rc = ros2ebpf_buf_append(b, "\\\"", 2); break;
        case '\n': rc = ros2ebpf_buf_append(b, "\\n", 2); break;
        default:   rc = ros2ebpf_buf_append(b, s, 1); break;
        }
        if (rc)
            return rc;
    }
    return 0;
}

void ros2ebpf_buf_free(struct ros2ebpf_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
