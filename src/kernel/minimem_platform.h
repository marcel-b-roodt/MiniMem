/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_platform.h — Platform compatibility for kernel and userspace
 *
 * When MINIMEM_KERNEL is defined, uses kernel headers.
 * Otherwise, uses standard C library headers.
 */

#ifndef MINIMEM_PLATFORM_H
#define MINIMEM_PLATFORM_H

#ifdef MINIMEM_KERNEL

/* Kernel environment */
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/bug.h>

typedef size_t minimem_size_t;
typedef uint8_t minimem_u8;
typedef uint16_t minimem_u16;
typedef uint32_t minimem_u32;
typedef uint64_t minimem_u64;

#define minimem_memcpy	memcpy
#define minimem_memset	memset
#define minimem_memcmp	memcmp

#else

/* Userspace environment */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef size_t minimem_size_t;
typedef uint8_t minimem_u8;
typedef uint16_t minimem_u16;
typedef uint32_t minimem_u32;
typedef uint64_t minimem_u64;

#define minimem_memcpy	memcpy
#define minimem_memset	memset
#define minimem_memcmp	memcmp

#endif /* MINIMEM_KERNEL */

#endif /* MINIMEM_PLATFORM_H */