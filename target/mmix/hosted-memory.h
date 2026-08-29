/*
 * MMIX hosted logical-memory interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MMIX_HOSTED_MEMORY_H
#define TARGET_MMIX_HOSTED_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qemu/typedefs.h"

#define MMIX_HOSTED_TEXT_BASE  UINT64_C(0x0000000000000000)
#define MMIX_HOSTED_DATA_BASE  UINT64_C(0x2000000000000000)
#define MMIX_HOSTED_POOL_BASE  UINT64_C(0x4000000000000000)
#define MMIX_HOSTED_STACK_BASE UINT64_C(0x6000000000000000)
#define MMIX_HOSTED_LIMIT      UINT64_C(0x8000000000000000)

typedef struct MMIXHostedMemoryOps {
    bool (*read)(void *opaque, uint64_t address, void *buffer, size_t size,
                 size_t alignment, Error **errp);
    bool (*write)(void *opaque, uint64_t address, const void *buffer,
                  size_t size, size_t alignment, Error **errp);
    bool (*compare_exchange_octa)(void *opaque, uint64_t address,
                                  uint64_t expected, uint64_t desired,
                                  uint64_t *observed, Error **errp);
} MMIXHostedMemoryOps;

#endif
