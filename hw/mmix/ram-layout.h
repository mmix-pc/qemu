/*
 * MMIX physical RAM layout shared by the virt machine and image loaders
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_RAM_LAYOUT_H
#define HW_MMIX_RAM_LAYOUT_H

#include "physical-layout.h"

typedef MMIXPhysRange MMIXPhysicalRAM;

static inline bool mmix_physical_ram_init(MMIXPhysicalRAM *ram,
                                          uint64_t size)
{
    return mmix_phys_range_init(ram, 0, size);
}

static inline bool mmix_physical_ram_contains(
    const MMIXPhysicalRAM *ram, uint64_t address, uint64_t size)
{
    MMIXPhysRange range;

    return mmix_phys_range_init(&range, address, size) &&
           mmix_phys_range_contains(ram, &range);
}

#endif
