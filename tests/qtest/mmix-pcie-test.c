/*
 * MMIX virt PCI Express host bridge tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define MMIX_PCIE_ECAM_BASE UINT64_C(0x0001000100000000)
#define MMIX_PCIE_ECAM_SIZE UINT64_C(0x10000000)
#define MMIX_PCIE_MMIO32_BASE UINT64_C(0x0001000200000000)
#define MMIX_PCIE_MMIO32_SIZE UINT64_C(0x0000000100000000)
#define MMIX_PCIE_MMIO32_BUS_BASE UINT64_C(0)
#define MMIX_PCIE_MMIO64_BASE UINT64_C(0x0001010000000000)
#define MMIX_PCIE_MMIO64_SIZE UINT64_C(0x0000100000000000)
#define MMIX_PCIE_MMIO64_BUS_BASE UINT64_C(0x0000010000000000)
#define MMIX_PCIE_BUS_SIZE UINT64_C(0x100000)
#define MMIX_PCIE_DEVICE_SIZE UINT64_C(0x8000)
#define MMIX_PCIE_FUNCTION_SIZE UINT64_C(0x1000)

#define MMIX_INTC_BASE UINT64_C(0x0001000030000000)
#define MMIX_INTC_PENDING_BASE 0x1000
#define MMIX_INTC_CONTEXT_BASE UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_STRIDE UINT64_C(0x10000)
#define MMIX_INTC_CLAIM 0x0800
#define MMIX_INTC_COMPLETE 0x0808
#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ "sysbus-irq"

#define MMIX_PCIE_INTX_IRQ_BASE 6144
#define MMIX_PCIE_INTX_IRQ_COUNT 4
#define MMIX_PCIE_RESERVED_IRQ_BASE 6148
#define MMIX_PCIE_RESERVED_IRQ_END 7168

#define MMIX_EDU_BAR_SIZE UINT64_C(0x100000)
#define MMIX_EDU_IRQ_RAISE 0x60
#define MMIX_EDU_IRQ_ACK 0x64

static uint64_t mmix_pcie_ecam_address(unsigned int bus,
                                       unsigned int device,
                                       unsigned int function,
                                       unsigned int reg)
{
    return MMIX_PCIE_ECAM_BASE + bus * MMIX_PCIE_BUS_SIZE +
           device * MMIX_PCIE_DEVICE_SIZE +
           function * MMIX_PCIE_FUNCTION_SIZE + reg;
}

static uint32_t mmix_pcie_readl(QTestState *qts, uint64_t address)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static void mmix_pcie_writew(QTestState *qts, uint64_t address,
                             uint16_t value)
{
    uint8_t bytes[sizeof(value)];

    stw_le_p(bytes, value);
    qtest_memwrite(qts, address, bytes, sizeof(bytes));
}

static void mmix_pcie_writel(QTestState *qts, uint64_t address,
                             uint32_t value)
{
    uint8_t bytes[sizeof(value)];

    stl_le_p(bytes, value);
    qtest_memwrite(qts, address, bytes, sizeof(bytes));
}

static uint64_t mmix_intc_word_reg(uint64_t base, unsigned int source)
{
    return base + (source / 64) * sizeof(uint64_t);
}

static uint64_t mmix_intc_source_bit(unsigned int source)
{
    return UINT64_C(1) << (source % 64);
}

static uint64_t mmix_intc_context_reg(unsigned int cpu, uint64_t reg)
{
    return MMIX_INTC_CONTEXT_BASE + cpu * MMIX_INTC_CONTEXT_STRIDE + reg;
}

static uint64_t mmix_intc_pending(QTestState *qts, unsigned int source)
{
    return qtest_readq(qts, mmix_intc_word_reg(
                           MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE, source));
}

static uint64_t mmix_intc_enable(QTestState *qts, unsigned int cpu,
                                 unsigned int source)
{
    return qtest_readq(qts, mmix_intc_word_reg(
                           mmix_intc_context_reg(cpu, 0), source));
}

static void mmix_intc_write_enable(QTestState *qts, unsigned int cpu,
                                   unsigned int source, uint64_t value)
{
    qtest_writeq(qts, mmix_intc_word_reg(
                     mmix_intc_context_reg(cpu, 0), source), value);
}

static uint64_t mmix_intc_claim(QTestState *qts, unsigned int cpu)
{
    return qtest_readq(qts, mmix_intc_context_reg(cpu, MMIX_INTC_CLAIM));
}

static void mmix_intc_complete(QTestState *qts, unsigned int cpu,
                               unsigned int source)
{
    qtest_writeq(qts, mmix_intc_context_reg(cpu, MMIX_INTC_COMPLETE), source);
}

static QTestState *mmix_pcie_irq_start(unsigned int cpus, const char *devices)
{
    QTestState *qts = qtest_initf("-machine virt -smp %u %s", cpus,
                                  devices);

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static uint64_t mmix_edu_configure(QTestState *qts, unsigned int slot,
                                   uint64_t pci_address)
{
    uint64_t config = mmix_pcie_ecam_address(0, slot, 0, 0);

    g_assert_cmphex(pci_address % MMIX_EDU_BAR_SIZE, ==, 0);
    mmix_pcie_writel(qts, config + PCI_BASE_ADDRESS_0, pci_address);
    mmix_pcie_writew(qts, config + PCI_COMMAND, PCI_COMMAND_MEMORY);
    return MMIX_PCIE_MMIO32_BASE + pci_address;
}

static void mmix_edu_set_irq(QTestState *qts, uint64_t bar,
                             uint32_t value, bool asserted)
{
    mmix_pcie_writel(qts, bar + (asserted ? MMIX_EDU_IRQ_RAISE
                                          : MMIX_EDU_IRQ_ACK),
                     value);
}

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint64_t probe = UINT64_C(0x5aa55aa50ff0f00f);

    qtest_writeq(qts, address, probe);
    g_assert_cmphex(qtest_readq(qts, address), !=, probe);
}

static void test_mmix_pcie_ecam_mapping(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *mapping = g_strdup_printf(
        "%016" PRIx64 "-%016" PRIx64
        " (prio 0, i/o): pcie-mmcfg-mmio",
        MMIX_PCIE_ECAM_BASE,
        MMIX_PCIE_ECAM_BASE + MMIX_PCIE_ECAM_SIZE - 1);

    g_assert_nonnull(strstr(mtree, mapping));
    mmix_assert_unassigned(qts, MMIX_PCIE_ECAM_BASE - 8);
    mmix_assert_unassigned(qts, MMIX_PCIE_ECAM_BASE + MMIX_PCIE_ECAM_SIZE);
    qtest_quit(qts);
}

static void mmix_assert_mapping(const char *mtree, uint64_t base,
                                uint64_t size, uint64_t bus_base)
{
    g_autofree char *mapping;

    if (bus_base) {
        mapping = g_strdup_printf(
            "%016" PRIx64 "-%016" PRIx64
            " (prio 0, container): gpex_mmio_window @%016" PRIx64,
            base, base + size - 1, bus_base);
    } else {
        mapping = g_strdup_printf(
            "%016" PRIx64 "-%016" PRIx64
            " (prio 0, container): gpex_mmio_window",
            base, base + size - 1);
    }

    g_assert_nonnull(strstr(mtree, mapping));
}

static void test_mmix_pcie_memory_mappings(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");

    mmix_assert_mapping(mtree, MMIX_PCIE_MMIO32_BASE,
                        MMIX_PCIE_MMIO32_SIZE,
                        MMIX_PCIE_MMIO32_BUS_BASE);
    mmix_assert_mapping(mtree, MMIX_PCIE_MMIO64_BASE,
                        MMIX_PCIE_MMIO64_SIZE,
                        MMIX_PCIE_MMIO64_BUS_BASE);
    mmix_assert_unassigned(qts, MMIX_PCIE_MMIO32_BASE - 8);
    mmix_assert_unassigned(qts,
                           MMIX_PCIE_MMIO32_BASE + MMIX_PCIE_MMIO32_SIZE);
    mmix_assert_unassigned(qts, MMIX_PCIE_MMIO64_BASE - 8);
    mmix_assert_unassigned(qts,
                           MMIX_PCIE_MMIO64_BASE + MMIX_PCIE_MMIO64_SIZE);
    qtest_quit(qts);
}

static void test_mmix_pcie_root_config(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint64_t root = mmix_pcie_ecam_address(0, 0, 0, 0);

    g_assert_cmphex(mmix_pcie_readl(qts, root), ==,
                    PCI_DEVICE_ID_REDHAT_PCIE_HOST << 16 |
                    PCI_VENDOR_ID_REDHAT);
    g_assert_cmphex(mmix_pcie_readl(qts, root + PCI_CLASS_REVISION) >> 16,
                    ==, PCI_CLASS_BRIDGE_HOST);
    qtest_quit(qts);
}

static void test_mmix_pcie_ecam_boundaries(void)
{
    QTestState *qts = qtest_init("-machine virt");

    g_assert_cmphex(mmix_pcie_readl(
                        qts, mmix_pcie_ecam_address(0, 0, 1, 0)), ==,
                    UINT32_MAX);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, mmix_pcie_ecam_address(0, 31, 7, 0xffc)), ==,
                    UINT32_MAX);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, mmix_pcie_ecam_address(255, 0, 0, 0)), ==,
                    UINT32_MAX);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, mmix_pcie_ecam_address(255, 31, 7, 0xffc)), ==,
                    UINT32_MAX);
    qtest_quit(qts);
}

static void test_mmix_pcie_intx_swizzle(void)
{
    static const char devices[] =
        "-device edu,bus=pcie.0,addr=1.0 "
        "-device edu,bus=pcie.0,addr=2.0 "
        "-device edu,bus=pcie.0,addr=3.0 "
        "-device edu,bus=pcie.0,addr=4.0";
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    unsigned int slot;

    for (slot = 1; slot <= MMIX_PCIE_INTX_IRQ_COUNT; slot++) {
        uint64_t bar = mmix_edu_configure(qts, slot,
                                          slot * MMIX_EDU_BAR_SIZE);
        unsigned int source = MMIX_PCIE_INTX_IRQ_BASE + slot % 4;
        uint64_t bit = mmix_intc_source_bit(source);

        mmix_edu_set_irq(qts, bar, bit, true);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
        mmix_edu_set_irq(qts, bar, bit, false);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    }

    qtest_quit(qts);
}

static void test_mmix_pcie_shared_intx(void)
{
    static const char devices[] =
        "-device edu,bus=pcie.0,addr=1.0 "
        "-device edu,bus=pcie.0,addr=5.0";
    const unsigned int source = MMIX_PCIE_INTX_IRQ_BASE + 1;
    const uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_pcie_irq_start(2, devices);
    uint64_t bar1 = mmix_edu_configure(qts, 1, MMIX_EDU_BAR_SIZE);
    uint64_t bar5 = mmix_edu_configure(qts, 5, 5 * MMIX_EDU_BAR_SIZE);

    mmix_edu_set_irq(qts, bar1, 1, true);
    mmix_edu_set_irq(qts, bar5, 2, true);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    mmix_intc_write_enable(qts, 1, source, bit);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    mmix_intc_write_enable(qts, 0, source, bit);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, source);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);
    mmix_intc_complete(qts, 0, source);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_edu_set_irq(qts, bar1, 1, false);
    mmix_intc_complete(qts, 1, source);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
    mmix_edu_set_irq(qts, bar5, 2, false);
    mmix_intc_complete(qts, 0, source);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);

    qtest_quit(qts);
}

static void test_mmix_pcie_reserved_irq_sources(void)
{
    static const unsigned int reserved[] = {
        MMIX_PCIE_RESERVED_IRQ_BASE,
        MMIX_PCIE_RESERVED_IRQ_END - 1,
    };
    QTestState *qts = mmix_pcie_irq_start(1, "");
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(reserved); i++) {
        unsigned int source = reserved[i];
        uint64_t bit = mmix_intc_source_bit(source);

        mmix_intc_write_enable(qts, 0, source, bit);
        qtest_set_irq_in(qts, MMIX_INTC_QOM_PATH, "unnamed-gpio-in",
                         source, 1);
        g_assert_cmphex(mmix_intc_enable(qts, 0, source) & bit, ==, 0);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/pcie/ecam-mapping",
                   test_mmix_pcie_ecam_mapping);
    qtest_add_func("/mmix/pcie/root-config",
                   test_mmix_pcie_root_config);
    qtest_add_func("/mmix/pcie/memory-mappings",
                   test_mmix_pcie_memory_mappings);
    qtest_add_func("/mmix/pcie/ecam-boundaries",
                   test_mmix_pcie_ecam_boundaries);
    qtest_add_func("/mmix/pcie/intx-swizzle",
                   test_mmix_pcie_intx_swizzle);
    qtest_add_func("/mmix/pcie/shared-intx",
                   test_mmix_pcie_shared_intx);
    qtest_add_func("/mmix/pcie/reserved-irq-sources",
                   test_mmix_pcie_reserved_irq_sources);

    return g_test_run();
}
