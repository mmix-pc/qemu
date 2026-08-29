/*
 * MMIX virt flattened device tree builder
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include <libfdt.h>
#include "fdt-builder.h"
#include "physical-layout.h"
#include "virt.h"

enum {
    MMIX_FDT_CPU_PHANDLE_BASE = 1,
};

static bool mmix_fdt_error(int ret, const char *operation, Error **errp)
{
    if (ret >= 0) {
        return false;
    }

    error_setg(errp, "could not %s in MMIX FDT: %s",
               operation, fdt_strerror(ret));
    return true;
}

static bool mmix_fdt_context_range(uint64_t base, uint64_t stride,
                                   unsigned int count, const char *device,
                                   MMIXPhysRange *range, Error **errp)
{
    uint64_t size;

    if (stride == 0 || count > UINT64_MAX / stride) {
        error_setg(errp, "MMIX FDT %s context extent overflows", device);
        return false;
    }
    size = count * stride;
    if (!mmix_phys_range_init(range, base, size)) {
        error_setg(errp, "MMIX FDT %s context range overflows", device);
        return false;
    }
    return true;
}

static bool mmix_fdt_validate_context_ranges(const MMIXFDTConfig *config,
                                             Error **errp)
{
    MMIXPhysRange range;

    return mmix_fdt_context_range(MMIX_VIRT_TIMER_CONTEXT_BASE,
                                  MMIX_VIRT_TIMER_CONTEXT_STRIDE,
                                  config->cpu_count, "timer", &range, errp) &&
           mmix_fdt_context_range(MMIX_VIRT_IPI_CONTEXT_BASE,
                                  MMIX_VIRT_IPI_CONTEXT_STRIDE,
                                  config->cpu_count, "IPI", &range, errp) &&
           mmix_fdt_context_range(MMIX_VIRT_INTC_CONTEXT_BASE,
                                  MMIX_VIRT_INTC_CONTEXT_STRIDE,
                                  config->cpu_count, "interrupt controller",
                                  &range, errp);
}

static bool mmix_fdt_validate_config(const MMIXFDTConfig *config,
                                     Error **errp)
{
    unsigned int i;
    unsigned int j;

    if (!config) {
        error_setg(errp, "MMIX FDT configuration is missing");
        return false;
    }
    if (config->ram_size < MMIX_VIRT_RAM_MIN_SIZE ||
        config->ram_size > MMIX_VIRT_RAM_MAX_SIZE ||
        config->ram_size % MMIX_VIRT_RAM_ALIGN != 0) {
        error_setg(errp, "MMIX FDT RAM size 0x%" PRIx64 " is invalid",
                   config->ram_size);
        return false;
    }
    if (!config->command_line) {
        error_setg(errp, "MMIX FDT command line is missing");
        return false;
    }
    if (strlen(config->command_line) > MMIX_FDT_COMMAND_LINE_MAX) {
        error_setg(errp, "MMIX FDT command line exceeds %u bytes",
                   MMIX_FDT_COMMAND_LINE_MAX);
        return false;
    }
    if (config->cpu_count == 0 || config->cpu_count > MMIX_VIRT_MAX_CPUS) {
        error_setg(errp, "MMIX FDT CPU count %u is invalid",
                   config->cpu_count);
        return false;
    }
    if (!config->cpu_stacks) {
        error_setg(errp, "MMIX FDT CPU stack reservations are missing");
        return false;
    }
    for (i = 0; i < config->cpu_count; i++) {
        const MMIXPhysRange *stack = &config->cpu_stacks[i];

        if (!mmix_phys_range_valid(stack) ||
            mmix_phys_range_size(stack) != MMIX_VIRT_INITIAL_STACK_SIZE ||
            stack->start % MMIX_VIRT_INITIAL_STACK_ALIGN != 0 ||
            stack->end > config->ram_size) {
            error_setg(errp, "MMIX FDT CPU %u stack reservation is invalid",
                       i);
            return false;
        }
        for (j = 0; j < i; j++) {
            if (mmix_phys_ranges_overlap(stack, &config->cpu_stacks[j])) {
                error_setg(errp, "MMIX FDT CPU stack reservations %u and %u "
                           "overlap", j, i);
                return false;
            }
        }
    }
    if (config->has_framebuffer) {
        const MMIXPhysRange *framebuffer = &config->framebuffer;

        if (!mmix_phys_range_valid(framebuffer) ||
            mmix_phys_range_size(framebuffer) != MMIX_VIRT_FRAMEBUFFER_SIZE ||
            framebuffer->start % MMIX_VIRT_FRAMEBUFFER_ALIGN != 0 ||
            framebuffer->end > config->ram_size) {
            error_setg(errp, "MMIX FDT framebuffer reservation is invalid");
            return false;
        }
        for (i = 0; i < config->cpu_count; i++) {
            if (mmix_phys_ranges_overlap(framebuffer,
                                         &config->cpu_stacks[i])) {
                error_setg(errp, "MMIX FDT framebuffer reservation overlaps "
                           "CPU %u stack reservation", i);
                return false;
            }
        }
    }
    return mmix_fdt_validate_context_ranges(config, errp);
}

static bool mmix_fdt_set_u32(void *fdt, int node, const char *name,
                             uint32_t value, Error **errp)
{
    value = cpu_to_be32(value);
    return !mmix_fdt_error(fdt_setprop(fdt, node, name, &value,
                                       sizeof(value)), name, errp);
}

static bool mmix_fdt_set_string(void *fdt, int node, const char *name,
                                const char *value, Error **errp)
{
    return !mmix_fdt_error(fdt_setprop_string(fdt, node, name, value),
                           name, errp);
}

static bool mmix_fdt_set_empty(void *fdt, int node, const char *name,
                               Error **errp)
{
    return !mmix_fdt_error(fdt_setprop(fdt, node, name, NULL, 0),
                           name, errp);
}

static bool mmix_fdt_set_u64_range(void *fdt, int node, const char *name,
                                   const MMIXPhysRange *range, Error **errp)
{
    const fdt64_t reg[] = {
        cpu_to_be64(range->start),
        cpu_to_be64(mmix_phys_range_size(range)),
    };

    return !mmix_fdt_error(fdt_setprop(fdt, node, name, reg, sizeof(reg)),
                           name, errp);
}

static bool mmix_fdt_set_u64_ranges(void *fdt, int node, const char *name,
                                    const MMIXPhysRange *ranges, size_t count,
                                    Error **errp)
{
    g_autofree fdt64_t *cells = g_new(fdt64_t, 2 * count);
    size_t i;

    for (i = 0; i < count; i++) {
        cells[2 * i] = cpu_to_be64(ranges[i].start);
        cells[2 * i + 1] = cpu_to_be64(mmix_phys_range_size(&ranges[i]));
    }
    return !mmix_fdt_error(fdt_setprop(fdt, node, name, cells,
                                       2 * count * sizeof(*cells)),
                           name, errp);
}

static bool mmix_fdt_set_u32_array(void *fdt, int node, const char *name,
                                   const uint32_t *values, size_t count,
                                   Error **errp)
{
    g_autofree fdt32_t *cells = g_new(fdt32_t, count);
    size_t i;

    for (i = 0; i < count; i++) {
        cells[i] = cpu_to_be32(values[i]);
    }
    return !mmix_fdt_error(fdt_setprop(fdt, node, name, cells,
                                       count * sizeof(*cells)),
                           name, errp);
}

static bool mmix_fdt_set_phandle(void *fdt, int node, uint32_t phandle,
                                 Error **errp)
{
    return mmix_fdt_set_u32(fdt, node, "phandle", phandle, errp) &&
           mmix_fdt_set_u32(fdt, node, "linux,phandle", phandle, errp);
}

static int mmix_fdt_add_node(void *fdt, int parent, const char *name,
                             Error **errp)
{
    int node = fdt_add_subnode(fdt, parent, name);

    if (mmix_fdt_error(node, name, errp)) {
        return -1;
    }
    return node;
}

static uint32_t mmix_fdt_cpu_phandle(unsigned int cpu)
{
    return MMIX_FDT_CPU_PHANDLE_BASE + cpu;
}

static uint32_t mmix_fdt_stack_phandle(const MMIXFDTConfig *config,
                                       unsigned int cpu)
{
    return MMIX_FDT_CPU_PHANDLE_BASE + config->cpu_count + cpu;
}

static uint32_t mmix_fdt_framebuffer_phandle(const MMIXFDTConfig *config)
{
    return MMIX_FDT_CPU_PHANDLE_BASE + 2 * config->cpu_count;
}

static uint32_t mmix_fdt_intc_phandle(const MMIXFDTConfig *config)
{
    return mmix_fdt_framebuffer_phandle(config) +
           (config->has_framebuffer ? 1 : 0);
}

static bool mmix_fdt_add_cpu_nodes(void *fdt,
                                   const MMIXFDTConfig *config,
                                   Error **errp)
{
    int root = fdt_path_offset(fdt, "/");
    int cpus;
    int i;

    if (mmix_fdt_error(root, "find root node", errp)) {
        return false;
    }
    cpus = mmix_fdt_add_node(fdt, root, "cpus", errp);
    if (cpus < 0 ||
        !mmix_fdt_set_u32(fdt, cpus, "#address-cells", 1, errp) ||
        !mmix_fdt_set_u32(fdt, cpus, "#size-cells", 0, errp)) {
        return false;
    }

    /* libfdt prepends subnodes, so reverse insertion preserves CPU order. */
    for (i = config->cpu_count - 1; i >= 0; i--) {
        g_autofree char *name = g_strdup_printf("cpu@%x", i);
        int cpu = mmix_fdt_add_node(fdt,
                                    fdt_path_offset(fdt, "/cpus"),
                                    name, errp);

        if (cpu < 0 ||
            !mmix_fdt_set_string(fdt, cpu, "device_type", "cpu", errp) ||
            !mmix_fdt_set_string(fdt, cpu, "compatible",
                                 "qemu,mmix-cpu", errp) ||
            !mmix_fdt_set_u32(fdt, cpu, "reg", i, errp) ||
            !mmix_fdt_set_string(fdt, cpu, "status", "okay", errp) ||
            !mmix_fdt_set_string(fdt, cpu, "enable-method",
                                 "qemu,mmix-immediate-entry", errp) ||
            !mmix_fdt_set_u32(fdt, cpu, "qemu,initial-register-stack",
                              mmix_fdt_stack_phandle(config, i), errp) ||
            !mmix_fdt_set_phandle(fdt, cpu,
                                  mmix_fdt_cpu_phandle(i), errp)) {
            return false;
        }
    }
    return true;
}

