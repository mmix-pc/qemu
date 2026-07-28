/*
 * MMIX MMO loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "mmo-loader.h"

#define MMIX_MMO_ESCAPE 0x98
#define MMIX_MMO_LOP_PRE 0x09
#define MMIX_MMO_VERSION 1
#define MMIX_MMO_PREAMBLE_SIZE 4

/*
 * Keep raw -kernel loading unchanged while separating MMO input from raw
 * images. Executable MMO records are added after this detection boundary is
 * in place.
 */

static bool mmix_read_mmo_preamble(const char *filename,
                                   uint8_t preamble[MMIX_MMO_PREAMBLE_SIZE],
                                   size_t *size, Error **errp)
{
    FILE *file;

    file = fopen(filename, "rb");
    if (!file) {
        error_setg_file_open(errp, errno, filename);
        return false;
    }

    *size = fread(preamble, 1, MMIX_MMO_PREAMBLE_SIZE, file);
    if (ferror(file)) {
        error_setg(errp, "could not read MMIX kernel image '%s'", filename);
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

static bool mmix_kernel_is_mmo(const char *filename, Error **errp)
{
    uint8_t preamble[MMIX_MMO_PREAMBLE_SIZE] = { 0 };
    size_t size;

    if (!mmix_read_mmo_preamble(filename, preamble, &size, errp)) {
        return false;
    }

    if (size == 0) {
        return false;
    }
    if (preamble[0] != MMIX_MMO_ESCAPE) {
        return false;
    }
    if (size >= 2 && preamble[1] != MMIX_MMO_LOP_PRE) {
        return false;
    }
    if (size < MMIX_MMO_PREAMBLE_SIZE) {
        error_setg(errp, "truncated MMIX .mmo preamble in '%s'", filename);
        return false;
    }
    if (preamble[2] != MMIX_MMO_VERSION) {
        error_setg(errp, "unsupported MMIX .mmo preamble version %u in '%s'",
                   preamble[2], filename);
        return false;
    }

    return true;
}

ssize_t mmix_load_kernel(const char *filename, uint64_t ram_size,
                         hwaddr *entry, Error **errp)
{
    Error *local_err = NULL;

    if (!mmix_kernel_is_mmo(filename, &local_err)) {
        if (local_err) {
            error_propagate(errp, local_err);
            return -1;
        }

        *entry = 0;
        return load_image_targphys(filename, 0, ram_size, errp);
    }

    error_setg(errp, "MMIX .mmo object loading is not implemented yet");
    return -1;
}
