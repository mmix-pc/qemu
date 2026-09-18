/*
 * MMIX virt firmware ELF loader
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "firmware.h"

#define ELF_HEADER_SIZE 64
#define ELF_PROGRAM_HEADER_SIZE 56

#define ELFCLASS64 2
#define ELFDATA2MSB 2
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_MMIX 80
#define PN_XNUM 0xffff
#define PT_LOAD 1
#define PT_INTERP 3
#define PF_X 1

typedef struct ELFSegment {
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
} ELFSegment;

typedef struct ELFImage {
    uint64_t entry;
    uint64_t table_offset;
    uint16_t entry_count;
} ELFImage;

static uint16_t data_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t data_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t data_be64(const uint8_t *data)
{
    return ((uint64_t)data_be32(data) << 32) | data_be32(data + 4);
}

static bool range_valid(uint64_t start, uint64_t size, uint64_t limit)
{
    return start <= limit && size <= limit - start;
}

static bool read_file(const FirmwareFile *file, uint64_t offset,
                      uint8_t *data, uint32_t size)
{
    uint64_t i;

    if (!range_valid(offset, size, file->size)) {
        return false;
    }
    firmware_fw_cfg_select(file->selector);
    for (i = 0; i < offset; i++) {
        firmware_fw_cfg_read8();
    }
    for (i = 0; i < size; i++) {
        data[i] = firmware_fw_cfg_read8();
    }
    return true;
}

static bool ranges_overlap(uint64_t left_base, uint64_t left_size,
                           uint64_t right_base, uint64_t right_size)
{
    return left_base < right_base + right_size &&
           right_base < left_base + left_size;
}

static bool segment_range_valid(const FirmwareBootInputs *inputs,
                                const ELFSegment *segment)
{
    uint32_t i;

    if (!range_valid(segment->physical_address, segment->memory_size,
                     inputs->ram.size)) {
        return false;
    }
    for (i = 0; i < inputs->reserved_count; i++) {
        const FirmwareRange *reserved = &inputs->reserved[i];

        if (ranges_overlap(segment->physical_address, segment->memory_size,
                           reserved->base, reserved->size)) {
            return false;
        }
    }
    return true;
}

static bool validate_header(const FirmwareFile *file, uint8_t *header,
                            const char **error)
{
    static const uint8_t magic[] = { 0x7f, 'E', 'L', 'F' };
    uint32_t i;

    if (!read_file(file, 0, header, ELF_HEADER_SIZE)) {
        *error = "truncated MMIX ELF header";
        return false;
    }
    for (i = 0; i < sizeof(magic); i++) {
        if (header[i] != magic[i]) {
            *error = "invalid MMIX ELF magic";
            return false;
        }
    }
    if (header[4] != ELFCLASS64 || header[5] != ELFDATA2MSB ||
        header[6] != EV_CURRENT || data_be16(header + 16) != ET_EXEC ||
        data_be16(header + 18) != EM_MMIX ||
        data_be32(header + 20) != EV_CURRENT ||
        data_be16(header + 52) != ELF_HEADER_SIZE) {
        *error = "unsupported MMIX ELF header";
        return false;
    }
    return true;
}

static bool alignment_valid(uint64_t alignment)
{
    return alignment <= 1 || (alignment & (alignment - 1)) == 0;
}

static void decode_segment(const uint8_t *header, ELFSegment *segment)
{
    *segment = (ELFSegment) {
        .offset = data_be64(header + 8),
        .virtual_address = data_be64(header + 16),
        .physical_address = data_be64(header + 24),
        .file_size = data_be64(header + 32),
        .memory_size = data_be64(header + 40),
    };
}

static bool validate_segment(const FirmwareBootInputs *inputs,
                             const uint8_t *header, ELFSegment *segment,
                             const char **error)
{
    uint64_t alignment = data_be64(header + 48);
    uint64_t direct_alias;

    decode_segment(header, segment);
    if (segment->file_size > segment->memory_size ||
        !range_valid(segment->offset, segment->file_size,
                     inputs->kernel.size)) {
        *error = "invalid MMIX ELF load segment";
        return false;
    }
    if (!alignment_valid(alignment) ||
        (alignment > 1 && segment->offset % alignment !=
                          segment->virtual_address % alignment)) {
        *error = "invalid MMIX ELF segment alignment";
        return false;
    }
    direct_alias = MMIX_DIRECT_ALIAS(segment->physical_address);
    if (segment->physical_address & UINT64_C(0x8000000000000000) ||
        segment->virtual_address != direct_alias) {
        *error = "MMIX ELF segment is not a negative direct alias";
        return false;
    }
    if (segment->memory_size == 0) {
        return true;
    }
    if (!segment_range_valid(inputs, segment)) {
        *error = "MMIX ELF segment targets reserved or non-RAM memory";
        return false;
    }
    return true;
}

static bool read_program_header(const FirmwareBootInputs *inputs,
                                const ELFImage *image, uint16_t index,
                                uint8_t *header)
{
    return read_file(&inputs->kernel,
                     image->table_offset +
                     (uint64_t)index * ELF_PROGRAM_HEADER_SIZE,
                     header, ELF_PROGRAM_HEADER_SIZE);
}

static bool preflight_elf(const FirmwareBootInputs *inputs, ELFImage *image,
                          const char **error)
{
    uint8_t header[ELF_HEADER_SIZE];
    uint8_t program_header[ELF_PROGRAM_HEADER_SIZE];
    uint64_t table_size;
    uint16_t entry_size;
    uint16_t i;
    uint16_t load_count = 0;
    bool entry_valid = false;

    if (!validate_header(&inputs->kernel, header, error)) {
        return false;
    }
    image->entry = data_be64(header + 24);
    image->table_offset = data_be64(header + 32);
    entry_size = data_be16(header + 54);
    image->entry_count = data_be16(header + 56);
    table_size = (uint64_t)entry_size * image->entry_count;
    if (image->entry_count == 0 || image->entry_count == PN_XNUM ||
        entry_size != ELF_PROGRAM_HEADER_SIZE) {
        *error = "invalid MMIX ELF program header table";
        return false;
    }
    if (!range_valid(image->table_offset, table_size,
                     inputs->kernel.size)) {
        *error = "truncated MMIX ELF program header table";
        return false;
    }
    for (i = 0; i < image->entry_count; i++) {
        ELFSegment segment;
        uint32_t type;
        uint32_t flags;
        uint16_t j;

        if (!read_program_header(inputs, image, i, program_header)) {
            *error = "truncated MMIX ELF program header table";
            return false;
        }
        type = data_be32(program_header);
        flags = data_be32(program_header + 4);
        if (type == PT_INTERP) {
            *error = "unsupported MMIX ELF interpreter";
            return false;
        }
        if (type != PT_LOAD) {
            continue;
        }
        if (!validate_segment(inputs, program_header, &segment, error)) {
            return false;
        }
        if (segment.memory_size == 0) {
            continue;
        }
        load_count++;
        for (j = 0; j < i; j++) {
            uint8_t previous_header[ELF_PROGRAM_HEADER_SIZE];
            ELFSegment previous;

            if (!read_program_header(inputs, image, j, previous_header) ||
                data_be32(previous_header) != PT_LOAD) {
                continue;
            }
            decode_segment(previous_header, &previous);
            if (previous.memory_size != 0 &&
                ranges_overlap(segment.physical_address,
                               segment.memory_size,
                               previous.physical_address,
                               previous.memory_size)) {
                *error = "overlapping MMIX ELF load segments";
                return false;
            }
        }
        if ((flags & PF_X) && segment.memory_size >= 4 &&
            image->entry % 4 == 0 &&
            image->entry >= segment.virtual_address &&
            image->entry - segment.virtual_address <=
            segment.memory_size - 4) {
            entry_valid = true;
        }
    }
    if (load_count == 0) {
        *error = "MMIX ELF has no nonempty load segment";
        return false;
    }
    if (!entry_valid) {
        *error = "invalid MMIX ELF entry";
        return false;
    }
    return true;
}

static void commit_segment(const FirmwareFile *file,
                           const ELFSegment *segment)
{
    uint64_t i;

    firmware_fw_cfg_select(file->selector);
    for (i = 0; i < segment->offset; i++) {
        firmware_fw_cfg_read8();
    }
    for (i = 0; i < segment->file_size; i++) {
        firmware_physical_write8(segment->physical_address + i,
                                 firmware_fw_cfg_read8());
    }
    for (; i < segment->memory_size; i++) {
        firmware_physical_write8(segment->physical_address + i, 0);
    }
}

bool firmware_preflight_elf(FirmwareBootInputs *inputs, const char **error)
{
    ELFImage image = { 0 };

    if (!inputs->kernel.present || !preflight_elf(inputs, &image, error)) {
        return false;
    }
    inputs->kernel_entry = image.entry;
    return true;
}

bool firmware_elf_ownership_overlap(const FirmwareBootInputs *inputs,
                                    uint64_t base, uint64_t size,
                                    uint64_t *collision_base)
{
    ELFImage image = { 0 };
    uint8_t program_header[ELF_PROGRAM_HEADER_SIZE];
    uint64_t end = base + size;
    uint16_t i;
    const char *error;

    if (!preflight_elf(inputs, &image, &error)) {
        return true;
    }
    for (i = 0; i < image.entry_count; i++) {
        ELFSegment segment;
        uint64_t segment_base;
        uint64_t segment_end;

        read_program_header(inputs, &image, i, program_header);
        if (data_be32(program_header) != PT_LOAD) {
            continue;
        }
        decode_segment(program_header, &segment);
        if (segment.memory_size == 0) {
            continue;
        }
        segment_base = segment.physical_address & ~(MMIX_RAM_ALIGNMENT - 1);
        segment_end = (segment.physical_address + segment.memory_size +
                       MMIX_RAM_ALIGNMENT - 1) & ~(MMIX_RAM_ALIGNMENT - 1);
        if (base < segment_end && segment_base < end) {
            if (collision_base != 0) {
                *collision_base = segment_base;
            }
            return true;
        }
    }
    return false;
}

void firmware_commit_elf(const FirmwareBootInputs *inputs)
{
    ELFImage image = { 0 };
    uint8_t program_header[ELF_PROGRAM_HEADER_SIZE];
    uint16_t i;
    const char *error;

    preflight_elf(inputs, &image, &error);
    for (i = 0; i < image.entry_count; i++) {
        ELFSegment segment;

        read_program_header(inputs, &image, i, program_header);
        if (data_be32(program_header) != PT_LOAD) {
            continue;
        }
        decode_segment(program_header, &segment);
        if (segment.memory_size != 0) {
            commit_segment(&inputs->kernel, &segment);
        }
    }
}

bool firmware_load_elf(FirmwareBootInputs *inputs, const char **error)
{
    if (!firmware_preflight_elf(inputs, error)) {
        return false;
    }
    firmware_commit_elf(inputs);
    return true;
}
