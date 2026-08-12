/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/char/xilinx_uartlite.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "semihosting/semihost.h"
#include "system/system.h"
#include "target/mmix/cpu.h"
#include "target/mmix/cpu-qom.h"
#include "bootinfo.h"
#include "kernel-loader.h"

#define MMIX_VIRT_UART_BASE 0x100000000ULL
#define MMIX_ARG_OCTA_SIZE 8

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
    }

    cpu_set_pc(cpu, info->entry);
}

static void mmix_virt_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    CPUState *cpu;
    DeviceState *dev;

    memory_region_add_subregion(sysmem, 0, machine->ram);

    cpu = cpu_create(machine->cpu_type);

    /*
     * Reuse UARTLite as a small polling MMIO serial device. The MMIX
     * machine only relies on its simple RX/TX/status register contract and
     * QEMU chardev plumbing; it does not model a Xilinx platform, and the
     * UART IRQ line is intentionally left unconnected.
     */
    dev = qdev_new(TYPE_XILINX_UARTLITE);
    qdev_prop_set_enum(dev, "endianness", ENDIAN_MODE_BIG);
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, MMIX_VIRT_UART_BASE);

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
        mmix_apply_kernel_load_info(cpu, &load_info);
    }

    mmix_setup_semihosting_arguments(machine, cpu);
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
