/*
 * MMIX ELF kernel loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_ELF_LOADER_H
#define HW_MMIX_ELF_LOADER_H

#include "qapi/error.h"
#include "boot-payload.h"
#include "kernel-loader.h"

bool mmix_kernel_is_elf(const char *filename, Error **errp);

bool mmix_preflight_elf_kernel(const char *filename,
                               const MMIXPhysicalRAM *ram,
                               MMIXKernelLoadInfo *info,
                               GArray **image_ranges, Error **errp);

bool mmix_prepare_elf_kernel(const char *filename,
                             const MMIXPhysicalRAM *ram,
                             MMIXKernelLoadInfo *info,
                             GArray **image_ranges, GBytes **source,
                             Error **errp);

bool mmix_elf_add_boot_payload(GBytes *source, const char *filename,
                               MMIXBootPayload *payload, Error **errp);

ssize_t mmix_commit_elf_kernel(const char *filename, Error **errp);

ssize_t mmix_load_elf(const char *filename,
                      const MMIXPhysicalRAM *ram,
                      MMIXKernelLoadInfo *info, Error **errp);

#endif
