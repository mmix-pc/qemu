/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "system/system.h"
#include "target/mmix/cpu-qom.h"

static void mmix_virt_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    CPUState *cpu;

    memory_region_add_subregion(sysmem, 0, machine->ram);

    cpu = cpu_create(machine->cpu_type);
    if (machine->kernel_filename) {
        load_image_targphys(machine->kernel_filename, 0, machine->ram_size, NULL);
        cpu_set_pc(cpu, 0);
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
