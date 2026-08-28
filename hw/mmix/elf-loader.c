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
    int64_t file_size = get_image_size(filename, errp);

    if (file_size < 0) {
        return false;
    }
    if (file_size < sizeof(*ehdr)) {
        error_setg(errp, "truncated MMIX ELF header in '%s'", filename);
        return false;
    }
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
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
        be32_to_cpu(ehdr->e_version) != EV_CURRENT ||
        be16_to_cpu(ehdr->e_ehsize) != sizeof(*ehdr)) {
        error_setg(errp, "invalid MMIX ELF header in '%s'", filename);
        return false;
    }

    return true;
}

static bool mmix_elf_file_range(gsize file_size, uint64_t offset,
                                uint64_t size)
{
    return offset <= file_size && size <= file_size - offset;
}

static void mmix_elf_read_phdr(const uint8_t *data, uint64_t table_offset,
                               uint16_t entry_size, uint16_t index,
                               Elf64_Phdr *phdr)
{
    memcpy(phdr, data + table_offset + (uint64_t)index * entry_size,
           sizeof(*phdr));
}

static bool mmix_elf_alignment_valid(uint64_t alignment)
{
    return alignment <= 1 || (alignment & (alignment - 1)) == 0;
}

static bool mmix_elf_ranges_overlap(const MMIXKernelImageRange *left,
                                    const MMIXKernelImageRange *right)
{
    return left->address < right->address + right->size &&
           right->address < left->address + left->size;
}

static bool mmix_preflight_elf_segments(
    const char *filename, const Elf64_Ehdr *ehdr,
    const MMIXPhysicalRAM *ram, GArray **image_ranges, Error **errp)
{
    g_autoptr(GMappedFile) mapped = NULL;
    g_autoptr(GError) gerr = NULL;
    const uint8_t *data;
    gsize file_size;
    uint64_t table_offset = be64_to_cpu(ehdr->e_phoff);
    uint16_t entry_size = be16_to_cpu(ehdr->e_phentsize);
    uint16_t entry_count = be16_to_cpu(ehdr->e_phnum);
    g_autoptr(GArray) ranges = g_array_new(false, false,
                                           sizeof(MMIXKernelImageRange));
    uint64_t entry = be64_to_cpu(ehdr->e_entry);
    bool entry_valid = false;
    unsigned int i;

    if (entry_count == PN_XNUM) {
        error_setg(errp,
                   "unsupported MMIX ELF extended program header numbering "
                   "in '%s'", filename);
        return false;
    }
    if (entry_count == 0 || entry_size != sizeof(Elf64_Phdr)) {
        error_setg(errp, "invalid MMIX ELF program header table in '%s'",
                   filename);
        return false;
    }

    mapped = g_mapped_file_new(filename, false, &gerr);
    if (!mapped) {
        error_setg(errp, "could not read MMIX ELF program headers in '%s': %s",
                   filename, gerr->message);
        return false;
    }
    data = (const uint8_t *)g_mapped_file_get_contents(mapped);
    file_size = g_mapped_file_get_length(mapped);
    if (!mmix_elf_file_range(file_size, table_offset,
                             (uint64_t)entry_count * entry_size)) {
        error_setg(errp, "truncated MMIX ELF program header table in '%s'",
                   filename);
        return false;
    }

    for (i = 0; i < entry_count; i++) {
        Elf64_Phdr phdr;
        MMIXKernelImageRange range;
        uint32_t type;
        uint32_t flags;
        uint64_t address;
        uint64_t virtual_address;
        uint64_t offset;
        uint64_t file_size_part;
        uint64_t memory_size;
        uint64_t alignment;
        unsigned int j;

        mmix_elf_read_phdr(data, table_offset, entry_size, i, &phdr);
        type = be32_to_cpu(phdr.p_type);
        if (type == PT_INTERP) {
            error_setg(errp, "unsupported MMIX ELF interpreter segment %u "
                       "in '%s'", i, filename);
            return false;
        }
        if (type != PT_LOAD) {
            continue;
        }

        address = be64_to_cpu(phdr.p_paddr);
        virtual_address = be64_to_cpu(phdr.p_vaddr);
        offset = be64_to_cpu(phdr.p_offset);
        file_size_part = be64_to_cpu(phdr.p_filesz);
        memory_size = be64_to_cpu(phdr.p_memsz);
        alignment = be64_to_cpu(phdr.p_align);
        flags = be32_to_cpu(phdr.p_flags);
        if (file_size_part > memory_size ||
            !mmix_elf_file_range(file_size, offset, file_size_part)) {
            error_setg(errp, "invalid MMIX ELF PT_LOAD segment %u in '%s'",
                       i, filename);
            return false;
        }
        if (!mmix_elf_alignment_valid(alignment) ||
            (alignment > 1 &&
             offset % alignment != virtual_address % alignment)) {
            error_setg(errp, "invalid MMIX ELF PT_LOAD alignment in segment "
                       "%u of '%s'", i, filename);
            return false;
        }
        if (virtual_address != address) {
            error_setg(errp, "MMIX ELF PT_LOAD segment %u in '%s' does not "
                       "use identical virtual and physical addresses", i,
                       filename);
            return false;
        }
        if (memory_size == 0) {
            continue;
        }
        if (!mmix_physical_ram_contains(ram, address, memory_size)) {
            error_setg(errp, "MMIX ELF PT_LOAD segment %u in '%s' targets "
                       "non-RAM physical range at 0x%" HWADDR_PRIx
                       " with size 0x%" PRIx64, i, filename, address,
                       memory_size);
            return false;
        }

        range = (MMIXKernelImageRange) {
            .address = address,
            .size = memory_size,
            .index = i,
        };
        for (j = 0; j < ranges->len; j++) {
            const MMIXKernelImageRange *previous =
                &g_array_index(ranges, MMIXKernelImageRange, j);

            if (mmix_elf_ranges_overlap(previous, &range)) {
                error_setg(errp, "MMIX ELF PT_LOAD segments %u and %u in "
                           "'%s' overlap", previous->index, i, filename);
                return false;
            }
        }
        g_array_append_val(ranges, range);

        if ((flags & PF_X) && memory_size >= 4 && entry % 4 == 0 &&
            entry >= address &&
            entry - address <= memory_size - 4) {
            entry_valid = true;
        }
    }

    if (ranges->len == 0) {
        error_setg(errp, "MMIX ELF '%s' has no nonempty PT_LOAD segment",
                   filename);
        return false;
    }
    if (!entry_valid) {
        error_setg(errp, "MMIX ELF entry 0x%" PRIx64 " in '%s' is not a "
                   "complete aligned instruction in an executable PT_LOAD "
                   "segment", entry, filename);
        return false;
    }

    *image_ranges = g_steal_pointer(&ranges);
    return true;
}

