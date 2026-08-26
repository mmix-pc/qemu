/*
 * MMIX virt inter-processor interrupt device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_IPI_H
#define HW_MMIX_IPI_H

#include "hw/core/sysbus.h"
#include "virt.h"

#define TYPE_MMIX_IPI "mmix-ipi"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXIPIState, MMIX_IPI)

enum {
    MMIX_VIRT_IPI_ACTIVE_TARGETS = 0x0000,
    MMIX_VIRT_IPI_SEND = 0x0008,
    MMIX_VIRT_IPI_CONTEXT_BASE = 0x0100,
    MMIX_VIRT_IPI_CONTEXT_STRIDE = 0x20,
    MMIX_VIRT_IPI_CONTEXT_STATUS = 0x00,
    MMIX_VIRT_IPI_CONTEXT_CLEAR = 0x08,
    MMIX_VIRT_IPI_STATUS_PENDING = 0x01,
    MMIX_VIRT_IPI_SIZE = 0x1000,
};

struct MMIXIPIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[MMIX_VIRT_MAX_CPUS];

    uint32_t num_cpus;
    uint16_t pending;
};

#endif
