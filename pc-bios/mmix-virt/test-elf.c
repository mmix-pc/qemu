/*
 * MMIX virt firmware ELF loader tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "firmware.h"

#define ELF_HEADER_SIZE 64
#define ELF_PROGRAM_HEADER_SIZE 56
#define ELF_DATA_OFFSET 0x100
#define KERNEL_CAPACITY 1024
#define RAM_CAPACITY (64 * 1024)
#define KERNEL_SELECTOR 0x20

#define PT_LOAD 1
#define PT_INTERP 3
#define PF_R 4
#define PF_X 1

static uint8_t kernel[KERNEL_CAPACITY];
static uint8_t ram[RAM_CAPACITY];
static uint32_t kernel_size;
static uint32_t selected_position;
static uint16_t selected;
static uint64_t write_count;

static void set_be16(uint8_t *data, uint16_t value)
{
    data[0] = value >> 8;
    data[1] = value;
}

static void set_be32(uint8_t *data, uint32_t value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static void set_be64(uint8_t *data, uint64_t value)
{
    set_be32(data, value >> 32);
    set_be32(data + 4, value);
}

static uint8_t *program_header(uint16_t index)
{
    return kernel + ELF_HEADER_SIZE + index * ELF_PROGRAM_HEADER_SIZE;
}

static void set_program_header(uint16_t index, uint32_t type,
                               uint32_t flags, uint64_t offset,
                               uint64_t physical_address,
                               uint64_t file_size, uint64_t memory_size)
{
    uint8_t *header = program_header(index);

    set_be32(header, type);
    set_be32(header + 4, flags);
    set_be64(header + 8, offset);
    set_be64(header + 16, MMIX_DIRECT_ALIAS(physical_address));
    set_be64(header + 24, physical_address);
    set_be64(header + 32, file_size);
    set_be64(header + 40, memory_size);
    set_be64(header + 48, 1);
}

static void build_elf(uint64_t physical_address, uint64_t memory_size)
{
    static const uint8_t contents[] = { 0xde, 0xad, 0xbe, 0xef };
    static const uint8_t ident[] = {
        0x7f, 'E', 'L', 'F', 2, 2, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };

    memset(kernel, 0, sizeof(kernel));
    memset(ram, 0xa5, sizeof(ram));
    memcpy(kernel, ident, sizeof(ident));
    set_be16(kernel + 16, 2);
    set_be16(kernel + 18, 80);
    set_be32(kernel + 20, 1);
    set_be64(kernel + 24, MMIX_DIRECT_ALIAS(physical_address));
    set_be64(kernel + 32, ELF_HEADER_SIZE);
    set_be16(kernel + 52, ELF_HEADER_SIZE);
    set_be16(kernel + 54, ELF_PROGRAM_HEADER_SIZE);
    set_be16(kernel + 56, 1);
    set_program_header(0, PT_LOAD, PF_R | PF_X, ELF_DATA_OFFSET,
                       physical_address, sizeof(contents), memory_size);
    memcpy(kernel + ELF_DATA_OFFSET, contents, sizeof(contents));
    kernel_size = ELF_DATA_OFFSET + sizeof(contents);
    selected = 0;
    selected_position = 0;
    write_count = 0;
}

static FirmwareBootInputs inputs(void)
{
    FirmwareBootInputs result = {
        .kernel = {
            .size = kernel_size,
            .selector = KERNEL_SELECTOR,
            .present = true,
        },
        .ram = {
            .base = 0,
            .size = RAM_CAPACITY,
        },
    };

    return result;
}

void firmware_fw_cfg_select(uint16_t selector)
{
    assert(selector == KERNEL_SELECTOR);
    selected = selector;
    selected_position = 0;
}

uint8_t firmware_fw_cfg_read8(void)
{
    assert(selected == KERNEL_SELECTOR);
    assert(selected_position < kernel_size);
    return kernel[selected_position++];
}

void firmware_physical_write8(uint64_t address, uint8_t value)
{
    assert(address < sizeof(ram));
    ram[address] = value;
    write_count++;
}

static void expect_failure(FirmwareBootInputs *boot_inputs,
                           const char *expected)
{
    const char *error = NULL;

    assert(!firmware_load_elf(boot_inputs, &error));
    assert(error != NULL && strcmp(error, expected) == 0);
    assert(write_count == 0);
}

static void test_valid_elf(void)
{
    FirmwareBootInputs boot_inputs;

    build_elf(0x4000, 8);
    boot_inputs = inputs();
    assert(firmware_load_elf(&boot_inputs, NULL));
    assert(boot_inputs.kernel_entry == MMIX_DIRECT_ALIAS(0x4000));
    assert(write_count == 8);
    assert(memcmp(ram + 0x4000, kernel + ELF_DATA_OFFSET, 4) == 0);
    assert(ram[0x4004] == 0 && ram[0x4007] == 0);
    assert(ram[0x3fff] == 0xa5 && ram[0x4008] == 0xa5);
}

static void test_ram_endpoint(void)
{
    FirmwareBootInputs boot_inputs;

    build_elf(RAM_CAPACITY - 8, 8);
    boot_inputs = inputs();
    assert(firmware_load_elf(&boot_inputs, NULL));
    assert(write_count == 8);
    assert(ram[RAM_CAPACITY - 1] == 0);
}

static void test_header_failures(void)
{
    FirmwareBootInputs boot_inputs;

    build_elf(0x4000, 8);
    boot_inputs = inputs();
    boot_inputs.kernel.size = 20;
    expect_failure(&boot_inputs, "truncated MMIX ELF header");

    build_elf(0x4000, 8);
    kernel[4] = 1;
    boot_inputs = inputs();
    expect_failure(&boot_inputs, "unsupported MMIX ELF header");

    build_elf(0x4000, 8);
    set_be64(kernel + 32, kernel_size - 1);
    boot_inputs = inputs();
    expect_failure(&boot_inputs,
                   "truncated MMIX ELF program header table");
}

static void test_segment_failures(void)
{
    FirmwareBootInputs boot_inputs;

    build_elf(0x4000, 8);
    set_be64(program_header(0) + 32, 9);
    boot_inputs = inputs();
    expect_failure(&boot_inputs, "invalid MMIX ELF load segment");

    build_elf(0x4000, 8);
    set_be64(program_header(0) + 8, UINT64_C(0xfffffffffffffffe));
    boot_inputs = inputs();
    expect_failure(&boot_inputs, "invalid MMIX ELF load segment");

    build_elf(RAM_CAPACITY - 4, 8);
    boot_inputs = inputs();
    expect_failure(&boot_inputs,
                   "MMIX ELF segment targets reserved or non-RAM memory");

    build_elf(0x4000, 8);
    set_be64(program_header(0) + 16, 0x4000);
    boot_inputs = inputs();
    expect_failure(&boot_inputs,
                   "MMIX ELF segment is not a negative direct alias");

    build_elf(0x4000, 8);
    boot_inputs = inputs();
    boot_inputs.reserved[0] = (FirmwareRange) {
        .base = 0x4004,
        .size = 4,
    };
    boot_inputs.reserved_count = 1;
    expect_failure(&boot_inputs,
                   "MMIX ELF segment targets reserved or non-RAM memory");
}

static void test_overlapping_segments(void)
{
    FirmwareBootInputs boot_inputs;

    build_elf(0x4000, 8);
    set_be16(kernel + 56, 2);
    set_program_header(1, PT_LOAD, PF_R, ELF_DATA_OFFSET, 0x4004, 4, 8);
    boot_inputs = inputs();
    expect_failure(&boot_inputs, "overlapping MMIX ELF load segments");
}

static void test_entry_and_interpreter_failures(void)
{
    FirmwareBootInputs boot_inputs;

    build_elf(0x4000, 8);
    set_be64(kernel + 24, MMIX_DIRECT_ALIAS(0x4001));
    boot_inputs = inputs();
    expect_failure(&boot_inputs, "invalid MMIX ELF entry");

    build_elf(0x4000, 8);
    set_be32(program_header(0), PT_INTERP);
    boot_inputs = inputs();
    expect_failure(&boot_inputs, "unsupported MMIX ELF interpreter");
}

int main(void)
{
    test_valid_elf();
    test_ram_endpoint();
    test_header_failures();
    test_segment_failures();
    test_overlapping_segments();
    test_entry_and_interpreter_failures();
    puts("MMIX firmware ELF tests passed");
    return 0;
}
