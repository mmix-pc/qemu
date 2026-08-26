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

static uint64_t mmix_ipi_active_targets(MMIXIPIState *s)
{
    return MAKE_64BIT_MASK(0, s->num_cpus);
}

static void mmix_ipi_update(MMIXIPIState *s)
{
    uint32_t cpu;

    for (cpu = 0; cpu < MMIX_VIRT_MAX_CPUS; cpu++) {
        qemu_set_irq(s->irq[cpu], extract16(s->pending, cpu, 1));
    }
}

static void mmix_ipi_set_pending(MMIXIPIState *s, uint16_t pending)
{
    uint16_t changed = s->pending ^ pending;
    uint32_t cpu;

    s->pending = pending;
    for (cpu = 0; cpu < MMIX_VIRT_MAX_CPUS; cpu++) {
        if (extract16(changed, cpu, 1)) {
            qemu_set_irq(s->irq[cpu], extract16(pending, cpu, 1));
        }
    }
}

static bool mmix_ipi_context_offset(hwaddr addr, uint32_t *cpu, hwaddr *reg)
{
    hwaddr context;

    if (addr < MMIX_VIRT_IPI_CONTEXT_BASE) {
        return false;
    }

    context = addr - MMIX_VIRT_IPI_CONTEXT_BASE;
    *cpu = context / MMIX_VIRT_IPI_CONTEXT_STRIDE;
    if (*cpu >= MMIX_VIRT_MAX_CPUS) {
        return false;
    }

    *reg = context % MMIX_VIRT_IPI_CONTEXT_STRIDE;
    return true;
}

static uint64_t mmix_ipi_read(void *opaque, hwaddr addr, unsigned size)
{
    MMIXIPIState *s = opaque;
    uint32_t cpu;
    hwaddr reg;

    (void)size;

    if (addr == MMIX_VIRT_IPI_ACTIVE_TARGETS) {
        return mmix_ipi_active_targets(s);
    }
    if (mmix_ipi_context_offset(addr, &cpu, &reg) &&
        reg == MMIX_VIRT_IPI_CONTEXT_STATUS) {
        if (cpu >= s->num_cpus) {
            return 0;
        }
        return extract16(s->pending, cpu, 1);
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register read 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void mmix_ipi_send(MMIXIPIState *s, uint64_t targets)
{
    uint64_t active = mmix_ipi_active_targets(s);
    uint64_t invalid = targets & ~active;
    uint16_t valid = targets & active;

    if (invalid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ignoring inactive target mask 0x%016" PRIx64
                      "\n", __func__, invalid);
    }

    mmix_ipi_set_pending(s, s->pending | valid);
}

static void mmix_ipi_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    MMIXIPIState *s = opaque;
    uint32_t cpu;
    hwaddr reg;

    (void)size;

    if (addr == MMIX_VIRT_IPI_SEND) {
        mmix_ipi_send(s, value);
        return;
    }
    if (mmix_ipi_context_offset(addr, &cpu, &reg) &&
        reg == MMIX_VIRT_IPI_CONTEXT_CLEAR) {
        if (cpu < s->num_cpus && (value & MMIX_VIRT_IPI_STATUS_PENDING)) {
            mmix_ipi_set_pending(s, s->pending & ~(1U << cpu));
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented register write 0x%02" HWADDR_PRIx "\n",
                  __func__, addr);
}

static const MemoryRegionOps mmix_ipi_ops = {
    .read = mmix_ipi_read,
    .write = mmix_ipi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static void mmix_ipi_reset(DeviceState *dev)
{
    MMIXIPIState *s = MMIX_IPI(dev);

    mmix_ipi_set_pending(s, 0);
}

static void mmix_ipi_realize(DeviceState *dev, Error **errp)
{
    MMIXIPIState *s = MMIX_IPI(dev);

    if (s->num_cpus == 0 || s->num_cpus > MMIX_VIRT_MAX_CPUS) {
        error_setg(errp, "num-cpus must be between 1 and %u",
                   MMIX_VIRT_MAX_CPUS);
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &mmix_ipi_ops, s,
                          TYPE_MMIX_IPI, MMIX_VIRT_IPI_SIZE);
}

static int mmix_ipi_post_load(void *opaque, int version_id)
{
    MMIXIPIState *s = opaque;

    (void)version_id;

    mmix_ipi_update(s);
    return 0;
}

static const VMStateDescription vmstate_mmix_ipi = {
    .name = TYPE_MMIX_IPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mmix_ipi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(pending, MMIXIPIState),
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

    sysbus_init_mmio(dev, &s->iomem);
    for (cpu = 0; cpu < MMIX_VIRT_MAX_CPUS; cpu++) {
        sysbus_init_irq(dev, &s->irq[cpu]);
    }
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
    .instance_size = sizeof(MMIXIPIState),
    .class_init = mmix_ipi_class_init,
};

static void mmix_ipi_register_types(void)
{
    type_register_static(&mmix_ipi_info);
}

type_init(mmix_ipi_register_types)