static bool mmix_fdt_add_reserved_memory(void *fdt,
                                         const MMIXFDTConfig *config,
                                         Error **errp)
{
    int reserved = fdt_path_offset(fdt, "/reserved-memory");
    int i;

    if (mmix_fdt_error(reserved, "find reserved-memory node", errp)) {
        return false;
    }
    if (config->has_framebuffer) {
        g_autofree char *name = g_strdup_printf(
            "framebuffer@%" PRIx64, config->framebuffer.start);
        int node = mmix_fdt_add_node(fdt, reserved, name, errp);
        uint32_t phandle = mmix_fdt_framebuffer_phandle(config);

        if (node < 0 ||
            !mmix_fdt_set_string(fdt, node, "compatible",
                                 "qemu,mmix-framebuffer-memory", errp) ||
            !mmix_fdt_set_u64_range(fdt, node, "reg",
                                    &config->framebuffer, errp) ||
            !mmix_fdt_set_empty(fdt, node, "no-map", errp) ||
            !mmix_fdt_set_phandle(fdt, node, phandle, errp)) {
            return false;
        }
    }

    /* Reverse insertion preserves ascending CPU order in the final blob. */
    for (i = config->cpu_count - 1; i >= 0; i--) {
        const MMIXPhysRange *range = &config->cpu_stacks[i];
        g_autofree char *name = g_strdup_printf(
            "register-stack@%" PRIx64, range->start);
        int node = mmix_fdt_add_node(
            fdt, fdt_path_offset(fdt, "/reserved-memory"), name, errp);

        if (node < 0 ||
            !mmix_fdt_set_string(fdt, node, "compatible",
                                 "qemu,mmix-register-stack", errp) ||
            !mmix_fdt_set_u64_range(fdt, node, "reg", range, errp) ||
            !mmix_fdt_set_u32(fdt, node, "qemu,cpu",
                              mmix_fdt_cpu_phandle(i), errp) ||
            !mmix_fdt_set_phandle(fdt, node,
                                  mmix_fdt_stack_phandle(config, i), errp)) {
            return false;
        }
    }
    return true;
}

