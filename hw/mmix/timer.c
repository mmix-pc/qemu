/*
 * MMIX virt timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "timer.h"

static uint64_t mmix_timer_control_mask(void)
{
    return MMIX_VIRT_TIMER_CONTROL_ENABLE |
           MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE;
}

static bool mmix_timer_context_offset(hwaddr addr, uint32_t *cpu, hwaddr *reg)
{
    hwaddr context;

    if (addr < MMIX_VIRT_TIMER_CONTEXT_BASE) {
        return false;
    }

    context = addr - MMIX_VIRT_TIMER_CONTEXT_BASE;
    *cpu = context / MMIX_VIRT_TIMER_CONTEXT_STRIDE;
    if (*cpu >= MMIX_VIRT_INTC_CONTEXT_COUNT) {
        return false;
    }

    *reg = context % MMIX_VIRT_TIMER_CONTEXT_STRIDE;
    return true;
}

static uint64_t mmix_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    MMIXTimerState *s = opaque;
    uint32_t cpu;
    hwaddr reg;

    (void)size;

    if (addr == MMIX_VIRT_TIMER_TIME) {
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (mmix_timer_context_offset(addr, &cpu, &reg)) {
        if (cpu != 0) {
            return 0;
        }
        switch (reg) {
        case MMIX_VIRT_TIMER_CONTEXT_COMPARE:
            return s->compare[cpu];
        case MMIX_VIRT_TIMER_CONTEXT_CONTROL:
            return s->control[cpu];
        case MMIX_VIRT_TIMER_CONTEXT_STATUS:
            return s->status[cpu];
        default:
            break;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register read 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void mmix_timer_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    MMIXTimerState *s = opaque;
    uint32_t cpu;
    hwaddr reg;

    (void)size;

    if (mmix_timer_context_offset(addr, &cpu, &reg)) {
        if (cpu != 0) {
            return;
        }
        switch (reg) {
        case MMIX_VIRT_TIMER_CONTEXT_COMPARE:
            s->compare[cpu] = value;
            return;
        case MMIX_VIRT_TIMER_CONTEXT_CONTROL:
            s->control[cpu] = value & mmix_timer_control_mask();
            return;
        case MMIX_VIRT_TIMER_CONTEXT_STATUS:
            s->status[cpu] &= ~(value & MMIX_VIRT_TIMER_STATUS_PENDING);
            return;
        default:
            break;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register write 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
}

static const MemoryRegionOps mmix_timer_ops = {
    .read = mmix_timer_read,
    .write = mmix_timer_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static void mmix_timer_reset(DeviceState *dev)
{
    MMIXTimerState *s = MMIX_TIMER(dev);

    memset(s->compare, 0, sizeof(s->compare));
    memset(s->control, 0, sizeof(s->control));
    memset(s->status, 0, sizeof(s->status));
    qemu_set_irq(s->irq, 0);
}

static void mmix_timer_realize(DeviceState *dev, Error **errp)
{
    MMIXTimerState *s = MMIX_TIMER(dev);

    (void)errp;

    memory_region_init_io(&s->iomem, OBJECT(s), &mmix_timer_ops, s,
                          TYPE_MMIX_TIMER, MMIX_VIRT_TIMER_SIZE);
}

static const VMStateDescription vmstate_mmix_timer = {
    .name = TYPE_MMIX_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(compare, MMIXTimerState,
                             MMIX_VIRT_INTC_CONTEXT_COUNT),
        VMSTATE_UINT64_ARRAY(control, MMIXTimerState,
                             MMIX_VIRT_INTC_CONTEXT_COUNT),
        VMSTATE_UINT64_ARRAY(status, MMIXTimerState,
                             MMIX_VIRT_INTC_CONTEXT_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void mmix_timer_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    MMIXTimerState *s = MMIX_TIMER(obj);

    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);
}

static void mmix_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    (void)data;

    device_class_set_legacy_reset(dc, mmix_timer_reset);
    dc->realize = mmix_timer_realize;
    dc->vmsd = &vmstate_mmix_timer;
}

static const TypeInfo mmix_timer_info = {
    .name = TYPE_MMIX_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mmix_timer_instance_init,
    .instance_size = sizeof(MMIXTimerState),
    .class_init = mmix_timer_class_init,
};

static void mmix_timer_register_types(void)
{
    type_register_static(&mmix_timer_info);
}

type_init(mmix_timer_register_types)
