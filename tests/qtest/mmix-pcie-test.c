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

    return g_test_run();
}