static bool mmix_fdt_add_intc_node(void *fdt,
                                   const MMIXFDTConfig *config,
                                   Error **errp)
{
    MMIXPhysRange ranges[] = {
        {
            .start = MMIX_VIRT_INTC_BASE,
            .end = MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_GLOBAL_SIZE,
        },
        { 0 },
    };
    g_autofree char *name = g_strdup_printf(
        "interrupt-controller@%" PRIx64, MMIX_VIRT_INTC_BASE);
    int node;

    if (!mmix_fdt_context_range(MMIX_VIRT_INTC_CONTEXT_BASE,
                                MMIX_VIRT_INTC_CONTEXT_STRIDE,
                                config->cpu_count, "interrupt controller",
                                &ranges[1], errp)) {
        return false;
    }
    node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/soc"), name, errp);
    return node >= 0 &&
           mmix_fdt_set_string(fdt, node, "compatible",
                               "qemu,mmix-intc", errp) &&
           mmix_fdt_set_empty(fdt, node, "interrupt-controller", errp) &&
           mmix_fdt_set_u32(fdt, node, "#interrupt-cells", 1, errp) &&
           mmix_fdt_set_u64_ranges(fdt, node, "reg", ranges,
                                   G_N_ELEMENTS(ranges), errp) &&
           /* Namespace capacity; active wiring comes from device nodes. */
           mmix_fdt_set_u32(fdt, node, "qemu,source-count",
                            MMIX_VIRT_INTC_IRQ_COUNT, errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,context-count",
                            config->cpu_count, errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,context-stride",
                            MMIX_VIRT_INTC_CONTEXT_STRIDE, errp) &&
           mmix_fdt_set_phandle(fdt, node,
                                mmix_fdt_intc_phandle(config), errp);
}

