/*
 * MMIX virt inter-processor interrupt device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "ipi.h"

static uint64_t mmix_ipi_active_targets(const MMIXIPIState *s)
{
    return s->num_cpus == 64 ? UINT64_MAX :
           MAKE_64BIT_MASK(0, s->num_cpus);
}

static void mmix_ipi_update_locked(MMIXIPIState *s, uint64_t changed)
{
    while (changed) {
        unsigned int cpu = ctz64(changed);
        uint64_t bit = BIT_ULL(cpu);

        qemu_set_irq(s->irq[cpu], !!(s->pending & bit));
        changed &= ~bit;
    }
}

static void mmix_ipi_set_pending_locked(MMIXIPIState *s, uint64_t pending)
{
    uint64_t changed = s->pending ^ pending;

    s->pending = pending;
    mmix_ipi_update_locked(s, changed);
}

static void mmix_ipi_send(MMIXIPIState *s, uint64_t targets)
{
    uint64_t active = mmix_ipi_active_targets(s);
    uint64_t invalid = targets & ~active;

    if (invalid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ignoring inactive target mask 0x%016" PRIx64
                      "\n", __func__, invalid);
    }

    qemu_mutex_lock(&s->lock);
    mmix_ipi_set_pending_locked(s, s->pending | (targets & active));
    qemu_mutex_unlock(&s->lock);
}

static uint64_t mmix_ipi_global_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    MMIXIPIState *s = opaque;

    (void)size;

    if (addr == MMIX_VIRT_IPI_ACTIVE_TARGETS) {
        return mmix_ipi_active_targets(s);
    }
    return 0;
}

static void mmix_ipi_global_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned int size)
{
    MMIXIPIState *s = opaque;

    (void)size;

    if (addr == MMIX_VIRT_IPI_SEND) {
        mmix_ipi_send(s, value);
    }
}

static uint64_t mmix_ipi_context_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    MMIXIPIContext *context = opaque;
    MMIXIPIState *s = context->ipi;
    uint64_t status = 0;

    (void)size;

    if (context->cpu >= s->num_cpus ||
        addr != MMIX_VIRT_IPI_CONTEXT_STATUS) {
        return 0;
    }

    qemu_mutex_lock(&s->lock);
    if (s->pending & BIT_ULL(context->cpu)) {
        status = MMIX_VIRT_IPI_STATUS_PENDING;
    }
    qemu_mutex_unlock(&s->lock);
    return status;
}

static void mmix_ipi_context_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned int size)
{
    MMIXIPIContext *context = opaque;
    MMIXIPIState *s = context->ipi;

    (void)size;

    if (context->cpu >= s->num_cpus ||
        addr != MMIX_VIRT_IPI_CONTEXT_CLEAR ||
        !(value & MMIX_VIRT_IPI_STATUS_PENDING)) {
        return;
    }

    qemu_mutex_lock(&s->lock);
    mmix_ipi_set_pending_locked(s,
                                s->pending & ~BIT_ULL(context->cpu));
    qemu_mutex_unlock(&s->lock);
}

static const MemoryRegionOps mmix_ipi_global_ops = {
    .read = mmix_ipi_global_read,
    .write = mmix_ipi_global_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .valid.unaligned = false,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps mmix_ipi_context_ops = {
    .read = mmix_ipi_context_read,
    .write = mmix_ipi_context_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .valid.unaligned = false,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static void mmix_ipi_reset(DeviceState *dev)
{
    MMIXIPIState *s = MMIX_IPI(dev);

    qemu_mutex_lock(&s->lock);
    mmix_ipi_set_pending_locked(s, 0);
    qemu_mutex_unlock(&s->lock);
}

static void mmix_ipi_realize(DeviceState *dev, Error **errp)
{
    MMIXIPIState *s = MMIX_IPI(dev);
    uint32_t cpu;

    if (s->num_cpus == 0 || s->num_cpus > MMIX_VIRT_IPI_CONTEXT_COUNT) {
        error_setg(errp, "num-cpus must be between 1 and %u",
                   MMIX_VIRT_IPI_CONTEXT_COUNT);
        return;
    }

    memory_region_init(&s->container, OBJECT(s), TYPE_MMIX_IPI,
                       MMIX_VIRT_IPI_RESERVATION_SIZE);
    memory_region_init_io(&s->global_iomem, OBJECT(s), &mmix_ipi_global_ops,
                          s, "mmix-ipi-global",
                          MMIX_VIRT_IPI_GLOBAL_REGISTER_SIZE);
    memory_region_add_subregion(&s->container, 0, &s->global_iomem);

    for (cpu = 0; cpu < MMIX_VIRT_IPI_CONTEXT_COUNT; cpu++) {
        MMIXIPIContext *context = &s->context[cpu];
        g_autofree char *name = g_strdup_printf("mmix-ipi-context[%u]", cpu);

        context->ipi = s;
        context->cpu = cpu;
        memory_region_init_io(&context->iomem, OBJECT(s),
                              &mmix_ipi_context_ops, context, name,
                              MMIX_VIRT_IPI_CONTEXT_REGISTER_SIZE);
        memory_region_add_subregion(
            &s->container,
            MMIX_VIRT_IPI_CONTEXTS_OFFSET +
            cpu * MMIX_VIRT_IPI_CONTEXT_STRIDE,
            &context->iomem);
    }
}

static int mmix_ipi_post_load(void *opaque, int version_id)
{
    MMIXIPIState *s = opaque;

    (void)version_id;

    if (s->pending & ~mmix_ipi_active_targets(s)) {
        return -EINVAL;
    }

    qemu_mutex_lock(&s->lock);
    mmix_ipi_update_locked(s, mmix_ipi_active_targets(s));
    qemu_mutex_unlock(&s->lock);
    return 0;
}

static const VMStateDescription vmstate_mmix_ipi = {
    .name = TYPE_MMIX_IPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mmix_ipi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(pending, MMIXIPIState),
        VMSTATE_END_OF_LIST()
    },
};

/* num_cpus is immutable machine configuration, not migrated device state. */
static const Property mmix_ipi_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", MMIXIPIState, num_cpus, 1),
};

static void mmix_ipi_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    MMIXIPIState *s = MMIX_IPI(obj);
    uint32_t cpu;

    qemu_mutex_init(&s->lock);
    sysbus_init_mmio(dev, &s->container);
    for (cpu = 0; cpu < MMIX_VIRT_IPI_CONTEXT_COUNT; cpu++) {
        sysbus_init_irq(dev, &s->irq[cpu]);
    }
}

static void mmix_ipi_instance_finalize(Object *obj)
{
    MMIXIPIState *s = MMIX_IPI(obj);

    qemu_mutex_destroy(&s->lock);
}

static void mmix_ipi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    (void)data;

    device_class_set_props(dc, mmix_ipi_properties);
    device_class_set_legacy_reset(dc, mmix_ipi_reset);
    dc->realize = mmix_ipi_realize;
    dc->vmsd = &vmstate_mmix_ipi;
}

static const TypeInfo mmix_ipi_info = {
    .name = TYPE_MMIX_IPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mmix_ipi_instance_init,
    .instance_finalize = mmix_ipi_instance_finalize,
    .instance_size = sizeof(MMIXIPIState),
    .class_init = mmix_ipi_class_init,
};

static void mmix_ipi_register_types(void)
{
    type_register_static(&mmix_ipi_info);
}

type_init(mmix_ipi_register_types)
