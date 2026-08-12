/*
 * MMIX virt timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_TIMER_H
#define HW_MMIX_TIMER_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "virt.h"

#define TYPE_MMIX_TIMER "mmix-timer"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXTimerState, MMIX_TIMER)

struct MMIXTimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    uint64_t compare[MMIX_VIRT_TIMER_CONTEXT_COUNT];
    uint64_t control[MMIX_VIRT_TIMER_CONTEXT_COUNT];
    uint64_t status[MMIX_VIRT_TIMER_CONTEXT_COUNT];
};

#endif
