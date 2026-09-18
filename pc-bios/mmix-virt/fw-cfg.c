/*
 * MMIX virt firmware fw_cfg discovery
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "firmware.h"

#define MMIX_FW_CFG_BASE MMIX_DIRECT_ALIAS(UINT64_C(0x0001000014000000))
#define MMIX_FW_CFG_DATA 0
#define MMIX_FW_CFG_SELECTOR 8

#define FW_CFG_SIGNATURE 0x00
#define FW_CFG_ID 0x01
#define FW_CFG_NB_CPUS 0x05
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_VERSION 0x01
#define FW_CFG_VERSION_DMA 0x02
#define FW_CFG_FILE_FIRST 0x20
#define FW_CFG_MAX_FILES 64
#define FW_CFG_FILE_NAME_SIZE 56
#define MMIX_FDT_MAX_SIZE (2U * 1024U * 1024U)
#define MMIX_COMMAND_LINE_MAX_SIZE 4096U

typedef struct FWCfgDirectoryEntry {
    uint32_t size;
    uint16_t selector;
    uint16_t reserved;
    char name[FW_CFG_FILE_NAME_SIZE];
} FWCfgDirectoryEntry;

static uint16_t read_be16(void)
{
    uint16_t value = firmware_fw_cfg_read8();

    return (value << 8) | firmware_fw_cfg_read8();
}

static uint32_t read_be32(void)
{
    uint32_t value = read_be16();

    return (value << 16) | read_be16();
}

static void clear_inputs(FirmwareBootInputs *inputs)
{
    uint8_t *bytes = (uint8_t *)inputs;
    uint32_t i;

    for (i = 0; i < sizeof(*inputs); i++) {
        bytes[i] = 0;
    }
}

static bool read_signature(void)
{
    static const char signature[] = "QEMU";
    uint32_t i;

    firmware_fw_cfg_select(FW_CFG_SIGNATURE);
    for (i = 0; i < sizeof(signature) - 1; i++) {
        if (firmware_fw_cfg_read8() != signature[i]) {
            return false;
        }
    }
    return true;
}

static bool name_equal(const char name[FW_CFG_FILE_NAME_SIZE],
                       const char *expected)
{
    uint32_t i;

    for (i = 0; i < FW_CFG_FILE_NAME_SIZE; i++) {
        if (name[i] != expected[i]) {
            return false;
        }
        if (expected[i] == '\0') {
            return true;
        }
    }
    return false;
}

static int name_compare(const char left[FW_CFG_FILE_NAME_SIZE],
                        const char right[FW_CFG_FILE_NAME_SIZE])
{
    uint32_t i;

    for (i = 0; i < FW_CFG_FILE_NAME_SIZE; i++) {
        if ((uint8_t)left[i] != (uint8_t)right[i]) {
            return (uint8_t)left[i] < (uint8_t)right[i] ? -1 : 1;
        }
        if (left[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

static bool read_directory_entry(FWCfgDirectoryEntry *entry)
{
    uint32_t i;
    bool terminated = false;

    entry->size = read_be32();
    entry->selector = read_be16();
    entry->reserved = read_be16();
    for (i = 0; i < FW_CFG_FILE_NAME_SIZE; i++) {
        entry->name[i] = firmware_fw_cfg_read8();
        if (entry->name[i] == '\0') {
            terminated = true;
        }
    }
    return terminated && entry->name[0] != '\0' && entry->reserved == 0;
}

static bool record_file(FirmwareFile *file,
                        const FWCfgDirectoryEntry *entry)
{
    if (file->present) {
        return false;
    }
    file->size = entry->size;
    file->selector = entry->selector;
    file->present = true;
    return true;
}

static bool record_known_file(FirmwareBootInputs *inputs,
                              const FWCfgDirectoryEntry *entry)
{
    if (name_equal(entry->name, "etc/fdt")) {
        return record_file(&inputs->fdt, entry);
    }
    if (name_equal(entry->name, "opt/mmix/kernel")) {
        return record_file(&inputs->kernel, entry);
    }
    if (name_equal(entry->name, "opt/mmix/initrd")) {
        return record_file(&inputs->initrd, entry);
    }
    if (name_equal(entry->name, "opt/mmix/cmdline")) {
        return record_file(&inputs->command_line, entry);
    }
    return true;
}

#ifndef MMIX_FIRMWARE_TEST
void firmware_fw_cfg_select(uint16_t selector)
{
    /* Volatile preserves each guest MMIO transaction. */
    *(volatile uint16_t *)(MMIX_FW_CFG_BASE + MMIX_FW_CFG_SELECTOR) = selector;
}

