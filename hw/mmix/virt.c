/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/char/serial-mm.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-mmio.h"
#include "semihosting/semihost.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/mmix/cpu.h"
#include "target/mmix/cpu-qom.h"
#include "bootinfo.h"
#include "framebuffer.h"
#include "intc.h"
#include "ipi.h"
#include "kernel-loader.h"
#include "timer.h"
#include "virt.h"

#define MMIX_ARG_OCTA_SIZE 8
#define TYPE_MMIX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")

typedef enum MMIXELFStartupABI {
    MMIX_ELF_STARTUP_ABI_BOOTINFO,
    MMIX_ELF_STARTUP_ABI_ARGC_ARGV,
} MMIXELFStartupABI;

OBJECT_DECLARE_SIMPLE_TYPE(MMIXVirtMachineState, MMIX_VIRT_MACHINE)

struct MMIXVirtMachineState {
    MachineState parent_obj;
    MMIXELFStartupABI elf_startup_abi;
    CPUState *cpus[MMIX_VIRT_MAX_CPUS];
    qemu_irq cpu_irqs[MMIX_VIRT_MAX_CPUS];
    MMIXKernelLoadInfo kernel_load_info;
    bool kernel_loaded;
};

const MemMapEntry mmix_virt_memmap[MMIX_VIRT_MEMMAP_COUNT] = {
    [MMIX_VIRT_LOW_RAM] =      { 0, MMIX_POOL_SEGMENT_PHYS_BASE },
    [MMIX_VIRT_INITIAL_STACKS] = {
        MMIX_VIRT_INITIAL_STACK_BASE, MMIX_VIRT_INITIAL_STACK_AREA_SIZE
    },
    [MMIX_VIRT_POOL] =         { MMIX_POOL_SEGMENT_PHYS_BASE,
                                 MMIX_POOL_SEGMENT_SIZE },
    [MMIX_VIRT_DATA] =         { MMIX_DATA_SEGMENT_PHYS_BASE,
                                 MMIX_DATA_SEGMENT_SIZE },
    [MMIX_VIRT_STACK] =        { MMIX_STACK_SEGMENT_PHYS_BASE,
                                 MMIX_STACK_SEGMENT_SIZE },
    [MMIX_VIRT_PLATFORM_RAM] = { 0x000000000e800000ULL, 0x0000000000800000ULL },
    [MMIX_VIRT_BOOTINFO] =     { 0x000000000e800000ULL, MMIX_BOOTINFO_SIZE },
    [MMIX_VIRT_KERNEL_CMDLINE] = {
        0x000000000e800000ULL + MMIX_BOOTINFO_SIZE,
        MMIX_BOOTINFO_KERNEL_CMDLINE_STORAGE_SIZE
    },
    [MMIX_VIRT_FRAMEBUFFER] =  { 0x000000000f000000ULL, 0x0000000001000000ULL },
    [MMIX_VIRT_MMIO] =         { 0x0000000010000000ULL, 0 },
    [MMIX_VIRT_UART0] =        { 0x0000000010000000ULL, 0x100 },
    [MMIX_VIRT_VIRTIO_MMIO] =  { 0x0000000010001000ULL,
                                 MMIX_VIRT_VIRTIO_MMIO_SIZE },
    [MMIX_VIRT_FRAMEBUFFER_CONTROL] = {
        0x0000000010002000ULL, MMIX_VIRT_FRAMEBUFFER_CONTROL_MMIO_SIZE
    },
    [MMIX_VIRT_TIMER] =        { 0x0000000010003000ULL, MMIX_VIRT_TIMER_SIZE },
    [MMIX_VIRT_INTC] =         { 0x0000000010004000ULL, MMIX_VIRT_INTC_SIZE },
    [MMIX_VIRT_IPI] =          { 0x0000000010006000ULL, MMIX_VIRT_IPI_SIZE },
};

static void mmix_virt_cpu_irq(void *opaque, int irq, int level)
{
    CPUState *cpu = opaque;

    g_assert(irq == 0);
    mmix_cpu_set_interrupt_controller(cpu, level);
}

static void mmix_bootinfo_store(uint8_t *bootinfo, MMIXBootInfoField field,
                                uint64_t value)
{
    stq_be_p(bootinfo + mmix_bootinfo_field_offset(field), value);
}

static bool mmix_range_contains(const MemMapEntry *container,
                                const MemMapEntry *range)
{
    uint64_t offset;

    if (range->base < container->base) {
        return false;
    }
    offset = range->base - container->base;
    return offset <= container->size &&
           range->size <= container->size - offset;
}

