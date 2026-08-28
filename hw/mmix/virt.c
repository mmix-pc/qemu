/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/virtio/virtio-mmio.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/mmix/cpu.h"
#include "target/mmix/cpu-qom.h"
#include "boot-plan.h"
#include "framebuffer.h"
#include "intc.h"
#include "ipi.h"
#include "kernel-loader.h"
#include "physical-layout.h"
#include "ram-layout.h"
#include "ram-reservation.h"
#include "timer.h"
#include "virt.h"

#define TYPE_MMIX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")

OBJECT_DECLARE_SIMPLE_TYPE(MMIXVirtMachineState, MMIX_VIRT_MACHINE)

typedef bool (*MMIXCreateDefaultMemdev)(MachineState *machine,
                                        const char *path, Error **errp);

struct MMIXVirtMachineState {
    MachineState parent_obj;
    MMIXPhysicalRAM ram;
    MMIXBootPlan *boot_plan;
    CPUState *cpus[MMIX_VIRT_MAX_CPUS];
    uint64_t initial_stacks[MMIX_VIRT_MAX_CPUS];
    qemu_irq cpu_irqs[MMIX_VIRT_MAX_CPUS];
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
};

static MMIXCreateDefaultMemdev mmix_parent_create_default_memdev;

static void mmix_virt_cpu_irq(void *opaque, int irq, int level)
{
    g_assert(irq == 0);
    mmix_cpu_set_interrupt_controller(opaque, level);
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
    return true;
}

static bool mmix_virt_plan_ram(MMIXVirtMachineState *vms, Error **errp)
{
    MachineState *machine = MACHINE(vms);
    enum {
        MMIX_RAM_REQUEST_FRAMEBUFFER,
        MMIX_RAM_REQUEST_STACK_BASE,
    };
    size_t request_count = MMIX_RAM_REQUEST_STACK_BASE + machine->smp.cpus;
    g_autofree MMIXRAMReservationRequest *requests =
        g_new0(MMIXRAMReservationRequest, request_count);
    g_auto(GStrv) stack_names = g_new0(char *, machine->smp.cpus + 1);
    const MMIXRAMReservationRequest framebuffer_request = {
        .owner = "mmix-framebuffer",
        .name = "pixels",
        .stable_id = 0,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_MACHINE,
        .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
        .placement_class = MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
        .size = MMIX_VIRT_FRAMEBUFFER_SIZE,
        .alignment = MMIX_VIRT_FRAMEBUFFER_ALIGN,
    };
    const MMIXRAMReservation *framebuffer;
    unsigned int i;

    requests[MMIX_RAM_REQUEST_FRAMEBUFFER] = framebuffer_request;
    for (i = 0; i < machine->smp.cpus; i++) {
        stack_names[i] = g_strdup_printf("initial-stack-%u", i);
        requests[MMIX_RAM_REQUEST_STACK_BASE + i] =
            (MMIXRAMReservationRequest) {
                .owner = "mmix-cpu",
                .name = stack_names[i],
                .stable_id = i,
                .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
                .ownership_class = MMIX_RAM_OWNERSHIP_CPU_BOOTSTRAP,
                .lifetime = MMIX_RAM_LIFETIME_UNTIL_GUEST_RELEASE,
                .placement_class = MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
                .size = MMIX_VIRT_INITIAL_STACK_SIZE,
                .alignment = MMIX_VIRT_INITIAL_STACK_ALIGN,
            };
    }

    if (!mmix_boot_plan_build(machine->ram_size, NULL, NULL, requests,
                              request_count, &vms->boot_plan, errp)) {
        return false;
    }

    framebuffer = mmix_boot_plan_reservation(
        vms->boot_plan, MMIX_RAM_REQUEST_FRAMEBUFFER);
    vms->framebuffer_base = framebuffer->content.start;
    vms->framebuffer_size =
        mmix_phys_range_size(&framebuffer->content);
    for (i = 0; i < machine->smp.cpus; i++) {
        const MMIXRAMReservation *stack = mmix_boot_plan_reservation(
            vms->boot_plan, MMIX_RAM_REQUEST_STACK_BASE + i);

        vms->initial_stacks[i] = stack->content.start;
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

    cpu->cpu_index = cpu_index;
    object_property_set_uint(cpuobj, "initial-stack",
                             vms->initial_stacks[cpu_index], &error_fatal);
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
        MemTxResult result = address_space_set(
            &address_space_memory, vms->initial_stacks[i], 0,
            MMIX_VIRT_INITIAL_STACK_SIZE, MEMTXATTRS_UNSPECIFIED);

        g_assert(result == MEMTX_OK);
        cpu_reset(vms->cpus[i]);
    }
}

