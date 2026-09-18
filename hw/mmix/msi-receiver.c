/*
 * MMIX virt PCI MSI receiver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "msi-receiver.h"

static uint64_t mmix_msi_receiver_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    (void)opaque;
    (void)size;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: invalid read at offset 0x%04" HWADDR_PRIx "\n",
                  TYPE_MMIX_MSI_RECEIVER, addr);
    return 0;
}

static void mmix_msi_receiver_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned int size)
{
    MMIXMSIReceiverState *s = opaque;

    if (addr != 0 || size != 4 ||
        value >= MMIX_VIRT_PCIE_MSI_IRQ_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid write at offset 0x%04" HWADDR_PRIx
                      " value 0x%08" PRIx64 "\n",
                      TYPE_MMIX_MSI_RECEIVER, addr, value);
        return;
    }

    mmix_intc_inject_edge(s->intc,
                          MMIX_VIRT_PCIE_MSI_IRQ_BASE + value);
}

static bool mmix_msi_receiver_accepts(void *opaque, hwaddr addr,
                                      unsigned int size, bool is_write,
                                      MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;

    return is_write && addr == 0 && size == sizeof(uint32_t);
}

static const MemoryRegionOps mmix_msi_receiver_ops = {
    .read = mmix_msi_receiver_read,
    .write = mmix_msi_receiver_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .valid.accepts = mmix_msi_receiver_accepts,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static void mmix_msi_receiver_realize(DeviceState *dev, Error **errp)
{
    MMIXMSIReceiverState *s = MMIX_MSI_RECEIVER(dev);

    if (!s->intc) {
        error_setg(errp, "MMIX MSI receiver requires an interrupt controller");
    }
}

static const Property mmix_msi_receiver_properties[] = {
    DEFINE_PROP_LINK("interrupt-controller", MMIXMSIReceiverState, intc,
                     TYPE_MMIX_INTC, MMIXIntcState *),
};

static void mmix_msi_receiver_instance_init(Object *obj)
{
    MMIXMSIReceiverState *s = MMIX_MSI_RECEIVER(obj);

    memory_region_init_io(&s->iomem, obj, &mmix_msi_receiver_ops, s,
                          TYPE_MMIX_MSI_RECEIVER,
                          MMIX_VIRT_PCIE_MSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mmix_msi_receiver_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    (void)data;

    dc->realize = mmix_msi_receiver_realize;
    device_class_set_props(dc, mmix_msi_receiver_properties);
}

static const TypeInfo mmix_msi_receiver_info = {
    .name = TYPE_MMIX_MSI_RECEIVER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MMIXMSIReceiverState),
    .instance_init = mmix_msi_receiver_instance_init,
    .class_init = mmix_msi_receiver_class_init,
};

static void mmix_msi_receiver_register_types(void)
{
    type_register_static(&mmix_msi_receiver_info);
}

type_init(mmix_msi_receiver_register_types)
