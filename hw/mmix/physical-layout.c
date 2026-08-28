/*
 * MMIX virt physical address layout
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/physical-layout.h"
#include "qemu/host-utils.h"

const MMIXPhysRange
mmix_virt_phys_regions[MMIX_VIRT_PHYS_REGION_COUNT] = {
    [MMIX_VIRT_PHYS_RAM] = {
        UINT64_C(0x0000000000000000), UINT64_C(0x0001000000000000),
    },
    [MMIX_VIRT_PHYS_FIRMWARE] = {
        UINT64_C(0x0001000000000000), UINT64_C(0x0001000010000000),
    },
    [MMIX_VIRT_PHYS_FIXED_DEVICES] = {
        UINT64_C(0x0001000010000000), UINT64_C(0x0001000020000000),
    },
    [MMIX_VIRT_PHYS_PER_CPU] = {
        UINT64_C(0x0001000020000000), UINT64_C(0x0001000030000000),
    },
    [MMIX_VIRT_PHYS_INTERRUPT_CONTROLLER] = {
        UINT64_C(0x0001000030000000), UINT64_C(0x0001000040000000),
    },
    [MMIX_VIRT_PHYS_DYNAMIC_PLATFORM] = {
        UINT64_C(0x0001000040000000), UINT64_C(0x0001000080000000),
    },
    [MMIX_VIRT_PHYS_SYSTEM_MMIO] = {
        UINT64_C(0x0001000080000000), UINT64_C(0x0001000100000000),
    },
    [MMIX_VIRT_PHYS_PCIE_ECAM] = {
        UINT64_C(0x0001000100000000), UINT64_C(0x0001000110000000),
    },
    [MMIX_VIRT_PHYS_PCIE_32BIT] = {
        UINT64_C(0x0001000200000000), UINT64_C(0x0001000300000000),
    },
    [MMIX_VIRT_PHYS_PCIE_64BIT] = {
        UINT64_C(0x0001010000000000), UINT64_C(0x0001110000000000),
    },
};

bool mmix_phys_add(uint64_t left, uint64_t right, uint64_t *result)
{
    return !uadd64_overflow(left, right, result);
}

bool mmix_phys_sub(uint64_t left, uint64_t right, uint64_t *result)
{
    return !usub64_overflow(left, right, result);
}

bool mmix_phys_align_down(uint64_t value, uint64_t alignment,
                          uint64_t *result)
{
    if (alignment == 0) {
        return false;
    }

    *result = value - value % alignment;
    return true;
}

bool mmix_phys_align_up(uint64_t value, uint64_t alignment,
                        uint64_t *result)
{
    uint64_t remainder;

    if (alignment == 0) {
        return false;
    }

    remainder = value % alignment;
    if (remainder == 0) {
        *result = value;
        return true;
    }

    return mmix_phys_add(value, alignment - remainder, result);
}

bool mmix_phys_range_init(MMIXPhysRange *range, uint64_t start,
                          uint64_t size)
{
    uint64_t end;

    if (size == 0 || !mmix_phys_add(start, size, &end)) {
        return false;
    }

    *range = (MMIXPhysRange) {
        .start = start,
        .end = end,
    };
    return true;
}

bool mmix_phys_range_valid(const MMIXPhysRange *range)
{
    return range->start < range->end;
}

uint64_t mmix_phys_range_size(const MMIXPhysRange *range)
{
    g_assert(mmix_phys_range_valid(range));
    return range->end - range->start;
}

bool mmix_phys_range_contains(const MMIXPhysRange *container,
                              const MMIXPhysRange *range)
{
    return mmix_phys_range_valid(container) &&
           mmix_phys_range_valid(range) &&
           container->start <= range->start && range->end <= container->end;
}

bool mmix_phys_range_contains_addr(const MMIXPhysRange *range,
                                   uint64_t address)
{
    return mmix_phys_range_valid(range) &&
           range->start <= address && address < range->end;
}

bool mmix_phys_ranges_overlap(const MMIXPhysRange *left,
                              const MMIXPhysRange *right)
{
    return mmix_phys_range_valid(left) && mmix_phys_range_valid(right) &&
           left->start < right->end && right->start < left->end;
}
