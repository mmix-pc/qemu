/*
 * MMIX ELF kernel loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_ELF_LOADER_H
#define HW_MMIX_ELF_LOADER_H

#include "qapi/error.h"
#include "kernel-loader.h"

bool mmix_kernel_is_elf(const char *filename, Error **errp);

ssize_t mmix_load_elf(const char *filename,
                      const MMIXPhysicalRAMLayout *ram_layout,
                      MMIXKernelLoadInfo *info, Error **errp);

#endif
