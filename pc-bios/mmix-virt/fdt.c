/*
 * MMIX virt firmware FDT validation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "firmware.h"

#define UINT32_C(value) value##U
#define FDT_MAGIC UINT32_C(0xd00dfeed)
#define FDT_HEADER_SIZE 40
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9
#define FDT_VERSION 17
#define FDT_MAX_DEPTH 16
#define FDT_NAME_SIZE 64
#define FDT_MAX_PROPERTY_DATA 32
#define MMIX_FW_CFG_PHYSICAL_BASE UINT64_C(0x0001000014000000)
#define MMIX_FW_CFG_SIZE UINT64_C(0x18)

typedef struct FDTHeader {
    uint32_t totalsize;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t off_reservations;
    uint32_t version;
    uint32_t last_compatible_version;
    uint32_t size_strings;
    uint32_t size_struct;
} FDTHeader;

typedef struct FDTNames {
    uint32_t address_cells;
    uint32_t size_cells;
    uint32_t device_type;
    uint32_t compatible;
    uint32_t reg;
    bool have_address_cells;
    bool have_size_cells;
    bool have_device_type;
    bool have_compatible;
    bool have_reg;
} FDTNames;

typedef struct FDTNode {
    bool is_cpus;
    bool is_reserved_memory;
    bool parent_is_cpus;
    bool parent_is_reserved_memory;
    bool is_cpu;
    bool is_memory;
    bool is_fw_cfg;
    uint8_t reg[FDT_MAX_PROPERTY_DATA];
    uint32_t reg_size;
} FDTNode;

typedef struct FDTState {
    FirmwareBootInputs *inputs;
    FDTHeader header;
    FDTNames names;
    FDTNode nodes[FDT_MAX_DEPTH];
    uint64_t cpu_ids;
    uint32_t cpu_count;
    uint32_t depth;
    uint32_t root_address_cells;
    uint32_t root_size_cells;
    bool have_ram;
    bool have_fw_cfg;
    bool have_end;
} FDTState;

typedef struct FDTReader {
    uint32_t position;
    uint32_t end;
} FDTReader;

static uint16_t read_be16(FDTReader *reader)
{
    uint16_t value;

    value = (uint16_t)firmware_fw_cfg_read8() << 8;
    value |= firmware_fw_cfg_read8();
    reader->position += 2;
    return value;
}

static uint32_t read_be32(FDTReader *reader)
{
    uint32_t value = read_be16(reader);

    return (value << 16) | read_be16(reader);
}

static uint64_t read_be64(FDTReader *reader)
{
    uint64_t value = read_be32(reader);

    return (value << 32) | read_be32(reader);
}

static bool range_valid(uint32_t start, uint32_t size, uint32_t limit)
{
    return start <= limit && size <= limit - start;
}

static bool seek_file(const FirmwareFile *file, FDTReader *reader,
                      uint32_t position, uint32_t size)
{
    uint32_t i;

    if (!range_valid(position, size, file->size)) {
        return false;
    }
    firmware_fw_cfg_select(file->selector);
    for (i = 0; i < position; i++) {
        firmware_fw_cfg_read8();
    }
    reader->position = position;
    reader->end = position + size;
    return true;
}

static bool reader_has(const FDTReader *reader, uint32_t size)
{
    return reader->position <= reader->end &&
           size <= reader->end - reader->position;
}

static bool skip(FDTReader *reader, uint32_t size)
{
    uint32_t i;

    if (!reader_has(reader, size)) {
        return false;
    }
    for (i = 0; i < size; i++) {
        firmware_fw_cfg_read8();
    }
    reader->position += size;
    return true;
}

static bool align_reader(FDTReader *reader)
{
    uint32_t aligned = (reader->position + 3) & ~3U;

    return aligned >= reader->position &&
           skip(reader, aligned - reader->position);
}

static bool read_header(FDTState *state)
{
    FDTReader reader;
    uint32_t boot_cpu;

    if (!seek_file(&state->inputs->fdt, &reader, 0, FDT_HEADER_SIZE) ||
        read_be32(&reader) != FDT_MAGIC) {
        return false;
    }
    state->header.totalsize = read_be32(&reader);
    state->header.off_struct = read_be32(&reader);
    state->header.off_strings = read_be32(&reader);
    state->header.off_reservations = read_be32(&reader);
    state->header.version = read_be32(&reader);
    state->header.last_compatible_version = read_be32(&reader);
    boot_cpu = read_be32(&reader);
    state->header.size_strings = read_be32(&reader);
    state->header.size_struct = read_be32(&reader);

    return state->header.totalsize == state->inputs->fdt.size &&
           state->header.version >= FDT_VERSION &&
           state->header.last_compatible_version <= FDT_VERSION &&
           boot_cpu < state->inputs->cpu_count &&
           (state->header.off_struct & 3) == 0 &&
           (state->header.off_reservations & 7) == 0 &&
           state->header.off_reservations >= FDT_HEADER_SIZE &&
           state->header.off_reservations < state->header.off_struct &&
           range_valid(state->header.off_struct,
                       state->header.size_struct,
                       state->header.totalsize) &&
           state->header.off_struct + state->header.size_struct <=
           state->header.off_strings &&
           range_valid(state->header.off_strings,
                       state->header.size_strings,
                       state->header.totalsize) &&
           state->header.off_strings + state->header.size_strings ==
           state->header.totalsize;
}

static bool add_reserved_range(FDTState *state, uint64_t base, uint64_t size)
{
    FirmwareBootInputs *inputs = state->inputs;

    if (size == 0) {
        return false;
    }
    if (inputs->reserved_count == MMIX_MAX_RESERVED_RANGES ||
        base + size < base) {
        return false;
    }
    inputs->reserved[inputs->reserved_count].base = base;
    inputs->reserved[inputs->reserved_count].size = size;
    inputs->reserved_count++;
    return true;
}

static bool read_reservations(FDTState *state)
{
    FDTReader reader;

    if (!seek_file(&state->inputs->fdt, &reader,
                   state->header.off_reservations,
                   state->header.off_struct -
                   state->header.off_reservations)) {
        return false;
    }
    while (reader_has(&reader, 16)) {
        uint64_t base = read_be64(&reader);
        uint64_t size = read_be64(&reader);

        if (base == 0 && size == 0) {
            return true;
        }
        if (!add_reserved_range(state, base, size)) {
            return false;
        }
    }
    return false;
}

static bool known_name(const char *name, uint32_t length,
                       const char *expected)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        if (expected[i] == '\0' || name[i] != expected[i]) {
            return false;
        }
    }
    return expected[length] == '\0';
}

static void record_name(FDTNames *names, const char *name, uint32_t length,
                        uint32_t offset)
{
    if (known_name(name, length, "#address-cells")) {
        names->address_cells = offset;
        names->have_address_cells = true;
    } else if (known_name(name, length, "#size-cells")) {
        names->size_cells = offset;
        names->have_size_cells = true;
    } else if (known_name(name, length, "device_type")) {
        names->device_type = offset;
        names->have_device_type = true;
    } else if (known_name(name, length, "compatible")) {
        names->compatible = offset;
        names->have_compatible = true;
    } else if (known_name(name, length, "reg")) {
        names->reg = offset;
        names->have_reg = true;
    }
}

static bool read_string_names(FDTState *state)
{
    FDTReader reader;
    char name[FDT_NAME_SIZE];
    uint32_t name_offset = 0;
    uint32_t length = 0;

    if (!seek_file(&state->inputs->fdt, &reader,
                   state->header.off_strings,
                   state->header.size_strings)) {
        return false;
    }
    while (reader.position < reader.end) {
        uint8_t byte = firmware_fw_cfg_read8();

        reader.position++;
        if (byte == '\0') {
            record_name(&state->names, name, length, name_offset);
            name_offset += length + 1;
            length = 0;
        } else if (length < sizeof(name)) {
            name[length++] = byte;
        } else {
            return false;
        }
    }
    return length == 0 && state->names.have_address_cells &&
           state->names.have_size_cells && state->names.have_device_type &&
           state->names.have_compatible && state->names.have_reg;
}

static bool read_node_name(FDTReader *reader, char name[FDT_NAME_SIZE])
{
    uint32_t length = 0;

    while (reader->position < reader->end) {
        uint8_t byte = firmware_fw_cfg_read8();

        reader->position++;
        if (byte == '\0') {
            name[length] = '\0';
            return align_reader(reader);
        }
        if (length + 1 >= FDT_NAME_SIZE) {
            return false;
        }
        name[length++] = byte;
    }
    return false;
}

static bool string_equal(const uint8_t *data, uint32_t size,
                         const char *expected)
{
    uint32_t i;

    for (i = 0; i < size; i++) {
        if (data[i] != expected[i]) {
            return false;
        }
        if (expected[i] == '\0') {
            return i + 1 == size;
        }
    }
    return false;
}

static bool compatible_contains(const uint8_t *data, uint32_t size,
                                const char *expected)
{
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t length = 0;

        while (offset + length < size && data[offset + length] != '\0') {
            length++;
        }
        if (offset + length == size) {
            return false;
        }
        if (string_equal(data + offset, length + 1, expected)) {
            return true;
        }
        offset += length + 1;
    }
    return false;
}

static uint64_t data_be64(const uint8_t *data)
{
    uint32_t i;
    uint64_t value = 0;

    for (i = 0; i < 8; i++) {
        value = (value << 8) | data[i];
    }
    return value;
}

static uint32_t data_be32(const uint8_t *data)
{
    uint32_t i;
    uint32_t value = 0;

    for (i = 0; i < 4; i++) {
        value = (value << 8) | data[i];
    }
    return value;
}

static bool handle_property(FDTState *state, uint32_t name_offset,
                            const uint8_t *data, uint32_t size)
{
    FDTNode *node = &state->nodes[state->depth - 1];

    if (name_offset == state->names.address_cells && state->depth == 1 &&
        size == 4) {
        state->root_address_cells = data_be32(data);
    } else if (name_offset == state->names.size_cells && state->depth == 1 &&
               size == 4) {
        state->root_size_cells = data_be32(data);
    } else if (name_offset == state->names.device_type) {
        node->is_cpu = string_equal(data, size, "cpu");
        node->is_memory = string_equal(data, size, "memory");
    } else if (name_offset == state->names.compatible) {
        node->is_fw_cfg = compatible_contains(data, size,
                                              "qemu,fw-cfg-mmio");
    } else if (name_offset == state->names.reg) {
        uint32_t i;

        if (size > sizeof(node->reg)) {
            return false;
        }
        for (i = 0; i < size; i++) {
            node->reg[i] = data[i];
        }
        node->reg_size = size;
    }
    return true;
}

static bool finish_node(FDTState *state)
{
    FDTNode *node = &state->nodes[state->depth - 1];

    if (node->is_memory) {
        if (state->have_ram || state->root_address_cells != 2 ||
            state->root_size_cells != 2 || node->reg_size != 16) {
            return false;
        }
        state->inputs->ram.base = data_be64(node->reg);
        state->inputs->ram.size = data_be64(node->reg + 8);
        if (state->inputs->ram.base != 0 || state->inputs->ram.size == 0) {
            return false;
        }
        state->have_ram = true;
    }
    if (node->is_cpu) {
        uint32_t cpu_id;

        if (!node->parent_is_cpus || node->reg_size != 4) {
            return false;
        }
        cpu_id = data_be32(node->reg);
        if (cpu_id >= MMIX_MAX_CPUS ||
            state->cpu_ids & (UINT64_C(1) << cpu_id)) {
            return false;
        }
        state->cpu_ids |= UINT64_C(1) << cpu_id;
        state->cpu_count++;
    }
    if (node->is_fw_cfg) {
        if (state->have_fw_cfg || node->reg_size != 16 ||
            data_be64(node->reg) != MMIX_FW_CFG_PHYSICAL_BASE ||
            data_be64(node->reg + 8) != MMIX_FW_CFG_SIZE) {
            return false;
        }
        state->have_fw_cfg = true;
    }
    if (node->parent_is_reserved_memory && node->reg_size != 0) {
        uint32_t offset;

        if (node->reg_size % 16 != 0) {
            return false;
        }
        for (offset = 0; offset < node->reg_size; offset += 16) {
            if (!add_reserved_range(state, data_be64(node->reg + offset),
                                    data_be64(node->reg + offset + 8))) {
                return false;
            }
        }
    }
    return true;
}

static bool begin_node(FDTState *state, FDTReader *reader)
{
    FDTNode *node;
    char name[FDT_NAME_SIZE];
    uint32_t i;

    if (state->depth == FDT_MAX_DEPTH || !read_node_name(reader, name)) {
        return false;
    }
    node = &state->nodes[state->depth];
    for (i = 0; i < sizeof(*node); i++) {
        ((uint8_t *)node)[i] = 0;
    }
    if (state->depth != 0) {
        FDTNode *parent = &state->nodes[state->depth - 1];

        node->parent_is_cpus = parent->is_cpus;
        node->parent_is_reserved_memory = parent->is_reserved_memory;
    }
    node->is_cpus = state->depth == 1 &&
                    string_equal((uint8_t *)name, 5, "cpus");
    node->is_reserved_memory = state->depth == 1 &&
        string_equal((uint8_t *)name, 16, "reserved-memory");
    state->depth++;
    return true;
}

static bool read_property(FDTState *state, FDTReader *reader)
{
    uint8_t data[FDT_MAX_PROPERTY_DATA];
    uint32_t size;
    uint32_t name_offset;
    uint32_t copied;
    uint32_t i;

    if (state->depth == 0 || !reader_has(reader, 8)) {
        return false;
    }
    size = read_be32(reader);
    name_offset = read_be32(reader);
    if (name_offset >= state->header.size_strings ||
        !reader_has(reader, size)) {
        return false;
    }
    copied = size < sizeof(data) ? size : sizeof(data);
    for (i = 0; i < copied; i++) {
        data[i] = firmware_fw_cfg_read8();
    }
    reader->position += copied;
    if (!skip(reader, size - copied) || !align_reader(reader)) {
        return false;
    }
    if (size > sizeof(data) &&
        (name_offset == state->names.device_type ||
         name_offset == state->names.reg)) {
        return false;
    }
    return handle_property(state, name_offset, data, copied);
}

static bool read_structure(FDTState *state)
{
    FDTReader reader;

    if (!seek_file(&state->inputs->fdt, &reader, state->header.off_struct,
                   state->header.size_struct)) {
        return false;
    }
    while (reader_has(&reader, 4)) {
        uint32_t token = read_be32(&reader);

        switch (token) {
        case FDT_BEGIN_NODE:
            if (!begin_node(state, &reader)) {
                return false;
            }
            break;
        case FDT_END_NODE:
            if (state->depth == 0 || !finish_node(state)) {
                return false;
            }
            state->depth--;
            break;
        case FDT_PROP:
            if (!read_property(state, &reader)) {
                return false;
            }
            break;
        case FDT_NOP:
            break;
        case FDT_END:
            if (state->depth != 0) {
                return false;
            }
            state->have_end = true;
            return true;
        default:
            return false;
        }
    }
    return false;
}

static bool cpu_ids_are_contiguous(const FDTState *state)
{
    uint64_t expected;

    if (state->cpu_count == MMIX_MAX_CPUS) {
        expected = ~UINT64_C(0);
    } else {
        expected = (UINT64_C(1) << state->cpu_count) - 1;
    }
    return state->cpu_ids == expected;
}

static bool reserved_ranges_fit_ram(const FDTState *state)
{
    const FirmwareBootInputs *inputs = state->inputs;
    uint32_t i;

    for (i = 0; i < inputs->reserved_count; i++) {
        const FirmwareRange *range = &inputs->reserved[i];

        if (range->base < inputs->ram.base ||
            range->base - inputs->ram.base > inputs->ram.size ||
            range->size > inputs->ram.size -
                          (range->base - inputs->ram.base)) {
            return false;
        }
    }
    return true;
}

bool firmware_validate_fdt(FirmwareBootInputs *inputs, const char **error)
{
    FDTState state = { .inputs = inputs };

    if (!read_header(&state) || !read_reservations(&state) ||
        !read_string_names(&state) || !read_structure(&state) ||
        !state.have_end || !state.have_ram || !state.have_fw_cfg ||
        state.cpu_count != inputs->cpu_count ||
        !cpu_ids_are_contiguous(&state) || !reserved_ranges_fit_ram(&state)) {
        *error = "invalid MMIX platform FDT";
        return false;
    }
    return true;
}