uint8_t firmware_fw_cfg_read8(void)
{
    /* Volatile preserves each guest MMIO transaction. */
    return *(volatile uint8_t *)(MMIX_FW_CFG_BASE + MMIX_FW_CFG_DATA);
}
#endif

static bool validate_optional_files(FirmwareBootInputs *inputs,
                                    const char **error)
{
    uint64_t payload_size = 0;
    uint32_t i;

    if (inputs->kernel.present) {
        if (inputs->kernel.size == 0 ||
            inputs->kernel.size > inputs->ram.size) {
            *error = "invalid kernel size";
            return false;
        }
        payload_size = inputs->kernel.size;
    }
    if (inputs->initrd.present) {
        if (inputs->initrd.size == 0 ||
            inputs->initrd.size > inputs->ram.size ||
            payload_size + inputs->initrd.size < payload_size) {
            *error = "invalid initrd size";
            return false;
        }
        payload_size += inputs->initrd.size;
    }
    if (payload_size > inputs->ram.size) {
        *error = "boot payloads exceed RAM";
        return false;
    }
    if (!inputs->command_line.present) {
        return true;
    }
    if (inputs->command_line.size == 0 ||
        inputs->command_line.size > MMIX_COMMAND_LINE_MAX_SIZE) {
        *error = "invalid command line size";
        return false;
    }
    firmware_fw_cfg_select(inputs->command_line.selector);
    for (i = 1; i < inputs->command_line.size; i++) {
        firmware_fw_cfg_read8();
    }
    if (firmware_fw_cfg_read8() != '\0') {
        *error = "unterminated command line";
        return false;
    }
    return true;
}

bool firmware_discover_boot_inputs(FirmwareBootInputs *inputs,
                                   const char **error)
{
    FWCfgDirectoryEntry entry;
    FWCfgDirectoryEntry previous = { 0 };
    uint32_t count;
    uint32_t i;
    uint8_t version;

    clear_inputs(inputs);
    if (!read_signature()) {
        *error = "invalid fw_cfg signature";
        return false;
    }
    firmware_fw_cfg_select(FW_CFG_ID);
    version = firmware_fw_cfg_read8();
    if ((version & (FW_CFG_VERSION | FW_CFG_VERSION_DMA)) !=
        (FW_CFG_VERSION | FW_CFG_VERSION_DMA)) {
        *error = "unsupported fw_cfg version";
        return false;
    }
    firmware_fw_cfg_select(FW_CFG_NB_CPUS);
    inputs->cpu_count = firmware_fw_cfg_read8();
    inputs->cpu_count |= (uint16_t)firmware_fw_cfg_read8() << 8;
    if (inputs->cpu_count == 0 || inputs->cpu_count > MMIX_MAX_CPUS) {
        *error = "invalid fw_cfg CPU count";
        return false;
    }

    firmware_fw_cfg_select(FW_CFG_FILE_DIR);
    count = read_be32();
    if (count == 0 || count > FW_CFG_MAX_FILES) {
        *error = "invalid fw_cfg directory count";
        return false;
    }
    for (i = 0; i < count; i++) {
        if (!read_directory_entry(&entry) ||
            entry.selector != FW_CFG_FILE_FIRST + i ||
            (i != 0 && name_compare(previous.name, entry.name) >= 0) ||
            !record_known_file(inputs, &entry)) {
            *error = "invalid fw_cfg directory entry";
            return false;
        }
        previous = entry;
    }
    if (!inputs->fdt.present || inputs->fdt.size < 40 ||
        inputs->fdt.size > MMIX_FDT_MAX_SIZE) {
        *error = "missing or invalid FDT";
        return false;
    }
    if (!firmware_validate_fdt(inputs, error)) {
        return false;
    }
    return validate_optional_files(inputs, error);
}
