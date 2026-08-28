/*
 * MMIX kernel image loader dispatch
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_KERNEL_LOADER_H
#define HW_MMIX_KERNEL_LOADER_H

#include "exec/hwaddr.h"
#include "qapi/error.h"
#include "ram-layout.h"

typedef enum MMIXKernelImageType {
    MMIX_KERNEL_IMAGE_RAW,
    MMIX_KERNEL_IMAGE_MMO,
    MMIX_KERNEL_IMAGE_ELF,
} MMIXKernelImageType;

typedef struct MMIXKernelLoadInfo {
    hwaddr entry;
    MMIXKernelImageType image_type;
    uint64_t boot_cpu_id;
    bool has_global_registers;
    uint8_t global_base;
    uint16_t global_count;
    uint64_t globals[256];
} MMIXKernelLoadInfo;

enum {
    MMIX_RAW_ENTRY = 0x100,
    MMIX_RAW_MIN_SIZE = MMIX_RAW_ENTRY + 4,
};

bool mmix_classify_kernel_image(const char *filename,
                                MMIXKernelImageType *type, Error **errp);

bool mmix_preflight_raw_kernel(const char *filename,
                               const MMIXPhysicalRAM *ram,
                               MMIXKernelLoadInfo *info,
                               uint64_t *image_size, Error **errp);

ssize_t mmix_commit_raw_kernel(const char *filename,
                               const MMIXPhysicalRAM *ram,
                               uint64_t expected_size, Error **errp);

ssize_t mmix_load_kernel(const char *filename,
                         const MMIXPhysicalRAM *ram,
                         MMIXKernelLoadInfo *info, Error **errp);

#endif
