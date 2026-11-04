/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal, hand-maintained subset of the kernel types the ros2ebpf BPF
 * programs touch. Every struct carries preserve_access_index so libbpf
 * relocates field offsets against the running kernel's BTF (CO-RE); the
 * field lists here only need to contain what the programs read, and may
 * list fields that some kernels lack (checked with bpf_core_field_exists).
 *
 * Using this instead of a generated vmlinux.h keeps the build reproducible
 * and lets one source tree cross-compile for x86_64 and aarch64.
 */
#ifndef ROS2EBPF_VMLINUX_MIN_H
#define ROS2EBPF_VMLINUX_MIN_H

/* Tell libbpf's bpf_tracing.h that kernel-style pt_regs field names are in use. */
#ifndef __VMLINUX_H__
#define __VMLINUX_H__
#endif

typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef signed char __s8;
typedef short __s16;
typedef int __s32;
typedef long long __s64;
typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __s64 s64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u64 __be64;
typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;
typedef __u32 __wsum;
typedef _Bool bool;
enum { false = 0, true = 1 };

/* Subset of uapi/linux/bpf.h used by the programs (values are ABI). */
enum bpf_map_type {
    BPF_MAP_TYPE_HASH = 1,
    BPF_MAP_TYPE_ARRAY = 2,
    BPF_MAP_TYPE_PERCPU_HASH = 5,
    BPF_MAP_TYPE_RINGBUF = 27,
};
enum {
    BPF_ANY = 0,
    BPF_NOEXIST = 1,
    BPF_EXIST = 2,
    BPF_F_LOCK = 4,
};

#ifndef NULL
#define NULL ((void *)0)
#endif

#pragma clang attribute push(__attribute__((preserve_access_index)), apply_to = record)

#if defined(__TARGET_ARCH_x86)
struct pt_regs {
    long unsigned int r15;
    long unsigned int r14;
    long unsigned int r13;
    long unsigned int r12;
    long unsigned int bp;
    long unsigned int bx;
    long unsigned int r11;
    long unsigned int r10;
    long unsigned int r9;
    long unsigned int r8;
    long unsigned int ax;
    long unsigned int cx;
    long unsigned int dx;
    long unsigned int si;
    long unsigned int di;
    long unsigned int orig_ax;
    long unsigned int ip;
    long unsigned int cs;
    long unsigned int flags;
    long unsigned int sp;
    long unsigned int ss;
};
#elif defined(__TARGET_ARCH_arm64)
struct user_pt_regs {
    __u64 regs[31];
    __u64 sp;
    __u64 pc;
    __u64 pstate;
};
struct pt_regs {
    union {
        struct user_pt_regs user_regs;
        struct {
            u64 regs[31];
            u64 sp;
            u64 pc;
            u64 pstate;
        };
    };
    u64 orig_x0;
    s32 syscallno;
    u32 unused2;
    u64 sdei_ttbr1;
    u64 pmr_save;
    u64 stackframe[2];
    u64 lockdep_hardirqs;
    u64 exit_rcu;
};
#else
#error "ros2ebpf: define __TARGET_ARCH_x86 or __TARGET_ARCH_arm64"
#endif

/* tp_btf/udp_fail_queue_rcv_skb: (int rc, struct sock *sk[, struct sk_buff *skb]) */
struct sock_common {
    short unsigned int skc_family;
    __u16 skc_num;              /* local port, host byte order */
};
struct sock {
    struct sock_common __sk_common;
};

/* tp_btf/sched_process_exit: (struct task_struct *p) */
struct task_struct {
    int pid;
    int tgid;
};

#pragma clang attribute pop

#endif /* ROS2EBPF_VMLINUX_MIN_H */
