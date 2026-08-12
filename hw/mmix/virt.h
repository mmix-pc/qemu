/*
 * MMIX virt machine layout
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_VIRT_H
#define HW_MMIX_VIRT_H

#include "exec/hwaddr.h"

typedef enum MMIXVirtMemMap {
    MMIX_VIRT_LOW_RAM,
    MMIX_VIRT_POOL,
    MMIX_VIRT_DATA,
    MMIX_VIRT_STACK,
    MMIX_VIRT_PLATFORM_RAM,
    MMIX_VIRT_BOOTINFO,
    MMIX_VIRT_FRAMEBUFFER,
    MMIX_VIRT_MMIO,
    MMIX_VIRT_UARTLITE,
    MMIX_VIRT_MEMMAP_COUNT,
} MMIXVirtMemMap;

extern const MemMapEntry mmix_virt_memmap[MMIX_VIRT_MEMMAP_COUNT];

#endif
