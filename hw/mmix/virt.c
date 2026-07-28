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
#include "system/system.h"
#include "target/mmix/cpu-qom.h"
#include "mmo-loader.h"

#define MMIX_VIRT_UART_BASE 0x100000000ULL

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
        hwaddr entry;

        image_size = mmix_load_kernel(machine->kernel_filename,
                                      machine->ram_size, &entry, &err);
        if (image_size < 0) {
            error_reportf_err(err, "could not load MMIX kernel image: ");
            exit(1);
        }
        cpu_set_pc(cpu, entry);
    }
}

static void mmix_virt_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "MMIX virtual machine";
    mc->init = mmix_virt_init;
    mc->default_cpu_type = TYPE_MMIX_ANY_CPU;
    mc->default_ram_size = 128 * MiB;
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
