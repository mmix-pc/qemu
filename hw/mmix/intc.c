/*
 * MMIX virt interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "intc.h"

static bool mmix_intc_context_offset(hwaddr addr, uint32_t *cpu,
                                     hwaddr *reg)
{
    hwaddr context;

    if (addr < MMIX_VIRT_INTC_CONTEXT_BASE) {
        return false;
    }

    context = addr - MMIX_VIRT_INTC_CONTEXT_BASE;
    *cpu = context / MMIX_VIRT_INTC_CONTEXT_STRIDE;
    if (*cpu >= MMIX_VIRT_INTC_CONTEXT_COUNT) {
        return false;
    }

    *reg = context % MMIX_VIRT_INTC_CONTEXT_STRIDE;
    return true;
}

static uint64_t mmix_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t cpu;
    hwaddr reg;

    (void)opaque;
    (void)size;

    if (addr == MMIX_VIRT_INTC_PENDING) {
        return 0;
    }
    if (mmix_intc_context_offset(addr, &cpu, &reg)) {
        (void)cpu;

        switch (reg) {
        case MMIX_VIRT_INTC_CONTEXT_ENABLE:
        case MMIX_VIRT_INTC_CONTEXT_CLAIM:
            return 0;
        default:
            break;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register read 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void mmix_intc_write(void *opaque, hwaddr addr,
                            uint64_t value, unsigned size)
{
    uint32_t cpu;
    hwaddr reg;

    (void)opaque;
    (void)value;
    (void)size;

    if (mmix_intc_context_offset(addr, &cpu, &reg)) {
        (void)cpu;

        switch (reg) {
        case MMIX_VIRT_INTC_CONTEXT_ENABLE:
        case MMIX_VIRT_INTC_CONTEXT_COMPLETE:
            return;
        default:
            break;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register write 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
}

static const MemoryRegionOps mmix_intc_ops = {
    .read = mmix_intc_read,
    .write = mmix_intc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mmix_intc_set_irq(void *opaque, int irq, int level)
{
    (void)opaque;
    (void)irq;
    (void)level;

    /*
     * The device skeleton exposes input lines early so machine wiring and
     * later interrupt semantics can evolve in separate commits.
     */
}

static void mmix_intc_reset(DeviceState *dev)
{
    MMIXIntcState *s = MMIX_INTC(dev);

    s->pending = 0;
    s->input_level = 0;
    memset(s->claimed, 0, sizeof(s->claimed));
    memset(s->enable, 0, sizeof(s->enable));
}

static void mmix_intc_realize(DeviceState *dev, Error **errp)
{
    MMIXIntcState *s = MMIX_INTC(dev);

    (void)errp;

    memory_region_init_io(&s->iomem, OBJECT(s), &mmix_intc_ops, s,
                          TYPE_MMIX_INTC, MMIX_VIRT_INTC_SIZE);
}

static const VMStateDescription vmstate_mmix_intc = {
    .name = TYPE_MMIX_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pending, MMIXIntcState),
        VMSTATE_UINT32(input_level, MMIXIntcState),
        VMSTATE_UINT32_ARRAY(claimed, MMIXIntcState,
                             MMIX_VIRT_INTC_CONTEXT_COUNT),
        VMSTATE_UINT32_ARRAY(enable, MMIXIntcState,
                             MMIX_VIRT_INTC_CONTEXT_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void mmix_intc_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    MMIXIntcState *s = MMIX_INTC(obj);

    sysbus_init_mmio(dev, &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), mmix_intc_set_irq,
                      MMIX_VIRT_INTC_IRQ_COUNT);
}

static void mmix_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    (void)data;

    device_class_set_legacy_reset(dc, mmix_intc_reset);
    dc->realize = mmix_intc_realize;
    dc->vmsd = &vmstate_mmix_intc;
}

static const TypeInfo mmix_intc_info = {
    .name = TYPE_MMIX_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mmix_intc_instance_init,
    .instance_size = sizeof(MMIXIntcState),
    .class_init = mmix_intc_class_init,
};

static void mmix_intc_register_types(void)
{
    type_register_static(&mmix_intc_info);
}

type_init(mmix_intc_register_types)
