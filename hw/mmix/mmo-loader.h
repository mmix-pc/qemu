/*
 * MMIX MMO loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_MMO_LOADER_H
#define HW_MMIX_MMO_LOADER_H

#include "exec/hwaddr.h"
#include "qapi/error.h"

typedef struct MMIXKernelLoadInfo {
    hwaddr entry;
    bool has_mmo_globals;
    uint8_t global_base;
    uint64_t globals[256];
} MMIXKernelLoadInfo;

ssize_t mmix_load_kernel(const char *filename, uint64_t ram_size,
                         MMIXKernelLoadInfo *info, Error **errp);

#endif