static bool mmix_fdt_add_ipi_node(void *fdt,
                                  const MMIXFDTConfig *config,
                                  Error **errp)
{
    MMIXPhysRange ranges[] = {
        {
            .start = MMIX_VIRT_IPI_BASE,
            .end = MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_GLOBAL_SLOT_SIZE,
        },
        { 0 },
    };
    g_autofree char *name = g_strdup_printf(
        "ipi@%" PRIx64, MMIX_VIRT_IPI_BASE);
    int node;

    if (!mmix_fdt_context_range(MMIX_VIRT_IPI_CONTEXT_BASE,
                                MMIX_VIRT_IPI_CONTEXT_STRIDE,
                                config->cpu_count, "IPI", &ranges[1], errp)) {
        return false;
    }
    node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/soc"), name, errp);
    return node >= 0 &&
           mmix_fdt_set_string(fdt, node, "compatible",
                               "qemu,mmix-ipi", errp) &&
           mmix_fdt_set_u64_ranges(fdt, node, "reg", ranges,
                                   G_N_ELEMENTS(ranges), errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,context-count",
                            config->cpu_count, errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,context-stride",
                            MMIX_VIRT_IPI_CONTEXT_STRIDE, errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,request-bit", 9, errp);
}

static bool mmix_fdt_add_timer_node(void *fdt,
                                    const MMIXFDTConfig *config,
                                    Error **errp)
{
    MMIXPhysRange ranges[] = {
        {
            .start = MMIX_VIRT_TIMER_BASE,
            .end = MMIX_VIRT_TIMER_BASE + MMIX_VIRT_TIMER_GLOBAL_SLOT_SIZE,
        },
        { 0 },
    };
    g_autofree uint32_t *interrupts = g_new(uint32_t, config->cpu_count);
    g_autofree uint32_t *affinity = g_new(uint32_t, config->cpu_count);
    g_autofree char *name = g_strdup_printf(
        "timer@%" PRIx64, MMIX_VIRT_TIMER_BASE);
    unsigned int i;
    int node;

    if (!mmix_fdt_context_range(MMIX_VIRT_TIMER_CONTEXT_BASE,
                                MMIX_VIRT_TIMER_CONTEXT_STRIDE,
                                config->cpu_count, "timer", &ranges[1],
                                errp)) {
        return false;
    }
    for (i = 0; i < config->cpu_count; i++) {
        interrupts[i] = MMIX_VIRT_TIMER_IRQ_BASE + i;
        affinity[i] = mmix_fdt_cpu_phandle(i);
    }
    node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/soc"), name, errp);
    return node >= 0 &&
           mmix_fdt_set_string(fdt, node, "compatible",
                               "qemu,mmix-timer", errp) &&
           mmix_fdt_set_u64_ranges(fdt, node, "reg", ranges,
                                   G_N_ELEMENTS(ranges), errp) &&
           mmix_fdt_set_u32_array(fdt, node, "interrupts", interrupts,
                                  config->cpu_count, errp) &&
           mmix_fdt_set_u32_array(fdt, node, "interrupt-affinity", affinity,
                                  config->cpu_count, errp) &&
           mmix_fdt_set_u32(fdt, node, "interrupt-parent",
                            mmix_fdt_intc_phandle(config), errp) &&
           mmix_fdt_set_u32(fdt, node, "clock-frequency",
                            MMIX_VIRT_TIMER_CLOCK_FREQUENCY, errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,context-count",
                            config->cpu_count, errp) &&
           mmix_fdt_set_u32(fdt, node, "qemu,context-stride",
                            MMIX_VIRT_TIMER_CONTEXT_STRIDE, errp);
}

static bool mmix_fdt_add_interrupt_topology(void *fdt,
                                            const MMIXFDTConfig *config,
                                            Error **errp)
{
    /* Reverse address order preserves ascending /soc node order. */
    return mmix_fdt_add_intc_node(fdt, config, errp) &&
           mmix_fdt_add_ipi_node(fdt, config, errp) &&
           mmix_fdt_add_timer_node(fdt, config, errp);
}

static bool mmix_fdt_add_uart_node(void *fdt,
                                   const MMIXFDTConfig *config,
                                   Error **errp)
{
    const MMIXPhysRange range = {
        .start = MMIX_VIRT_UART0_BASE,
        .end = MMIX_VIRT_UART0_BASE + MMIX_VIRT_UART0_REGISTER_SIZE,
    };
    g_autofree char *name = g_strdup_printf(
        "serial@%" PRIx64, MMIX_VIRT_UART0_BASE);
    g_autofree char *path = g_strdup_printf("/soc/%s", name);
    int aliases;
    int chosen;
    int node;

    node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/soc"), name, errp);
    if (node < 0 ||
        !mmix_fdt_set_string(fdt, node, "compatible", "ns16550a", errp) ||
        !mmix_fdt_set_u64_range(fdt, node, "reg", &range, errp) ||
        !mmix_fdt_set_u32(fdt, node, "clock-frequency",
                          MMIX_VIRT_UART0_CLOCK_FREQUENCY, errp) ||
        !mmix_fdt_set_u32(fdt, node, "current-speed",
                          MMIX_VIRT_UART0_BAUD_BASE, errp) ||
        !mmix_fdt_set_u32(fdt, node, "reg-shift",
                          MMIX_VIRT_UART0_REGISTER_SHIFT, errp) ||
        !mmix_fdt_set_u32(fdt, node, "reg-io-width", 1, errp) ||
        !mmix_fdt_set_u32(fdt, node, "interrupts",
                          MMIX_VIRT_UART0_IRQ, errp) ||
        !mmix_fdt_set_u32(fdt, node, "interrupt-parent",
                          mmix_fdt_intc_phandle(config), errp)) {
        return false;
    }

    aliases = fdt_path_offset(fdt, "/aliases");
    if (mmix_fdt_error(aliases, "find aliases node", errp) ||
        !mmix_fdt_set_string(fdt, aliases, "serial0", path, errp)) {
        return false;
    }
    chosen = fdt_path_offset(fdt, "/chosen");
    return !mmix_fdt_error(chosen, "find chosen node", errp) &&
           mmix_fdt_set_string(fdt, chosen, "stdout-path",
                               "serial0:115200n8", errp);
}

static bool mmix_fdt_add_framebuffer_nodes(void *fdt,
                                           const MMIXFDTConfig *config,
                                           Error **errp)
{
    const MMIXPhysRange control = {
        .start = MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE,
        .end = MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE +
               MMIX_VIRT_FRAMEBUFFER_CONTROL_MMIO_SIZE,
    };
    uint32_t phandle;
    g_autofree char *control_name = NULL;
    g_autofree char *simple_name = NULL;
    int node;

    if (!config->has_framebuffer) {
        return true;
    }
    phandle = mmix_fdt_framebuffer_phandle(config);
    control_name = g_strdup_printf("framebuffer@%" PRIx64,
                                   MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE);
    node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/soc"),
                             control_name, errp);
    if (node < 0 ||
        !mmix_fdt_set_string(fdt, node, "compatible",
                             "qemu,mmix-framebuffer", errp) ||
        !mmix_fdt_set_u64_range(fdt, node, "reg", &control, errp) ||
        !mmix_fdt_set_u32(fdt, node, "memory-region", phandle, errp)) {
        return false;
    }

    simple_name = g_strdup_printf("framebuffer@%" PRIx64,
                                  config->framebuffer.start);
    node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/chosen"),
                             simple_name, errp);
    return node >= 0 &&
           mmix_fdt_set_string(fdt, node, "compatible",
                               "simple-framebuffer", errp) &&
           mmix_fdt_set_u64_range(fdt, node, "reg",
                                  &config->framebuffer, errp) &&
           mmix_fdt_set_u32(fdt, node, "width",
                            MMIX_VIRT_FRAMEBUFFER_WIDTH, errp) &&
           mmix_fdt_set_u32(fdt, node, "height",
                            MMIX_VIRT_FRAMEBUFFER_HEIGHT, errp) &&
           mmix_fdt_set_u32(fdt, node, "stride",
                            MMIX_VIRT_FRAMEBUFFER_STRIDE, errp) &&
           mmix_fdt_set_string(fdt, node, "format", "x8r8g8b8", errp) &&
           mmix_fdt_set_string(fdt, node, "status", "okay", errp) &&
           mmix_fdt_set_u32(fdt, node, "memory-region", phandle, errp);
}

