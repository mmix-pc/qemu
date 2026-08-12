/*
 * MMIX ELF kernel loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/loader.h"
#include "target/mmix/cpu.h"
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

static bool mmix_load_elf_segments(const char *filename, uint64_t *entry,
                                   uint64_t *lowaddr, uint64_t *highaddr,
                                   ssize_t *loaded_size, Error **errp)
{
    ssize_t size;

    size = load_elf_ram_sym(filename, NULL, NULL, NULL, entry, lowaddr,
                            highaddr, NULL, ELFDATA2MSB, EM_MMIX, 0, 0,
                            NULL, false, NULL);
    if (size < 0) {
        error_setg(errp, "could not load MMIX ELF kernel '%s': %s", filename,
                   load_elf_strerror(size));
        return false;
    }

    *loaded_size = size;
    return true;
}

static bool mmix_validate_elf_low_ram_range(const char *filename,
                                            uint64_t lowaddr,
                                            uint64_t highaddr,
                                            Error **errp)
{
    if (lowaddr < MMIX_POOL_SEGMENT_PHYS_BASE &&
        highaddr <= MMIX_POOL_SEGMENT_PHYS_BASE) {
        return true;
    }

    error_setg(errp, "MMIX ELF kernel '%s' loads outside Low RAM", filename);
    return false;
}

static void mmix_apply_elf_load_info(MMIXKernelLoadInfo *info, uint64_t entry)
{
    info->entry = entry;
    info->image_type = MMIX_KERNEL_IMAGE_ELF;
    info->boot_cpu_id = 0;
    info->has_mmo_globals = false;
}

ssize_t mmix_load_elf(const char *filename, uint64_t ram_size,
                      MMIXKernelLoadInfo *info, Error **errp)
{
    uint64_t entry;
    uint64_t lowaddr;
    uint64_t highaddr;
    ssize_t loaded_size;

    if (!mmix_validate_elf_header(filename, errp)) {
        return -1;
    }
    if (ram_size < MMIX_POOL_SEGMENT_PHYS_BASE) {
        error_setg(errp, "MMIX ELF Low RAM window does not fit in machine RAM");
        return -1;
    }
    if (!mmix_load_elf_segments(filename, &entry, &lowaddr, &highaddr,
                                &loaded_size, errp)) {
        return -1;
    }
    if (!mmix_validate_elf_low_ram_range(filename, lowaddr, highaddr, errp)) {
        return -1;
    }

    mmix_apply_elf_load_info(info, entry);
    return loaded_size;
}