static bool mmix_range_fits_ram(const MachineState *machine,
                                const MemMapEntry *range)
{
    return range->base <= machine->ram_size &&
           range->size <= machine->ram_size - range->base;
}

static bool mmix_ranges_overlap(const MemMapEntry *a, const MemMapEntry *b)
{
    if (a->base <= b->base) {
        return a->size > b->base - a->base;
    }
    return b->size > a->base - b->base;
}

static uint64_t mmix_virt_initial_stack(unsigned int cpu_index)
{
    return MMIX_VIRT_INITIAL_STACK_BASE +
           cpu_index * MMIX_VIRT_INITIAL_STACK_SLOT_SIZE;
}

static bool mmix_validate_initial_stack_layout(const MachineState *machine,
                                               Error **errp)
{
    const MemMapEntry *area =
        &mmix_virt_memmap[MMIX_VIRT_INITIAL_STACKS];
    const MMIXVirtMemMap excluded[] = {
        MMIX_VIRT_POOL,
        MMIX_VIRT_DATA,
        MMIX_VIRT_STACK,
        MMIX_VIRT_PLATFORM_RAM,
        MMIX_VIRT_BOOTINFO,
        MMIX_VIRT_KERNEL_CMDLINE,
        MMIX_VIRT_FRAMEBUFFER,
    };
    uint64_t previous_end = area->base;
    unsigned int i;

    if (MMIX_VIRT_INITIAL_STACK_BASE != MMIX_INITIAL_STACK) {
        error_setg(errp, "MMIX CPU0 initial stack address changed");
        return false;
    }
    if (MMIX_VIRT_INITIAL_STACK_SLOT_SIZE == 0 ||
        MMIX_VIRT_INITIAL_STACK_SLOT_SIZE % MMIX_OCTA_SIZE != 0 ||
        MMIX_VIRT_INITIAL_STACK_BASE % MMIX_OCTA_SIZE != 0) {
        error_setg(errp, "MMIX initial stack slots are not octa-aligned");
        return false;
    }
    if (MMIX_VIRT_MAX_CPUS >
        (UINT64_MAX - MMIX_VIRT_INITIAL_STACK_BASE) /
        MMIX_VIRT_INITIAL_STACK_SLOT_SIZE) {
        error_setg(errp, "MMIX initial stack slot address overflows");
        return false;
    }
    if (area->size != MMIX_VIRT_INITIAL_STACK_AREA_SIZE ||
        !mmix_range_contains(&mmix_virt_memmap[MMIX_VIRT_LOW_RAM], area) ||
        !mmix_range_fits_ram(machine, area)) {
        error_setg(errp, "MMIX initial stack slots do not fit in Low RAM");
        return false;
    }

    for (i = 0; i < ARRAY_SIZE(excluded); i++) {
        if (mmix_ranges_overlap(area, &mmix_virt_memmap[excluded[i]])) {
            error_setg(errp,
                       "MMIX initial stack slots overlap reserved memory");
            return false;
        }
    }

    for (i = 0; i < MMIX_VIRT_MAX_CPUS; i++) {
        MemMapEntry slot = {
            .base = mmix_virt_initial_stack(i),
            .size = MMIX_VIRT_INITIAL_STACK_SLOT_SIZE,
        };

        if (slot.base != previous_end || !mmix_range_contains(area, &slot)) {
            error_setg(errp,
                       "MMIX initial stack slots overlap or leave gaps");
            return false;
        }
        previous_end = slot.base + slot.size;
    }
    if (previous_end != area->base + area->size) {
        error_setg(errp, "MMIX initial stack slot range is incomplete");
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

static bool mmix_validate_bootinfo_layout(const MachineState *machine,
                                          size_t cmdline_size, Error **errp)
{
    const MemMapEntry *platform =
        &mmix_virt_memmap[MMIX_VIRT_PLATFORM_RAM];
    const MemMapEntry *bootinfo = &mmix_virt_memmap[MMIX_VIRT_BOOTINFO];
    const MemMapEntry *cmdline =
        &mmix_virt_memmap[MMIX_VIRT_KERNEL_CMDLINE];

    if (!mmix_range_contains(platform, bootinfo) ||
        !mmix_range_fits_ram(machine, bootinfo)) {
        error_setg(errp, "MMIX boot info does not fit in platform RAM");
        return false;
    }
    if (cmdline->base < bootinfo->base ||
        cmdline->base - bootinfo->base < bootinfo->size ||
        !mmix_range_contains(platform, cmdline)) {
        error_setg(errp, "MMIX kernel command line overlaps platform data");
        return false;
    }
    if (cmdline_size > MMIX_BOOTINFO_KERNEL_CMDLINE_MAX) {
        error_setg(errp,
                   "MMIX kernel command line exceeds maximum length %u",
                   MMIX_BOOTINFO_KERNEL_CMDLINE_MAX);
        return false;
    }
    if (cmdline_size != 0 && !mmix_range_fits_ram(machine, cmdline)) {
        error_setg(errp,
                   "MMIX kernel command line does not fit in machine RAM");
        return false;
    }

    return true;
}

static bool mmix_write_bootinfo(MachineState *machine, uint64_t boot_cpu_id,
                                Error **errp)
{
    const char *cmdline = machine->kernel_cmdline;
    const MemMapEntry *cmdline_region =
        &mmix_virt_memmap[MMIX_VIRT_KERNEL_CMDLINE];
    g_autofree uint8_t *bootinfo = NULL;
    size_t cmdline_size = cmdline ? strlen(cmdline) : 0;
    uint64_t flags = 0;
    MemTxResult result;

    if (!mmix_validate_bootinfo_layout(machine, cmdline_size, errp)) {
        return false;
    }

    bootinfo = g_malloc0(mmix_virt_memmap[MMIX_VIRT_BOOTINFO].size);

    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_MAGIC,
                        MMIX_BOOTINFO_MAGIC);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_VERSION,
                        MMIX_BOOTINFO_VERSION);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_SIZE,
                        MMIX_BOOTINFO_SIZE);
    if (cmdline_size != 0) {
        flags |= MMIX_BOOTINFO_FLAG_KERNEL_CMDLINE;
    }
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FLAGS, flags);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_CPU_COUNT,
                        machine->smp.cpus);
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
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_TIMER_BASE,
                        mmix_virt_memmap[MMIX_VIRT_TIMER].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_TIMER_IRQ_BASE,
                        MMIX_VIRT_TIMER_IRQ_BASE);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_TIMER_IRQ_COUNT,
                        machine->smp.cpus);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_INTC_BASE,
                        mmix_virt_memmap[MMIX_VIRT_INTC].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_INTC_IRQ_COUNT,
                        MMIX_VIRT_INTC_IRQ_COUNT);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_VIRTIO_MMIO_BASE,
                        mmix_virt_memmap[MMIX_VIRT_VIRTIO_MMIO].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_VIRTIO_MMIO_IRQ,
                        MMIX_VIRT_VIRTIO_BLOCK0_IRQ);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_VIRTIO_MMIO_COUNT,
                        MMIX_VIRT_VIRTIO_MMIO_COUNT);
    mmix_bootinfo_store(
        bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_CONTROL_BASE,
        mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER_CONTROL].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_BASE,
                        mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER].base);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_SIZE,
                        mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER].size);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_IRQ,
                        MMIX_VIRT_FRAMEBUFFER_IRQ);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_WIDTH,
                        MMIX_VIRT_FRAMEBUFFER_WIDTH);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_HEIGHT,
                        MMIX_VIRT_FRAMEBUFFER_HEIGHT);
    mmix_bootinfo_store(bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_STRIDE,
                        MMIX_VIRT_FRAMEBUFFER_STRIDE);
    mmix_bootinfo_store(
        bootinfo, MMIX_BOOTINFO_FIELD_FRAMEBUFFER_FORMAT,
        MMIX_VIRT_FRAMEBUFFER_FORMAT_XRGB8888);
    if (cmdline_size != 0) {
        mmix_bootinfo_store(bootinfo,
                            MMIX_BOOTINFO_FIELD_KERNEL_CMDLINE_ADDR,
                            cmdline_region->base);
        mmix_bootinfo_store(bootinfo,
                            MMIX_BOOTINFO_FIELD_KERNEL_CMDLINE_SIZE,
                            cmdline_size);
    }

    result = address_space_write(&address_space_memory,
                                 mmix_virt_memmap[MMIX_VIRT_BOOTINFO].base,
                                 MEMTXATTRS_UNSPECIFIED,
                                 bootinfo,
                                 mmix_virt_memmap[MMIX_VIRT_BOOTINFO].size);
    if (result != MEMTX_OK) {
        error_setg(errp, "could not write MMIX boot info");
        return false;
    }

    if (cmdline_size != 0) {
        result = address_space_write(&address_space_memory,
                                     cmdline_region->base,
                                     MEMTXATTRS_UNSPECIFIED,
                                     cmdline, cmdline_size + 1);
        if (result != MEMTX_OK) {
            error_setg(errp, "could not write MMIX kernel command line");
            return false;
        }
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

static void mmix_apply_kernel_load_state(CPUState *cpu,
                                         const MMIXKernelLoadInfo *info)
{
    CPUMMIXState *env = cpu_env(cpu);
    unsigned reg;

    if (info->has_global_registers) {
        env->sregs[MMIX_SREG_RG] = info->global_base;
        for (reg = info->global_base;
             reg < info->global_base + info->global_count; reg++) {
            mmix_cpu_write_reg(env, reg, info->globals[reg]);
        }
    }

    cpu_set_pc(cpu, info->entry);
}

static void mmix_apply_elf_bootinfo_startup_state(CPUState *cpu)
{
    CPUMMIXState *env = cpu_env(cpu);

    mmix_cpu_write_reg(env, 0, cpu->cpu_index);
    mmix_cpu_write_reg(env, 1,
                       mmix_virt_memmap[MMIX_VIRT_BOOTINFO].base);
}

static bool mmix_prepare_elf_startup(MMIXVirtMachineState *vms,
                                     const MMIXKernelLoadInfo *info,
                                     Error **errp)
{
    MachineState *machine = MACHINE(vms);

    g_assert(info->image_type == MMIX_KERNEL_IMAGE_ELF);

    switch (vms->elf_startup_abi) {
    case MMIX_ELF_STARTUP_ABI_BOOTINFO:
        if (!mmix_write_bootinfo(machine, info->boot_cpu_id, errp)) {
            error_prepend(errp, "could not set up MMIX boot info: ");
            return false;
        }
        return true;
    case MMIX_ELF_STARTUP_ABI_ARGC_ARGV:
        if (!semihosting_enabled(false)) {
            error_setg(errp, "MMIX ELF startup ABI 'argc-argv' requires "
                       "semihosting");
            return false;
        }
        if (machine->smp.cpus != 1) {
            error_setg(errp, "MMIX ELF startup ABI 'argc-argv' supports "
                       "only one CPU");
            return false;
        }
        return true;
    default:
        g_assert_not_reached();
    }
}

static void mmix_apply_elf_startup_state(MMIXVirtMachineState *vms,
                                         CPUState *cpu)
{
    switch (vms->elf_startup_abi) {
    case MMIX_ELF_STARTUP_ABI_BOOTINFO:
        mmix_apply_elf_bootinfo_startup_state(cpu);
        return;
    case MMIX_ELF_STARTUP_ABI_ARGC_ARGV:
        mmix_setup_semihosting_arguments(MACHINE(vms), cpu);
        return;
    default:
        g_assert_not_reached();
    }
}

static void mmix_start_loaded_kernel(MMIXVirtMachineState *vms,
                                     const MMIXKernelLoadInfo *info)
{
    MachineState *machine = MACHINE(vms);
    unsigned int i;

    if (info->image_type == MMIX_KERNEL_IMAGE_ELF) {
        Error *err = NULL;

        if (!mmix_prepare_elf_startup(vms, info, &err)) {
            error_report_err(err);
            exit(1);
        }

        for (i = 0; i < machine->smp.cpus; i++) {
            mmix_apply_kernel_load_state(vms->cpus[i], info);
            mmix_apply_elf_startup_state(vms, vms->cpus[i]);
        }
    } else {
        mmix_apply_kernel_load_state(vms->cpus[0], info);
        mmix_setup_semihosting_arguments(machine, vms->cpus[0]);
    }
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

    if (vms->kernel_loaded) {
        mmix_start_loaded_kernel(vms, &vms->kernel_load_info);
    } else {
        mmix_setup_semihosting_arguments(machine, vms->cpus[0]);
    }
}

static void mmix_virt_init(MachineState *machine)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *framebuffer;
    DeviceState *intc;
    DeviceState *ipi;
    DeviceState *timer;
    DeviceState *virtio_mmio;
    unsigned int i;

    if (!mmix_validate_initial_stack_layout(machine, &error_fatal)) {
        return;
    }

    memory_region_add_subregion(sysmem, 0, machine->ram);

    for (i = 0; i < machine->smp.cpus; i++) {
        mmix_virt_create_cpu(vms, i);
    }
    intc = qdev_new(TYPE_MMIX_INTC);
    object_property_add_child(OBJECT(machine), "intc", OBJECT(intc));
    qdev_prop_set_uint32(intc, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0,
                    mmix_virt_memmap[MMIX_VIRT_INTC].base);
    for (i = 0; i < machine->smp.cpus; i++) {
        vms->cpu_irqs[i] = qemu_allocate_irq(mmix_virt_cpu_irq,
                                              vms->cpus[i], 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(intc), i, vms->cpu_irqs[i]);
    }

    ipi = qdev_new(TYPE_MMIX_IPI);
    object_property_add_child(OBJECT(machine), "ipi", OBJECT(ipi));
    qdev_prop_set_uint32(ipi, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ipi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ipi), 0,
                    mmix_virt_memmap[MMIX_VIRT_IPI].base);

    timer = qdev_new(TYPE_MMIX_TIMER);
    object_property_add_child(OBJECT(machine), "timer", OBJECT(timer));
    qdev_prop_set_uint32(timer, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0,
                    mmix_virt_memmap[MMIX_VIRT_TIMER].base);
    for (i = 0; i < machine->smp.cpus; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc,
                                           MMIX_VIRT_TIMER_IRQ_BASE + i));
    }

    virtio_mmio = qdev_new(TYPE_VIRTIO_MMIO);
    object_property_add_child(OBJECT(machine), "virtio-mmio",
                              OBJECT(virtio_mmio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(virtio_mmio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(virtio_mmio), 0,
                    mmix_virt_memmap[MMIX_VIRT_VIRTIO_MMIO].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(virtio_mmio), 0,
                       qdev_get_gpio_in(intc, MMIX_VIRT_VIRTIO_BLOCK0_IRQ));

    framebuffer = qdev_new(TYPE_MMIX_FRAMEBUFFER);
    object_property_add_child(OBJECT(machine), "framebuffer",
                              OBJECT(framebuffer));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(framebuffer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(framebuffer), 0,
                    mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER_CONTROL].base);

    serial_mm_init(
        sysmem, mmix_virt_memmap[MMIX_VIRT_UART0].base, 0,
        qdev_get_gpio_in(intc, MMIX_VIRT_UART0_IRQ),
        MMIX_VIRT_UART0_BAUD_BASE, serial_hd(0), DEVICE_BIG_ENDIAN);

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
        vms->kernel_load_info = load_info;
        vms->kernel_loaded = true;
        mmix_start_loaded_kernel(vms, &vms->kernel_load_info);
    } else {
        mmix_setup_semihosting_arguments(machine, vms->cpus[0]);
    }
}

