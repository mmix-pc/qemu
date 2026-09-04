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

typedef struct MMIXIntcContext {
    MemoryRegion iomem;
    MMIXIntcState *intc;
    uint32_t cpu;
} MMIXIntcContext;

struct MMIXIntcState {
    SysBusDevice parent_obj;

    MemoryRegion container;
    MemoryRegion global_iomem;
    MMIXIntcContext context[MMIX_VIRT_INTC_CONTEXT_COUNT];
    qemu_irq irq[MMIX_VIRT_INTC_CONTEXT_COUNT];

    uint32_t num_cpus;
    uint64_t pending[MMIX_VIRT_INTC_BITMAP_WORDS];
    uint64_t input_level[MMIX_VIRT_INTC_BITMAP_WORDS];
    int16_t owner[MMIX_VIRT_INTC_IRQ_COUNT];
    uint64_t enable[MMIX_VIRT_INTC_CONTEXT_COUNT]
                   [MMIX_VIRT_INTC_BITMAP_WORDS];
};

#endif
