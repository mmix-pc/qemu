/*
 * MMIX physical RAM layout shared by the virt machine and image loaders
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_RAM_LAYOUT_H
#define HW_MMIX_RAM_LAYOUT_H

#include "exec/hwaddr.h"

typedef struct MMIXPhysicalRAMRange {
    hwaddr base;
    uint64_t size;
} MMIXPhysicalRAMRange;

typedef struct MMIXPhysicalRAMLayout {
    MMIXPhysicalRAMRange low;
    MMIXPhysicalRAMRange high;
} MMIXPhysicalRAMLayout;

static inline bool mmix_physical_ram_range_contains(
    const MMIXPhysicalRAMRange *ram, hwaddr address, uint64_t size)
{
    uint64_t offset;

    if (address < ram->base) {
        return false;
    }
    offset = address - ram->base;
    return offset <= ram->size && size <= ram->size - offset;
}

static inline bool mmix_physical_ram_contains(
    const MMIXPhysicalRAMLayout *layout, hwaddr address, uint64_t size)
{
    return mmix_physical_ram_range_contains(&layout->low, address, size) ||
           mmix_physical_ram_range_contains(&layout->high, address, size);
}

#endif
