/*
 * MMIX virt interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_INTC_H
#define HW_MMIX_INTC_H

#include "hw/core/sysbus.h"
#include "virt.h"

#define TYPE_MMIX_INTC "mmix-intc"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXIntcState, MMIX_INTC)

struct MMIXIntcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t pending;
    uint32_t input_level;
    uint32_t claimed[MMIX_VIRT_INTC_CONTEXT_COUNT];
    uint32_t enable[MMIX_VIRT_INTC_CONTEXT_COUNT];
};

#endif
