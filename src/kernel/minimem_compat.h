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
 *
 * Note: minimem.h itself is kernel-aware (uses MINIMEM_KERNEL guard).
 * This header handles the remaining libc includes that algorithm
 * source files may use directly.
 */

#ifndef MINIMEM_COMPAT_H
#define MINIMEM_COMPAT_H

#ifdef MINIMEM_KERNEL

/* Prevent libc headers from being included by algorithm files.
 * Algorithm files may include <string.h>, <stddef.h>, <stdint.h>,
 * <stdbool.h>, <stdlib.h> directly. Define their guards so the
 * compiler skips them (kernel headers already provide equivalents).
 */
#define _STDBOOL_H
#define __STDBOOL_H
#define __bool_true_false_are_defined 1

#define _STDINT_H
#define __STDINT_H
#define _STDINT_H__

#define _STDDEF_H
#define __STDDEF_H

#define _STRING_H
#define __STRING_H

#define _STDLIB_H
#define __STDLIB_H

/* Map allocation functions to kernel equivalents */
#define malloc(x) kmalloc(x, GFP_KERNEL)
#define calloc(n, s) kcalloc(n, s, GFP_KERNEL)
#define free(x) kfree(x)

#endif /* MINIMEM_KERNEL */

#endif /* MINIMEM_COMPAT_H */