static bool mmix_fdt_add_virtio_mmio_nodes(void *fdt,
                                           const MMIXFDTConfig *config,
                                           Error **errp)
{
    int i;

    /* libfdt prepends subnodes, so reverse insertion preserves slot order. */
    for (i = MMIX_VIRT_VIRTIO_MMIO_COUNT - 1; i >= 0; i--) {
        uint64_t base = MMIX_VIRT_VIRTIO_MMIO_BASE +
                        i * MMIX_VIRT_VIRTIO_MMIO_STRIDE;
        const MMIXPhysRange range = {
            .start = base,
            .end = base + MMIX_VIRT_VIRTIO_MMIO_REGISTER_SIZE,
        };
        g_autofree char *name = g_strdup_printf(
            "virtio_mmio@%" PRIx64, base);
        int node = mmix_fdt_add_node(fdt, fdt_path_offset(fdt, "/soc"),
                                     name, errp);

        if (node < 0 ||
            !mmix_fdt_set_string(fdt, node, "compatible",
                                 "virtio,mmio", errp) ||
            !mmix_fdt_set_u64_range(fdt, node, "reg", &range, errp) ||
            !mmix_fdt_set_u32(fdt, node, "interrupts",
                              MMIX_VIRT_VIRTIO_MMIO_IRQ_BASE + i, errp) ||
            !mmix_fdt_set_u32(fdt, node, "interrupt-parent",
                              mmix_fdt_intc_phandle(config), errp)) {
            return false;
        }
    }
    return true;
}

