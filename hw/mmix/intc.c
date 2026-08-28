/*
 * MMIX virt interrupt controller
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
#include "intc.h"

#define MMIX_INTC_NO_OWNER (-1)

static unsigned int mmix_intc_word(unsigned int source)
{
    return source / 64;
}

static uint64_t mmix_intc_bit(unsigned int source)
{
    return UINT64_C(1) << (source % 64);
}

static bool mmix_intc_source_active(unsigned int source)
{
    if (source == MMIX_VIRT_UART0_IRQ) {
        return true;
    }
    if (source >= MMIX_VIRT_TIMER_IRQ_BASE &&
        source < MMIX_VIRT_TIMER_IRQ_BASE + MMIX_VIRT_MAX_CPUS) {
        return true;
    }
    return source >= MMIX_VIRT_VIRTIO_MMIO_IRQ_BASE &&
           source < MMIX_VIRT_VIRTIO_MMIO_IRQ_BASE +
                    MMIX_VIRT_VIRTIO_MMIO_COUNT;
}

static bool mmix_intc_source_fixed(unsigned int source, uint32_t *cpu)
{
    if (source < MMIX_VIRT_TIMER_IRQ_BASE ||
        source >= MMIX_VIRT_TIMER_IRQ_BASE + MMIX_VIRT_MAX_CPUS) {
        return false;
    }

    *cpu = source - MMIX_VIRT_TIMER_IRQ_BASE;
    return true;
}

static bool mmix_intc_context_active(const MMIXIntcState *s, uint32_t cpu)
{
    return cpu < s->num_cpus;
}

static uint64_t mmix_intc_active_mask(unsigned int word)
{
    uint64_t mask = 0;
    unsigned int first = word * 64;
    unsigned int source;

    for (source = first; source < first + 64; source++) {
        if (mmix_intc_source_active(source)) {
            mask |= mmix_intc_bit(source);
        }
    }
    return mask;
}

static uint64_t mmix_intc_enable_mask(uint32_t cpu, unsigned int word)
{
    uint64_t mask = mmix_intc_active_mask(word);
    unsigned int first = word * 64;
    unsigned int source;

    for (source = first; source < first + 64; source++) {
        uint32_t fixed_cpu;

        if (mmix_intc_source_fixed(source, &fixed_cpu) &&
            fixed_cpu != cpu) {
            mask &= ~mmix_intc_bit(source);
        }
    }
    return mask;
}

static bool mmix_intc_context_claimable(const MMIXIntcState *s,
                                        uint32_t cpu)
{
    unsigned int word;

    if (!mmix_intc_context_active(s, cpu)) {
        return false;
    }
    for (word = 0; word < MMIX_VIRT_INTC_BITMAP_WORDS; word++) {
        if (s->pending[word] & s->enable[cpu][word]) {
            return true;
        }
    }
    return false;
}

static void mmix_intc_update(MMIXIntcState *s)
{
    uint32_t cpu;

    for (cpu = 0; cpu < MMIX_VIRT_INTC_CONTEXT_COUNT; cpu++) {
        qemu_set_irq(s->irq[cpu], mmix_intc_context_claimable(s, cpu));
    }
}

static uint64_t mmix_intc_claim(MMIXIntcState *s, uint32_t cpu)
{
    unsigned int word;

    if (!mmix_intc_context_active(s, cpu)) {
        return 0;
    }

    for (word = 0; word < MMIX_VIRT_INTC_BITMAP_WORDS; word++) {
        uint64_t candidates = s->pending[word] & s->enable[cpu][word];
        unsigned int source;

        if (!candidates) {
            continue;
        }
        source = word * 64 + ctz64(candidates);
        g_assert(s->owner[source] == MMIX_INTC_NO_OWNER);
        s->pending[word] &= ~mmix_intc_bit(source);
        s->owner[source] = cpu;
        mmix_intc_update(s);
        return source;
    }
    return 0;
}

static void mmix_intc_complete(MMIXIntcState *s, uint32_t cpu,
                               unsigned int source)
{
    unsigned int word;
    uint64_t bit;

    if (!mmix_intc_context_active(s, cpu) ||
        source >= MMIX_VIRT_INTC_IRQ_COUNT || s->owner[source] != cpu) {
        return;
    }

    word = mmix_intc_word(source);
    bit = mmix_intc_bit(source);
    s->owner[source] = MMIX_INTC_NO_OWNER;
    if (s->input_level[word] & bit) {
        s->pending[word] |= bit;
    }
    mmix_intc_update(s);
}

static uint64_t mmix_intc_global_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    MMIXIntcState *s = opaque;

    (void)size;

    if (addr == MMIX_VIRT_INTC_GLOBAL_SOURCE_COUNT) {
        return MMIX_VIRT_INTC_IRQ_COUNT;
    }
    if (addr == MMIX_VIRT_INTC_GLOBAL_CONTEXT_COUNT) {
        return s->num_cpus;
    }
    if (addr >= MMIX_VIRT_INTC_GLOBAL_PENDING_BASE &&
        addr < MMIX_VIRT_INTC_GLOBAL_PENDING_BASE +
               MMIX_VIRT_INTC_BITMAP_WORDS * sizeof(uint64_t)) {
        unsigned int word =
            (addr - MMIX_VIRT_INTC_GLOBAL_PENDING_BASE) / sizeof(uint64_t);

        return s->pending[word];
    }
    return 0;
}

static void mmix_intc_global_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned int size)
{
    (void)opaque;
    (void)addr;
    (void)value;
    (void)size;
}

static uint64_t mmix_intc_context_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    MMIXIntcContext *context = opaque;
    MMIXIntcState *s = context->intc;

    (void)size;

    if (!mmix_intc_context_active(s, context->cpu)) {
        return 0;
    }
    if (addr < MMIX_VIRT_INTC_BITMAP_WORDS * sizeof(uint64_t)) {
        return s->enable[context->cpu][addr / sizeof(uint64_t)];
    }
    if (addr == MMIX_VIRT_INTC_CONTEXT_CLAIM) {
        return mmix_intc_claim(s, context->cpu);
    }
    return 0;
}

static void mmix_intc_context_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned int size)
{
    MMIXIntcContext *context = opaque;
    MMIXIntcState *s = context->intc;

    (void)size;

    if (!mmix_intc_context_active(s, context->cpu)) {
        return;
    }
    if (addr < MMIX_VIRT_INTC_BITMAP_WORDS * sizeof(uint64_t)) {
        unsigned int word = addr / sizeof(uint64_t);

        s->enable[context->cpu][word] =
            value & mmix_intc_enable_mask(context->cpu, word);
        mmix_intc_update(s);
        return;
    }
    if (addr == MMIX_VIRT_INTC_CONTEXT_COMPLETE) {
        mmix_intc_complete(s, context->cpu, value);
    }
}

static const MemoryRegionOps mmix_intc_global_ops = {
    .read = mmix_intc_global_read,
    .write = mmix_intc_global_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .valid.unaligned = false,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps mmix_intc_context_ops = {
    .read = mmix_intc_context_read,
    .write = mmix_intc_context_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .valid.unaligned = false,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static void mmix_intc_set_irq(void *opaque, int source, int level)
{
    MMIXIntcState *s = opaque;
    unsigned int word;
    uint64_t bit;

    if (!mmix_intc_source_active(source)) {
        return;
    }

    word = mmix_intc_word(source);
    bit = mmix_intc_bit(source);
    if (level) {
        s->input_level[word] |= bit;
        if (s->owner[source] == MMIX_INTC_NO_OWNER) {
            s->pending[word] |= bit;
        }
    } else {
        s->input_level[word] &= ~bit;
        s->pending[word] &= ~bit;
    }
    mmix_intc_update(s);
}

static void mmix_intc_reset(DeviceState *dev)
{
    MMIXIntcState *s = MMIX_INTC(dev);

    memset(s->pending, 0, sizeof(s->pending));
    memset(s->input_level, 0, sizeof(s->input_level));
    memset(s->owner, 0xff, sizeof(s->owner));
    memset(s->enable, 0, sizeof(s->enable));
    mmix_intc_update(s);
}

static void mmix_intc_realize(DeviceState *dev, Error **errp)
{
    MMIXIntcState *s = MMIX_INTC(dev);
    uint32_t cpu;

    if (s->num_cpus == 0 || s->num_cpus > MMIX_VIRT_INTC_CONTEXT_COUNT) {
        error_setg(errp, "num-cpus must be between 1 and %u",
                   MMIX_VIRT_INTC_CONTEXT_COUNT);
        return;
    }

    memory_region_init(&s->container, OBJECT(s), TYPE_MMIX_INTC,
                       MMIX_VIRT_INTC_RESERVATION_SIZE);
    memory_region_init_io(&s->global_iomem, OBJECT(s), &mmix_intc_global_ops,
                          s, "mmix-intc-global",
                          MMIX_VIRT_INTC_GLOBAL_SIZE);
    memory_region_add_subregion(&s->container, 0, &s->global_iomem);

    for (cpu = 0; cpu < MMIX_VIRT_INTC_CONTEXT_COUNT; cpu++) {
        MMIXIntcContext *context = &s->context[cpu];
        g_autofree char *name = g_strdup_printf("mmix-intc-context[%u]", cpu);

        context->intc = s;
        context->cpu = cpu;
        memory_region_init_io(&context->iomem, OBJECT(s),
                              &mmix_intc_context_ops, context, name,
                              MMIX_VIRT_INTC_CONTEXT_REGISTER_SIZE);
        memory_region_add_subregion(
            &s->container,
            MMIX_VIRT_INTC_CONTEXTS_OFFSET +
            cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE,
            &context->iomem);
    }
}

static int mmix_intc_post_load(void *opaque, int version_id)
{
    MMIXIntcState *s = opaque;
    unsigned int cpu;
    unsigned int source;
    unsigned int word;

    (void)version_id;

    for (word = 0; word < MMIX_VIRT_INTC_BITMAP_WORDS; word++) {
        uint64_t active = mmix_intc_active_mask(word);

        if ((s->pending[word] | s->input_level[word]) & ~active ||
            s->pending[word] & ~s->input_level[word]) {
            return -EINVAL;
        }
        for (cpu = 0; cpu < MMIX_VIRT_INTC_CONTEXT_COUNT; cpu++) {
            uint64_t allowed = mmix_intc_context_active(s, cpu) ?
                mmix_intc_enable_mask(cpu, word) : 0;

            if (s->enable[cpu][word] & ~allowed) {
                return -EINVAL;
            }
        }
    }
    for (source = 0; source < MMIX_VIRT_INTC_IRQ_COUNT; source++) {
        int owner = s->owner[source];
        uint32_t fixed_cpu;

        if (owner != MMIX_INTC_NO_OWNER &&
            (owner < 0 || owner >= (int)s->num_cpus ||
             !mmix_intc_source_active(source) ||
             (mmix_intc_source_fixed(source, &fixed_cpu) &&
              owner != (int)fixed_cpu) ||
             (s->pending[mmix_intc_word(source)] &
              mmix_intc_bit(source)))) {
            return -EINVAL;
        }
    }
    mmix_intc_update(s);
    return 0;
}

static const VMStateDescription vmstate_mmix_intc = {
    .name = TYPE_MMIX_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mmix_intc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(pending, MMIXIntcState,
                             MMIX_VIRT_INTC_BITMAP_WORDS),
        VMSTATE_UINT64_ARRAY(input_level, MMIXIntcState,
                             MMIX_VIRT_INTC_BITMAP_WORDS),
        VMSTATE_INT16_ARRAY(owner, MMIXIntcState, MMIX_VIRT_INTC_IRQ_COUNT),
        VMSTATE_UINT64_2DARRAY(enable, MMIXIntcState,
                              MMIX_VIRT_INTC_CONTEXT_COUNT,
                              MMIX_VIRT_INTC_BITMAP_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

/* num_cpus is immutable machine configuration, not migrated device state. */
static const Property mmix_intc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", MMIXIntcState, num_cpus, 1),
};

static void mmix_intc_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    MMIXIntcState *s = MMIX_INTC(obj);
    uint32_t cpu;

    sysbus_init_mmio(dev, &s->container);
    for (cpu = 0; cpu < MMIX_VIRT_INTC_CONTEXT_COUNT; cpu++) {
        sysbus_init_irq(dev, &s->irq[cpu]);
    }
    qdev_init_gpio_in(DEVICE(obj), mmix_intc_set_irq,
                      MMIX_VIRT_INTC_IRQ_COUNT);
}

static void mmix_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    (void)data;

    device_class_set_props(dc, mmix_intc_properties);
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
