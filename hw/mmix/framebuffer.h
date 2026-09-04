/*
 * MMIX virt framebuffer
 *
 * This is an internal header for the MMIX machine implementation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_FRAMEBUFFER_H
#define HW_MMIX_FRAMEBUFFER_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "virt.h"

#define TYPE_MMIX_FRAMEBUFFER "mmix-framebuffer"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXFramebufferState, MMIX_FRAMEBUFFER)

struct MMIXFramebufferState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    uint64_t base;
    uint64_t size;
    bool invalidate;
    bool refresh_pending;
};

#endif
