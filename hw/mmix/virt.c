/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/cpu-interrupt.h"
#include "hw/char/serial-mm.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "semihosting/semihost.h"
#include "system/system.h"
#include "target/mmix/cpu.h"
#include "target/mmix/cpu-qom.h"
#include "bootinfo.h"
#include "intc.h"
#include "kernel-loader.h"
#include "timer.h"
#include "virt.h"

#define MMIX_ARG_OCTA_SIZE 8

const MemMapEntry mmix_virt_memmap[MMIX_VIRT_MEMMAP_COUNT] = {
    [MMIX_VIRT_LOW_RAM] =      { 0, MMIX_POOL_SEGMENT_PHYS_BASE },
    [MMIX_VIRT_POOL] =         { MMIX_POOL_SEGMENT_PHYS_BASE,
                                 MMIX_POOL_SEGMENT_SIZE },
    [MMIX_VIRT_DATA] =         { MMIX_DATA_SEGMENT_PHYS_BASE,
                                 MMIX_DATA_SEGMENT_SIZE },
    [MMIX_VIRT_STACK] =        { MMIX_STACK_SEGMENT_PHYS_BASE,
                                 MMIX_STACK_SEGMENT_SIZE },
    [MMIX_VIRT_PLATFORM_RAM] = { 0x000000000e800000ULL, 0x0000000000800000ULL },
    [MMIX_VIRT_BOOTINFO] =     { 0x000000000e800000ULL, MMIX_BOOTINFO_SIZE },
    [MMIX_VIRT_FRAMEBUFFER] =  { 0x000000000f000000ULL, 0x0000000001000000ULL },
    [MMIX_VIRT_MMIO] =         { 0x0000000010000000ULL, 0 },
    [MMIX_VIRT_UART0] =        { 0x0000000010000000ULL, 0x100 },
    [MMIX_VIRT_TIMER] =        { 0x0000000010003000ULL, MMIX_VIRT_TIMER_SIZE },
    [MMIX_VIRT_INTC] =         { 0x0000000010004000ULL, MMIX_VIRT_INTC_SIZE },
};

static void mmix_virt_cpu_irq(void *opaque, int irq, int level)
{
    CPUState *cpu = opaque;

    g_assert(irq == 0);

    if (level) {
        cpu_interrupt(cpu, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cpu, CPU_INTERRUPT_HARD);
    }
}

static void mmix_bootinfo_store(uint8_t *bootinfo, MMIXBootInfoField field,
                                uint64_t value)
{
    stq_be_p(bootinfo + mmix_bootinfo_field_offset(field), value);
}

static bool mmix_write_bootinfo(MachineState *machine, uint64_t boot_cpu_id,
                                Error **errp)
{
    g_autofree uint8_t *bootinfo = NULL;
    MemTxResult result;

    if (machine->ram_size < mmix_virt_memmap[MMIX_VIRT_BOOTINFO].base ||
        machine->ram_size - mmix_virt_memmap[MMIX_VIRT_BOOTINFO].base <
        mmix_virt_memmap[MMIX_VIRT_BOOTINFO].size) {
        error_setg(errp, "MMIX boot info does not fit in machine RAM");
        return false;
    }

    bootinfo = g_malloc0(mmix_virt_memmap[MMIX_VIRT_BOOTINFO].size);

    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_MAGIC,
                        MMIX_BOOTINFO_MAGIC);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_VERSION,
                        MMIX_BOOTINFO_VERSION);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_SIZE,
                        MMIX_BOOTINFO_SIZE);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_CPU_COUNT, 1);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_BOOT_CPU_ID,
                        boot_cpu_id);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_RAM_BASE, 0);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_RAM_SIZE,
                        machine->ram_size);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_LOW_RAM_BASE,
                        mmix_virt_memmap[MMIX_VIRT_LOW_RAM].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_LOW_RAM_SIZE,
                        mmix_virt_memmap[MMIX_VIRT_LOW_RAM].size);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_POOL_LOGICAL_BASE,
                        MMIX_POOL_SEGMENT_BASE);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_POOL_PHYS_BASE,
                        mmix_virt_memmap[MMIX_VIRT_POOL].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_POOL_SIZE,
                        mmix_virt_memmap[MMIX_VIRT_POOL].size);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_DATA_LOGICAL_BASE,
                        MMIX_DATA_SEGMENT_BASE);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_DATA_PHYS_BASE,
                        mmix_virt_memmap[MMIX_VIRT_DATA].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_DATA_SIZE,
                        mmix_virt_memmap[MMIX_VIRT_DATA].size);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_STACK_LOGICAL_BASE,
                        MMIX_STACK_SEGMENT_BASE);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_STACK_PHYS_BASE,
                        mmix_virt_memmap[MMIX_VIRT_STACK].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_STACK_SIZE,
                        mmix_virt_memmap[MMIX_VIRT_STACK].size);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_MMIO_BASE,
                        mmix_virt_memmap[MMIX_VIRT_MMIO].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_UART_BASE,
                        mmix_virt_memmap[MMIX_VIRT_UART0].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_UART_IRQ,
                        MMIX_VIRT_UART0_IRQ);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_TIMER_IRQ_BASE,
                        MMIX_VIRT_TIMER_IRQ_BASE);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_TIMER_IRQ_COUNT, 1);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_INTC_BASE,
                        mmix_virt_memmap[MMIX_VIRT_INTC].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_INTC_IRQ_COUNT,
                        MMIX_VIRT_INTC_IRQ_COUNT);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_VIRTIO_MMIO_IRQ,
                        MMIX_VIRT_VIRTIO_BLOCK0_IRQ);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_IRQ,
                        MMIX_VIRT_FRAMEBUFFER_IRQ);

    result = address_space_write(&address_space_memory,
                                 mmix_virt_memmap[MMIX_VIRT_BOOTINFO].base,
                                 MEMTXATTRS_UNSPECIFIED,
                                 bootinfo,
                                 mmix_virt_memmap[MMIX_VIRT_BOOTINFO].size);
    if (result != MEMTX_OK) {
        error_setg(errp, "could not write MMIX boot info");
        return false;
    }

    return true;
}

