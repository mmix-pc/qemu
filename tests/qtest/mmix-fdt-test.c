/*
 * MMIX virt direct-boot FDT tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "elf.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include <libfdt.h>

#ifndef EM_MMIX
#define EM_MMIX 80
#endif

enum {
    MMIX_MAX_CPUS = 64,
    MMIX_STACK_SIZE = 32 * KiB,
    MMIX_FRAMEBUFFER_SIZE = 3 * MiB,
    MMIX_FRAMEBUFFER_WIDTH = 1024,
    MMIX_FRAMEBUFFER_HEIGHT = 768,
    MMIX_FRAMEBUFFER_STRIDE = 4096,
    MMIX_INTC_SOURCE_COUNT = 8192,
    MMIX_UART_IRQ = 1,
    MMIX_RTC_IRQ = 2,
    MMIX_WATCHDOG_IRQ = 3,
    MMIX_TIMER_IRQ_BASE = 16,
    MMIX_VIRTIO_IRQ_BASE = 2048,
    MMIX_VIRTIO_COUNT = 32,
    MMIX_PCIE_INTX_IRQ_BASE = 6144,
    MMIX_PCIE_SLOT_COUNT = 32,
    MMIX_PCIE_PIN_COUNT = 4,
};

#define MMIX_UART_BASE UINT64_C(0x0001000010000000)
#define MMIX_RTC_BASE UINT64_C(0x0001000010010000)
#define MMIX_WATCHDOG_REFRESH_BASE UINT64_C(0x0001000010020000)
#define MMIX_WATCHDOG_CONTROL_BASE UINT64_C(0x0001000010030000)
#define MMIX_POWER_BASE UINT64_C(0x0001000010040000)
#define MMIX_FLASH0_BASE UINT64_C(0x0001000000000000)
#define MMIX_FLASH1_BASE UINT64_C(0x0001000004000000)
#define MMIX_FLASH_BANK_SIZE UINT64_C(0x4000000)
#define MMIX_FRAMEBUFFER_CONTROL_BASE UINT64_C(0x0001000018000000)
#define MMIX_TIMER_BASE UINT64_C(0x0001000020000000)
#define MMIX_TIMER_CONTEXT_BASE UINT64_C(0x0001000020010000)
#define MMIX_IPI_BASE UINT64_C(0x0001000024000000)
#define MMIX_IPI_CONTEXT_BASE UINT64_C(0x0001000024010000)
#define MMIX_INTC_BASE UINT64_C(0x0001000030000000)
#define MMIX_INTC_CONTEXT_BASE UINT64_C(0x0001000034000000)
#define MMIX_VIRTIO_BASE UINT64_C(0x0001000040000000)
#define MMIX_PCIE_ECAM_BASE UINT64_C(0x0001000100000000)
#define MMIX_PCIE_MMIO32_BASE UINT64_C(0x0001000200000000)
#define MMIX_PCIE_MMIO64_BASE UINT64_C(0x0001010000000000)
#define MMIX_PCIE_MMIO64_BUS_BASE UINT64_C(0x0000010000000000)

#define MMIX_CONTEXT_STRIDE UINT64_C(0x10000)
#define MMIX_FRAMEBUFFER_BASE_REGISTER 0x20

typedef struct MMIXFDTCase {
    const char *name;
    const char *memory;
    uint64_t ram_size;
    unsigned int cpu_count;
    const char *command_line;
    bool has_initrd;
} MMIXFDTCase;

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

static void assert_string_list(const void *fdt, const char *path,
                               const char *name, const char *const *expected,
                               size_t count)
{
    int node = node_offset(fdt, path);
    size_t i;

    g_assert_cmpint(fdt_stringlist_count(fdt, node, name), ==, count);
    for (i = 0; i < count; i++) {
        int length;
        const char *actual = fdt_stringlist_get(fdt, node, name, i,
                                                &length);

        g_assert_nonnull(actual);
        g_assert_cmpint(length, ==, strlen(expected[i]));
        g_assert_cmpstr(actual, ==, expected[i]);
    }
}

static void assert_compatible_count(const void *fdt, const char *compatible,
                                    unsigned int expected)
{
    unsigned int count = 0;
    int node = -1;

    while ((node = fdt_node_offset_by_compatible(fdt, node,
                                                  compatible)) >= 0) {
        count++;
    }
    g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
    g_assert_cmpuint(count, ==, expected);
}

static uint32_t get_u32(const void *fdt, const char *path,
                        const char *name)
{
    int length;
    const fdt32_t *value = fdt_getprop(fdt, node_offset(fdt, path), name,
                                       &length);

    g_assert_nonnull(value);
    g_assert_cmpint(length, ==, sizeof(*value));
    return fdt32_to_cpu(*value);
}

static uint64_t get_u64(const void *fdt, const char *path,
                        const char *name)
{
    int length;
    const fdt64_t *value = fdt_getprop(fdt, node_offset(fdt, path), name,
                                       &length);

    g_assert_nonnull(value);
    g_assert_cmpint(length, ==, sizeof(*value));
    return fdt64_to_cpu(*value);
}

static void assert_u32(const void *fdt, const char *path,
                       const char *name, uint32_t expected)
{
    g_assert_cmpuint(get_u32(fdt, path, name), ==, expected);
}

static void assert_empty(const void *fdt, const char *path,
                         const char *name)
{
    int length;
    const void *value = fdt_getprop(fdt, node_offset(fdt, path), name,
                                    &length);

    g_assert_nonnull(value);
    g_assert_cmpint(length, ==, 0);
}

static void assert_absent(const void *fdt, const char *path,
                          const char *name)
{
    int length;

    g_assert_null(fdt_getprop(fdt, node_offset(fdt, path), name, &length));
    g_assert_cmpint(length, ==, -FDT_ERR_NOTFOUND);
}

static void assert_range(const void *fdt, const char *path,
                         uint64_t base, uint64_t size)
{
    int length;
    const fdt64_t *reg = fdt_getprop(fdt, node_offset(fdt, path), "reg",
                                     &length);

    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 2 * sizeof(*reg));
    g_assert_cmphex(fdt64_to_cpu(reg[0]), ==, base);
    g_assert_cmphex(fdt64_to_cpu(reg[1]), ==, size);
}

static void assert_two_ranges(const void *fdt, const char *path,
                              uint64_t base0, uint64_t size0,
                              uint64_t base1, uint64_t size1)
{
    int length;
    const fdt64_t *reg = fdt_getprop(fdt, node_offset(fdt, path), "reg",
                                     &length);

    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 4 * sizeof(*reg));
    g_assert_cmphex(fdt64_to_cpu(reg[0]), ==, base0);
    g_assert_cmphex(fdt64_to_cpu(reg[1]), ==, size0);
    g_assert_cmphex(fdt64_to_cpu(reg[2]), ==, base1);
    g_assert_cmphex(fdt64_to_cpu(reg[3]), ==, size1);
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
        g_assert_cmpuint(fdt32_to_cpu(actual[i]), ==, expected[i]);
    }
}

static void assert_pcie_host(const void *fdt, uint32_t intc_phandle)
{
    const char *path = "/pcie@1000100000000";
    const uint32_t bus_range[] = { 0, 255 };
    const uint32_t ranges[] = {
        0x02000000, 0, 0,
        MMIX_PCIE_MMIO32_BASE >> 32, (uint32_t)MMIX_PCIE_MMIO32_BASE,
        1, 0,
        0x43000000,
        MMIX_PCIE_MMIO64_BUS_BASE >> 32,
        (uint32_t)MMIX_PCIE_MMIO64_BUS_BASE,
        MMIX_PCIE_MMIO64_BASE >> 32, (uint32_t)MMIX_PCIE_MMIO64_BASE,
        UINT64_C(0x0000100000000000) >> 32, 0,
    };
    const uint32_t mask[] = { 0xf800, 0, 0, 0x7 };
    const fdt32_t *map;
    int length;
    unsigned int slot;
    unsigned int pin;
    size_t cell = 0;

    assert_string(fdt, path, "compatible", "pci-host-ecam-generic");
    assert_string(fdt, path, "device_type", "pci");
    assert_u32(fdt, path, "#address-cells", 3);
    assert_u32(fdt, path, "#size-cells", 2);
    assert_u32(fdt, path, "#interrupt-cells", 1);
    assert_u32_array(fdt, path, "bus-range", bus_range,
                     G_N_ELEMENTS(bus_range));
    assert_range(fdt, path, MMIX_PCIE_ECAM_BASE, 0x10000000);
    assert_empty(fdt, path, "dma-coherent");
    assert_u32_array(fdt, path, "ranges", ranges, G_N_ELEMENTS(ranges));
    assert_u32_array(fdt, path, "interrupt-map-mask", mask,
                     G_N_ELEMENTS(mask));
    map = fdt_getprop(fdt, node_offset(fdt, path), "interrupt-map",
                      &length);
    g_assert_nonnull(map);
    g_assert_cmpint(length, ==, MMIX_PCIE_SLOT_COUNT *
                                MMIX_PCIE_PIN_COUNT * 6 * sizeof(*map));
    for (slot = 0; slot < MMIX_PCIE_SLOT_COUNT; slot++) {
        for (pin = 0; pin < MMIX_PCIE_PIN_COUNT; pin++, cell += 6) {
            g_assert_cmpuint(fdt32_to_cpu(map[cell]), ==, slot << 11);
            g_assert_cmpuint(fdt32_to_cpu(map[cell + 1]), ==, 0);
            g_assert_cmpuint(fdt32_to_cpu(map[cell + 2]), ==, 0);
            g_assert_cmpuint(fdt32_to_cpu(map[cell + 3]), ==, pin + 1);
            g_assert_cmpuint(fdt32_to_cpu(map[cell + 4]), ==,
                             intc_phandle);
            g_assert_cmpuint(fdt32_to_cpu(map[cell + 5]), ==,
                             MMIX_PCIE_INTX_IRQ_BASE +
                             (slot + pin) % MMIX_PCIE_PIN_COUNT);
        }
    }
    assert_absent(fdt, path, "msi-parent");
    assert_absent(fdt, path, "dma-ranges");
    assert_absent(fdt, path, "iommu-map");
    assert_compatible_count(fdt, "pci-host-ecam-generic", 1);
}

static uint64_t hmp_register_value(const char *registers, const char *name)
{
    g_autofree char *prefix = g_strdup_printf("%s=0x", name);
    const char *value = strstr(registers, prefix);

    g_assert_nonnull(value);
    return g_ascii_strtoull(value + strlen(prefix), NULL, 16);
}

static uint64_t cpu_initial_stack(QTestState *qts, unsigned int cpu)
{
    g_autofree char *path = g_strdup_printf("/machine/cpu[%u]", cpu);
    g_autoptr(QDict) response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get',"
        "  'arguments': { 'path': %s,"
        "                 'property': 'initial-stack' } }",
        path);

    return qdict_get_int(response, "return");
}

static char *create_linux_elf(const char *directory)
{
    enum { CODE_OFFSET = 0x100 };
    const uint64_t entry = 0x10000;
    const uint8_t code[] = { 0xfd, 0x00, 0x00, 0x00 };
    uint8_t image[CODE_OFFSET + sizeof(code)] = { 0 };
    Elf64_Ehdr ehdr = { 0 };
    Elf64_Phdr phdr = { 0 };
    g_autofree char *filename = g_build_filename(directory, "linux.elf",
                                                  NULL);
    g_autoptr(GError) error = NULL;

    memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = cpu_to_be16(ET_EXEC);
    ehdr.e_machine = cpu_to_be16(EM_MMIX);
    ehdr.e_version = cpu_to_be32(EV_CURRENT);
    ehdr.e_entry = cpu_to_be64(entry);
    ehdr.e_phoff = cpu_to_be64(sizeof(ehdr));
    ehdr.e_ehsize = cpu_to_be16(sizeof(ehdr));
    ehdr.e_phentsize = cpu_to_be16(sizeof(phdr));
    ehdr.e_phnum = cpu_to_be16(1);

    phdr.p_type = cpu_to_be32(PT_LOAD);
    phdr.p_flags = cpu_to_be32(PF_R | PF_X);
    phdr.p_offset = cpu_to_be64(CODE_OFFSET);
    phdr.p_vaddr = cpu_to_be64(entry);
    phdr.p_paddr = cpu_to_be64(entry);
    phdr.p_filesz = cpu_to_be64(sizeof(code));
    phdr.p_memsz = cpu_to_be64(sizeof(code));
    phdr.p_align = cpu_to_be64(1);

    memcpy(image, &ehdr, sizeof(ehdr));
    memcpy(image + sizeof(ehdr), &phdr, sizeof(phdr));
    memcpy(image + CODE_OFFSET, code, sizeof(code));
    g_assert_true(g_file_set_contents(filename, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    return g_steal_pointer(&filename);
}

static char *create_initrd(const char *directory)
{
    uint8_t image[2 * KiB];
    g_autofree char *filename = g_build_filename(directory, "initrd.img",
                                                  NULL);
    g_autoptr(GError) error = NULL;
    unsigned int i;

    for (i = 0; i < sizeof(image); i++) {
        image[i] = i;
    }
    g_assert_true(g_file_set_contents(filename, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    return g_steal_pointer(&filename);
}

static void *read_guest_fdt(QTestState *qts, uint64_t *guest_address,
                            size_t *size)
{
    struct fdt_header header;
    g_autofree char *registers = qtest_hmp(qts, "info registers 0");
    void *fdt;

    *guest_address = hmp_register_value(registers, "r1  ");
    g_assert_cmphex(*guest_address, !=, 0);
    qtest_memread(qts, *guest_address, &header, sizeof(header));
    g_assert_cmphex(fdt32_to_cpu(header.magic), ==, FDT_MAGIC);
    *size = fdt32_to_cpu(header.totalsize);
    g_assert_cmpuint(*size, >=, sizeof(header));
    g_assert_cmpuint(*size, <=, 2 * MiB);
    fdt = g_malloc(*size);
    qtest_memread(qts, *guest_address, fdt, *size);
    g_assert_cmpint(fdt_check_full(fdt, *size), ==, 0);
    return fdt;
}

static void assert_phandles(const void *fdt)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash,
                                                  g_direct_equal);
    int depth = 0;
    int node = -1;

    while ((node = fdt_next_node(fdt, node, &depth)) >= 0) {
        int length;
        const fdt32_t *property = fdt_getprop(fdt, node, "phandle",
                                              &length);

        if (property) {
            uint32_t phandle = fdt32_to_cpu(*property);
            const fdt32_t *linux_phandle;

            g_assert_cmpint(length, ==, sizeof(*property));
            g_assert_cmpuint(phandle, !=, 0);
            g_assert_false(g_hash_table_contains(
                               seen, GUINT_TO_POINTER(phandle)));
            g_hash_table_add(seen, GUINT_TO_POINTER(phandle));
            linux_phandle = fdt_getprop(fdt, node, "linux,phandle",
                                        &length);
            g_assert_nonnull(linux_phandle);
            g_assert_cmpint(length, ==, sizeof(*linux_phandle));
            g_assert_cmpuint(fdt32_to_cpu(*linux_phandle), ==, phandle);
            g_assert_cmpint(fdt_node_offset_by_phandle(fdt, phandle), ==,
                            node);
        }
    }
    g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
}

static void assert_cpu_topology(QTestState *qts, const void *fdt,
                                unsigned int cpu_count)
{
    int child = fdt_first_subnode(fdt, node_offset(fdt, "/cpus"));
    unsigned int cpu;

    for (cpu = 0; cpu < cpu_count; cpu++) {
        g_autofree char *name = g_strdup_printf("cpu@%x", cpu);
        g_autofree char *cpu_path = g_strdup_printf("/cpus/%s", name);
        uint64_t stack = cpu_initial_stack(qts, cpu);
        g_autofree char *stack_path = g_strdup_printf(
            "/reserved-memory/register-stack@%" PRIx64, stack);
        uint32_t cpu_phandle;
        uint32_t stack_phandle;

        g_assert_cmpint(child, >=, 0);
        g_assert_cmpstr(fdt_get_name(fdt, child, NULL), ==, name);
        assert_string(fdt, cpu_path, "device_type", "cpu");
        assert_string(fdt, cpu_path, "compatible", "qemu,mmix-cpu");
        assert_u32(fdt, cpu_path, "reg", cpu);
        assert_string(fdt, cpu_path, "status", "okay");
        assert_string(fdt, cpu_path, "enable-method",
                      "qemu,mmix-immediate-entry");
        cpu_phandle = get_u32(fdt, cpu_path, "phandle");
        stack_phandle = get_u32(fdt, cpu_path,
                                "qemu,initial-register-stack");
        assert_string(fdt, stack_path, "compatible",
                      "qemu,mmix-register-stack");
        assert_range(fdt, stack_path, stack, MMIX_STACK_SIZE);
        assert_u32(fdt, stack_path, "phandle", stack_phandle);
        assert_u32(fdt, stack_path, "qemu,cpu", cpu_phandle);
        assert_absent(fdt, stack_path, "no-map");
        assert_absent(fdt, stack_path, "reusable");
        child = fdt_next_subnode(fdt, child);
    }
    g_assert_cmpint(child, ==, -FDT_ERR_NOTFOUND);
}

static void assert_interrupt_topology(QTestState *qts, const void *fdt,
                                      unsigned int cpu_count,
                                      uint32_t intc_phandle)
{
    const char *intc = "/soc/interrupt-controller@1000030000000";
    const char *ipi = "/soc/ipi@1000024000000";
    const char *timer = "/soc/timer@1000020000000";
    int length;
    const fdt32_t *interrupts;
    const fdt32_t *affinity;
    unsigned int cpu;

    assert_string(fdt, intc, "compatible", "qemu,mmix-intc");
    assert_empty(fdt, intc, "interrupt-controller");
    assert_u32(fdt, intc, "#interrupt-cells", 1);
    assert_two_ranges(fdt, intc, MMIX_INTC_BASE, 0x10000,
                      MMIX_INTC_CONTEXT_BASE,
                      cpu_count * MMIX_CONTEXT_STRIDE);
    assert_u32(fdt, intc, "qemu,source-count", MMIX_INTC_SOURCE_COUNT);
    assert_u32(fdt, intc, "qemu,context-count", cpu_count);
    assert_u32(fdt, intc, "qemu,context-stride", MMIX_CONTEXT_STRIDE);
    assert_u32(fdt, intc, "phandle", intc_phandle);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE), ==,
                    MMIX_INTC_SOURCE_COUNT);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE + 8), ==, cpu_count);

    assert_string(fdt, ipi, "compatible", "qemu,mmix-ipi");
    assert_two_ranges(fdt, ipi, MMIX_IPI_BASE, 0x10000,
                      MMIX_IPI_CONTEXT_BASE,
                      cpu_count * MMIX_CONTEXT_STRIDE);
    assert_u32(fdt, ipi, "qemu,context-count", cpu_count);
    assert_u32(fdt, ipi, "qemu,context-stride", MMIX_CONTEXT_STRIDE);
    assert_u32(fdt, ipi, "qemu,request-bit", 9);
    assert_absent(fdt, ipi, "interrupt-parent");
    assert_absent(fdt, ipi, "interrupts");

    assert_string(fdt, timer, "compatible", "qemu,mmix-timer");
    assert_two_ranges(fdt, timer, MMIX_TIMER_BASE, 0x10000,
                      MMIX_TIMER_CONTEXT_BASE,
                      cpu_count * MMIX_CONTEXT_STRIDE);
    assert_u32(fdt, timer, "interrupt-parent", intc_phandle);
    assert_u32(fdt, timer, "clock-frequency", 1000000000);
    assert_u32(fdt, timer, "qemu,context-count", cpu_count);
    assert_u32(fdt, timer, "qemu,context-stride", MMIX_CONTEXT_STRIDE);
    interrupts = fdt_getprop(fdt, node_offset(fdt, timer), "interrupts",
                             &length);
    g_assert_nonnull(interrupts);
    g_assert_cmpint(length, ==, cpu_count * sizeof(*interrupts));
    affinity = fdt_getprop(fdt, node_offset(fdt, timer),
                           "interrupt-affinity", &length);
    g_assert_nonnull(affinity);
    g_assert_cmpint(length, ==, cpu_count * sizeof(*affinity));
    for (cpu = 0; cpu < cpu_count; cpu++) {
        g_autofree char *cpu_path = g_strdup_printf("/cpus/cpu@%x", cpu);

        g_assert_cmpuint(fdt32_to_cpu(interrupts[cpu]), ==,
                         MMIX_TIMER_IRQ_BASE + cpu);
        g_assert_cmpuint(fdt32_to_cpu(affinity[cpu]), ==,
                         get_u32(fdt, cpu_path, "phandle"));
    }
}

static void assert_virtio_node_order(const void *fdt)
{
    unsigned int slot = 0;
    int soc = node_offset(fdt, "/soc");
    int node;

    fdt_for_each_subnode(node, fdt, soc) {
        const fdt64_t *reg;
        int length;

        if (fdt_node_check_compatible(fdt, node, "virtio,mmio") != 0) {
            continue;
        }
        g_assert_cmpuint(slot, <, MMIX_VIRTIO_COUNT);
        reg = fdt_getprop(fdt, node, "reg", &length);
        g_assert_nonnull(reg);
        g_assert_cmpint(length, ==, 2 * sizeof(*reg));
        g_assert_cmphex(fdt64_to_cpu(reg[0]), ==,
                        MMIX_VIRTIO_BASE + slot * MMIX_CONTEXT_STRIDE);
        slot++;
    }
    g_assert_cmpuint(slot, ==, MMIX_VIRTIO_COUNT);
}

static void assert_active_devices(QTestState *qts, const void *fdt,
                                  uint32_t intc_phandle)
{
    static const char *const power_compatible[] = {
        "qemu,mmix-virt-syscon",
        "syscon",
    };
    const char *flash = "/flash@1000000000000";
    const char *uart = "/soc/serial@1000010000000";
    const char *rtc = "/soc/rtc@1000010010000";
    const char *watchdog = "/soc/watchdog@1000010030000";
    const char *power = "/soc/syscon@1000010040000";
    const char *control = "/soc/framebuffer@1000018000000";
    uint64_t framebuffer = qtest_readq(
        qts, MMIX_FRAMEBUFFER_CONTROL_BASE +
             MMIX_FRAMEBUFFER_BASE_REGISTER);
    g_autofree char *memory = g_strdup_printf(
        "/reserved-memory/framebuffer@%" PRIx64, framebuffer);
    g_autofree char *simple = g_strdup_printf(
        "/chosen/framebuffer@%" PRIx64, framebuffer);
    uint32_t framebuffer_phandle;
    unsigned int slot;

    assert_string(fdt, uart, "compatible", "ns16550a");
    assert_range(fdt, uart, MMIX_UART_BASE, 8);
    assert_u32(fdt, uart, "clock-frequency", 1843200);
    assert_u32(fdt, uart, "current-speed", 115200);
    assert_u32(fdt, uart, "reg-shift", 0);
    assert_u32(fdt, uart, "reg-io-width", 1);
    assert_u32(fdt, uart, "interrupts", MMIX_UART_IRQ);
    assert_u32(fdt, uart, "interrupt-parent", intc_phandle);
    assert_string(fdt, "/aliases", "serial0", uart);
    assert_string(fdt, "/chosen", "stdout-path", "serial0:115200n8");

    assert_string(fdt, rtc, "compatible", "google,goldfish-rtc");
    assert_range(fdt, rtc, MMIX_RTC_BASE, 0x24);
    assert_u32(fdt, rtc, "interrupts", MMIX_RTC_IRQ);
    assert_u32(fdt, rtc, "interrupt-parent", intc_phandle);
    assert_absent(fdt, rtc, "little-endian");
    assert_absent(fdt, rtc, "big-endian");
    assert_absent(fdt, rtc, "native-endian");

    assert_string(fdt, watchdog, "compatible", "arm,sbsa-gwdt");
    assert_two_ranges(fdt, watchdog,
                      MMIX_WATCHDOG_CONTROL_BASE, 0x1000,
                      MMIX_WATCHDOG_REFRESH_BASE, 0x1000);
    assert_u32(fdt, watchdog, "interrupts", MMIX_WATCHDOG_IRQ);
    assert_u32(fdt, watchdog, "interrupt-parent", intc_phandle);
    assert_u32(fdt, watchdog, "clock-frequency", 1000000000);
    assert_absent(fdt, watchdog, "little-endian");
    assert_absent(fdt, watchdog, "big-endian");
    assert_absent(fdt, watchdog, "native-endian");

    assert_string_list(fdt, power, "compatible", power_compatible,
                       G_N_ELEMENTS(power_compatible));
    assert_range(fdt, power, MMIX_POWER_BASE, 0x100);
    assert_empty(fdt, power, "big-endian");
    assert_absent(fdt, power, "little-endian");
    assert_absent(fdt, power, "native-endian");
    assert_absent(fdt, power, "interrupts");
    assert_absent(fdt, power, "interrupt-parent");

    assert_compatible_count(fdt, "google,goldfish-rtc", 1);
    assert_compatible_count(fdt, "arm,sbsa-gwdt", 1);
    assert_compatible_count(fdt, "qemu,mmix-virt-syscon", 1);

    assert_string(fdt, flash, "compatible", "cfi-flash");
    assert_two_ranges(fdt, flash,
                      MMIX_FLASH0_BASE, MMIX_FLASH_BANK_SIZE,
                      MMIX_FLASH1_BASE, MMIX_FLASH_BANK_SIZE);
    assert_u32(fdt, flash, "bank-width", 4);

    assert_string(fdt, "/fw-cfg@1000014000000", "compatible",
                  "qemu,fw-cfg-mmio");
    assert_range(fdt, "/fw-cfg@1000014000000",
                 UINT64_C(0x0001000014000000), 0x18);
    assert_empty(fdt, "/fw-cfg@1000014000000", "dma-coherent");

    framebuffer_phandle = get_u32(fdt, memory, "phandle");
    assert_string(fdt, memory, "compatible",
                  "qemu,mmix-framebuffer-memory");
    assert_range(fdt, memory, framebuffer, MMIX_FRAMEBUFFER_SIZE);
    assert_empty(fdt, memory, "no-map");
    assert_string(fdt, control, "compatible", "qemu,mmix-framebuffer");
    assert_range(fdt, control, MMIX_FRAMEBUFFER_CONTROL_BASE, 0x1000);
    assert_u32(fdt, control, "memory-region", framebuffer_phandle);
    assert_string(fdt, simple, "compatible", "simple-framebuffer");
    assert_range(fdt, simple, framebuffer, MMIX_FRAMEBUFFER_SIZE);
    assert_u32(fdt, simple, "width", MMIX_FRAMEBUFFER_WIDTH);
    assert_u32(fdt, simple, "height", MMIX_FRAMEBUFFER_HEIGHT);
    assert_u32(fdt, simple, "stride", MMIX_FRAMEBUFFER_STRIDE);
    assert_string(fdt, simple, "format", "x8r8g8b8");
    assert_string(fdt, simple, "status", "okay");
    assert_u32(fdt, simple, "memory-region", framebuffer_phandle);

    for (slot = 0; slot < MMIX_VIRTIO_COUNT; slot++) {
        uint64_t base = MMIX_VIRTIO_BASE + slot * MMIX_CONTEXT_STRIDE;
        g_autofree char *path = g_strdup_printf(
            "/soc/virtio_mmio@%" PRIx64, base);

        assert_string(fdt, path, "compatible", "virtio,mmio");
        assert_range(fdt, path, base, 0x200);
        assert_u32(fdt, path, "interrupts", MMIX_VIRTIO_IRQ_BASE + slot);
        assert_u32(fdt, path, "interrupt-parent", intc_phandle);
        g_assert_cmphex(qtest_readl(qts, base), ==, 0x74726976);
    }
    g_assert_cmpint(fdt_path_offset(fdt,
                                   "/soc/virtio_mmio@1000040200000"), ==,
                    -FDT_ERR_NOTFOUND);
    assert_pcie_host(fdt, intc_phandle);
    assert_virtio_node_order(fdt);
}

static void assert_foundation(const void *fdt, size_t size,
                              const MMIXFDTCase *test)
{
    assert_string(fdt, "/", "compatible", "qemu,mmix-virt");
    assert_string(fdt, "/", "model", "QEMU MMIX Virt Machine");
    assert_u32(fdt, "/", "#address-cells", 2);
    assert_u32(fdt, "/", "#size-cells", 2);
    g_assert_cmpuint(fdt_totalsize(fdt), ==, size);
    g_assert_cmpuint(fdt_version(fdt), ==, 17);
    assert_u32(fdt, "/chosen", "#address-cells", 2);
    assert_u32(fdt, "/chosen", "#size-cells", 2);
    assert_string(fdt, "/chosen", "bootargs", test->command_line);
    assert_string(fdt, "/memory@0", "device_type", "memory");
    assert_range(fdt, "/memory@0", 0, test->ram_size);
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

static void assert_reservations(QTestState *qts, const void *fdt,
                                uint64_t fdt_address, size_t fdt_size,
                                bool has_initrd)
{
    uint64_t address;
    uint64_t size;

    g_assert_cmpint(fdt_num_mem_rsv(fdt), ==, has_initrd ? 2 : 1);
    g_assert_cmpint(fdt_get_mem_rsv(fdt, 0, &address, &size), ==, 0);
    g_assert_cmphex(address, ==, fdt_address);
    g_assert_cmphex(size, ==, fdt_size);
    if (has_initrd) {
        uint8_t first[16];
        unsigned int i;

        g_assert_cmpint(fdt_get_mem_rsv(fdt, 1, &address, &size), ==, 0);
        g_assert_cmphex(address, ==,
                        get_u64(fdt, "/chosen", "linux,initrd-start"));
        g_assert_cmphex(address + size, ==,
                        get_u64(fdt, "/chosen", "linux,initrd-end"));
        g_assert_cmphex(size, ==, 2 * KiB);
        qtest_memread(qts, address, first, sizeof(first));
        for (i = 0; i < sizeof(first); i++) {
            g_assert_cmpuint(first[i], ==, i);
        }
    } else {
        assert_absent(fdt, "/chosen", "linux,initrd-start");
        assert_absent(fdt, "/chosen", "linux,initrd-end");
    }
}

static void test_direct_boot_fdt(gconstpointer opaque)
{
    const MMIXFDTCase *test = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mmix-fdt-XXXXXX", &error);
    g_autofree char *kernel = NULL;
    g_autofree char *initrd = NULL;
    g_autofree char *args = NULL;
    g_autofree void *fdt = NULL;
    uint64_t fdt_address;
    size_t fdt_size;
    uint32_t intc_phandle;
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_nonnull(directory);
    kernel = create_linux_elf(directory);
    if (test->has_initrd) {
        initrd = create_initrd(directory);
    }
    args = g_strdup_printf(
        "-machine virt,elf-startup-abi=linux -m %s -smp %u "
        "-kernel %s -append '%s'%s%s",
        test->memory, test->cpu_count, kernel, test->command_line,
        initrd ? " -initrd " : "", initrd ?: "");
    qts = qtest_init(args);
    fdt = read_guest_fdt(qts, &fdt_address, &fdt_size);

    assert_foundation(fdt, fdt_size, test);
    assert_phandles(fdt);
    assert_cpu_topology(qts, fdt, test->cpu_count);
    intc_phandle = get_u32(
        fdt, "/soc/interrupt-controller@1000030000000", "phandle");
    assert_interrupt_topology(qts, fdt, test->cpu_count, intc_phandle);
    assert_active_devices(qts, fdt, intc_phandle);
    assert_reservations(qts, fdt, fdt_address, fdt_size,
                        test->has_initrd);
    qtest_quit(qts);

    if (initrd) {
        g_assert_cmpint(g_unlink(initrd), ==, 0);
    }
    g_assert_cmpint(g_unlink(kernel), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    static const MMIXFDTCase cases[] = {
        {
            .name = "minimum-ram",
            .memory = "128M",
            .ram_size = 128 * MiB,
            .cpu_count = 1,
            .command_line = "",
        },
        {
            .name = "default-ram",
            .memory = "512M",
            .ram_size = 512 * MiB,
            .cpu_count = 1,
            .command_line = "console=ttyS0",
        },
        {
            .name = "large-smp-initrd",
            .memory = "8G",
            .ram_size = 8 * GiB,
            .cpu_count = MMIX_MAX_CPUS,
            .command_line = "console=ttyS0 root=/dev/vda",
            .has_initrd = true,
        },
    };
    unsigned int i;

    g_test_init(&argc, &argv, NULL);
    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *path = g_strdup_printf("/mmix/fdt/%s",
                                                cases[i].name);

        g_test_add_data_func(path, &cases[i], test_direct_boot_fdt);
    }
    return g_test_run();
}
