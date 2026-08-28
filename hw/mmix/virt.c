/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/mmix/cpu.h"
#include "target/mmix/cpu-qom.h"
#include "intc.h"
#include "ipi.h"
#include "physical-layout.h"
#include "ram-layout.h"
#include "timer.h"
#include "virt.h"

#define TYPE_MMIX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")

OBJECT_DECLARE_SIMPLE_TYPE(MMIXVirtMachineState, MMIX_VIRT_MACHINE)

typedef bool (*MMIXCreateDefaultMemdev)(MachineState *machine,
                                        const char *path, Error **errp);

struct MMIXVirtMachineState {
    MachineState parent_obj;
    MMIXPhysicalRAM ram;
    CPUState *cpus[MMIX_VIRT_MAX_CPUS];
    qemu_irq cpu_irqs[MMIX_VIRT_MAX_CPUS];
};

static MMIXCreateDefaultMemdev mmix_parent_create_default_memdev;

static void mmix_virt_cpu_irq(void *opaque, int irq, int level)
{
    g_assert(irq == 0);
    mmix_cpu_set_interrupt_controller(opaque, level);
}

static uint64_t mmix_virt_initial_stack(unsigned int cpu_index)
{
    return MMIX_VIRT_INITIAL_STACK_BASE +
           cpu_index * MMIX_VIRT_INITIAL_STACK_SLOT_SIZE;
}

static bool mmix_virt_validate_memory(MachineState *machine, Error **errp)
{
    uint64_t size = machine->ram_size;

    if (size < MMIX_VIRT_RAM_MIN_SIZE) {
        error_setg(errp,
                   "MMIX virt RAM size 0x%" PRIx64
                   " is below the minimum 0x%" PRIx64,
                   size, MMIX_VIRT_RAM_MIN_SIZE);
        return false;
    }
    if (size > MMIX_VIRT_RAM_MAX_SIZE) {
        error_setg(errp,
                   "MMIX virt RAM size 0x%" PRIx64
                   " exceeds the maximum 0x%" PRIx64,
                   size, MMIX_VIRT_RAM_MAX_SIZE);
        return false;
    }
    if (size % MMIX_VIRT_RAM_ALIGN != 0) {
        error_setg(errp,
                   "MMIX virt RAM size 0x%" PRIx64
                   " must be aligned to 0x%x bytes",
                   size, MMIX_VIRT_RAM_ALIGN);
        return false;
    }
    if (machine->maxram_size != size) {
        error_setg(errp,
                   "MMIX virt does not support maxmem 0x%" PRIx64,
                   machine->maxram_size);
        return false;
    }
    if (machine->ram_slots) {
        error_setg(errp,
                   "MMIX virt does not support memory slots 0x%" PRIx64,
                   machine->ram_slots);
        return false;
    }

    return true;
}

static bool mmix_virt_create_default_memdev(MachineState *machine,
                                            const char *path, Error **errp)
{
    if (!mmix_virt_validate_memory(machine, errp)) {
        return false;
    }

    return mmix_parent_create_default_memdev(machine, path, errp);
}

static bool mmix_virt_build_ram(MMIXVirtMachineState *vms, Error **errp)
{
    MachineState *machine = MACHINE(vms);

    if (!mmix_physical_ram_init(&vms->ram, machine->ram_size)) {
        error_setg(errp, "could not represent MMIX virt RAM size 0x%" PRIx64,
                   machine->ram_size);
        return false;
    }
    if (!mmix_physical_ram_contains(&vms->ram,
                                    MMIX_VIRT_INITIAL_STACK_BASE,
                                    MMIX_VIRT_INITIAL_STACK_AREA_SIZE)) {
        error_setg(errp, "MMIX initial stack area does not fit in RAM");
        return false;
    }

    return true;
}

