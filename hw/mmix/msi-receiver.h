/*
 * MMIX virt PCI MSI receiver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_MSI_RECEIVER_H
#define HW_MMIX_MSI_RECEIVER_H

#include "hw/core/sysbus.h"
#include "intc.h"

#define TYPE_MMIX_MSI_RECEIVER "mmix-msi-receiver"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXMSIReceiverState, MMIX_MSI_RECEIVER)

struct MMIXMSIReceiverState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MMIXIntcState *intc;
};

#endif
