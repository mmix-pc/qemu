/*
 * MMIX kernel image loader dispatch
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "elf-loader.h"
#include "kernel-loader.h"
#include "mmo-loader.h"

typedef enum MMIXKernelImageType {
    MMIX_KERNEL_IMAGE_MMO,
    MMIX_KERNEL_IMAGE_ELF,
    MMIX_KERNEL_IMAGE_RAW,
} MMIXKernelImageType;

static bool mmix_detect_kernel_image_type(const char *filename,
                                          MMIXKernelImageType *type,
                                          Error **errp)
{
    Error *local_err = NULL;

    if (mmix_kernel_is_mmo(filename, &local_err)) {
        *type = MMIX_KERNEL_IMAGE_MMO;
        return true;
    }
    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }

    if (mmix_kernel_is_elf(filename, &local_err)) {
        *type = MMIX_KERNEL_IMAGE_ELF;
        return true;
    }
    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }

    *type = MMIX_KERNEL_IMAGE_RAW;
    return true;
}

static ssize_t mmix_load_raw_kernel(const char *filename, uint64_t ram_size,
                                    Error **errp)
{
    return load_image_targphys(filename, 0, ram_size, errp);
}

ssize_t mmix_load_kernel(const char *filename, uint64_t ram_size,
                         MMIXKernelLoadInfo *info, Error **errp)
{
    MMIXKernelImageType type;

    *info = (MMIXKernelLoadInfo) {
        .entry = 0,
    };

    if (!mmix_detect_kernel_image_type(filename, &type, errp)) {
        return -1;
    }

    switch (type) {
    case MMIX_KERNEL_IMAGE_MMO:
        return mmix_load_mmo(filename, ram_size, info, errp);
    case MMIX_KERNEL_IMAGE_ELF:
        return mmix_load_elf(filename, ram_size, info, errp);
    case MMIX_KERNEL_IMAGE_RAW:
        return mmix_load_raw_kernel(filename, ram_size, errp);
    default:
        g_assert_not_reached();
    }
}
