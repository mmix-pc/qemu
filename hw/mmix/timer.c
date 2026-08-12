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
#include "timer.h"

static uint64_t mmix_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)size;

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register read 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void mmix_timer_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    (void)opaque;
    (void)value;
    (void)size;

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