bool mmix_preflight_elf_kernel(const char *filename,
                               const MMIXPhysicalRAM *ram,
                               MMIXKernelLoadInfo *info,
                               GArray **image_ranges, Error **errp)
{
    g_autoptr(GArray) ranges = NULL;
    Elf64_Ehdr ehdr;

    g_return_val_if_fail(image_ranges != NULL, false);
    if (!mmix_validate_elf_header(filename, &ehdr, errp) ||
        !mmix_preflight_elf_segments(filename, &ehdr, ram, &ranges, errp)) {
        return false;
    }

    *info = (MMIXKernelLoadInfo) {
        .entry = be64_to_cpu(ehdr.e_entry),
        .image_type = MMIX_KERNEL_IMAGE_ELF,
        .boot_cpu_id = 0,
    };
    g_clear_pointer(image_ranges, g_array_unref);
    *image_ranges = g_steal_pointer(&ranges);
    return true;
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

static void mmix_apply_elf_load_info(MMIXKernelLoadInfo *info, uint64_t entry)
{
    info->entry = entry;
    info->image_type = MMIX_KERNEL_IMAGE_ELF;
    info->boot_cpu_id = 0;
}

ssize_t mmix_commit_elf_kernel(const char *filename, Error **errp)
{
    ssize_t size;

    size = load_elf(filename, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    ELFDATA2MSB, EM_MMIX, 0, 0);
    if (size < 0) {
        error_setg(errp, "could not load MMIX ELF kernel '%s': %s", filename,
                   load_elf_strerror(size));
    }
    return size;
}

ssize_t mmix_load_elf(const char *filename,
                      const MMIXPhysicalRAM *ram,
                      MMIXKernelLoadInfo *info, Error **errp)
{
    g_autoptr(GArray) image_ranges = NULL;
    Elf64_Ehdr ehdr;
    uint64_t entry;
    uint64_t lowaddr;
    uint64_t highaddr;
    ssize_t loaded_size;

    if (!mmix_preflight_elf_kernel(filename, ram, info, &image_ranges,
                                   errp)) {
        return -1;
    }
    if (!mmix_validate_elf_header(filename, &ehdr, errp)) {
        return -1;
    }
    if (!mmix_load_elf_registers(filename, &ehdr, info, errp)) {
        return -1;
    }
    if (!mmix_load_elf_segments(filename, &entry, &lowaddr, &highaddr,
                                &loaded_size, errp)) {
        return -1;
    }
    mmix_apply_elf_load_info(info, entry);
    return loaded_size;
}
