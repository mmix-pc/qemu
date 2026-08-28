/*
 * MMIX virt inter-processor interrupt device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_IPI_H
#define HW_MMIX_IPI_H

#include "hw/core/sysbus.h"
#include "qemu/thread.h"
#include "virt.h"

#define TYPE_MMIX_IPI "mmix-ipi"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXIPIState, MMIX_IPI)

enum {
    MMIX_VIRT_IPI_ACTIVE_TARGETS = 0x0000,
    MMIX_VIRT_IPI_SEND = 0x0008,
    MMIX_VIRT_IPI_CONTEXT_STATUS = 0x00,
    MMIX_VIRT_IPI_CONTEXT_CLEAR = 0x08,
    MMIX_VIRT_IPI_STATUS_PENDING = 0x01,
};

typedef struct MMIXIPIContext {
    MemoryRegion iomem;
    MMIXIPIState *ipi;
    uint32_t cpu;
} MMIXIPIContext;

struct MMIXIPIState {
    SysBusDevice parent_obj;

    MemoryRegion container;
    MemoryRegion global_iomem;
    MMIXIPIContext context[MMIX_VIRT_IPI_CONTEXT_COUNT];
    qemu_irq irq[MMIX_VIRT_MAX_CPUS];
    QemuMutex lock;

    uint32_t num_cpus;
    uint64_t pending;
};

#endif
