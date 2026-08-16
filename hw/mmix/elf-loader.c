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

#define MMIX_ELF_REG_CONTENTS_LIMIT (MMIX_REGS - 1)
#define MMIX_ELF_SHN_XINDEX UINT16_MAX

static const char mmix_elf_reg_contents_name[] = ".MMIX.reg_contents";

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

static bool mmix_validate_elf_header(const char *filename, Elf64_Ehdr *ehdr,
                                     Error **errp)
{
    if (!load_elf_hdr(filename, ehdr, NULL, errp)) {
        return false;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        error_setg(errp, "unsupported MMIX ELF class %u in '%s'",
                   ehdr->e_ident[EI_CLASS], filename);
        return false;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2MSB) {
        error_setg(errp, "unsupported MMIX ELF data encoding %u in '%s'",
                   ehdr->e_ident[EI_DATA], filename);
        return false;
    }
    if (be16_to_cpu(ehdr->e_type) != ET_EXEC) {
        error_setg(errp, "unsupported MMIX ELF type %u in '%s'",
                   be16_to_cpu(ehdr->e_type), filename);
        return false;
    }
    if (be16_to_cpu(ehdr->e_machine) != EM_MMIX) {
        error_setg(errp, "unsupported MMIX ELF machine %u in '%s'",
                   be16_to_cpu(ehdr->e_machine), filename);
        return false;
    }

    return true;
}

static bool mmix_elf_file_range(gsize file_size, uint64_t offset,
                                uint64_t size)
{
    return offset <= file_size && size <= file_size - offset;
}

static void mmix_elf_read_shdr(const uint8_t *data, uint64_t table_offset,
                               uint16_t entry_size, uint16_t index,
                               Elf64_Shdr *shdr)
{
    memcpy(shdr, data + table_offset + (uint64_t)index * entry_size,
           sizeof(*shdr));
}

static bool mmix_elf_section_name(const char *filename, const uint8_t *names,
                                  uint64_t names_size, uint32_t name_offset,
                                  const char **name, Error **errp)
{
    const uint8_t *end;

    if (name_offset >= names_size) {
        error_setg(errp, "invalid MMIX ELF section name offset in '%s'",
                   filename);
        return false;
    }

    end = memchr(names + name_offset, '\0', names_size - name_offset);
    if (!end) {
        error_setg(errp, "unterminated MMIX ELF section name in '%s'",
                   filename);
        return false;
    }

    *name = (const char *)names + name_offset;
    return true;
}

static bool mmix_elf_decode_registers(const char *filename,
                                      const uint8_t *data, gsize file_size,
                                      const Elf64_Shdr *shdr,
                                      MMIXKernelLoadInfo *info, Error **errp)
{
    uint64_t address = be64_to_cpu(shdr->sh_addr);
    uint64_t offset = be64_to_cpu(shdr->sh_offset);
    uint64_t size = be64_to_cpu(shdr->sh_size);
    uint64_t base;
    uint64_t count;
    unsigned reg;

    if (be32_to_cpu(shdr->sh_type) != SHT_PROGBITS) {
        error_setg(errp, "invalid %s section type in '%s'",
                   mmix_elf_reg_contents_name, filename);
        return false;
    }
    if (address % MMIX_OCTA_SIZE || size % MMIX_OCTA_SIZE) {
        error_setg(errp, "unaligned %s section in '%s'",
                   mmix_elf_reg_contents_name, filename);
        return false;
    }

    base = address / MMIX_OCTA_SIZE;
    count = size / MMIX_OCTA_SIZE;
    if (base < MMIX_GLOBAL_REG_MIN ||
        base > MMIX_ELF_REG_CONTENTS_LIMIT ||
        count > MMIX_ELF_REG_CONTENTS_LIMIT - base) {
        error_setg(errp, "invalid %s register range in '%s'",
                   mmix_elf_reg_contents_name, filename);
        return false;
    }
    if (!mmix_elf_file_range(file_size, offset, size)) {
        error_setg(errp, "truncated %s section in '%s'",
                   mmix_elf_reg_contents_name, filename);
        return false;
    }

    for (reg = base; reg < base + count; reg++) {
        info->globals[reg] = ldq_be_p(data + offset +
                                     (uint64_t)(reg - base) *
                                     MMIX_OCTA_SIZE);
    }
    info->has_global_registers = true;
    info->global_base = base;
    info->global_count = count;
    return true;
}