static char *mmix_virt_get_elf_startup_abi(Object *obj, Error **errp)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    switch (vms->elf_startup_abi) {
    case MMIX_ELF_STARTUP_ABI_BOOTINFO:
        return g_strdup("bootinfo");
    case MMIX_ELF_STARTUP_ABI_ARGC_ARGV:
        return g_strdup("argc-argv");
    default:
        g_assert_not_reached();
    }
}

static void mmix_virt_set_elf_startup_abi(Object *obj, const char *value,
                                          Error **errp)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    if (!strcmp(value, "bootinfo")) {
        vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_BOOTINFO;
    } else if (!strcmp(value, "argc-argv")) {
        vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_ARGC_ARGV;
    } else {
        error_setg(errp, "Invalid MMIX ELF startup ABI '%s'", value);
        error_append_hint(errp,
                          "Valid values are bootinfo and argc-argv.\n");
    }
}

static void mmix_virt_instance_init(Object *obj)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_BOOTINFO;
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
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mmix.ram";

    object_class_property_add_str(oc, "elf-startup-abi",
                                  mmix_virt_get_elf_startup_abi,
                                  mmix_virt_set_elf_startup_abi);
    object_class_property_set_description(
        oc, "elf-startup-abi",
        "Set the ELF startup ABI (bootinfo or argc-argv)");
}

static const TypeInfo mmix_virt_machine_typeinfo = {
    .name = TYPE_MMIX_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MMIXVirtMachineState),
    .instance_init = mmix_virt_instance_init,
    .class_init = mmix_virt_class_init,
};

static void mmix_virt_machine_init(void)
{
    type_register_static(&mmix_virt_machine_typeinfo);
}

type_init(mmix_virt_machine_init)