static void mmix_virt_reject_kernel(MachineState *machine)
{
    MMIXKernelImageType type;
    Error *err = NULL;

    if (!mmix_classify_kernel_image(machine->kernel_filename, &type, &err)) {
        error_report_err(err);
        exit(EXIT_FAILURE);
    }

    switch (type) {
    case MMIX_KERNEL_IMAGE_MMO:
        error_report("MMIX MMO -kernel loading is unavailable until sparse "
                     "memory support is implemented");
        break;
    case MMIX_KERNEL_IMAGE_ELF:
        error_report("MMIX ELF -kernel loading is not yet implemented for "
                     "the contiguous RAM layout");
        break;
    case MMIX_KERNEL_IMAGE_RAW:
        error_report("MMIX raw -kernel loading is not yet implemented for "
                     "the contiguous RAM layout");
        break;
    default:
        g_assert_not_reached();
    }

    exit(EXIT_FAILURE);
}

static void mmix_virt_init(MachineState *machine)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(machine);
    DeviceState *intc;
    DeviceState *ipi;
    DeviceState *timer;
    DeviceState *framebuffer;
    unsigned int i;

    if (!mmix_virt_validate_memory(machine, &error_fatal) ||
        !mmix_virt_build_ram(vms, &error_fatal) ||
        !mmix_virt_plan_ram(vms, &error_fatal)) {
        return;
    }
    if (machine->kernel_filename) {
        mmix_virt_reject_kernel(machine);
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

    serial_mm_init(get_system_memory(), MMIX_VIRT_UART0_BASE,
                   MMIX_VIRT_UART0_REGISTER_SHIFT,
                   qdev_get_gpio_in(intc, MMIX_VIRT_UART0_IRQ),
                   MMIX_VIRT_UART0_BAUD_BASE, serial_hd(0),
                   DEVICE_BIG_ENDIAN);

    framebuffer = qdev_new(TYPE_MMIX_FRAMEBUFFER);
    object_property_add_child(OBJECT(machine), "framebuffer",
                              OBJECT(framebuffer));
    qdev_prop_set_uint64(framebuffer, "base", vms->framebuffer_base);
    qdev_prop_set_uint64(framebuffer, "size", vms->framebuffer_size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(framebuffer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(framebuffer), 0,
                    MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE);

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

    /*
     * Realization prepends each bus to QEMU's default-bus search order.
     * Creating high slots first makes command-line devices fill from slot 0.
     */
    for (i = MMIX_VIRT_VIRTIO_MMIO_COUNT; i-- > 0;) {
        g_autofree char *name = g_strdup_printf("virtio-mmio[%u]", i);
        DeviceState *virtio_mmio = qdev_new(TYPE_VIRTIO_MMIO);

        object_property_add_child(OBJECT(machine), name, OBJECT(virtio_mmio));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(virtio_mmio), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(virtio_mmio), 0,
                        MMIX_VIRT_VIRTIO_MMIO_BASE +
                        i * MMIX_VIRT_VIRTIO_MMIO_STRIDE);
        sysbus_connect_irq(SYS_BUS_DEVICE(virtio_mmio), 0,
                           qdev_get_gpio_in(intc,
                                            MMIX_VIRT_VIRTIO_MMIO_IRQ_BASE +
                                            i));
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

static void mmix_virt_instance_finalize(Object *obj)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    mmix_boot_plan_free(vms->boot_plan);
}

static const TypeInfo mmix_virt_machine_typeinfo = {
    .name = TYPE_MMIX_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = mmix_virt_class_init,
    .instance_size = sizeof(MMIXVirtMachineState),
    .instance_finalize = mmix_virt_instance_finalize,
};

static void mmix_virt_machine_init_register_types(void)
{
    type_register_static(&mmix_virt_machine_typeinfo);
}

type_init(mmix_virt_machine_init_register_types)
