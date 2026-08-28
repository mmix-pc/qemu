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

ssize_t mmix_load_kernel(const char *filename,
                         const MMIXPhysicalRAM *ram,
                         MMIXKernelLoadInfo *info, Error **errp);

#endif
