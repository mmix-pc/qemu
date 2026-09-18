/*
 * MMIX virt firmware discovery tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "firmware.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FDT_CAPACITY 2048
#define DIRECTORY_CAPACITY 512
#define MOCK_FILE_CAPACITY 8

#define FW_CFG_SIGNATURE 0x00
#define FW_CFG_ID 0x01
#define FW_CFG_NB_CPUS 0x05
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_FILE_FIRST 0x20

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_END 9

typedef struct Buffer {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
} Buffer;

typedef struct MockFile {
    const char *name;
    const uint8_t *data;
    uint32_t size;
    uint16_t selector;
} MockFile;

static uint8_t fdt_data[FDT_CAPACITY];
static uint8_t directory_data[DIRECTORY_CAPACITY];
static const uint8_t signature[] = "QEMU";
static const uint8_t version[] = { 3 };
static const uint8_t cpu_count[] = { 1, 0 };
static const uint8_t kernel[] = { 0x7f, 'E', 'L', 'F' };
static const uint8_t initrd[] = { 1, 2, 3, 4 };
static const uint8_t command_line[] = "console=ttyS0";
static MockFile files[MOCK_FILE_CAPACITY];
static uint32_t file_count;
static uint16_t selected;
static uint32_t selected_position;

static void put8(Buffer *buffer, uint8_t value)
{
    assert(buffer->size < buffer->capacity);
    buffer->data[buffer->size++] = value;
}

static void put_be32(Buffer *buffer, uint32_t value)
{
    put8(buffer, value >> 24);
    put8(buffer, value >> 16);
    put8(buffer, value >> 8);
    put8(buffer, value);
}

static void put_be64(Buffer *buffer, uint64_t value)
{
    put_be32(buffer, value >> 32);
    put_be32(buffer, value);
}

static void set_be32(uint8_t *data, uint32_t value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static void align4(Buffer *buffer)
{
    while (buffer->size & 3) {
        put8(buffer, 0);
    }
}

static void put_bytes(Buffer *buffer, const void *data, uint32_t size)
{
    const uint8_t *bytes = data;
    uint32_t i;

    for (i = 0; i < size; i++) {
        put8(buffer, bytes[i]);
    }
}

static void put_node(Buffer *buffer, const char *name)
{
    put_be32(buffer, FDT_BEGIN_NODE);
    put_bytes(buffer, name, strlen(name) + 1);
    align4(buffer);
}

static void put_property(Buffer *buffer, uint32_t name_offset,
                         const void *data, uint32_t size)
{
    put_be32(buffer, FDT_PROP);
    put_be32(buffer, size);
    put_be32(buffer, name_offset);
    put_bytes(buffer, data, size);
    align4(buffer);
}

static uint32_t append_string(Buffer *buffer, const char *value)
{
    uint32_t offset = buffer->size;

    put_bytes(buffer, value, strlen(value) + 1);
    return offset;
}

static uint32_t build_fdt(void)
{
    static const char cpu[] = "cpu";
    static const char memory[] = "memory";
    static const char fw_cfg[] = "qemu,fw-cfg-mmio";
    uint8_t strings_data[256];
    uint8_t structure_data[1024];
    Buffer strings = { strings_data, 0, sizeof(strings_data) };
    Buffer structure = { structure_data, 0, sizeof(structure_data) };
    Buffer fdt = { fdt_data, 0, sizeof(fdt_data) };
    uint32_t address_cells = append_string(&strings, "#address-cells");
    uint32_t size_cells = append_string(&strings, "#size-cells");
    uint32_t device_type = append_string(&strings, "device_type");
    uint32_t compatible = append_string(&strings, "compatible");
    uint32_t reg = append_string(&strings, "reg");
    uint32_t bootargs = append_string(&strings, "bootargs");
    uint8_t one[4] = { 0, 0, 0, 1 };
    uint8_t two[4] = { 0, 0, 0, 2 };
    uint8_t zero[4] = { 0, 0, 0, 0 };
    uint8_t ram_reg[16] = { 0 };
    uint8_t fw_cfg_reg[16] = { 0 };
    uint32_t off_reservations;
    uint32_t off_structure;
    uint32_t off_strings;

    set_be32(ram_reg + 12, 128U * 1024U * 1024U);
    set_be32(fw_cfg_reg, 0x00010000);
    set_be32(fw_cfg_reg + 4, 0x14000000);
    set_be32(fw_cfg_reg + 12, 0x18);

    put_node(&structure, "");
    put_property(&structure, address_cells, two, sizeof(two));
    put_property(&structure, size_cells, two, sizeof(two));
    put_node(&structure, "chosen");
    put_property(&structure, bootargs, command_line, sizeof(command_line));
    put_be32(&structure, FDT_END_NODE);
    put_node(&structure, "memory@0");
    put_property(&structure, device_type, memory, sizeof(memory));
    put_property(&structure, reg, ram_reg, sizeof(ram_reg));
    put_be32(&structure, FDT_END_NODE);
    put_node(&structure, "cpus");
    put_property(&structure, address_cells, one, sizeof(one));
    put_property(&structure, size_cells, zero, sizeof(zero));
    put_node(&structure, "cpu@0");
    put_property(&structure, device_type, cpu, sizeof(cpu));
    put_property(&structure, reg, zero, sizeof(zero));
    put_be32(&structure, FDT_END_NODE);
    put_be32(&structure, FDT_END_NODE);
    put_node(&structure, "fw-cfg@1000014000000");
    put_property(&structure, compatible, fw_cfg, sizeof(fw_cfg));
    put_property(&structure, reg, fw_cfg_reg, sizeof(fw_cfg_reg));
    put_be32(&structure, FDT_END_NODE);
    put_be32(&structure, FDT_END_NODE);
    put_be32(&structure, FDT_END);

    while (fdt.size < 40) {
        put8(&fdt, 0);
    }
    off_reservations = fdt.size;
    put_be64(&fdt, 0x1000);
    put_be64(&fdt, 0x1000);
    put_be64(&fdt, 0);
    put_be64(&fdt, 0);
    off_structure = fdt.size;
    put_bytes(&fdt, structure.data, structure.size);
    off_strings = fdt.size;
    put_bytes(&fdt, strings.data, strings.size);

    set_be32(fdt.data, FDT_MAGIC);
    set_be32(fdt.data + 4, fdt.size);
    set_be32(fdt.data + 8, off_structure);
    set_be32(fdt.data + 12, off_strings);
    set_be32(fdt.data + 16, off_reservations);
    set_be32(fdt.data + 20, 17);
    set_be32(fdt.data + 24, 16);
    set_be32(fdt.data + 28, 0);
    set_be32(fdt.data + 32, strings.size);
    set_be32(fdt.data + 36, structure.size);
    return fdt.size;
}

static void add_file(const char *name, const uint8_t *data, uint32_t size)
{
    assert(file_count < ARRAY_SIZE(files));
    files[file_count].name = name;
    files[file_count].data = data;
    files[file_count].size = size;
    files[file_count].selector = FW_CFG_FILE_FIRST + file_count;
    file_count++;
}

static void build_directory(void)
{
    Buffer directory = {
        directory_data, 0, sizeof(directory_data),
    };
    uint32_t i;

    put_be32(&directory, file_count);
    for (i = 0; i < file_count; i++) {
        uint32_t name_size = strlen(files[i].name);
        uint32_t padding;

        assert(name_size < 56);
        put_be32(&directory, files[i].size);
        put8(&directory, files[i].selector >> 8);
        put8(&directory, files[i].selector);
        put8(&directory, 0);
        put8(&directory, 0);
        put_bytes(&directory, files[i].name, name_size);
        padding = 56 - name_size;
        while (padding--) {
            put8(&directory, 0);
        }
    }
}

static void reset_files(bool full)
{
    file_count = 0;
    add_file("etc/fdt", fdt_data, build_fdt());
    if (full) {
        add_file("opt/mmix/cmdline", command_line, sizeof(command_line));
        add_file("opt/mmix/initrd", initrd, sizeof(initrd));
        add_file("opt/mmix/kernel", kernel, sizeof(kernel));
    }
    build_directory();
}

void firmware_fw_cfg_select(uint16_t selector)
{
    selected = selector;
    selected_position = 0;
}

static const uint8_t *selected_data(uint32_t *size)
{
    uint32_t i;

    switch (selected) {
    case FW_CFG_SIGNATURE:
        *size = sizeof(signature) - 1;
        return signature;
    case FW_CFG_ID:
        *size = sizeof(version);
        return version;
    case FW_CFG_NB_CPUS:
        *size = sizeof(cpu_count);
        return cpu_count;
    case FW_CFG_FILE_DIR:
        *size = sizeof(directory_data);
        return directory_data;
    default:
        for (i = 0; i < file_count; i++) {
            if (files[i].selector == selected) {
                *size = files[i].size;
                return files[i].data;
            }
        }
        *size = 0;
        return NULL;
    }
}

uint8_t firmware_fw_cfg_read8(void)
{
    const uint8_t *data;
    uint32_t size;

    data = selected_data(&size);
    assert(data != NULL && selected_position < size);
    return data[selected_position++];
}

static void expect_success(bool kernel_expected)
{
    FirmwareBootInputs inputs;
    const char *error = NULL;

    assert(firmware_discover_boot_inputs(&inputs, &error));
    assert(error == NULL);
    assert(inputs.kernel.present == kernel_expected);
    assert(inputs.cpu_count == 1);
    assert(inputs.ram.base == 0);
    assert(inputs.ram.size == 128U * 1024U * 1024U);
    assert(inputs.reserved_count == 1);
}

static void expect_failure(const char *expected)
{
    FirmwareBootInputs inputs;
    const char *error = NULL;

    assert(!firmware_discover_boot_inputs(&inputs, &error));
    assert(error != NULL && strcmp(error, expected) == 0);
}

static void test_valid_inputs(void)
{
    reset_files(false);
    expect_success(false);
    reset_files(true);
    expect_success(true);
}

static void test_missing_fdt(void)
{
    file_count = 0;
    add_file("opt/mmix/kernel", kernel, sizeof(kernel));
    build_directory();
    expect_failure("missing or invalid FDT");
}

static void test_fdt_size_bounds(void)
{
    reset_files(false);
    files[0].size = 20;
    build_directory();
    expect_failure("missing or invalid FDT");

    reset_files(false);
    files[0].size = 2U * 1024U * 1024U + 1;
    build_directory();
    expect_failure("missing or invalid FDT");
}

static void test_malformed_fdt(void)
{
    reset_files(false);
    fdt_data[0] ^= 0xff;
    expect_failure("invalid MMIX platform FDT");
}

static void test_duplicate_file(void)
{
    reset_files(false);
    add_file("etc/fdt", fdt_data, files[0].size);
    build_directory();
    expect_failure("invalid fw_cfg directory entry");
}

static void test_invalid_optional_files(void)
{
    static const uint8_t unterminated[] = "unterminated";

    reset_files(false);
    add_file("opt/mmix/kernel", kernel, 0);
    build_directory();
    expect_failure("invalid kernel size");

    reset_files(false);
    add_file("opt/mmix/cmdline", unterminated,
             sizeof(unterminated) - 1);
    build_directory();
    expect_failure("unterminated command line");
}

int main(void)
{
    test_valid_inputs();
    test_missing_fdt();
    test_fdt_size_bounds();
    test_malformed_fdt();
    test_duplicate_file();
    test_invalid_optional_files();
    puts("MMIX firmware discovery tests passed");
    return 0;
}