static size_t mmix_arg_octa_align(size_t size)
{
    return QEMU_ALIGN_UP(size, MMIX_ARG_OCTA_SIZE);
}

static bool mmix_add_arg_layout_size(size_t *layout_size, size_t size,
                                     Error **errp)
{
    if (size > MMIX_POOL_SEGMENT_SIZE - *layout_size) {
        error_setg(errp, "MMIX semihosting argument block exceeds "
                   "Pool Segment size 0x%" PRIx64,
                   (uint64_t)MMIX_POOL_SEGMENT_SIZE);
        return false;
    }

    *layout_size += size;
    return true;
}

static bool mmix_argument_layout_size(int argc, size_t *layout_size,
                                      Error **errp)
{
    size_t size;
    int i;

    if (argc < 0 ||
        argc > (MMIX_POOL_SEGMENT_SIZE / MMIX_ARG_OCTA_SIZE) - 2) {
        error_setg(errp, "MMIX semihosting argument count %d exceeds "
                   "Pool Segment capacity", argc);
        return false;
    }

    size = ((size_t)argc + 2) * MMIX_ARG_OCTA_SIZE;
    for (i = 0; i < argc; i++) {
        const char *arg = semihosting_get_arg(i);
        size_t arg_size;

        if (arg == NULL) {
            error_setg(errp, "MMIX semihosting argument %d is NULL", i);
            return false;
        }

        arg_size = mmix_arg_octa_align(strlen(arg) + 1);
        if (!mmix_add_arg_layout_size(&size, arg_size, errp)) {
            return false;
        }
    }

    *layout_size = size;
    return true;
}

static bool mmix_write_semihosting_arguments(MachineState *machine,
                                             Error **errp)
{
    int argc = semihosting_get_argc();
    size_t layout_size;
    size_t string_offset;
    g_autofree uint8_t *layout = NULL;
    MemTxResult result;
    int i;

    if (!mmix_argument_layout_size(argc, &layout_size, errp)) {
        return false;
    }
    if (machine->ram_size < MMIX_POOL_SEGMENT_PHYS_BASE ||
        machine->ram_size - MMIX_POOL_SEGMENT_PHYS_BASE < layout_size) {
        error_setg(errp, "MMIX semihosting argument block does not fit "
                   "in machine RAM");
        return false;
    }

    layout = g_malloc0(layout_size);
    string_offset = ((size_t)argc + 2) * MMIX_ARG_OCTA_SIZE;

    stq_be_p(layout, MMIX_POOL_SEGMENT_BASE + layout_size);
    for (i = 0; i < argc; i++) {
        const char *arg = semihosting_get_arg(i);
        size_t arg_size = strlen(arg) + 1;

        stq_be_p(layout + ((size_t)i + 1) * MMIX_ARG_OCTA_SIZE,
                 MMIX_POOL_SEGMENT_BASE + string_offset);
        memcpy(layout + string_offset, arg, arg_size);
        string_offset += mmix_arg_octa_align(arg_size);
    }

    result = address_space_write(&address_space_memory,
                                 MMIX_POOL_SEGMENT_PHYS_BASE,
                                 MEMTXATTRS_UNSPECIFIED, layout, layout_size);
    if (result != MEMTX_OK) {
        error_setg(errp, "could not write MMIX semihosting argument block");
        return false;
    }

    return true;
}

