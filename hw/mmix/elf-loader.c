/*
 * MMIX ELF kernel loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/loader.h"
#include "elf-loader.h"

#ifndef EM_MMIX
#define EM_MMIX 80
#endif

bool mmix_kernel_is_elf(const char *filename, Error **errp)
{
    uint8_t e_ident[EI_NIDENT] = { 0 };
    size_t size;
    FILE *file;

    file = fopen(filename, "rb");
    if (!file) {
        error_setg_file_open(errp, errno, filename);
        return false;
    }

    size = fread(e_ident, 1, sizeof(e_ident), file);
    if (ferror(file)) {
        error_setg(errp, "could not read MMIX kernel image '%s'", filename);
        fclose(file);
        return false;
    }

    fclose(file);

    if (size < SELFMAG) {
        return false;
    }
    if (memcmp(e_ident, ELFMAG, SELFMAG) != 0) {
        return false;
    }
    if (size < sizeof(e_ident)) {
        error_setg(errp, "truncated MMIX ELF header in '%s'", filename);
        return false;
    }

    return true;
}

static bool mmix_validate_elf_header(const char *filename, Error **errp)
{
    Elf64_Ehdr ehdr;

    if (!load_elf_hdr(filename, &ehdr, NULL, errp)) {
        return false;
    }

    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        error_setg(errp, "unsupported MMIX ELF class %u in '%s'",
                   ehdr.e_ident[EI_CLASS], filename);
        return false;
    }
    if (ehdr.e_ident[EI_DATA] != ELFDATA2MSB) {
        error_setg(errp, "unsupported MMIX ELF data encoding %u in '%s'",
                   ehdr.e_ident[EI_DATA], filename);
        return false;
    }
    if (be16_to_cpu(ehdr.e_type) != ET_EXEC) {
        error_setg(errp, "unsupported MMIX ELF type %u in '%s'",
                   be16_to_cpu(ehdr.e_type), filename);
        return false;
    }
    if (be16_to_cpu(ehdr.e_machine) != EM_MMIX) {
        error_setg(errp, "unsupported MMIX ELF machine %u in '%s'",
                   be16_to_cpu(ehdr.e_machine), filename);
        return false;
    }

    return true;
}

ssize_t mmix_load_elf(const char *filename, uint64_t ram_size,
                      MMIXKernelLoadInfo *info, Error **errp)
{
    (void)ram_size;
    (void)info;

    if (!mmix_validate_elf_header(filename, errp)) {
        return -1;
    }

    error_setg(errp, "MMIX ELF kernel loading is not implemented yet");
    return -1;
}
