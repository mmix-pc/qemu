/*
 * MMIX virt firmware internal interfaces
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MMIX_VIRT_FIRMWARE_H
#define MMIX_VIRT_FIRMWARE_H

typedef _Bool bool;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#define false 0
#define true 1

#define UINT64_C(value) value##ULL
#define MMIX_DIRECT_ALIAS(address) (UINT64_C(0x8000000000000000) | (address))

#define MMIX_MAX_CPUS 64
#define MMIX_MAX_RESERVED_RANGES 128

typedef struct FirmwareFile {
    uint32_t size;
    uint16_t selector;
    bool present;
} FirmwareFile;

typedef struct FirmwareRange {
    uint64_t base;
    uint64_t size;
} FirmwareRange;

typedef struct FirmwareBootInputs {
    FirmwareFile fdt;
    FirmwareFile kernel;
    FirmwareFile initrd;
    FirmwareFile command_line;
    FirmwareRange ram;
    FirmwareRange reserved[MMIX_MAX_RESERVED_RANGES];
    uint32_t reserved_count;
    uint16_t cpu_count;
} FirmwareBootInputs;

_Static_assert(sizeof(uint8_t) == 1, "unexpected MMIX byte size");
_Static_assert(sizeof(uint16_t) == 2, "unexpected MMIX wyde size");
_Static_assert(sizeof(uint32_t) == 4, "unexpected MMIX tetra size");
_Static_assert(sizeof(uint64_t) == 8, "unexpected MMIX octa size");

bool firmware_discover_boot_inputs(FirmwareBootInputs *inputs,
                                   const char **error);
bool firmware_validate_fdt(FirmwareBootInputs *inputs, const char **error);
void firmware_fw_cfg_select(uint16_t selector);
uint8_t firmware_fw_cfg_read8(void);
void *memset(void *destination, int value, unsigned long size);

#endif
