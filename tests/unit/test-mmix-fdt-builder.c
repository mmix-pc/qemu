/*
 * MMIX virt flattened device tree builder tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/fdt-builder.h"
#include "hw/mmix/fdt-validator.h"
#include "hw/mmix/physical-layout.h"
#include "hw/mmix/virt.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include <libfdt.h>

static const MMIXPhysRange default_stack = {
    .start = 0x10000,
    .end = 0x18000,
};

static MMIXFDTConfig default_config(uint64_t ram_size,
                                    const char *command_line)
{
    return (MMIXFDTConfig) {
        .ram_size = ram_size,
        .command_line = command_line,
        .cpu_count = 1,
        .cpu_stacks = &default_stack,
    };
}

static GBytes *build_configured_fdt(const MMIXFDTConfig *config)
{
    GBytes *blob = NULL;
    Error *err = NULL;

    if (!mmix_fdt_build(config, &blob, &err)) {
        g_error("could not build test FDT: %s", error_get_pretty(err));
    }
    g_assert_null(err);
    return blob;
}

static GBytes *build_fdt(uint64_t ram_size, const char *command_line)
{
    MMIXFDTConfig config = default_config(ram_size, command_line);

    return build_configured_fdt(&config);
}

static int node_offset(const void *fdt, const char *path)
{
    int node = fdt_path_offset(fdt, path);

    g_assert_cmpint(node, >=, 0);
    return node;
}

static void assert_string(const void *fdt, const char *path,
                          const char *name, const char *expected)
{
    int length;
    const char *actual = fdt_getprop(fdt, node_offset(fdt, path), name,
                                     &length);

    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, strlen(expected) + 1);
    g_assert_cmpstr(actual, ==, expected);
}

static void assert_u32(const void *fdt, const char *path,
                       const char *name, uint32_t expected)
{
    int length;
    const fdt32_t *actual = fdt_getprop(fdt, node_offset(fdt, path), name,
                                        &length);

    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, sizeof(*actual));
    g_assert_cmpuint(be32_to_cpu(*actual), ==, expected);
}

static void assert_empty(const void *fdt, const char *path, const char *name)
{
    int length;
    const void *actual = fdt_getprop(fdt, node_offset(fdt, path), name,
                                     &length);

    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, 0);
}

static void assert_absent(const void *fdt, const char *path, const char *name)
{
    int length;

    g_assert_null(fdt_getprop(fdt, node_offset(fdt, path), name, &length));
    g_assert_cmpint(length, ==, -FDT_ERR_NOTFOUND);
}

static void assert_node_absent(const void *fdt, const char *path)
{
    g_assert_cmpint(fdt_path_offset(fdt, path), ==, -FDT_ERR_NOTFOUND);
}

static void assert_range(const void *fdt, const char *path,
                         const MMIXPhysRange *expected)
{
    const fdt64_t *reg;
    int length;

    reg = fdt_getprop(fdt, node_offset(fdt, path), "reg", &length);
    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 2 * sizeof(*reg));
    g_assert_cmphex(be64_to_cpu(reg[0]), ==, expected->start);
    g_assert_cmphex(be64_to_cpu(reg[1]), ==,
                    mmix_phys_range_size(expected));
}

static void assert_ranges(const void *fdt, const char *path,
                          const MMIXPhysRange *expected, size_t count)
{
    const fdt64_t *reg;
    int length;
    size_t i;

    reg = fdt_getprop(fdt, node_offset(fdt, path), "reg", &length);
    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 2 * count * sizeof(*reg));
    for (i = 0; i < count; i++) {
        g_assert_cmphex(be64_to_cpu(reg[2 * i]), ==, expected[i].start);
        g_assert_cmphex(be64_to_cpu(reg[2 * i + 1]), ==,
                        mmix_phys_range_size(&expected[i]));
    }
}

static void assert_u32_array(const void *fdt, const char *path,
                             const char *name, const uint32_t *expected,
                             size_t count)
{
    const fdt32_t *actual;
    int length;
    size_t i;

    actual = fdt_getprop(fdt, node_offset(fdt, path), name, &length);
    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, count * sizeof(*actual));
    for (i = 0; i < count; i++) {
        g_assert_cmpuint(be32_to_cpu(actual[i]), ==, expected[i]);
    }
}

static void assert_foundation(const void *fdt, size_t size,
                              uint64_t ram_size, const char *command_line)
{
    const fdt64_t *reg;
    int length;

    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    g_assert_cmpuint(fdt_version(fdt), ==, 17);
    g_assert_cmpuint(fdt_totalsize(fdt), ==, size);
    assert_string(fdt, "/", "compatible", "qemu,mmix-virt");
    assert_string(fdt, "/", "model", "QEMU MMIX Virt Machine");
    assert_u32(fdt, "/", "#address-cells", 2);
    assert_u32(fdt, "/", "#size-cells", 2);
    node_offset(fdt, "/aliases");
    assert_u32(fdt, "/chosen", "#address-cells", 2);
    assert_u32(fdt, "/chosen", "#size-cells", 2);
    assert_string(fdt, "/chosen", "bootargs", command_line);
    assert_string(fdt, "/memory@0", "device_type", "memory");
    reg = fdt_getprop(fdt, node_offset(fdt, "/memory@0"), "reg", &length);
    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 2 * sizeof(*reg));
    g_assert_cmphex(be64_to_cpu(reg[0]), ==, 0);
    g_assert_cmphex(be64_to_cpu(reg[1]), ==, ram_size);
    assert_u32(fdt, "/reserved-memory", "#address-cells", 2);
    assert_u32(fdt, "/reserved-memory", "#size-cells", 2);
    assert_empty(fdt, "/reserved-memory", "ranges");
    assert_u32(fdt, "/cpus", "#address-cells", 1);
    assert_u32(fdt, "/cpus", "#size-cells", 0);
    assert_string(fdt, "/soc", "compatible", "simple-bus");
    assert_u32(fdt, "/soc", "#address-cells", 2);
    assert_u32(fdt, "/soc", "#size-cells", 2);
    assert_empty(fdt, "/soc", "ranges");
}

static void assert_cpu_stack_pair(const void *fdt, unsigned int cpu_id,
                                  const MMIXPhysRange *stack,
                                  uint32_t cpu_phandle,
                                  uint32_t stack_phandle)
{
    g_autofree char *cpu_path = g_strdup_printf("/cpus/cpu@%x", cpu_id);
    g_autofree char *stack_path = g_strdup_printf(
        "/reserved-memory/register-stack@%" PRIx64, stack->start);

    assert_string(fdt, cpu_path, "device_type", "cpu");
    assert_string(fdt, cpu_path, "compatible", "qemu,mmix-cpu");
    assert_u32(fdt, cpu_path, "reg", cpu_id);
    assert_string(fdt, cpu_path, "status", "okay");
    assert_string(fdt, cpu_path, "enable-method",
                  "qemu,mmix-immediate-entry");
    assert_u32(fdt, cpu_path, "phandle", cpu_phandle);
    assert_u32(fdt, cpu_path, "linux,phandle", cpu_phandle);
    assert_u32(fdt, cpu_path, "qemu,initial-register-stack",
               stack_phandle);
    g_assert_cmpint(fdt_node_offset_by_phandle(fdt, cpu_phandle), ==,
                    node_offset(fdt, cpu_path));

    assert_string(fdt, stack_path, "compatible",
                  "qemu,mmix-register-stack");
    assert_range(fdt, stack_path, stack);
    assert_u32(fdt, stack_path, "phandle", stack_phandle);
    assert_u32(fdt, stack_path, "linux,phandle", stack_phandle);
    assert_u32(fdt, stack_path, "qemu,cpu", cpu_phandle);
    g_assert_cmpint(fdt_node_offset_by_phandle(fdt, stack_phandle), ==,
                    node_offset(fdt, stack_path));
    assert_absent(fdt, stack_path, "no-map");
    assert_absent(fdt, stack_path, "reusable");
}

static void assert_interrupt_topology(const void *fdt,
                                      unsigned int cpu_count,
                                      bool has_framebuffer)
{
    const char *intc_path = "/soc/interrupt-controller@1000030000000";
    const char *ipi_path = "/soc/ipi@1000024000000";
    const char *timer_path = "/soc/timer@1000020000000";
    const MMIXPhysRange intc_ranges[] = {
        {
            .start = MMIX_VIRT_INTC_BASE,
            .end = MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_GLOBAL_SIZE,
        },
        {
            .start = MMIX_VIRT_INTC_CONTEXT_BASE,
            .end = MMIX_VIRT_INTC_CONTEXT_BASE +
                   cpu_count * MMIX_VIRT_INTC_CONTEXT_STRIDE,
        },
    };
    const MMIXPhysRange ipi_ranges[] = {
        {
            .start = MMIX_VIRT_IPI_BASE,
            .end = MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_GLOBAL_SLOT_SIZE,
        },
        {
            .start = MMIX_VIRT_IPI_CONTEXT_BASE,
            .end = MMIX_VIRT_IPI_CONTEXT_BASE +
                   cpu_count * MMIX_VIRT_IPI_CONTEXT_STRIDE,
        },
    };
    const MMIXPhysRange timer_ranges[] = {
        {
            .start = MMIX_VIRT_TIMER_BASE,
            .end = MMIX_VIRT_TIMER_BASE + MMIX_VIRT_TIMER_GLOBAL_SLOT_SIZE,
        },
        {
            .start = MMIX_VIRT_TIMER_CONTEXT_BASE,
            .end = MMIX_VIRT_TIMER_CONTEXT_BASE +
                   cpu_count * MMIX_VIRT_TIMER_CONTEXT_STRIDE,
        },
    };
    g_autofree uint32_t *interrupts = g_new(uint32_t, cpu_count);
    g_autofree uint32_t *affinity = g_new(uint32_t, cpu_count);
    uint32_t intc_phandle = 1 + 2 * cpu_count + has_framebuffer;
    unsigned int i;

    assert_string(fdt, intc_path, "compatible", "qemu,mmix-intc");
    assert_empty(fdt, intc_path, "interrupt-controller");
    assert_u32(fdt, intc_path, "#interrupt-cells", 1);
    assert_ranges(fdt, intc_path, intc_ranges, G_N_ELEMENTS(intc_ranges));
    assert_u32(fdt, intc_path, "qemu,source-count",
               MMIX_VIRT_INTC_IRQ_COUNT);
    assert_u32(fdt, intc_path, "qemu,context-count", cpu_count);
    assert_u32(fdt, intc_path, "qemu,context-stride",
               MMIX_VIRT_INTC_CONTEXT_STRIDE);
    assert_u32(fdt, intc_path, "phandle", intc_phandle);
    assert_u32(fdt, intc_path, "linux,phandle", intc_phandle);
    g_assert_cmpint(fdt_node_offset_by_phandle(fdt, intc_phandle), ==,
                    node_offset(fdt, intc_path));

    assert_string(fdt, ipi_path, "compatible", "qemu,mmix-ipi");
    assert_ranges(fdt, ipi_path, ipi_ranges, G_N_ELEMENTS(ipi_ranges));
    assert_u32(fdt, ipi_path, "qemu,context-count", cpu_count);
    assert_u32(fdt, ipi_path, "qemu,context-stride",
               MMIX_VIRT_IPI_CONTEXT_STRIDE);
    assert_u32(fdt, ipi_path, "qemu,request-bit", 9);
    assert_absent(fdt, ipi_path, "interrupt-parent");
    assert_absent(fdt, ipi_path, "interrupts");
    assert_absent(fdt, ipi_path, "interrupt-affinity");

    for (i = 0; i < cpu_count; i++) {
        g_autofree char *cpu_path = g_strdup_printf("/cpus/cpu@%x", i);

        interrupts[i] = MMIX_VIRT_TIMER_IRQ_BASE + i;
        affinity[i] = 1 + i;
        g_assert_cmpint(fdt_node_offset_by_phandle(fdt, affinity[i]), ==,
                        node_offset(fdt, cpu_path));
    }
    assert_string(fdt, timer_path, "compatible", "qemu,mmix-timer");
    assert_ranges(fdt, timer_path, timer_ranges,
                  G_N_ELEMENTS(timer_ranges));
    assert_u32_array(fdt, timer_path, "interrupts", interrupts, cpu_count);
    assert_u32_array(fdt, timer_path, "interrupt-affinity", affinity,
                     cpu_count);
    assert_u32(fdt, timer_path, "interrupt-parent", intc_phandle);
    assert_u32(fdt, timer_path, "clock-frequency",
               MMIX_VIRT_TIMER_CLOCK_FREQUENCY);
    assert_u32(fdt, timer_path, "qemu,context-count", cpu_count);
    assert_u32(fdt, timer_path, "qemu,context-stride",
               MMIX_VIRT_TIMER_CONTEXT_STRIDE);
}

static void assert_active_devices(const void *fdt,
                                  unsigned int cpu_count,
                                  const MMIXPhysRange *framebuffer)
{
    const char *uart_path = "/soc/serial@1000010000000";
    const MMIXPhysRange uart = {
        .start = MMIX_VIRT_UART0_BASE,
        .end = MMIX_VIRT_UART0_BASE + MMIX_VIRT_UART0_REGISTER_SIZE,
    };
    const unsigned int reserved_slots[] = {
        MMIX_VIRT_VIRTIO_MMIO_COUNT,
        MMIX_VIRT_VIRTIO_MMIO_SLOT_CAPACITY - 1,
    };
    uint32_t intc_phandle = 1 + 2 * cpu_count + (framebuffer != NULL);
    int node = fdt_first_subnode(fdt, node_offset(fdt, "/soc"));
    unsigned int child_count = 0;
    unsigned int virtio_slot = 0;
    unsigned int i;

    assert_string(fdt, uart_path, "compatible", "ns16550a");
    assert_range(fdt, uart_path, &uart);
    assert_u32(fdt, uart_path, "clock-frequency",
               MMIX_VIRT_UART0_CLOCK_FREQUENCY);
    assert_u32(fdt, uart_path, "current-speed",
               MMIX_VIRT_UART0_BAUD_BASE);
    assert_u32(fdt, uart_path, "reg-shift",
               MMIX_VIRT_UART0_REGISTER_SHIFT);
    assert_u32(fdt, uart_path, "reg-io-width", 1);
    assert_u32(fdt, uart_path, "interrupts", MMIX_VIRT_UART0_IRQ);
    assert_u32(fdt, uart_path, "interrupt-parent", intc_phandle);
    assert_string(fdt, "/aliases", "serial0", uart_path);
    assert_string(fdt, "/chosen", "stdout-path", "serial0:115200n8");

    for (i = 0; i < MMIX_VIRT_VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = MMIX_VIRT_VIRTIO_MMIO_BASE +
                        i * MMIX_VIRT_VIRTIO_MMIO_STRIDE;
        const MMIXPhysRange range = {
            .start = base,
            .end = base + MMIX_VIRT_VIRTIO_MMIO_REGISTER_SIZE,
        };
        g_autofree char *path = g_strdup_printf(
            "/soc/virtio_mmio@%" PRIx64, base);

        assert_string(fdt, path, "compatible", "virtio,mmio");
        assert_range(fdt, path, &range);
        assert_u32(fdt, path, "interrupts",
                   MMIX_VIRT_VIRTIO_MMIO_IRQ_BASE + i);
        assert_u32(fdt, path, "interrupt-parent", intc_phandle);
    }

    while (node >= 0) {
        const char *compatible;
        int length;

        child_count++;
        compatible = fdt_getprop(fdt, node, "compatible", &length);
        if (compatible && !strcmp(compatible, "virtio,mmio")) {
            uint64_t base = MMIX_VIRT_VIRTIO_MMIO_BASE +
                            virtio_slot * MMIX_VIRT_VIRTIO_MMIO_STRIDE;
            g_autofree char *name = g_strdup_printf(
                "virtio_mmio@%" PRIx64, base);

            g_assert_cmpstr(fdt_get_name(fdt, node, NULL), ==, name);
            virtio_slot++;
        }
        node = fdt_next_subnode(fdt, node);
    }
    g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
    g_assert_cmpuint(virtio_slot, ==, MMIX_VIRT_VIRTIO_MMIO_COUNT);
    g_assert_cmpuint(child_count, ==,
                     1 + 3 + MMIX_VIRT_VIRTIO_MMIO_COUNT +
                     (framebuffer != NULL));
    assert_node_absent(fdt, "/flash@1000000000000");
    assert_node_absent(fdt, "/pcie@1000100000000");

    for (i = 0; i < G_N_ELEMENTS(reserved_slots); i++) {
        uint64_t base = MMIX_VIRT_VIRTIO_MMIO_BASE +
                        reserved_slots[i] * MMIX_VIRT_VIRTIO_MMIO_STRIDE;
        g_autofree char *path = g_strdup_printf(
            "/soc/virtio_mmio@%" PRIx64, base);

        assert_node_absent(fdt, path);
    }

    if (framebuffer) {
        const char *control_path =
            "/soc/framebuffer@1000018000000";
        const MMIXPhysRange control = {
            .start = MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE,
            .end = MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE +
                   MMIX_VIRT_FRAMEBUFFER_CONTROL_MMIO_SIZE,
        };
        g_autofree char *simple_path = g_strdup_printf(
            "/chosen/framebuffer@%" PRIx64, framebuffer->start);
        g_autofree char *memory_path = g_strdup_printf(
            "/reserved-memory/framebuffer@%" PRIx64, framebuffer->start);
        uint32_t framebuffer_phandle = 1 + 2 * cpu_count;

        assert_string(fdt, control_path, "compatible",
                      "qemu,mmix-framebuffer");
        assert_range(fdt, control_path, &control);
        assert_u32(fdt, control_path, "memory-region",
                   framebuffer_phandle);
        assert_string(fdt, simple_path, "compatible",
                      "simple-framebuffer");
        assert_range(fdt, simple_path, framebuffer);
        assert_u32(fdt, simple_path, "width", MMIX_VIRT_FRAMEBUFFER_WIDTH);
        assert_u32(fdt, simple_path, "height",
                   MMIX_VIRT_FRAMEBUFFER_HEIGHT);
        assert_u32(fdt, simple_path, "stride",
                   MMIX_VIRT_FRAMEBUFFER_STRIDE);
        assert_string(fdt, simple_path, "format", "x8r8g8b8");
        assert_string(fdt, simple_path, "status", "okay");
        assert_u32(fdt, simple_path, "memory-region",
                   framebuffer_phandle);
        g_assert_cmpint(fdt_node_offset_by_phandle(
                            fdt, framebuffer_phandle), ==,
                        node_offset(fdt, memory_path));
    } else {
        assert_node_absent(fdt, "/soc/framebuffer@1000018000000");
        g_assert_cmpint(fdt_first_subnode(fdt,
                                         node_offset(fdt, "/chosen")), ==,
                        -FDT_ERR_NOTFOUND);
    }
}

static void test_default_foundation(void)
{
    g_autoptr(GBytes) blob = build_fdt(MMIX_VIRT_RAM_DEFAULT_SIZE, "");
    gsize size;
    const void *fdt = g_bytes_get_data(blob, &size);

    assert_foundation(fdt, size, MMIX_VIRT_RAM_DEFAULT_SIZE, "");
    assert_interrupt_topology(fdt, 1, false);
    assert_active_devices(fdt, 1, NULL);
}

static void test_large_ram_and_command_line(void)
{
    const char *command_line = "console=ttyS0 root=/dev/vda";
    g_autoptr(GBytes) blob = build_fdt(8 * GiB, command_line);
    gsize size;
    const void *fdt = g_bytes_get_data(blob, &size);

    assert_foundation(fdt, size, 8 * GiB, command_line);
}

static void test_deterministic_output(void)
{
    g_autoptr(GBytes) first = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "test");
    g_autoptr(GBytes) second = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "test");

    g_assert_true(g_bytes_equal(first, second));
}

static void test_single_cpu_with_framebuffer(void)
{
    const MMIXPhysRange framebuffer = {
        .start = 0x400000,
        .end = 0x400000 + MMIX_VIRT_FRAMEBUFFER_SIZE,
    };
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");
    g_autofree char *framebuffer_path = g_strdup_printf(
        "/reserved-memory/framebuffer@%" PRIx64, framebuffer.start);
    g_autoptr(GBytes) blob;
    const void *fdt;

    config.has_framebuffer = true;
    config.framebuffer = framebuffer;
    blob = build_configured_fdt(&config);
    fdt = g_bytes_get_data(blob, NULL);

    assert_cpu_stack_pair(fdt, 0, &default_stack, 1, 2);
    assert_string(fdt, framebuffer_path, "compatible",
                  "qemu,mmix-framebuffer-memory");
    assert_range(fdt, framebuffer_path, &framebuffer);
    assert_empty(fdt, framebuffer_path, "no-map");
    assert_u32(fdt, framebuffer_path, "phandle", 3);
    assert_u32(fdt, framebuffer_path, "linux,phandle", 3);
    g_assert_cmpint(fdt_node_offset_by_phandle(fdt, 3), ==,
                    node_offset(fdt, framebuffer_path));
    assert_interrupt_topology(fdt, 1, true);
    assert_active_devices(fdt, 1, &framebuffer);
}

static void test_maximum_cpu_topology(void)
{
    MMIXPhysRange stacks[MMIX_VIRT_MAX_CPUS];
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");
    g_autoptr(GBytes) blob;
    const void *fdt;
    int node;
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(stacks); i++) {
        stacks[i].start = 0x100000 + i * MMIX_VIRT_INITIAL_STACK_SIZE;
        stacks[i].end = stacks[i].start + MMIX_VIRT_INITIAL_STACK_SIZE;
    }
    config.cpu_count = G_N_ELEMENTS(stacks);
    config.cpu_stacks = stacks;
    blob = build_configured_fdt(&config);
    fdt = g_bytes_get_data(blob, NULL);

    node = fdt_first_subnode(fdt, node_offset(fdt, "/cpus"));
    for (i = 0; i < G_N_ELEMENTS(stacks); i++) {
        g_autofree char *name = g_strdup_printf("cpu@%x", i);

        g_assert_cmpint(node, >=, 0);
        g_assert_cmpstr(fdt_get_name(fdt, node, NULL), ==, name);
        assert_cpu_stack_pair(fdt, i, &stacks[i], 1 + i,
                              1 + G_N_ELEMENTS(stacks) + i);
        node = fdt_next_subnode(fdt, node);
    }
    g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
    assert_interrupt_topology(fdt, G_N_ELEMENTS(stacks), false);
}

static void assert_invalid_config(const MMIXFDTConfig *config,
                                  const char *diagnostic)
{
    g_autoptr(GBytes) original = build_fdt(MMIX_VIRT_RAM_DEFAULT_SIZE, "");
    GBytes *result = g_bytes_ref(original);
    Error *err = NULL;

    g_assert_false(mmix_fdt_build(config, &result, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    g_assert_true(result == original);
    error_free(err);
    g_bytes_unref(result);
}

static void test_invalid_inputs_preserve_result(void)
{
    MMIXFDTConfig below_minimum = default_config(
        MMIX_VIRT_RAM_MIN_SIZE - MMIX_VIRT_RAM_ALIGN, "");
    MMIXFDTConfig above_maximum = default_config(
        MMIX_VIRT_RAM_MAX_SIZE + MMIX_VIRT_RAM_ALIGN, "");
    MMIXFDTConfig unaligned = default_config(
        MMIX_VIRT_RAM_MIN_SIZE + 1, "");
    MMIXFDTConfig missing_command_line = default_config(
        MMIX_VIRT_RAM_MIN_SIZE, NULL);
    g_autofree char *long_command_line =
        g_strnfill(MMIX_FDT_COMMAND_LINE_MAX + 1, 'x');
    MMIXFDTConfig oversized_command_line = default_config(
        MMIX_VIRT_RAM_MIN_SIZE, long_command_line);

    assert_invalid_config(NULL, "configuration is missing");
    assert_invalid_config(&below_minimum, "RAM size");
    assert_invalid_config(&above_maximum, "RAM size");
    assert_invalid_config(&unaligned, "RAM size");
    assert_invalid_config(&missing_command_line, "command line is missing");
    assert_invalid_config(&oversized_command_line,
                          "command line exceeds 4095 bytes");
}

static void test_invalid_topology_preserves_result(void)
{
    const MMIXPhysRange short_stack = {
        .start = 0x10000,
        .end = 0x12000,
    };
    const MMIXPhysRange unaligned_stack = {
        .start = 0x10001,
        .end = 0x18001,
    };
    const MMIXPhysRange outside_stack = {
        .start = MMIX_VIRT_RAM_MIN_SIZE,
        .end = MMIX_VIRT_RAM_MIN_SIZE + MMIX_VIRT_INITIAL_STACK_SIZE,
    };
    const MMIXPhysRange overlapping_stacks[] = {
        default_stack,
        default_stack,
    };
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");

    config.cpu_count = 0;
    assert_invalid_config(&config, "CPU count 0 is invalid");
    config.cpu_count = MMIX_VIRT_MAX_CPUS + 1;
    assert_invalid_config(&config, "CPU count 65 is invalid");

    config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");
    config.cpu_stacks = NULL;
    assert_invalid_config(&config, "stack reservations are missing");
    config.cpu_stacks = &short_stack;
    assert_invalid_config(&config, "CPU 0 stack reservation is invalid");
    config.cpu_stacks = &unaligned_stack;
    assert_invalid_config(&config, "CPU 0 stack reservation is invalid");
    config.cpu_stacks = &outside_stack;
    assert_invalid_config(&config, "CPU 0 stack reservation is invalid");

    config.cpu_count = G_N_ELEMENTS(overlapping_stacks);
    config.cpu_stacks = overlapping_stacks;
    assert_invalid_config(&config, "stack reservations 0 and 1 overlap");
}

static void test_invalid_framebuffer_preserves_result(void)
{
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");

    config.has_framebuffer = true;
    config.framebuffer = (MMIXPhysRange) {
        .start = 0x10001,
        .end = 0x10001 + MMIX_VIRT_FRAMEBUFFER_SIZE,
    };
    assert_invalid_config(&config, "framebuffer reservation is invalid");

    config.framebuffer = (MMIXPhysRange) {
        .start = MMIX_VIRT_RAM_MIN_SIZE - MMIX_VIRT_FRAMEBUFFER_SIZE +
                 MMIX_VIRT_RAM_ALIGN,
        .end = MMIX_VIRT_RAM_MIN_SIZE + MMIX_VIRT_RAM_ALIGN,
    };
    assert_invalid_config(&config, "framebuffer reservation is invalid");

    config.framebuffer = (MMIXPhysRange) {
        .start = default_stack.start,
        .end = default_stack.start + MMIX_VIRT_FRAMEBUFFER_SIZE,
    };
    assert_invalid_config(&config,
                          "framebuffer reservation overlaps CPU 0");
}

static void test_missing_result_pointer(void)
{
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");
    Error *err = NULL;

    g_assert_false(mmix_fdt_build(&config, NULL, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "result pointer is missing"));
    error_free(err);
}

static void *mutable_fdt(GBytes *blob)
{
    gsize size;
    const void *source = g_bytes_get_data(blob, &size);
    void *fdt = g_malloc0(MMIX_FDT_MAX_SIZE);

    g_assert_cmpint(fdt_open_into(source, fdt, MMIX_FDT_MAX_SIZE), ==, 0);
    return fdt;
}

static void assert_invalid_fdt(void *fdt, const char *diagnostic)
{
    Error *err = NULL;

    g_assert_cmpint(fdt_pack(fdt), ==, 0);
    g_assert_false(mmix_fdt_validate(fdt, fdt_totalsize(fdt), &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    error_free(err);
}

static void test_invalid_serialized_fdt(void)
{
    g_autoptr(GBytes) blob = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "");
    g_autofree void *duplicate = mutable_fdt(blob);
    g_autofree void *dangling = mutable_fdt(blob);
    g_autofree void *invalid_cells = mutable_fdt(blob);
    g_autofree void *truncated_reg = mutable_fdt(blob);
    g_autofree void *overlap = mutable_fdt(blob);
    fdt32_t value;
    int node;

    value = cpu_to_fdt32(2);
    node = node_offset(duplicate, "/cpus/cpu@0");
    g_assert_cmpint(fdt_setprop(duplicate, node, "phandle", &value,
                               sizeof(value)), ==, 0);
    g_assert_cmpint(fdt_setprop(duplicate, node, "linux,phandle", &value,
                               sizeof(value)), ==, 0);
    assert_invalid_fdt(duplicate, "phandle 2 is duplicated");

    value = cpu_to_fdt32(0xdead);
    node = node_offset(dangling, "/soc/timer@1000020000000");
    g_assert_cmpint(fdt_setprop(dangling, node, "interrupt-parent", &value,
                               sizeof(value)), ==, 0);
    assert_invalid_fdt(dangling, "interrupt parent is missing");

    value = cpu_to_fdt32(1);
    g_assert_cmpint(fdt_setprop(invalid_cells, 0, "#address-cells", &value,
                               sizeof(value)), ==, 0);
    assert_invalid_fdt(invalid_cells,
                       "property '/#address-cells' is invalid");

    node = node_offset(truncated_reg, "/memory@0");
    g_assert_cmpint(fdt_setprop(truncated_reg, node, "reg", &value,
                               sizeof(value)), ==, 0);
    assert_invalid_fdt(truncated_reg, "truncated reg cells");

    g_assert_cmpint(fdt_add_mem_rsv(overlap, 0x4000000, 0x2000), ==, 0);
    g_assert_cmpint(fdt_add_mem_rsv(overlap, 0x4001000, 0x2000), ==, 0);
    assert_invalid_fdt(overlap, "reservations 0 and 1 overlap");
}

static void test_oversized_serialized_fdt(void)
{
    g_autoptr(GBytes) blob = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "");
    g_autofree void *fdt = g_malloc0(MMIX_FDT_MAX_SIZE + 1);
    Error *err = NULL;
    gsize size;
    const void *source = g_bytes_get_data(blob, &size);

    memcpy(fdt, source, size);
    fdt_set_totalsize(fdt, MMIX_FDT_MAX_SIZE + 1);
    g_assert_false(mmix_fdt_validate(fdt, MMIX_FDT_MAX_SIZE + 1, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "exceeds 2097152 bytes"));
    error_free(err);
}

static void test_linux_placement_finalization(void)
{
    const MMIXPhysRange fdt_range = {
        .start = 0x400000,
    };
    const MMIXPhysRange initrd_range = {
        .start = 0x200000,
        .end = 0x210000,
    };
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE,
                                          "console=ttyS0");
    g_autoptr(GBytes) template = NULL;
    g_autoptr(GBytes) finalized = NULL;
    MMIXPhysRange selected_fdt = fdt_range;
    const fdt64_t *value;
    const void *fdt;
    uint64_t address;
    uint64_t size;
    gsize template_size;
    int length;

    config.linux_direct = true;
    config.has_initrd = true;
    config.initrd_size = mmix_phys_range_size(&initrd_range);
    template = build_configured_fdt(&config);
    template_size = g_bytes_get_size(template);
    selected_fdt.end = selected_fdt.start + template_size;

    g_assert_true(mmix_fdt_finalize_linux(
        template, &selected_fdt, &initrd_range, &finalized, &error_abort));
    g_assert_cmpuint(g_bytes_get_size(finalized), ==, template_size);
    fdt = g_bytes_get_data(finalized, NULL);
    g_assert_cmpint(fdt_num_mem_rsv(fdt), ==, 2);
    g_assert_cmpint(fdt_get_mem_rsv(fdt, 0, &address, &size), ==, 0);
    g_assert_cmphex(address, ==, selected_fdt.start);
    g_assert_cmphex(size, ==, template_size);
    g_assert_cmpint(fdt_get_mem_rsv(fdt, 1, &address, &size), ==, 0);
    g_assert_cmphex(address, ==, initrd_range.start);
    g_assert_cmphex(size, ==, mmix_phys_range_size(&initrd_range));

    value = fdt_getprop(fdt, node_offset(fdt, "/chosen"),
                        "linux,initrd-start", &length);
    g_assert_nonnull(value);
    g_assert_cmpint(length, ==, sizeof(*value));
    g_assert_cmphex(fdt64_to_cpu(*value), ==, initrd_range.start);
    value = fdt_getprop(fdt, node_offset(fdt, "/chosen"),
                        "linux,initrd-end", &length);
    g_assert_nonnull(value);
    g_assert_cmpint(length, ==, sizeof(*value));
    g_assert_cmphex(fdt64_to_cpu(*value), ==, initrd_range.end);
}

static void test_linux_finalization_failure_is_atomic(void)
{
    MMIXFDTConfig config = default_config(MMIX_VIRT_RAM_MIN_SIZE, "");
    g_autoptr(GBytes) template = NULL;
    g_autoptr(GBytes) original = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "");
    GBytes *result = g_bytes_ref(original);
    MMIXPhysRange wrong_size = {
        .start = 0x400000,
        .end = 0x400008,
    };
    Error *err = NULL;

    config.linux_direct = true;
    template = build_configured_fdt(&config);
    g_assert_false(mmix_fdt_finalize_linux(template, &wrong_size, NULL,
                                           &result, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "reservation does not match its blob"));
    g_assert_true(result == original);
    error_free(err);

    err = NULL;
    g_clear_pointer(&template, g_bytes_unref);
    config.has_initrd = true;
    config.initrd_size = 0x10000;
    template = build_configured_fdt(&config);
    wrong_size = (MMIXPhysRange) {
        .start = 0x400000,
        .end = 0x400000 + g_bytes_get_size(template),
    };
    g_assert_false(mmix_fdt_finalize_linux(
        template, &wrong_size,
        &(MMIXPhysRange) { .start = 0x400000, .end = 0x410000 },
        &result, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "reservations 0 and 1 "
                            "overlap"));
    g_assert_true(result == original);
    error_free(err);
    g_bytes_unref(result);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/fdt-builder/default-foundation",
                    test_default_foundation);
    g_test_add_func("/mmix/fdt-builder/large-ram-command-line",
                    test_large_ram_and_command_line);
    g_test_add_func("/mmix/fdt-builder/deterministic-output",
                    test_deterministic_output);
    g_test_add_func("/mmix/fdt-builder/single-cpu-framebuffer",
                    test_single_cpu_with_framebuffer);
    g_test_add_func("/mmix/fdt-builder/maximum-cpu-topology",
                    test_maximum_cpu_topology);
    g_test_add_func("/mmix/fdt-builder/invalid-inputs-preserve-result",
                    test_invalid_inputs_preserve_result);
    g_test_add_func("/mmix/fdt-builder/invalid-topology-preserve-result",
                    test_invalid_topology_preserves_result);
    g_test_add_func("/mmix/fdt-builder/invalid-framebuffer-preserve-result",
                    test_invalid_framebuffer_preserves_result);
    g_test_add_func("/mmix/fdt-builder/missing-result-pointer",
                    test_missing_result_pointer);
    g_test_add_func("/mmix/fdt-builder/invalid-serialized-fdt",
                    test_invalid_serialized_fdt);
    g_test_add_func("/mmix/fdt-builder/oversized-serialized-fdt",
                    test_oversized_serialized_fdt);
    g_test_add_func("/mmix/fdt-builder/linux-placement-finalization",
                    test_linux_placement_finalization);
    g_test_add_func("/mmix/fdt-builder/linux-finalization-failure-atomic",
                    test_linux_finalization_failure_is_atomic);
    return g_test_run();
}