static bool mmix_fdt_add_active_devices(void *fdt,
                                        const MMIXFDTConfig *config,
                                        Error **errp)
{
    return mmix_fdt_add_virtio_mmio_nodes(fdt, config, errp) &&
           mmix_fdt_add_framebuffer_nodes(fdt, config, errp) &&
           mmix_fdt_add_uart_node(fdt, config, errp);
}

static bool mmix_fdt_add_foundation(void *fdt,
                                    const MMIXFDTConfig *config,
                                    Error **errp)
{
    const fdt64_t memory_reg[] = {
        cpu_to_be64(0),
        cpu_to_be64(config->ram_size),
    };
    int root = fdt_path_offset(fdt, "/");
    int chosen;
    int memory;
    int reserved;
    int soc;

    if (mmix_fdt_error(root, "find root node", errp) ||
        !mmix_fdt_set_string(fdt, root, "compatible",
                             "qemu,mmix-virt", errp) ||
        !mmix_fdt_set_string(fdt, root, "model",
                             "QEMU MMIX Virt Machine", errp) ||
        !mmix_fdt_set_u32(fdt, root, "#address-cells", 2, errp) ||
        !mmix_fdt_set_u32(fdt, root, "#size-cells", 2, errp)) {
        return false;
    }

    if (mmix_fdt_add_node(fdt, root, "aliases", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "chosen", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "memory@0", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "reserved-memory", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "soc", errp) < 0) {
        return false;
    }

    if (mmix_fdt_error(fdt_path_offset(fdt, "/aliases"),
                       "find aliases node", errp)) {
        return false;
    }
    chosen = fdt_path_offset(fdt, "/chosen");
    if (mmix_fdt_error(chosen, "find chosen node", errp) ||
        !mmix_fdt_set_string(fdt, chosen, "bootargs",
                             config->command_line, errp)) {
        return false;
    }

    memory = fdt_path_offset(fdt, "/memory@0");
    if (mmix_fdt_error(memory, "find memory node", errp) ||
        !mmix_fdt_set_string(fdt, memory, "device_type", "memory", errp) ||
        mmix_fdt_error(fdt_setprop(fdt, memory, "reg", memory_reg,
                                   sizeof(memory_reg)), "memory reg", errp)) {
        return false;
    }

    reserved = fdt_path_offset(fdt, "/reserved-memory");
    if (mmix_fdt_error(reserved, "find reserved-memory node", errp) ||
        !mmix_fdt_set_u32(fdt, reserved, "#address-cells", 2, errp) ||
        !mmix_fdt_set_u32(fdt, reserved, "#size-cells", 2, errp) ||
        !mmix_fdt_set_empty(fdt, reserved, "ranges", errp)) {
        return false;
    }

    soc = fdt_path_offset(fdt, "/soc");
    if (mmix_fdt_error(soc, "find soc node", errp) ||
        !mmix_fdt_set_string(fdt, soc, "compatible", "simple-bus", errp) ||
        !mmix_fdt_set_u32(fdt, soc, "#address-cells", 2, errp) ||
        !mmix_fdt_set_u32(fdt, soc, "#size-cells", 2, errp) ||
        !mmix_fdt_set_empty(fdt, soc, "ranges", errp) ||
        !mmix_fdt_add_cpu_nodes(fdt, config, errp) ||
        !mmix_fdt_add_reserved_memory(fdt, config, errp) ||
        !mmix_fdt_add_interrupt_topology(fdt, config, errp) ||
        !mmix_fdt_add_active_devices(fdt, config, errp)) {
        return false;
    }
    return true;
}

bool mmix_fdt_build(const MMIXFDTConfig *config, GBytes **result,
                    Error **errp)
{
    g_autofree void *fdt = NULL;
    GBytes *blob;
    int ret;
    size_t size;

    if (!result) {
        error_setg(errp, "MMIX FDT result pointer is missing");
        return false;
    }
    if (!mmix_fdt_validate_config(config, errp)) {
        return false;
    }

    fdt = g_malloc0(MMIX_FDT_MAX_SIZE);
    ret = fdt_create_empty_tree(fdt, MMIX_FDT_MAX_SIZE);
    if (mmix_fdt_error(ret, "create tree", errp) ||
        !mmix_fdt_add_foundation(fdt, config, errp)) {
        return false;
    }
    ret = fdt_pack(fdt);
    if (mmix_fdt_error(ret, "pack tree", errp)) {
        return false;
    }
    size = fdt_totalsize(fdt);
    blob = g_bytes_new_take(g_steal_pointer(&fdt), size);
    g_clear_pointer(result, g_bytes_unref);
    *result = blob;
    return true;
}