static bool mmix_load_elf_registers(const char *filename,
                                    const Elf64_Ehdr *ehdr,
                                    MMIXKernelLoadInfo *info, Error **errp)
{
    g_autoptr(GMappedFile) mapped = NULL;
    g_autoptr(GError) gerr = NULL;
    const uint8_t *data;
    const uint8_t *names;
    gsize file_size;
    uint64_t table_offset = be64_to_cpu(ehdr->e_shoff);
    uint16_t entry_size = be16_to_cpu(ehdr->e_shentsize);
    uint16_t section_count = be16_to_cpu(ehdr->e_shnum);
    uint16_t names_index = be16_to_cpu(ehdr->e_shstrndx);
    uint64_t names_offset;
    uint64_t names_size;
    Elf64_Shdr names_shdr;
    Elf64_Shdr shdr;
    MMIXKernelLoadInfo registers = { 0 };
    const char *name;
    bool found = false;
    unsigned i;

    if (section_count == 0) {
        if (table_offset != 0) {
            error_setg(errp,
                       "unsupported MMIX ELF extended section numbering in "
                       "'%s'", filename);
            return false;
        }
        if (names_index != SHN_UNDEF) {
            error_setg(errp, "invalid MMIX ELF section-name index in '%s'",
                       filename);
            return false;
        }
        return true;
    }
    if (entry_size != sizeof(Elf64_Shdr)) {
        error_setg(errp, "invalid MMIX ELF section table in '%s'", filename);
        return false;
    }
    if (names_index == MMIX_ELF_SHN_XINDEX) {
        error_setg(errp,
                   "unsupported MMIX ELF extended section-name index in '%s'",
                   filename);
        return false;
    }
    if (names_index != SHN_UNDEF && names_index >= section_count) {
        error_setg(errp, "invalid MMIX ELF section-name index in '%s'",
                   filename);
        return false;
    }

    mapped = g_mapped_file_new(filename, false, &gerr);
    if (!mapped) {
        error_setg(errp, "could not read MMIX ELF sections in '%s': %s",
                   filename, gerr->message);
        return false;
    }
    data = (const uint8_t *)g_mapped_file_get_contents(mapped);
    file_size = g_mapped_file_get_length(mapped);
    if (!mmix_elf_file_range(file_size, table_offset,
                             (uint64_t)section_count * entry_size)) {
        error_setg(errp, "truncated MMIX ELF section table in '%s'", filename);
        return false;
    }

    mmix_elf_read_shdr(data, table_offset, entry_size, 0, &shdr);
    if (be32_to_cpu(shdr.sh_type) != SHT_NULL) {
        error_setg(errp, "invalid MMIX ELF null section in '%s'", filename);
        return false;
    }
    if (names_index == SHN_UNDEF) {
        return true;
    }

    mmix_elf_read_shdr(data, table_offset, entry_size, names_index,
                       &names_shdr);
    names_offset = be64_to_cpu(names_shdr.sh_offset);
    names_size = be64_to_cpu(names_shdr.sh_size);
    if (be32_to_cpu(names_shdr.sh_type) != SHT_STRTAB ||
        !mmix_elf_file_range(file_size, names_offset, names_size)) {
        error_setg(errp, "invalid MMIX ELF section-name table in '%s'",
                   filename);
        return false;
    }
    names = data + names_offset;

    for (i = 1; i < section_count; i++) {
        mmix_elf_read_shdr(data, table_offset, entry_size, i, &shdr);
        if (!mmix_elf_section_name(filename, names, names_size,
                                   be32_to_cpu(shdr.sh_name), &name, errp)) {
            return false;
        }
        if (strcmp(name, mmix_elf_reg_contents_name) != 0) {
            continue;
        }
        if (found) {
            error_setg(errp, "duplicate %s section in '%s'",
                       mmix_elf_reg_contents_name, filename);
            return false;
        }
        if (!mmix_elf_decode_registers(filename, data, file_size, &shdr,
                                       &registers, errp)) {
            return false;
        }
        found = true;
    }

    if (found) {
        info->has_global_registers = true;
        info->global_base = registers.global_base;
        info->global_count = registers.global_count;
        memcpy(info->globals, registers.globals, sizeof(info->globals));
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
}

ssize_t mmix_load_elf(const char *filename, uint64_t ram_size,
                      MMIXKernelLoadInfo *info, Error **errp)
{
    Elf64_Ehdr ehdr;
    uint64_t entry;
    uint64_t lowaddr;
    uint64_t highaddr;
    ssize_t loaded_size;

    if (!mmix_validate_elf_header(filename, &ehdr, errp)) {
        return -1;
    }
    if (ram_size < MMIX_POOL_SEGMENT_PHYS_BASE) {
        error_setg(errp, "MMIX ELF Low RAM window does not fit in machine RAM");
        return -1;
    }
    if (!mmix_load_elf_registers(filename, &ehdr, info, errp)) {
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
