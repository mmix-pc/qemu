/*
 * MMIX virt firmware boot-data placement and handoff
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "firmware.h"

#define FDT_HEADER_SIZE 40
#define FDT_PROP 3
#define FDT_MAX_SIZE (2U * 1024U * 1024U)

#define SHARED_PHASE_OFFSET 0
#define SHARED_ENTRY_OFFSET 8
#define SHARED_FDT_OFFSET 16
#define SHARED_CPU_COUNT_OFFSET 24
#define SHARED_READY_OFFSET 32
#define SHARED_PHASE_READY UINT64_C(1)
#define SHARED_PHASE_GO UINT64_C(2)

typedef struct FDTLayout {
    uint32_t source_reservations;
    uint32_t source_struct;
    uint32_t source_struct_size;
    uint32_t source_strings;
    uint32_t source_strings_size;
    uint32_t bootargs_property;
    uint32_t bootargs_property_size;
    uint32_t bootargs_name;
    uint32_t chosen_end;
    uint32_t reservation_count;
    uint32_t final_struct;
    uint32_t final_struct_size;
    uint32_t final_strings;
    uint32_t final_strings_size;
    uint32_t final_size;
} FDTLayout;

static void set_be32(uint8_t *data, uint32_t value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static bool range_valid(uint64_t base, uint64_t size, uint64_t limit)
{
    return base <= limit && size <= limit - base;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

static bool ranges_overlap(uint64_t left_base, uint64_t left_size,
                           uint64_t right_base, uint64_t right_size)
{
    return left_base < right_base + right_size &&
           right_base < left_base + left_size;
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

static uint32_t padded_property_size(uint32_t size)
{
    return 12 + ((size + 3) & ~3U);
}

static bool find_fdt_layout(const FirmwareBootInputs *inputs,
                            FDTLayout *layout, const char **error)
{
    uint32_t command_line_size = inputs->command_line.present ?
        inputs->command_line.size : 1;
    uint32_t inserted_size = inputs->initrd.present ? 40 : 0;
    uint32_t appended_strings = inputs->initrd.present ? 36 : 0;
    uint32_t added_reservations = inputs->initrd.present ? 3 : 2;

    layout->source_struct = inputs->fdt_struct;
    layout->source_struct_size = inputs->fdt_struct_size;
    layout->source_strings = inputs->fdt_strings;
    layout->source_strings_size = inputs->fdt_strings_size;
    layout->source_reservations = inputs->fdt_reservations;
    layout->reservation_count = inputs->fdt_reservation_count;
    layout->bootargs_property = inputs->fdt_bootargs_property;
    layout->bootargs_property_size = inputs->fdt_bootargs_property_size;
    layout->bootargs_name = inputs->fdt_bootargs_name;
    layout->chosen_end = inputs->fdt_chosen_end;
    if (layout->bootargs_property >= layout->chosen_end) {
        *error = "invalid FDT chosen node";
        return false;
    }
    layout->final_struct = FDT_HEADER_SIZE +
        (layout->reservation_count + added_reservations + 1) * 16;
    layout->final_struct_size = layout->source_struct_size -
        layout->bootargs_property_size +
        padded_property_size(command_line_size) + inserted_size;
    layout->final_strings = layout->final_struct +
                            layout->final_struct_size;
    layout->final_strings_size = layout->source_strings_size +
                                 appended_strings;
    layout->final_size = layout->final_strings +
                         layout->final_strings_size;
    if (layout->final_size > FDT_MAX_SIZE) {
        *error = "finalized FDT exceeds size limit";
        return false;
    }
    return true;
}

static bool collision(const FirmwareBootInputs *inputs,
                      const FirmwareBootPlan *plan, uint64_t base,
                      uint64_t size, bool include_shared,
                      bool include_initrd,
                      uint64_t *collision_base)
{
    uint64_t found = UINT64_C(0xffffffffffffffff);
    uint64_t other;
    uint32_t i;
    bool overlaps = false;

    for (i = 0; i < inputs->reserved_count; i++) {
        const FirmwareRange *range = &inputs->reserved[i];

        if (ranges_overlap(base, size, range->base, range->size)) {
            found = range->base < found ? range->base : found;
            overlaps = true;
        }
    }
    if (firmware_elf_ownership_overlap(inputs, base, size, &other)) {
        found = other < found ? other : found;
        overlaps = true;
    }
    if (include_shared &&
        ranges_overlap(base, size, plan->shared.base, plan->shared.size)) {
        found = plan->shared.base < found ? plan->shared.base : found;
        overlaps = true;
    }
    if (include_initrd &&
        ranges_overlap(base, size, plan->initrd.base,
                       align_up(plan->initrd.size, MMIX_RAM_ALIGNMENT))) {
        found = plan->initrd.base < found ? plan->initrd.base : found;
        overlaps = true;
    }
    if (overlaps) {
        *collision_base = found;
    }
    return overlaps;
}

static bool place_high(const FirmwareBootInputs *inputs,
                       const FirmwareBootPlan *plan, uint64_t content_size,
                       bool include_initrd, FirmwareRange *result)
{
    uint64_t allocation_size = align_up(content_size, MMIX_RAM_ALIGNMENT);
    uint64_t cursor = inputs->ram.size;

    while (cursor >= allocation_size) {
        uint64_t candidate = align_down(cursor - allocation_size,
                                        MMIX_RAM_ALIGNMENT);
        uint64_t blocker;

        if (!collision(inputs, plan, candidate, allocation_size, true,
                       include_initrd, &blocker)) {
            result->base = candidate;
            result->size = content_size;
            return true;
        }
        if (blocker >= cursor) {
            return false;
        }
        cursor = blocker;
    }
    return false;
}

static bool shared_range_valid(const FirmwareBootInputs *inputs,
                               const FirmwareBootPlan *plan)
{
    uint64_t ignored;

    return range_valid(plan->shared.base, plan->shared.size,
                       inputs->ram.size) &&
           !collision(inputs, plan, plan->shared.base, plan->shared.size,
                      false, false, &ignored);
}

bool firmware_prepare_boot(FirmwareBootInputs *inputs,
                           FirmwareBootPlan *plan, const char **error)
{
    FDTLayout layout = { 0 };

    plan->shared = (FirmwareRange) {
        .base = MMIX_FIRMWARE_SHARED_BASE,
        .size = MMIX_FIRMWARE_SHARED_SIZE,
    };
    if (!firmware_preflight_elf(inputs, error) ||
        !find_fdt_layout(inputs, &layout, error)) {
        return false;
    }
    plan->entry = inputs->kernel_entry;
    if (!shared_range_valid(inputs, plan)) {
        *error = "firmware shared state has no valid RAM range";
        return false;
    }
    if (inputs->initrd.present &&
        !place_high(inputs, plan, inputs->initrd.size, false,
                    &plan->initrd)) {
        *error = "could not place firmware initrd";
        return false;
    }
    if (!place_high(inputs, plan, layout.final_size,
                    inputs->initrd.present, &plan->fdt)) {
        *error = "could not place finalized firmware FDT";
        return false;
    }
    return true;
}

static void write8(uint64_t *address, uint8_t value)
{
    firmware_physical_write8((*address)++, value);
}

static void write_be32(uint64_t *address, uint32_t value)
{
    write8(address, value >> 24);
    write8(address, value >> 16);
    write8(address, value >> 8);
    write8(address, value);
}

static void write_be64(uint64_t *address, uint64_t value)
{
    write_be32(address, value >> 32);
    write_be32(address, value);
}

static void copy_file_range(const FirmwareFile *file, uint32_t offset,
                            uint32_t size, uint64_t *destination)
{
    uint32_t i;

    firmware_fw_cfg_select(file->selector);
    for (i = 0; i < offset; i++) {
        firmware_fw_cfg_read8();
    }
    for (i = 0; i < size; i++) {
        write8(destination, firmware_fw_cfg_read8());
    }
}

static void write_property_header(uint64_t *destination, uint32_t size,
                                  uint32_t name)
{
    write_be32(destination, FDT_PROP);
    write_be32(destination, size);
    write_be32(destination, name);
}

static void write_bootargs(const FirmwareBootInputs *inputs,
                           const FDTLayout *layout, uint64_t *destination)
{
    uint32_t size = inputs->command_line.present ?
        inputs->command_line.size : 1;
    uint32_t padding = (-size) & 3U;

    write_property_header(destination, size, layout->bootargs_name);
    if (inputs->command_line.present) {
        copy_file_range(&inputs->command_line, 0, size, destination);
    } else {
        write8(destination, 0);
    }
    while (padding--) {
        write8(destination, 0);
    }
}

static void write_initrd_properties(const FirmwareBootPlan *plan,
                                    const FDTLayout *layout,
                                    uint64_t *destination)
{
    uint32_t start_name = layout->source_strings_size;
    uint32_t end_name = start_name + 19;

    write_property_header(destination, 8, start_name);
    write_be64(destination, plan->initrd.base);
    write_property_header(destination, 8, end_name);
    write_be64(destination, plan->initrd.base + plan->initrd.size);
}

static void commit_fdt(const FirmwareBootInputs *inputs,
                       const FirmwareBootPlan *plan)
{
    static const char initrd_names[] =
        "linux,initrd-start\0linux,initrd-end\0";
    FDTLayout layout = { 0 };
    uint8_t header[FDT_HEADER_SIZE];
    uint64_t destination = plan->fdt.base;
    uint32_t position;
    uint32_t i;
    const char *error;

    find_fdt_layout(inputs, &layout, &error);
    read_file(&inputs->fdt, 0, header, sizeof(header));
    set_be32(header + 4, layout.final_size);
    set_be32(header + 8, layout.final_struct);
    set_be32(header + 12, layout.final_strings);
    set_be32(header + 16, FDT_HEADER_SIZE);
    set_be32(header + 32, layout.final_strings_size);
    set_be32(header + 36, layout.final_struct_size);
    for (i = 0; i < sizeof(header); i++) {
        write8(&destination, header[i]);
    }
    position = layout.source_reservations;
    for (i = 0; i < layout.reservation_count; i++) {
        copy_file_range(&inputs->fdt, position + i * 16,
                        16, &destination);
    }
    write_be64(&destination, plan->shared.base);
    write_be64(&destination, plan->shared.size);
    if (inputs->initrd.present) {
        write_be64(&destination, plan->initrd.base);
        write_be64(&destination, plan->initrd.size);
    }
    write_be64(&destination, plan->fdt.base);
    write_be64(&destination, plan->fdt.size);
    write_be64(&destination, 0);
    write_be64(&destination, 0);

    copy_file_range(&inputs->fdt, layout.source_struct,
                    layout.bootargs_property - layout.source_struct,
                    &destination);
    write_bootargs(inputs, &layout, &destination);
    position = layout.bootargs_property + layout.bootargs_property_size;
    copy_file_range(&inputs->fdt, position,
                    layout.chosen_end - position, &destination);
    if (inputs->initrd.present) {
        write_initrd_properties(plan, &layout, &destination);
    }
    copy_file_range(&inputs->fdt, layout.chosen_end,
                    layout.source_struct + layout.source_struct_size -
                    layout.chosen_end, &destination);
    copy_file_range(&inputs->fdt, layout.source_strings,
                    layout.source_strings_size, &destination);
    if (inputs->initrd.present) {
        for (i = 0; i < sizeof(initrd_names) - 1; i++) {
            write8(&destination, initrd_names[i]);
        }
    }
}

static void commit_initrd(const FirmwareBootInputs *inputs,
                          const FirmwareBootPlan *plan)
{
    uint64_t destination = plan->initrd.base;

    if (inputs->initrd.present) {
        copy_file_range(&inputs->initrd, 0, inputs->initrd.size,
                        &destination);
    }
}

static void write_shared64(uint64_t offset, uint64_t value)
{
    uint64_t address = MMIX_FIRMWARE_SHARED_BASE + offset;

    write_be64(&address, value);
}

void firmware_commit_boot_data(const FirmwareBootInputs *inputs,
                               const FirmwareBootPlan *plan)
{
    uint64_t i;

    firmware_commit_elf(inputs);
    commit_initrd(inputs, plan);
    commit_fdt(inputs, plan);
    for (i = 0; i < plan->shared.size; i++) {
        firmware_physical_write8(plan->shared.base + i, 0);
    }
    write_shared64(SHARED_ENTRY_OFFSET, plan->entry);
    write_shared64(SHARED_FDT_OFFSET, plan->fdt.base);
    write_shared64(SHARED_CPU_COUNT_OFFSET, inputs->cpu_count);
}

void firmware_release_cpus(const FirmwareBootInputs *inputs,
                           const FirmwareBootPlan *plan)
{
    uint32_t cpu;

    __asm__ volatile("SYNC 1" : : : "memory");
    write_shared64(SHARED_PHASE_OFFSET, SHARED_PHASE_READY);
    for (cpu = 1; cpu < inputs->cpu_count; cpu++) {
        while (firmware_physical_read8(plan->shared.base +
                                       SHARED_READY_OFFSET + cpu) == 0) {
        }
    }
    __asm__ volatile("SYNC 1" : : : "memory");
    write_shared64(SHARED_PHASE_OFFSET, SHARED_PHASE_GO);
    firmware_enter_kernel(0, plan->fdt.base, plan->entry);
}
