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

bool mmix_preflight_raw_kernel(const char *filename,
                               const MMIXPhysicalRAM *ram,
                               MMIXKernelLoadInfo *info,
                               uint64_t *image_size, Error **errp)
{
    int64_t size = get_image_size(filename, errp);

    if (size < 0) {
        return false;
    }
    if (size < MMIX_RAW_MIN_SIZE) {
        error_setg(errp,
                   "MMIX raw kernel '%s' is too small: got 0x%" PRIx64
                   ", need at least 0x%x bytes",
                   filename, (uint64_t)size, MMIX_RAW_MIN_SIZE);
        return false;
    }
    if (!mmix_physical_ram_contains(ram, 0, size)) {
        error_setg(errp, "MMIX raw kernel '%s' does not fit in physical RAM",
                   filename);
        return false;
    }

    *info = (MMIXKernelLoadInfo) {
        .entry = MMIX_RAW_ENTRY,
        .image_type = MMIX_KERNEL_IMAGE_RAW,
        .boot_cpu_id = 0,
    };
    *image_size = size;
    return true;
}

ssize_t mmix_commit_raw_kernel(const char *filename,
                               const MMIXPhysicalRAM *ram,
                               uint64_t expected_size, Error **errp)
{
    ssize_t size = load_image_targphys(filename, 0,
                                       mmix_phys_range_size(ram), errp);

    if (size >= 0 && (uint64_t)size != expected_size) {
        error_setg(errp,
                   "MMIX raw kernel '%s' changed size after preflight: "
                   "expected 0x%" PRIx64 ", got 0x%" PRIx64,
                   filename, expected_size, (uint64_t)size);
        return -1;
    }
    return size;
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
        error_setg(errp, "MMIX MMO kernel commit requires hosted sparse "
                   "memory integration");
        return -1;
    case MMIX_KERNEL_IMAGE_ELF:
        return mmix_load_elf(filename, ram, info, errp);
    case MMIX_KERNEL_IMAGE_RAW: {
        uint64_t image_size;

        if (!mmix_preflight_raw_kernel(filename, ram, info, &image_size,
                                       errp)) {
            return -1;
        }
        return mmix_commit_raw_kernel(filename, ram, image_size, errp);
    }
    default:
        g_assert_not_reached();
    }
}
