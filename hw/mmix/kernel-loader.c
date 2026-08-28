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

bool mmix_classify_kernel_image(const char *filename,
                                MMIXKernelImageType *type, Error **errp)
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

static ssize_t mmix_load_raw_kernel(const char *filename,
                                    const MMIXPhysicalRAM *ram,
                                    Error **errp)
{
    int64_t size = get_image_size(filename, errp);

    if (size < 0) {
        return -1;
    }
    if (!mmix_physical_ram_contains(ram, 0, size)) {
        error_setg(errp, "MMIX raw kernel '%s' does not fit in physical RAM",
                   filename);
        return -1;
    }

    return load_image_targphys(filename, 0, mmix_phys_range_size(ram), errp);
}

ssize_t mmix_load_kernel(const char *filename,
                         const MMIXPhysicalRAM *ram,
                         MMIXKernelLoadInfo *info, Error **errp)
{
    MMIXKernelImageType type;

    *info = (MMIXKernelLoadInfo) {
        .entry = 0,
        .image_type = MMIX_KERNEL_IMAGE_RAW,
    };

    if (!mmix_classify_kernel_image(filename, &type, errp)) {
        return -1;
    }

    switch (type) {
    case MMIX_KERNEL_IMAGE_MMO:
        return mmix_load_mmo(filename, ram, info, errp);
    case MMIX_KERNEL_IMAGE_ELF:
        return mmix_load_elf(filename, ram, info, errp);
    case MMIX_KERNEL_IMAGE_RAW:
        return mmix_load_raw_kernel(filename, ram, errp);
    default:
        g_assert_not_reached();
    }
}
