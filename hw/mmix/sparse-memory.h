/*
 * MMIX hosted sparse logical memory
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_SPARSE_MEMORY_H
#define HW_MMIX_SPARSE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "qapi/error.h"
#include "target/mmix/hosted-memory.h"

enum {
    MMIX_SPARSE_PAGE_SIZE = 0x2000,
};

#define MMIX_SPARSE_TEXT_BASE  MMIX_HOSTED_TEXT_BASE
#define MMIX_SPARSE_DATA_BASE  MMIX_HOSTED_DATA_BASE
#define MMIX_SPARSE_POOL_BASE  MMIX_HOSTED_POOL_BASE
#define MMIX_SPARSE_STACK_BASE MMIX_HOSTED_STACK_BASE
#define MMIX_SPARSE_LIMIT      MMIX_HOSTED_LIMIT

typedef enum MMIXSparseSegment {
    MMIX_SPARSE_SEGMENT_TEXT,
    MMIX_SPARSE_SEGMENT_DATA,
    MMIX_SPARSE_SEGMENT_POOL,
    MMIX_SPARSE_SEGMENT_STACK,
    MMIX_SPARSE_SEGMENT_COUNT,
} MMIXSparseSegment;

typedef struct MMIXSparseMemory MMIXSparseMemory;

MMIXSparseMemory *mmix_sparse_memory_new(uint64_t budget, Error **errp);
void mmix_sparse_memory_free(MMIXSparseMemory *memory);
void mmix_sparse_memory_clear(MMIXSparseMemory *memory);

bool mmix_sparse_memory_validate_range(uint64_t address, size_t size,
                                       size_t alignment,
                                       MMIXSparseSegment *segment,
                                       Error **errp);
bool mmix_sparse_memory_read(MMIXSparseMemory *memory, uint64_t address,
                             void *buffer, size_t size, size_t alignment,
                             Error **errp);
bool mmix_sparse_memory_write(MMIXSparseMemory *memory, uint64_t address,
                              const void *buffer, size_t size,
                              size_t alignment, Error **errp);

uint64_t mmix_sparse_memory_budget(const MMIXSparseMemory *memory);
uint64_t mmix_sparse_memory_materialized_bytes(
    const MMIXSparseMemory *memory);
uint64_t mmix_sparse_memory_materialized_pages(
    const MMIXSparseMemory *memory);

#endif