static CPUState *mmix_virt_create_cpu(MMIXVirtMachineState *vms,
                                      unsigned int cpu_index)
{
    MachineState *machine = MACHINE(vms);
    g_autofree char *name = g_strdup_printf("cpu[%u]", cpu_index);
    Object *cpuobj = object_new(machine->cpu_type);
    CPUState *cpu = CPU(cpuobj);
    uint64_t initial_stack = object_property_get_uint(
        cpuobj, "initial-stack", &error_fatal);

    cpu->cpu_index = cpu_index;
    if (machine->smp.cpus != 1 || cpu_index != 0 ||
        initial_stack == MMIX_INITIAL_STACK) {
        object_property_set_uint(cpuobj, "initial-stack",
                                 mmix_virt_initial_stack(cpu_index),
                                 &error_fatal);
    }
    object_property_add_child(OBJECT(machine), name, cpuobj);
    qdev_realize_and_unref(DEVICE(cpuobj), NULL, &error_fatal);
    vms->cpus[cpu_index] = cpu;
    return cpu;
}

static void mmix_virt_reset(MachineState *machine, ResetType type)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(machine);
    unsigned int i;

    qemu_devices_reset(type);
    if (type == RESET_TYPE_SNAPSHOT_LOAD) {
        return;
    }

    for (i = 0; i < machine->smp.cpus; i++) {
        cpu_reset(vms->cpus[i]);
    }
}

static void mmix_virt_init(MachineState *machine)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(machine);
    DeviceState *intc;
    DeviceState *ipi;
    DeviceState *timer;
    unsigned int i;

    if (!mmix_virt_validate_memory(machine, &error_fatal) ||
        !mmix_virt_build_ram(vms, &error_fatal)) {
        return;
    }
    if (machine->kernel_filename) {
        error_report("MMIX virt -kernel loading is unavailable during the "
                     "physical layout transition");
        exit(EXIT_FAILURE);
    }

    memory_region_add_subregion(get_system_memory(), vms->ram.start,
                                machine->ram);

    for (i = 0; i < machine->smp.cpus; i++) {
        mmix_virt_create_cpu(vms, i);
    }

    intc = qdev_new(TYPE_MMIX_INTC);
    object_property_add_child(OBJECT(machine), "intc", OBJECT(intc));
    qdev_prop_set_uint32(intc, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, MMIX_VIRT_INTC_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        vms->cpu_irqs[i] = qemu_allocate_irq(mmix_virt_cpu_irq,
                                              vms->cpus[i], 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(intc), i, vms->cpu_irqs[i]);
    }

    ipi = qdev_new(TYPE_MMIX_IPI);
    object_property_add_child(OBJECT(machine), "ipi", OBJECT(ipi));
    qdev_prop_set_uint32(ipi, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ipi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ipi), 0, MMIX_VIRT_IPI_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(ipi), i,
                           qdev_get_gpio_in_named(DEVICE(vms->cpus[i]),
                                                  "ipi", 0));
    }

    timer = qdev_new(TYPE_MMIX_TIMER);
    object_property_add_child(OBJECT(machine), "timer", OBJECT(timer));
    qdev_prop_set_uint32(timer, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, MMIX_VIRT_TIMER_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc,
                                           MMIX_VIRT_TIMER_IRQ_BASE + i));
    }
}

static void mmix_virt_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "MMIX virtual machine";
    mc->init = mmix_virt_init;
    mc->reset = mmix_virt_reset;
    mc->default_cpu_type = TYPE_MMIX_ANY_CPU;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = MMIX_VIRT_MAX_CPUS;
    mc->default_ram_size = MMIX_VIRT_RAM_DEFAULT_SIZE;
    mc->default_ram_id = "mmix.ram";
    mmix_parent_create_default_memdev = mc->create_default_memdev;
    mc->create_default_memdev = mmix_virt_create_default_memdev;
}

static const TypeInfo mmix_virt_machine_typeinfo = {
    .name = TYPE_MMIX_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = mmix_virt_class_init,
    .instance_size = sizeof(MMIXVirtMachineState),
};

static void mmix_virt_machine_init_register_types(void)
{
    type_register_static(&mmix_virt_machine_typeinfo);
}

type_init(mmix_virt_machine_init_register_types)
