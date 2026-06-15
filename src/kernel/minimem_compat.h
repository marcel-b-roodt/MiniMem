/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * minimem_compat.h — Kernel/userspace compatibility shim
 *
 * When building the kernel module, this header is force-included
 * via -include in CFLAGS. It provides kernel-compatible definitions
 * for the types and functions that the algorithm library expects from
 * libc headers.
 *
 * When building the userspace library, this header is not included
 * and the standard libc headers are used normally.
 */

#ifndef MINIMEM_COMPAT_H
#define MINIMEM_COMPAT_H

#ifdef MINIMEM_KERNEL

/*
 * Prevent libc headers from being included. The algorithm files
 * include <string.h>, <stddef.h>, <stdint.h>, <stdbool.h>.
 * We define guard macros so their include guards are already set,
 * then provide the kernel equivalents.
 */

/* Provide standard integer types from kernel headers */
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;

/* Provide standard bool/type definitions */
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/bug.h>

#ifndef bool
#define bool _Bool
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

/* Prevent libc string.h from being included */
#define _STRING_H
#define __STRING_H

/* Prevent libc stddef.h from being included */
#define _STDDEF_H
#define __STDDEF_H

/* Prevent libc stdint.h from being included */
#define _STDINT_H
#define __STDINT_H

/* Prevent libc stdbool.h from being included */
#define _STDBOOL_H
#define __STDBOOL_H

/* Map allocation functions */
#define malloc(x) kmalloc(x, GFP_KERNEL)
#define calloc(n, s) kcalloc(n, s, GFP_KERNEL)
#define free(x) kfree(x)

/* string functions are provided by <linux/string.h> */

#endif /* MINIMEM_KERNEL */

#endif /* MINIMEM_COMPAT_H */