/*
 * MMIX virt interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "intc.h"

static uint32_t mmix_intc_irq_mask(unsigned irq)
{
    if (irq < MMIX_VIRT_SHARED_IRQ_FIRST || irq >= MMIX_VIRT_INTC_IRQ_COUNT) {
        return 0;
    }
    return 1U << irq;
}

static uint32_t mmix_intc_valid_irq_mask(void)
{
    return UINT32_MAX & ~1U;
}

static uint32_t mmix_intc_claimable(MMIXIntcState *s, uint32_t cpu)
{
    /* CPU1+ contexts are reserved for future SMP support. */
    if (cpu != 0) {
        return 0;
    }

    return s->pending & s->enable[cpu] & ~s->claimed[cpu];
}

static void mmix_intc_update(MMIXIntcState *s)
{
    qemu_set_irq(s->irq, !!mmix_intc_claimable(s, 0));
}

static uint32_t mmix_intc_claim(MMIXIntcState *s, uint32_t cpu)
{
    uint32_t claimable;
    uint32_t irq;
    uint32_t mask;

    claimable = mmix_intc_claimable(s, cpu);
    if (!claimable) {
        return 0;
    }

    irq = ctz32(claimable);
    mask = mmix_intc_irq_mask(irq);
    g_assert(mask != 0);

    s->pending &= ~mask;
    s->claimed[cpu] |= mask;
    mmix_intc_update(s);

    return irq;
}

static void mmix_intc_complete(MMIXIntcState *s, uint32_t cpu, uint32_t irq)
{
    uint32_t mask;

    if (cpu != 0) {
        return;
    }

    mask = mmix_intc_irq_mask(irq);
    if (!(mask & s->claimed[cpu])) {
        return;
    }

    s->claimed[cpu] &= ~mask;
    if (s->input_level & mask) {
        s->pending |= mask;
    }
    mmix_intc_update(s);
}

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
    MMIXIntcState *s = opaque;
    uint32_t cpu;
    hwaddr reg;

    (void)size;

    if (addr == MMIX_VIRT_INTC_PENDING) {
        return s->pending;
    }
    if (mmix_intc_context_offset(addr, &cpu, &reg)) {
        switch (reg) {
        case MMIX_VIRT_INTC_CONTEXT_ENABLE:
            return cpu == 0 ? s->enable[cpu] : 0;
        case MMIX_VIRT_INTC_CONTEXT_CLAIM:
            return mmix_intc_claim(s, cpu);
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
    MMIXIntcState *s = opaque;
    uint32_t cpu;
    hwaddr reg;

    (void)size;

    if (mmix_intc_context_offset(addr, &cpu, &reg)) {
        switch (reg) {
        case MMIX_VIRT_INTC_CONTEXT_ENABLE:
            if (cpu == 0) {
                s->enable[cpu] = value & mmix_intc_valid_irq_mask();
                mmix_intc_update(s);
            }
            return;
        case MMIX_VIRT_INTC_CONTEXT_COMPLETE:
            mmix_intc_complete(s, cpu, value);
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
    MMIXIntcState *s = opaque;
    uint32_t mask = mmix_intc_irq_mask(irq);

    if (!mask) {
        return;
    }

    if (level) {
        s->input_level |= mask;
        s->pending |= mask;
    } else {
        s->input_level &= ~mask;
        s->pending &= ~mask;
    }
    mmix_intc_update(s);
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
    sysbus_init_irq(dev, &s->irq);
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