static void mmix_apply_semihosting_startup_state(CPUState *cpu)
{
    CPUMMIXState *env = cpu_env(cpu);

    mmix_cpu_write_reg(env, 0, semihosting_get_argc());
    mmix_cpu_write_reg(env, 1, MMIX_POOL_SEGMENT_BASE + MMIX_ARG_OCTA_SIZE);
}

static void mmix_setup_semihosting_arguments(MachineState *machine,
                                             CPUState *cpu)
{
    Error *err = NULL;

    if (!semihosting_enabled(false)) {
        return;
    }

    if (!mmix_write_semihosting_arguments(machine, &err)) {
        error_reportf_err(err,
                          "could not set up MMIX semihosting arguments: ");
        exit(1);
    }

    mmix_apply_semihosting_startup_state(cpu);
}

static void mmix_apply_kernel_load_info(CPUState *cpu,
                                        const MMIXKernelLoadInfo *info)
{
    CPUMMIXState *env = cpu_env(cpu);
    unsigned reg;

    if (info->has_mmo_globals) {
        env->sregs[MMIX_SREG_RG] = info->global_base;
        for (reg = info->global_base; reg < MMIX_REGS; reg++) {
            mmix_cpu_write_reg(env, reg, info->globals[reg]);
        }
    }

    if (info->image_type == MMIX_KERNEL_IMAGE_ELF) {
        mmix_cpu_write_reg(env, 0, info->boot_cpu_id);
        mmix_cpu_write_reg(env, 1,
                           mmix_virt_memmap[MMIX_VIRT_BOOTINFO].base);
    }

    cpu_set_pc(cpu, info->entry);
}

static void mmix_virt_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    CPUState *cpu;
    DeviceState *intc;
    DeviceState *timer;
    qemu_irq cpu_irq;

    memory_region_add_subregion(sysmem, 0, machine->ram);

    cpu = cpu_create(machine->cpu_type);

    intc = qdev_new(TYPE_MMIX_INTC);
    object_property_add_child(OBJECT(machine), "intc", OBJECT(intc));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0,
                    mmix_virt_memmap[MMIX_VIRT_INTC].base);
    cpu_irq = qemu_allocate_irq(mmix_virt_cpu_irq, cpu, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(intc), 0, cpu_irq);

    timer = qdev_new(TYPE_MMIX_TIMER);
    object_property_add_child(OBJECT(machine), "timer", OBJECT(timer));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0,
                    mmix_virt_memmap[MMIX_VIRT_TIMER].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 0,
                       qdev_get_gpio_in(intc, MMIX_VIRT_TIMER_IRQ_BASE));

    serial_mm_init(sysmem, mmix_virt_memmap[MMIX_VIRT_UART0].base, 0, NULL,
                   115200, serial_hd(0), DEVICE_BIG_ENDIAN);

    if (machine->kernel_filename) {
        Error *err = NULL;
        ssize_t image_size;
        MMIXKernelLoadInfo load_info;

        image_size = mmix_load_kernel(machine->kernel_filename,
                                      machine->ram_size, &load_info, &err);
        if (image_size < 0) {
            error_reportf_err(err, "could not load MMIX kernel image: ");
            exit(1);
        }
        if (load_info.image_type == MMIX_KERNEL_IMAGE_ELF &&
            !mmix_write_bootinfo(machine, load_info.boot_cpu_id, &err)) {
            error_reportf_err(err, "could not set up MMIX boot info: ");
            exit(1);
        }
        mmix_apply_kernel_load_info(cpu, &load_info);

        if (load_info.image_type != MMIX_KERNEL_IMAGE_ELF) {
            mmix_setup_semihosting_arguments(machine, cpu);
        }
    } else {
        mmix_setup_semihosting_arguments(machine, cpu);
    }
}

static void mmix_virt_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "MMIX virtual machine";
    mc->init = mmix_virt_init;
    mc->default_cpu_type = TYPE_MMIX_ANY_CPU;
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mmix.ram";
}

static const TypeInfo mmix_virt_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("virt"),
    .parent = TYPE_MACHINE,
    .class_init = mmix_virt_class_init,
};

static void mmix_virt_machine_init(void)
{
    type_register_static(&mmix_virt_machine_typeinfo);
}

type_init(mmix_virt_machine_init)
