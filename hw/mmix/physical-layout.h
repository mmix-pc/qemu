/*
 * MMIX virt physical address layout
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_PHYSICAL_LAYOUT_H
#define HW_MMIX_PHYSICAL_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

enum {
    MMIX_VIRT_RAM_ALIGN = 0x2000,
};

#define MMIX_VIRT_RAM_MIN_SIZE     UINT64_C(0x0000000008000000)
#define MMIX_VIRT_RAM_DEFAULT_SIZE UINT64_C(0x0000000020000000)
#define MMIX_VIRT_RAM_MAX_SIZE     UINT64_C(0x0000010000000000)
#define MMIX_VIRT_RAM_PHYS_LIMIT   UINT64_C(0x0001000000000000)

typedef struct MMIXPhysRange {
    uint64_t start;
    uint64_t end;
} MMIXPhysRange;

typedef enum MMIXVirtPhysRegion {
    MMIX_VIRT_PHYS_RAM,
    MMIX_VIRT_PHYS_FIRMWARE,
    MMIX_VIRT_PHYS_FIXED_DEVICES,
    MMIX_VIRT_PHYS_PER_CPU,
    MMIX_VIRT_PHYS_INTERRUPT_CONTROLLER,
    MMIX_VIRT_PHYS_DYNAMIC_PLATFORM,
    MMIX_VIRT_PHYS_SYSTEM_MMIO,
    MMIX_VIRT_PHYS_PCIE_ECAM,
    MMIX_VIRT_PHYS_PCIE_32BIT,
    MMIX_VIRT_PHYS_PCIE_64BIT,
    MMIX_VIRT_PHYS_REGION_COUNT,
} MMIXVirtPhysRegion;

extern const MMIXPhysRange
    mmix_virt_phys_regions[MMIX_VIRT_PHYS_REGION_COUNT];

bool mmix_phys_add(uint64_t left, uint64_t right, uint64_t *result);
bool mmix_phys_sub(uint64_t left, uint64_t right, uint64_t *result);
bool mmix_phys_align_down(uint64_t value, uint64_t alignment,
                          uint64_t *result);
bool mmix_phys_align_up(uint64_t value, uint64_t alignment,
                        uint64_t *result);

bool mmix_phys_range_init(MMIXPhysRange *range, uint64_t start,
                          uint64_t size);
bool mmix_phys_range_valid(const MMIXPhysRange *range);
uint64_t mmix_phys_range_size(const MMIXPhysRange *range);
bool mmix_phys_range_contains(const MMIXPhysRange *container,
                              const MMIXPhysRange *range);
bool mmix_phys_range_contains_addr(const MMIXPhysRange *range,
                                   uint64_t address);
bool mmix_phys_ranges_overlap(const MMIXPhysRange *left,
                              const MMIXPhysRange *right);

#endif
