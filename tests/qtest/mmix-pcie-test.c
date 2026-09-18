/*
 * MMIX virt PCI Express host bridge tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "hw/net/e1000_regs.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"

#define MMIX_PCIE_ECAM_BASE UINT64_C(0x0001000100000000)
#define MMIX_PCIE_ECAM_SIZE UINT64_C(0x10000000)
#define MMIX_PCIE_MMIO32_BASE UINT64_C(0x0001000200000000)
#define MMIX_PCIE_MMIO32_SIZE UINT64_C(0x0000000100000000)
#define MMIX_PCIE_MMIO32_BUS_BASE UINT64_C(0)
#define MMIX_PCIE_MMIO32_ALLOC_SIZE UINT64_C(0x00000000ffff0000)
#define MMIX_PCIE_MSI_BUS_BASE UINT64_C(0x00000000ffff0000)
#define MMIX_PCIE_MSI_SIZE UINT64_C(0x0000000000010000)
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
#define MMIX_PCIE_MSI_IRQ_BASE 6148
#define MMIX_PCIE_MSI_IRQ_END 7168

#define MMIX_EDU_BAR_SIZE UINT64_C(0x100000)
#define MMIX_EDU_ID UINT32_C(0x010000ed)
#define MMIX_EDU_IRQ_RAISE 0x60
#define MMIX_EDU_IRQ_ACK 0x64
#define MMIX_EDU_DMA_SRC 0x80
#define MMIX_EDU_DMA_DST 0x88
#define MMIX_EDU_DMA_COUNT 0x90
#define MMIX_EDU_DMA_COMMAND 0x98
#define MMIX_EDU_DMA_BUFFER UINT64_C(0x40000)
#define MMIX_EDU_DMA_RUN UINT64_C(0x1)
#define MMIX_EDU_DMA_TO_PCI UINT64_C(0x2)

#define MMIX_TESTDEV_BAR_SIZE UINT64_C(0x100000)

#define MMIX_E1000_BAR_SIZE UINT64_C(0x20000)
#define MMIX_E1000_ICR 0x00c0
#define MMIX_E1000_ICS 0x00c8
#define MMIX_E1000_IMS 0x00d0
#define MMIX_E1000_TEST_CAUSE UINT32_C(0x1)

#define MMIX_E1000E_MMIO_SIZE UINT64_C(0x20000)
#define MMIX_E1000E_MSIX_SIZE UINT64_C(0x4000)
#define MMIX_E1000E_MSIX_BAR 3
#define MMIX_E1000E_MSIX_VECTORS 5
#define MMIX_E1000E_MSIX_PBA 0x2000

#define MMIX_UART_SCRATCH UINT64_C(0x0001000010000007)

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

static uint16_t mmix_pcie_readw(QTestState *qts, uint64_t address)
{
    uint8_t bytes[sizeof(uint16_t)];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return lduw_le_p(bytes);
}

static uint8_t mmix_pcie_readb(QTestState *qts, uint64_t address)
{
    uint8_t value;

    qtest_memread(qts, address, &value, sizeof(value));
    return value;
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

static void mmix_intc_enable_source(QTestState *qts, unsigned int cpu,
                                    unsigned int source)
{
    uint64_t value = mmix_intc_enable(qts, cpu, source);

    mmix_intc_write_enable(qts, cpu, source,
                           value | mmix_intc_source_bit(source));
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

static void mmix_testdev_configure_bar2(QTestState *qts,
                                        unsigned int slot,
                                        uint64_t pci_address,
                                        uint64_t bar_size)
{
    uint64_t config = mmix_pcie_ecam_address(0, slot, 0, 0);

    g_assert_cmphex(pci_address % bar_size, ==, 0);
    mmix_pcie_writel(qts, config + PCI_BASE_ADDRESS_2,
                     pci_address | PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH);
    mmix_pcie_writel(qts, config + PCI_BASE_ADDRESS_3,
                     pci_address >> 32);
    mmix_pcie_writew(qts, config + PCI_COMMAND, PCI_COMMAND_MEMORY);
}

static unsigned int mmix_pcie_find_capability(QTestState *qts,
                                               uint64_t config,
                                               uint8_t capability)
{
    uint8_t offset = mmix_pcie_readb(qts, config + PCI_CAPABILITY_LIST);
    unsigned int remaining = 48;

    while (offset && remaining--) {
        offset &= ~0x3;
        if (mmix_pcie_readb(qts, config + offset) == capability) {
            return offset;
        }
        offset = mmix_pcie_readb(qts, config + offset + 1);
    }
    return 0;
}

static uint16_t mmix_pcie_program_msi(QTestState *qts, uint64_t config,
                                      unsigned int capability,
                                      uint64_t address, uint16_t data,
                                      unsigned int vectors)
{
    uint16_t flags = mmix_pcie_readw(
        qts, config + capability + PCI_MSI_FLAGS);
    unsigned int order;

    g_assert_cmpuint(vectors, >, 0);
    g_assert_cmpuint(vectors & (vectors - 1), ==, 0);
    order = ctz32(vectors);
    mmix_pcie_writel(qts, config + capability + PCI_MSI_ADDRESS_LO,
                     (uint32_t)address);
    if (flags & PCI_MSI_FLAGS_64BIT) {
        mmix_pcie_writel(qts, config + capability + PCI_MSI_ADDRESS_HI,
                         (uint32_t)(address >> 32));
        mmix_pcie_writew(qts, config + capability + PCI_MSI_DATA_64,
                         data);
    } else {
        mmix_pcie_writew(qts, config + capability + PCI_MSI_DATA_32,
                         data);
    }
    flags &= ~PCI_MSI_FLAGS_QSIZE;
    flags |= order << ctz32(PCI_MSI_FLAGS_QSIZE);
    flags |= PCI_MSI_FLAGS_ENABLE;
    mmix_pcie_writew(qts, config + capability + PCI_MSI_FLAGS, flags);
    return mmix_pcie_readw(qts,
                           config + capability + PCI_MSI_FLAGS);
}

typedef struct MMIXE1000EMSIX {
    uint64_t config;
    uint64_t registers;
    uint64_t table;
    uint64_t pba;
    unsigned int capability;
} MMIXE1000EMSIX;

static MMIXE1000EMSIX mmix_e1000e_msix_configure(QTestState *qts,
                                                  unsigned int slot,
                                                  uint32_t mmio_address,
                                                  uint32_t msix_address)
{
    MMIXE1000EMSIX dev = {
        .config = mmix_pcie_ecam_address(0, slot, 0, 0),
        .registers = MMIX_PCIE_MMIO32_BASE + mmio_address,
        .table = MMIX_PCIE_MMIO32_BASE + msix_address,
        .pba = MMIX_PCIE_MMIO32_BASE + msix_address + MMIX_E1000E_MSIX_PBA,
    };
    uint32_t table;
    uint32_t pba;
    uint16_t flags;

    g_assert_cmphex(mmio_address % MMIX_E1000E_MMIO_SIZE, ==, 0);
    g_assert_cmphex(msix_address % MMIX_E1000E_MSIX_SIZE, ==, 0);
    mmix_pcie_writel(qts, dev.config + PCI_BASE_ADDRESS_0, mmio_address);
    mmix_pcie_writel(qts,
                     dev.config + PCI_BASE_ADDRESS_0 +
                     MMIX_E1000E_MSIX_BAR * sizeof(uint32_t),
                     msix_address);
    mmix_pcie_writew(qts, dev.config + PCI_COMMAND,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    dev.capability = mmix_pcie_find_capability(qts, dev.config,
                                                PCI_CAP_ID_MSIX);
    g_assert_cmpuint(dev.capability, !=, 0);
    flags = mmix_pcie_readw(qts, dev.config + dev.capability +
                            PCI_MSIX_FLAGS);
    g_assert_cmpuint((flags & PCI_MSIX_FLAGS_QSIZE) + 1, ==,
                     MMIX_E1000E_MSIX_VECTORS);
    table = mmix_pcie_readl(qts, dev.config + dev.capability +
                            PCI_MSIX_TABLE);
    pba = mmix_pcie_readl(qts, dev.config + dev.capability + PCI_MSIX_PBA);
    g_assert_cmpuint(table & PCI_MSIX_FLAGS_BIRMASK, ==,
                     MMIX_E1000E_MSIX_BAR);
    g_assert_cmphex(table & ~PCI_MSIX_FLAGS_BIRMASK, ==, 0);
    g_assert_cmpuint(pba & PCI_MSIX_FLAGS_BIRMASK, ==,
                     MMIX_E1000E_MSIX_BAR);
    g_assert_cmphex(pba & ~PCI_MSIX_FLAGS_BIRMASK, ==,
                    MMIX_E1000E_MSIX_PBA);
    return dev;
}

static void mmix_e1000e_msix_program_vector(QTestState *qts,
                                             const MMIXE1000EMSIX *dev,
                                             unsigned int vector,
                                             uint64_t address,
                                             uint32_t data,
                                             bool masked)
{
    uint64_t entry = dev->table + vector * PCI_MSIX_ENTRY_SIZE;

    g_assert_cmpuint(vector, <, MMIX_E1000E_MSIX_VECTORS);
    mmix_pcie_writel(qts, entry + PCI_MSIX_ENTRY_LOWER_ADDR,
                     (uint32_t)address);
    mmix_pcie_writel(qts, entry + PCI_MSIX_ENTRY_UPPER_ADDR,
                     (uint32_t)(address >> 32));
    mmix_pcie_writel(qts, entry + PCI_MSIX_ENTRY_DATA, data);
    mmix_pcie_writel(qts, entry + PCI_MSIX_ENTRY_VECTOR_CTRL,
                     masked ? PCI_MSIX_ENTRY_CTRL_MASKBIT : 0);
}

static void mmix_e1000e_msix_set_enabled(QTestState *qts,
                                         const MMIXE1000EMSIX *dev,
                                         bool enabled,
                                         bool function_masked)
{
    uint16_t flags = mmix_pcie_readw(
        qts, dev->config + dev->capability + PCI_MSIX_FLAGS);

    flags &= ~(PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);
    if (enabled) {
        flags |= PCI_MSIX_FLAGS_ENABLE;
    }
    if (function_masked) {
        flags |= PCI_MSIX_FLAGS_MASKALL;
    }
    mmix_pcie_writew(qts, dev->config + dev->capability +
                     PCI_MSIX_FLAGS, flags);
}

static void mmix_e1000e_route_vectors(QTestState *qts,
                                      const MMIXE1000EMSIX *dev)
{
    uint32_t ivar =
        (0 | E1000_IVAR_INT_ALLOC_VALID) << E1000_IVAR_RXQ0_SHIFT |
        (1 | E1000_IVAR_INT_ALLOC_VALID) << E1000_IVAR_TXQ0_SHIFT |
        (2 | E1000_IVAR_INT_ALLOC_VALID) << E1000_IVAR_OTHER_SHIFT;

    mmix_pcie_writel(qts, dev->registers + E1000_IVAR, ivar);
}

static void mmix_e1000e_trigger(QTestState *qts,
                                const MMIXE1000EMSIX *dev,
                                uint32_t cause)
{
    mmix_pcie_writel(qts, dev->registers + E1000_IMS, cause);
    mmix_pcie_writel(qts, dev->registers + E1000_ICS, cause);
}

static uint64_t mmix_e1000e_msix_pba(QTestState *qts,
                                      const MMIXE1000EMSIX *dev)
{
    uint8_t value[sizeof(uint64_t)];

    qtest_memread(qts, dev->pba, value, sizeof(value));
    return ldq_le_p(value);
}

static void mmix_assert_edu_config(QTestState *qts, unsigned int slot,
                                   unsigned int function,
                                   bool multifunction)
{
    uint64_t config = mmix_pcie_ecam_address(0, slot, function, 0);
    unsigned int msi;

    g_assert_cmphex(mmix_pcie_readl(qts, config), ==,
                    0x11e8 << 16 | PCI_VENDOR_ID_QEMU);
    g_assert_cmphex(mmix_pcie_readl(qts, config + PCI_CLASS_REVISION), ==,
                    PCI_CLASS_OTHERS << 16 | 0x10);
    g_assert_cmphex(mmix_pcie_readb(qts, config + PCI_HEADER_TYPE), ==,
                    PCI_HEADER_TYPE_NORMAL |
                    (multifunction ? PCI_HEADER_TYPE_MULTI_FUNCTION : 0));
    g_assert_cmpuint(mmix_pcie_readb(qts, config + PCI_INTERRUPT_PIN), ==,
                     1);
    msi = mmix_pcie_find_capability(qts, config, PCI_CAP_ID_MSI);
    g_assert_cmpuint(msi, !=, 0);
    g_assert_cmphex(mmix_pcie_readw(qts, config + msi + PCI_MSI_FLAGS) &
                    PCI_MSI_FLAGS_ENABLE, ==, 0);
}

static void mmix_edu_set_irq(QTestState *qts, uint64_t bar,
                             uint32_t value, bool asserted)
{
    mmix_pcie_writel(qts, bar + (asserted ? MMIX_EDU_IRQ_RAISE
                                          : MMIX_EDU_IRQ_ACK),
                     value);
}

static void mmix_edu_dma_run(QTestState *qts, uint64_t bar,
                             uint64_t source, uint64_t destination,
                             uint64_t size, uint64_t command)
{
    qtest_writeq(qts, bar + MMIX_EDU_DMA_SRC, source);
    qtest_writeq(qts, bar + MMIX_EDU_DMA_DST, destination);
    qtest_writeq(qts, bar + MMIX_EDU_DMA_COUNT, size);
    qtest_writeq(qts, bar + MMIX_EDU_DMA_COMMAND,
                 command | MMIX_EDU_DMA_RUN);
    qtest_clock_step(qts, 100 * SCALE_MS);
    g_assert_cmphex(qtest_readq(qts, bar + MMIX_EDU_DMA_COMMAND) &
                    MMIX_EDU_DMA_RUN, ==, 0);
}

static void mmix_edu_dma_copy(QTestState *qts, uint64_t bar,
                              uint64_t source, uint64_t destination,
                              uint64_t size)
{
    mmix_edu_dma_run(qts, bar, source, MMIX_EDU_DMA_BUFFER, size, 0);
    mmix_edu_dma_run(qts, bar, MMIX_EDU_DMA_BUFFER, destination, size,
                     MMIX_EDU_DMA_TO_PCI);
}

static void mmix_e1000_assert_irq(QTestState *qts, uint64_t bar)
{
    mmix_pcie_readl(qts, bar + MMIX_E1000_ICR);
    mmix_pcie_writel(qts, bar + MMIX_E1000_IMS,
                     MMIX_E1000_TEST_CAUSE);
    mmix_pcie_writel(qts, bar + MMIX_E1000_ICS,
                     MMIX_E1000_TEST_CAUSE);
}

static void mmix_e1000_clear_irq(QTestState *qts, uint64_t bar)
{
    g_assert_cmphex(mmix_pcie_readl(qts, bar + MMIX_E1000_ICR) &
                    MMIX_E1000_TEST_CAUSE, ==,
                    MMIX_E1000_TEST_CAUSE);
}

static void mmix_qtest_migrate(QTestState *from, QTestState *to,
                               const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(from, "STOP");
    qtest_qmp_eventwait(to, "RESUME");
}

static QTestState *mmix_edu_dma_start(const char *memory,
                                      uint64_t dma_mask, uint64_t *bar)
{
    QTestState *qts = qtest_initf(
        "-machine virt -m %s "
        "-device edu,bus=pcie.0,addr=1.0,"
        "dma_mask=0x%" PRIx64,
        memory, dma_mask);
    uint64_t config = mmix_pcie_ecam_address(0, 1, 0, 0);

    *bar = mmix_edu_configure(qts, 1, MMIX_EDU_BAR_SIZE);
    mmix_pcie_writew(qts, config + PCI_COMMAND,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return qts;
}

static void mmix_edu_dma_write(QTestState *qts, uint64_t bar,
                               uint64_t destination,
                               const void *data, size_t size)
{
    const uint64_t source = 0x00010000;

    qtest_memwrite(qts, source, data, size);
    mmix_edu_dma_run(qts, bar, source, MMIX_EDU_DMA_BUFFER, size, 0);
    mmix_edu_dma_run(qts, bar, MMIX_EDU_DMA_BUFFER, destination, size,
                     MMIX_EDU_DMA_TO_PCI);
}

static uint64_t mmix_e1000_configure(QTestState *qts, unsigned int bus,
                                     unsigned int slot,
                                     uint64_t pci_address)
{
    uint64_t config = mmix_pcie_ecam_address(bus, slot, 0, 0);

    g_assert_cmphex(pci_address % MMIX_E1000_BAR_SIZE, ==, 0);
    mmix_pcie_writel(qts, config + PCI_BASE_ADDRESS_0, pci_address);
    mmix_pcie_writew(qts, config + PCI_COMMAND,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return MMIX_PCIE_MMIO32_BASE + pci_address;
}

static void mmix_configure_test_bridge(QTestState *qts)
{
    uint64_t bridge = mmix_pcie_ecam_address(0, 4, 0, 0);

    mmix_pcie_writel(qts, bridge + PCI_PRIMARY_BUS, 0x00010100);
    mmix_pcie_writel(qts, bridge + PCI_MEMORY_BASE, 0x05f00400);
    mmix_pcie_writew(qts, bridge + PCI_COMMAND, PCI_COMMAND_MEMORY);
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
                        MMIX_PCIE_MMIO32_ALLOC_SIZE,
                        MMIX_PCIE_MMIO32_BUS_BASE);
    mmix_assert_mapping(mtree, MMIX_PCIE_MMIO64_BASE,
                        MMIX_PCIE_MMIO64_SIZE,
                        MMIX_PCIE_MMIO64_BUS_BASE);
    mmix_assert_unassigned(qts, MMIX_PCIE_MMIO32_BASE - 8);
    mmix_assert_unassigned(qts,
                           MMIX_PCIE_MMIO32_BASE +
                           MMIX_PCIE_MMIO32_ALLOC_SIZE);
    mmix_assert_unassigned(qts,
                           MMIX_PCIE_MMIO32_BASE + MMIX_PCIE_MMIO32_SIZE);
    mmix_assert_unassigned(qts, MMIX_PCIE_MMIO64_BASE - 8);
    mmix_assert_unassigned(qts,
                           MMIX_PCIE_MMIO64_BASE + MMIX_PCIE_MMIO64_SIZE);
    qtest_quit(qts);
}

static void test_mmix_pcie_msi_aperture_reserved(void)
{
    const uint64_t config = mmix_pcie_ecam_address(0, 1, 0, 0);
    const uint64_t valid_pci = MMIX_PCIE_MSI_BUS_BASE -
                               MMIX_PCIE_MSI_SIZE;
    const uint64_t valid_cpu = MMIX_PCIE_MMIO32_BASE + valid_pci;
    const uint64_t reserved_cpu = MMIX_PCIE_MMIO32_BASE +
                                  MMIX_PCIE_MSI_BUS_BASE;
    const uint64_t marker = UINT64_C(0x1122334455667788);
    QTestState *qts = qtest_init(
        "-machine virt "
        "-device pci-testdev,bus=pcie.0,addr=1.0,membar=64K,"
        "membar-backed=on");

    mmix_testdev_configure_bar2(qts, 1, valid_pci, MMIX_PCIE_MSI_SIZE);
    qtest_writeq(qts, valid_cpu, marker);
    g_assert_cmphex(qtest_readq(qts, valid_cpu), ==, marker);

    mmix_pcie_writew(qts, config + PCI_COMMAND, 0);
    mmix_testdev_configure_bar2(qts, 1, MMIX_PCIE_MSI_BUS_BASE,
                                MMIX_PCIE_MSI_SIZE);
    mmix_assert_unassigned(qts, reserved_cpu);
    mmix_assert_unassigned(qts,
                           reserved_cpu + MMIX_PCIE_MSI_SIZE - 8);
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

static void test_mmix_pcie_msi_single_vector(void)
{
    static const char devices[] =
        "-device edu,bus=pcie.0,addr=1.0";
    const uint64_t config = mmix_pcie_ecam_address(0, 1, 0, 0);
    const unsigned int msi_source = MMIX_PCIE_MSI_IRQ_BASE + 8;
    const unsigned int intx_source = MMIX_PCIE_INTX_IRQ_BASE + 1;
    const uint64_t msi_bit = mmix_intc_source_bit(msi_source);
    const uint64_t intx_bit = mmix_intc_source_bit(intx_source);
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    unsigned int capability = mmix_pcie_find_capability(
        qts, config, PCI_CAP_ID_MSI);
    uint16_t flags;
    uint64_t bar;

    g_assert_cmpuint(capability, !=, 0);
    flags = mmix_pcie_readw(qts, config + capability + PCI_MSI_FLAGS);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_64BIT, !=, 0);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_QMASK, ==, 0);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_MASKBIT, ==, 0);
    bar = mmix_edu_configure(qts, 1, MMIX_EDU_BAR_SIZE);
    mmix_pcie_writew(qts, config + PCI_COMMAND,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    mmix_intc_enable_source(qts, 0, msi_source);
    mmix_intc_enable_source(qts, 0, intx_source);
    flags = mmix_pcie_program_msi(
        qts, config, capability, MMIX_PCIE_MSI_BUS_BASE,
        msi_source - MMIX_PCIE_MSI_IRQ_BASE, 1);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_ENABLE, !=, 0);
    mmix_edu_set_irq(qts, bar, 1, true);
    g_assert_cmphex(mmix_intc_pending(qts, msi_source) & msi_bit, ==,
                    msi_bit);
    g_assert_cmphex(mmix_intc_pending(qts, intx_source) & intx_bit, ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, msi_source);
    mmix_edu_set_irq(qts, bar, 1, false);
    mmix_intc_complete(qts, 0, msi_source);

    flags &= ~PCI_MSI_FLAGS_ENABLE;
    mmix_pcie_writew(qts, config + capability + PCI_MSI_FLAGS, flags);
    mmix_edu_set_irq(qts, bar, 1, true);
    g_assert_cmphex(mmix_intc_pending(qts, intx_source) & intx_bit, ==,
                    intx_bit);
    g_assert_cmphex(mmix_intc_pending(qts, msi_source) & msi_bit, ==, 0);
    mmix_edu_set_irq(qts, bar, 1, false);

    qtest_quit(qts);
}

static void test_mmix_pcie_msi_invalid_programming(void)
{
    static const char devices[] =
        "-device edu,bus=pcie.0,addr=1.0";
    const uint64_t config = mmix_pcie_ecam_address(0, 1, 0, 0);
    const unsigned int source = MMIX_PCIE_MSI_IRQ_BASE;
    const uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    unsigned int capability = mmix_pcie_find_capability(
        qts, config, PCI_CAP_ID_MSI);
    uint64_t bar = mmix_edu_configure(qts, 1, MMIX_EDU_BAR_SIZE);
    uint16_t flags;

    mmix_pcie_writew(qts, config + PCI_COMMAND,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    mmix_intc_write_enable(qts, 0, source, bit);
    mmix_pcie_program_msi(qts, config, capability,
                          MMIX_PCIE_MSI_BUS_BASE + sizeof(uint32_t), 0, 1);
    mmix_edu_set_irq(qts, bar, 1, true);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    mmix_edu_set_irq(qts, bar, 1, false);

    mmix_pcie_program_msi(qts, config, capability,
                          MMIX_PCIE_MSI_BUS_BASE,
                          MMIX_PCIE_MSI_IRQ_END -
                          MMIX_PCIE_MSI_IRQ_BASE, 1);
    mmix_edu_set_irq(qts, bar, 1, true);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    mmix_edu_set_irq(qts, bar, 1, false);

    flags = mmix_pcie_readw(qts, config + capability + PCI_MSI_FLAGS);
    flags |= PCI_MSI_FLAGS_QSIZE;
    mmix_pcie_writew(qts, config + capability + PCI_MSI_FLAGS, flags);
    flags = mmix_pcie_readw(qts, config + capability + PCI_MSI_FLAGS);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_QSIZE, ==, 0);
    qtest_quit(qts);
}

static void test_mmix_pcie_msi_independent_endpoints(void)
{
    static const char devices[] =
        "-device edu,bus=pcie.0,addr=1.0 "
        "-device edu,bus=pcie.0,addr=2.0";
    const unsigned int sources[] = {
        MMIX_PCIE_MSI_IRQ_BASE + 10,
        MMIX_PCIE_MSI_IRQ_BASE + 11,
    };
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    uint64_t bars[ARRAY_SIZE(sources)];
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(sources); i++) {
        unsigned int slot = i + 1;
        uint64_t config = mmix_pcie_ecam_address(0, slot, 0, 0);
        unsigned int capability = mmix_pcie_find_capability(
            qts, config, PCI_CAP_ID_MSI);
        uint64_t bit = mmix_intc_source_bit(sources[i]);

        bars[i] = mmix_edu_configure(qts, slot,
                                     slot * MMIX_EDU_BAR_SIZE);
        mmix_pcie_writew(qts, config + PCI_COMMAND,
                         PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        mmix_intc_enable_source(qts, 0, sources[i]);
        mmix_pcie_program_msi(
            qts, config, capability, MMIX_PCIE_MSI_BUS_BASE,
            sources[i] - MMIX_PCIE_MSI_IRQ_BASE, 1);
        mmix_edu_set_irq(qts, bars[i], 1, true);
        g_assert_cmphex(mmix_intc_pending(qts, sources[i]) & bit, ==, bit);
    }
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, sources[0]);
    mmix_edu_set_irq(qts, bars[0], 1, false);
    mmix_intc_complete(qts, 0, sources[0]);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, sources[1]);
    mmix_edu_set_irq(qts, bars[1], 1, false);
    mmix_intc_complete(qts, 0, sources[1]);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_mmix_pcie_msi_multi_vector_masking(void)
{
    static const char devices[] =
        "-device ioh3420,id=rp,bus=pcie.0,addr=1.0,port=1,chassis=1";
    const uint64_t config = mmix_pcie_ecam_address(0, 1, 0, 0);
    const unsigned int data = 20;
    const unsigned int source = MMIX_PCIE_MSI_IRQ_BASE + data + 1;
    const uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    unsigned int capability = mmix_pcie_find_capability(
        qts, config, PCI_CAP_ID_MSI);
    uint16_t flags;
    g_autofree char *response = NULL;

    g_assert_cmpuint(capability, !=, 0);
    flags = mmix_pcie_readw(qts, config + capability + PCI_MSI_FLAGS);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_64BIT, ==, 0);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_MASKBIT, !=, 0);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_QMASK, ==,
                    1 << ctz32(PCI_MSI_FLAGS_QMASK));
    flags = mmix_pcie_program_msi(qts, config, capability,
                                  MMIX_PCIE_MSI_BUS_BASE, data, 2);
    g_assert_cmphex(flags & PCI_MSI_FLAGS_QSIZE, ==,
                    1 << ctz32(PCI_MSI_FLAGS_QSIZE));
    mmix_intc_write_enable(qts, 0, source, bit);
    mmix_pcie_writew(qts, config + PCI_COMMAND, PCI_COMMAND_MASTER);
    mmix_pcie_writew(qts, config + PCI_BRIDGE_CONTROL,
                     PCI_BRIDGE_CTL_SERR);
    mmix_pcie_writel(qts, config + capability + PCI_MSI_MASK_32,
                     1U << 1);
    mmix_pcie_writew(qts, config + 0x90 + PCI_EXP_DEVCTL,
                     PCI_EXP_DEVCTL_CERE);
    mmix_pcie_writel(qts, config + 0x100 + PCI_ERR_ROOT_COMMAND,
                     PCI_ERR_ROOT_CMD_COR_EN);

    response = qtest_hmp(qts, "pcie_aer_inject_error rp BAD_TLP");
    g_assert_nonnull(strstr(response, "OK id: rp"));
    g_assert_cmphex(mmix_pcie_readl(
                        qts, config + capability + PCI_MSI_PENDING_32) &
                    (1U << 1), ==, 1U << 1);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);

    mmix_pcie_writel(qts, config + capability + PCI_MSI_MASK_32, 0);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, config + capability + PCI_MSI_PENDING_32) &
                    (1U << 1), ==, 0);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
    mmix_intc_complete(qts, 0, source);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_mmix_pcie_msix_table_and_masking(void)
{
    static const char devices[] =
        "-device e1000e,bus=pcie.0,addr=1.0";
    static const uint32_t causes[] = {
        E1000_ICR_RXQ0,
        E1000_ICR_TXQ0,
        E1000_ICR_OTHER,
    };
    const unsigned int data[] = { 30, 31, 32 };
    MMIXE1000EMSIX dev;
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    unsigned int i;

    dev = mmix_e1000e_msix_configure(qts, 1, 0x01000000, 0x01200000);
    for (i = 0; i < ARRAY_SIZE(data); i++) {
        mmix_intc_enable_source(qts, 0,
                               MMIX_PCIE_MSI_IRQ_BASE + data[i]);
    }
    mmix_e1000e_msix_program_vector(
        qts, &dev, 0, MMIX_PCIE_MSI_BUS_BASE, data[0], false);
    mmix_e1000e_msix_program_vector(
        qts, &dev, 1, MMIX_PCIE_MSI_BUS_BASE, data[1], true);
    mmix_e1000e_msix_program_vector(
        qts, &dev, 2, MMIX_PCIE_MSI_BUS_BASE + sizeof(uint32_t),
        data[2], false);
    mmix_e1000e_route_vectors(qts, &dev);
    mmix_e1000e_msix_set_enabled(qts, &dev, true, false);

    mmix_e1000e_trigger(qts, &dev, causes[0]);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==,
                     MMIX_PCIE_MSI_IRQ_BASE + data[0]);
    mmix_intc_complete(qts, 0, MMIX_PCIE_MSI_IRQ_BASE + data[0]);
    mmix_pcie_readl(qts, dev.registers + E1000_ICR);

    mmix_e1000e_trigger(qts, &dev, causes[1]);
    g_assert_cmphex(mmix_e1000e_msix_pba(qts, &dev) & (1U << 1), ==,
                    1U << 1);
    g_assert_cmphex(mmix_intc_pending(
                        qts, MMIX_PCIE_MSI_IRQ_BASE + data[1]) &
                    mmix_intc_source_bit(
                        MMIX_PCIE_MSI_IRQ_BASE + data[1]), ==, 0);
    mmix_pcie_writel(qts,
                     dev.table + PCI_MSIX_ENTRY_SIZE +
                     PCI_MSIX_ENTRY_VECTOR_CTRL, 0);
    g_assert_cmphex(mmix_e1000e_msix_pba(qts, &dev) & (1U << 1), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==,
                     MMIX_PCIE_MSI_IRQ_BASE + data[1]);
    mmix_intc_complete(qts, 0, MMIX_PCIE_MSI_IRQ_BASE + data[1]);
    mmix_pcie_readl(qts, dev.registers + E1000_ICR);

    mmix_e1000e_msix_set_enabled(qts, &dev, true, true);
    mmix_e1000e_trigger(qts, &dev, causes[2]);
    g_assert_cmphex(mmix_e1000e_msix_pba(qts, &dev) & (1U << 2), ==,
                    1U << 2);
    mmix_e1000e_msix_program_vector(
        qts, &dev, 2, MMIX_PCIE_MSI_BUS_BASE, data[2], false);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, dev.table + 2 * PCI_MSIX_ENTRY_SIZE +
                        PCI_MSIX_ENTRY_LOWER_ADDR), ==,
                    MMIX_PCIE_MSI_BUS_BASE);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, dev.table + 2 * PCI_MSIX_ENTRY_SIZE +
                        PCI_MSIX_ENTRY_DATA), ==, data[2]);
    mmix_e1000e_msix_set_enabled(qts, &dev, true, false);
    g_assert_cmphex(mmix_e1000e_msix_pba(qts, &dev) & (1U << 2), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==,
                     MMIX_PCIE_MSI_IRQ_BASE + data[2]);
    mmix_intc_complete(qts, 0, MMIX_PCIE_MSI_IRQ_BASE + data[2]);
    qtest_quit(qts);
}

static void test_mmix_pcie_msix_independent_endpoints(void)
{
    static const char devices[] =
        "-device e1000e,bus=pcie.0,addr=1.0 "
        "-device e1000e,bus=pcie.0,addr=2.0";
    const unsigned int data[] = { 40, 41 };
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    MMIXE1000EMSIX dev[] = {
        mmix_e1000e_msix_configure(qts, 1, 0x01000000, 0x01200000),
        mmix_e1000e_msix_configure(qts, 2, 0x01400000, 0x01600000),
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(dev); i++) {
        unsigned int vector = i;

        mmix_intc_enable_source(qts, 0,
                               MMIX_PCIE_MSI_IRQ_BASE + data[i]);
        mmix_e1000e_msix_program_vector(
            qts, &dev[i], vector, MMIX_PCIE_MSI_BUS_BASE, data[i], false);
        mmix_e1000e_route_vectors(qts, &dev[i]);
        mmix_e1000e_msix_set_enabled(qts, &dev[i], true, false);
    }
    mmix_e1000e_trigger(qts, &dev[0], E1000_ICR_RXQ0);
    mmix_e1000e_trigger(qts, &dev[1], E1000_ICR_TXQ0);
    for (i = 0; i < ARRAY_SIZE(dev); i++) {
        unsigned int source = MMIX_PCIE_MSI_IRQ_BASE + data[i];

        g_assert_cmphex(mmix_intc_pending(qts, source) &
                        mmix_intc_source_bit(source), !=, 0);
        g_assert_cmphex(mmix_e1000e_msix_pba(qts, &dev[i]), ==, 0);
        g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
        mmix_intc_complete(qts, 0, source);
    }
    qtest_quit(qts);
}

static void test_mmix_pcie_msix_intx_fallback(void)
{
    static const char devices[] =
        "-device e1000e,bus=pcie.0,addr=3.0";
    const unsigned int source = MMIX_PCIE_INTX_IRQ_BASE + 3;
    const uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    MMIXE1000EMSIX dev = mmix_e1000e_msix_configure(
        qts, 3, 0x01800000, 0x01a00000);
    uint16_t flags;

    flags = mmix_pcie_readw(qts, dev.config + dev.capability +
                            PCI_MSIX_FLAGS);
    g_assert_cmphex(flags & PCI_MSIX_FLAGS_ENABLE, ==, 0);
    mmix_intc_enable_source(qts, 0, source);
    mmix_e1000e_trigger(qts, &dev, E1000_ICR_RXQ0);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
    mmix_pcie_readl(qts, dev.registers + E1000_ICR);
    mmix_intc_complete(qts, 0, source);
    g_assert_false(qtest_get_irq(qts, 0));
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

static void test_mmix_pcie_device_enumeration(void)
{
    static const char *const device_orders[] = {
        "-device edu,bus=pcie.0,addr=5.0 "
        "-device edu,bus=pcie.0,addr=2.0,multifunction=on "
        "-device edu,bus=pcie.0,addr=2.1 "
        "-device pci-testdev,bus=pcie.0,addr=7.0,membar=1M",
        "-device pci-testdev,bus=pcie.0,addr=7.0,membar=1M "
        "-device edu,bus=pcie.0,addr=2.0,multifunction=on "
        "-device edu,bus=pcie.0,addr=2.1 "
        "-device edu,bus=pcie.0,addr=5.0",
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(device_orders); i++) {
        QTestState *qts = qtest_initf("-machine virt %s",
                                      device_orders[i]);
        uint64_t testdev = mmix_pcie_ecam_address(0, 7, 0, 0);

        mmix_assert_edu_config(qts, 2, 0, true);
        mmix_assert_edu_config(qts, 2, 1, false);
        mmix_assert_edu_config(qts, 5, 0, false);
        g_assert_cmphex(mmix_pcie_readl(qts, testdev), ==,
                        PCI_DEVICE_ID_REDHAT_TEST << 16 |
                        PCI_VENDOR_ID_REDHAT);
        g_assert_cmphex(mmix_pcie_readl(
                            qts, testdev + PCI_CLASS_REVISION), ==,
                        PCI_CLASS_OTHERS << 16);
        g_assert_cmpuint(mmix_pcie_readb(
                             qts, testdev + PCI_INTERRUPT_PIN), ==, 0);
        g_assert_cmphex(mmix_pcie_readl(
                            qts, testdev + PCI_BASE_ADDRESS_2) &
                        (PCI_BASE_ADDRESS_SPACE |
                         PCI_BASE_ADDRESS_MEM_TYPE_MASK |
                         PCI_BASE_ADDRESS_MEM_PREFETCH), ==,
                        PCI_BASE_ADDRESS_MEM_TYPE_64 |
                        PCI_BASE_ADDRESS_MEM_PREFETCH);
        qtest_quit(qts);
    }
}

static void test_mmix_pcie_bar_access(void)
{
    static const char devices[] =
        "-device edu,bus=pcie.0,addr=1.0 "
        "-device pci-testdev,bus=pcie.0,addr=6.0,membar=1M,"
        "membar-backed=on "
        "-device pci-testdev,bus=pcie.0,addr=7.0,membar=1M,"
        "membar-backed=on";
    const uint64_t edu_config = mmix_pcie_ecam_address(0, 1, 0, 0);
    const uint64_t testdev6 = mmix_pcie_ecam_address(0, 6, 0, 0);
    const uint64_t testdev7 = mmix_pcie_ecam_address(0, 7, 0, 0);
    const uint64_t edu_pci32 = 0x00200000;
    const uint64_t test_pci32 = 0x00400000;
    const uint64_t test_pci64 = MMIX_PCIE_MMIO64_BUS_BASE + 0x00600000;
    const uint64_t edu_cpu32 = MMIX_PCIE_MMIO32_BASE + edu_pci32;
    const uint64_t test_cpu32 = MMIX_PCIE_MMIO32_BASE + test_pci32;
    const uint64_t test_cpu64 = MMIX_PCIE_MMIO64_BASE + 0x00600000;
    const uint64_t marker32 = UINT64_C(0x1122334455667788);
    const uint64_t marker64 = UINT64_C(0x8877665544332211);
    QTestState *qts = qtest_initf("-machine virt %s", devices);

    mmix_pcie_writel(qts, edu_config + PCI_BASE_ADDRESS_0, UINT32_MAX);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, edu_config + PCI_BASE_ADDRESS_0), ==,
                    UINT32_MAX & ~(MMIX_EDU_BAR_SIZE - 1));
    mmix_pcie_writel(qts, edu_config + PCI_BASE_ADDRESS_0,
                     edu_pci32 + MMIX_EDU_BAR_SIZE / 2);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, edu_config + PCI_BASE_ADDRESS_0), ==,
                    edu_pci32);
    mmix_pcie_writew(qts, edu_config + PCI_COMMAND, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(qts, edu_cpu32), ==, MMIX_EDU_ID);
    mmix_pcie_writew(qts, edu_config + PCI_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, edu_cpu32), !=, MMIX_EDU_ID);
    mmix_pcie_writew(qts, edu_config + PCI_COMMAND, PCI_COMMAND_MEMORY);

    mmix_pcie_writel(qts, testdev6 + PCI_BASE_ADDRESS_2, UINT32_MAX);
    mmix_pcie_writel(qts, testdev6 + PCI_BASE_ADDRESS_3, UINT32_MAX);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, testdev6 + PCI_BASE_ADDRESS_2), ==,
                    (UINT32_MAX & ~(MMIX_TESTDEV_BAR_SIZE - 1)) |
                    PCI_BASE_ADDRESS_MEM_TYPE_64 |
                    PCI_BASE_ADDRESS_MEM_PREFETCH);
    g_assert_cmphex(mmix_pcie_readl(
                        qts, testdev6 + PCI_BASE_ADDRESS_3), ==,
                    UINT32_MAX);

    mmix_testdev_configure_bar2(qts, 6, test_pci32,
                                MMIX_TESTDEV_BAR_SIZE);
    mmix_testdev_configure_bar2(qts, 7, test_pci64,
                                MMIX_TESTDEV_BAR_SIZE);
    qtest_writeq(qts, test_cpu32, marker32);
    qtest_writeq(qts, test_cpu64, marker64);
    g_assert_cmphex(qtest_readq(qts, test_cpu32), ==, marker32);
    g_assert_cmphex(qtest_readq(qts, test_cpu64), ==, marker64);
    g_assert_cmphex(qtest_readl(qts, edu_cpu32), ==, MMIX_EDU_ID);

    mmix_pcie_writew(qts, testdev7 + PCI_COMMAND, 0);
    mmix_assert_unassigned(qts, test_cpu64);
    mmix_testdev_configure_bar2(
        qts, 7, MMIX_PCIE_MMIO64_BUS_BASE + MMIX_PCIE_MMIO64_SIZE,
        MMIX_TESTDEV_BAR_SIZE);
    mmix_assert_unassigned(qts,
                           MMIX_PCIE_MMIO64_BASE + MMIX_PCIE_MMIO64_SIZE);
    qtest_quit(qts);
}

static void test_mmix_pcie_bridge_device(void)
{
    static const char devices[] =
        "-device pcie-pci-bridge,id=bridge,bus=pcie.0,addr=4.0,msi=off "
        "-device edu,bus=bridge,addr=1.0";
    const uint64_t bridge = mmix_pcie_ecam_address(0, 4, 0, 0);
    const uint64_t pci_address = 0x04000000;
    const unsigned int source = MMIX_PCIE_INTX_IRQ_BASE + 1;
    const uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    uint64_t child;
    uint64_t bar;

    g_assert_cmphex(mmix_pcie_readl(qts, bridge), ==,
                    PCI_DEVICE_ID_REDHAT_PCIE_BRIDGE << 16 |
                    PCI_VENDOR_ID_REDHAT);
    g_assert_cmphex(mmix_pcie_readl(qts, bridge + PCI_CLASS_REVISION) >> 16,
                    ==, PCI_CLASS_BRIDGE_PCI);
    g_assert_cmphex(mmix_pcie_readb(qts, bridge + PCI_HEADER_TYPE), ==,
                    PCI_HEADER_TYPE_BRIDGE);
    g_assert_cmpuint(mmix_pcie_find_capability(qts, bridge, PCI_CAP_ID_MSI),
                     ==, 0);

    mmix_pcie_writel(qts, bridge + PCI_PRIMARY_BUS, 0x00010100);
    mmix_pcie_writel(qts, bridge + PCI_MEMORY_BASE, 0x04f00400);
    mmix_pcie_writew(qts, bridge + PCI_COMMAND, PCI_COMMAND_MEMORY);
    child = mmix_pcie_ecam_address(1, 1, 0, 0);
    g_assert_cmphex(mmix_pcie_readl(qts, child), ==,
                    0x11e8 << 16 | PCI_VENDOR_ID_QEMU);
    mmix_pcie_writel(qts, child + PCI_BASE_ADDRESS_0, pci_address);
    mmix_pcie_writew(qts, child + PCI_COMMAND, PCI_COMMAND_MEMORY);
    bar = MMIX_PCIE_MMIO32_BASE + pci_address;
    g_assert_cmphex(qtest_readl(qts, bar), ==, MMIX_EDU_ID);

    mmix_edu_set_irq(qts, bar, 1, true);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    mmix_edu_set_irq(qts, bar, 1, false);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    qtest_quit(qts);
}

static void test_mmix_pcie_dma_below_4g(void)
{
    uint8_t source[32];
    uint8_t destination[sizeof(source)] = { 0 };
    const uint64_t source_address = 0x00010000;
    const uint64_t destination_address = 0x00020000;
    uint64_t bar;
    QTestState *qts = mmix_edu_dma_start("512M", UINT64_MAX, &bar);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(source); i++) {
        source[i] = 0x80 + i;
    }
    qtest_memwrite(qts, source_address, source, sizeof(source));
    mmix_edu_dma_copy(qts, bar, source_address, destination_address,
                      sizeof(source));
    qtest_memread(qts, destination_address, destination,
                  sizeof(destination));
    g_assert_cmpmem(destination, sizeof(destination), source, sizeof(source));
    qtest_quit(qts);
}

static void test_mmix_pcie_dma_above_4g(void)
{
    uint8_t source[32];
    uint8_t destination[sizeof(source)] = { 0 };
    const uint64_t source_address = UINT64_C(0x0000000140000000);
    const uint64_t destination_address = UINT64_C(0x0000000140010000);
    uint64_t bar;
    QTestState *qts = mmix_edu_dma_start("8G", UINT64_MAX, &bar);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(source); i++) {
        source[i] = 0x40 + i;
    }
    qtest_memwrite(qts, source_address, source, sizeof(source));
    mmix_edu_dma_copy(qts, bar, source_address, destination_address,
                      sizeof(source));
    qtest_memread(qts, destination_address, destination,
                  sizeof(destination));
    g_assert_cmpmem(destination, sizeof(destination), source, sizeof(source));
    qtest_quit(qts);
}

static void test_mmix_pcie_dma_invalid_targets(void)
{
    uint8_t source[32];
    const uint64_t source_address = 0x00010000;
    const uint64_t outside_ram = UINT64_C(0x0000000040010000);
    const uint64_t above_phys_limit = MMIX_UART_SCRATCH;
    const uint64_t low_probe1 = 0x00030000;
    const uint64_t low_probe2 = 0x00040000;
    const uint64_t sentinel1 = UINT64_C(0x1122334455667788);
    const uint64_t sentinel2 = UINT64_C(0x8877665544332211);
    const uint8_t uart_sentinel = 0x5a;
    uint64_t bar;
    QTestState *qts = mmix_edu_dma_start("512M", UINT64_MAX, &bar);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(source); i++) {
        source[i] = 0xc0 + i;
    }
    qtest_memwrite(qts, source_address, source, sizeof(source));
    qtest_writeq(qts, low_probe1, sentinel1);
    qtest_writeq(qts, low_probe2, sentinel2);
    qtest_writeb(qts, MMIX_UART_SCRATCH, uart_sentinel);
    mmix_edu_dma_run(qts, bar, source_address, MMIX_EDU_DMA_BUFFER,
                     sizeof(source), 0);
    mmix_edu_dma_run(qts, bar, MMIX_EDU_DMA_BUFFER, outside_ram,
                     sizeof(source), MMIX_EDU_DMA_TO_PCI);
    mmix_edu_dma_run(qts, bar, MMIX_EDU_DMA_BUFFER, above_phys_limit, 1,
                     MMIX_EDU_DMA_TO_PCI);
    g_assert_cmphex(qtest_readq(qts, low_probe1), ==, sentinel1);
    g_assert_cmphex(qtest_readq(qts, low_probe2), ==, sentinel2);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_SCRATCH), ==, uart_sentinel);
    mmix_assert_unassigned(qts, outside_ram);
    qtest_quit(qts);
}

static void test_mmix_pcie_dma_msi_doorbell_widths(void)
{
    static const uint64_t dma_masks[] = { UINT32_MAX, UINT64_MAX };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(dma_masks); i++) {
        const unsigned int vector = i;
        const unsigned int source = MMIX_PCIE_MSI_IRQ_BASE + vector;
        uint64_t bit = mmix_intc_source_bit(source);
        uint8_t data[sizeof(uint32_t)];
        uint64_t bar;
        QTestState *qts = mmix_edu_dma_start(
            "512M", dma_masks[i], &bar);

        qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                      MMIX_INTC_OUTPUT_IRQ);
        mmix_intc_write_enable(qts, 0, source, bit);
        stl_le_p(data, vector);
        mmix_edu_dma_write(qts, bar, MMIX_PCIE_MSI_BUS_BASE,
                           data, sizeof(data));
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
        g_assert_true(qtest_get_irq(qts, 0));
        g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
        mmix_intc_complete(qts, 0, source);
        g_assert_false(qtest_get_irq(qts, 0));
        qtest_quit(qts);
    }
}

static void test_mmix_pcie_dma_msi_doorbell_boundaries(void)
{
    const uint64_t before = MMIX_PCIE_MSI_BUS_BASE - sizeof(uint32_t);
    const uint64_t reserved = MMIX_PCIE_MSI_BUS_BASE + sizeof(uint32_t);
    const uint64_t after = MMIX_PCIE_MSI_BUS_BASE + MMIX_PCIE_MSI_SIZE;
    const uint32_t initial = UINT32_C(0x11223344);
    const uint32_t replacement = UINT32_C(0xa5b6c7d8);
    uint8_t data[sizeof(replacement)];
    uint64_t bar;
    QTestState *qts = mmix_edu_dma_start("8G", UINT64_MAX, &bar);

    mmix_pcie_writel(qts, before, initial);
    mmix_pcie_writel(qts, MMIX_PCIE_MSI_BUS_BASE, initial);
    mmix_pcie_writel(qts, reserved, initial);
    mmix_pcie_writel(qts, after, initial);
    stl_le_p(data, replacement);

    mmix_edu_dma_write(qts, bar, before, data, sizeof(data));
    g_assert_cmphex(mmix_pcie_readl(qts, before), ==, replacement);

    mmix_edu_dma_write(qts, bar, reserved, data, sizeof(data));
    g_assert_cmphex(mmix_pcie_readl(qts, reserved), ==, initial);

    mmix_edu_dma_write(qts, bar, after, data, sizeof(data));
    g_assert_cmphex(mmix_pcie_readl(qts, after), ==, replacement);
    g_assert_cmphex(mmix_pcie_readl(qts, MMIX_PCIE_MSI_BUS_BASE), ==,
                    initial);

    qtest_quit(qts);
}

static void test_mmix_pcie_reset_state(void)
{
    static const char devices[] =
        "-device e1000,bus=pcie.0,addr=3.0";
    const uint64_t config = mmix_pcie_ecam_address(0, 3, 0, 0);
    const uint64_t pci_address = 0x00800000;
    const unsigned int source = MMIX_PCIE_INTX_IRQ_BASE + 3;
    const uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_pcie_irq_start(1, devices);
    uint32_t id = mmix_pcie_readl(qts, config);
    uint64_t bar = mmix_e1000_configure(qts, 0, 3, pci_address);

    g_assert_cmphex(id, !=, UINT32_MAX);
    mmix_e1000_assert_irq(qts, bar);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);

    qtest_system_reset(qts);
    g_assert_cmphex(mmix_pcie_readl(qts, config), ==, id);
    g_assert_cmphex(mmix_pcie_readl(qts, config + PCI_BASE_ADDRESS_0) &
                    PCI_BASE_ADDRESS_MEM_MASK, ==, 0);
    g_assert_cmphex(mmix_pcie_readw(qts, config + PCI_COMMAND) &
                    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER), ==, 0);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);

    bar = mmix_e1000_configure(qts, 0, 3, pci_address);
    mmix_e1000_assert_irq(qts, bar);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    mmix_e1000_clear_irq(qts, bar);
    qtest_quit(qts);
}

static void test_mmix_pcie_empty_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir = g_dir_make_tmp("mmix-pcie-empty-XXXXXX",
                                             &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    const uint64_t empty = mmix_pcie_ecam_address(0, 1, 0, 0);
    QTestState *from;
    QTestState *to;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("-machine virt -incoming %s", uri);
    from = qtest_init("-machine virt");
    to = qtest_init(incoming);
    mmix_qtest_migrate(from, to, uri);
    g_assert_cmphex(mmix_pcie_readl(
                        to, mmix_pcie_ecam_address(0, 0, 0, 0)), ==,
                    PCI_DEVICE_ID_REDHAT_PCIE_HOST << 16 |
                    PCI_VENDOR_ID_REDHAT);
    g_assert_cmphex(mmix_pcie_readl(to, empty), ==, UINT32_MAX);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_mmix_pcie_populated_migration(void)
{
    static const char args[] =
        "-machine virt -smp 2 "
        "-device pcie-pci-bridge,id=bridge,bus=pcie.0,addr=4.0,msi=off "
        "-device e1000,bus=bridge,addr=1.0 "
        "-device e1000,bus=pcie.0,addr=1.0";
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir = g_dir_make_tmp("mmix-pcie-state-XXXXXX",
                                             &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    const uint64_t bridge = mmix_pcie_ecam_address(0, 4, 0, 0);
    const uint64_t root = mmix_pcie_ecam_address(0, 1, 0, 0);
    const uint64_t child = mmix_pcie_ecam_address(1, 1, 0, 0);
    const uint64_t root_pci_address = 0x00800000;
    const uint64_t child_pci_address = 0x04000000;
    const unsigned int source = MMIX_PCIE_INTX_IRQ_BASE + 1;
    const uint64_t bit = mmix_intc_source_bit(source);
    uint64_t root_bar;
    uint64_t child_bar;
    QTestState *from;
    QTestState *to;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("%s -incoming %s", args, uri);
    from = qtest_init(args);
    to = qtest_init(incoming);
    qtest_irq_intercept_out_named(from, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    qtest_irq_intercept_out_named(to, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    mmix_intc_write_enable(from, 0, source, bit);
    mmix_configure_test_bridge(from);
    root_bar = mmix_e1000_configure(from, 0, 1, root_pci_address);
    child_bar = mmix_e1000_configure(from, 1, 1, child_pci_address);
    mmix_e1000_assert_irq(from, root_bar);
    mmix_e1000_assert_irq(from, child_bar);
    g_assert_cmphex(mmix_intc_pending(from, source) & bit, ==, bit);
    g_assert_true(qtest_get_irq(from, 0));

    mmix_qtest_migrate(from, to, uri);
    g_assert_cmphex(mmix_pcie_readl(to, bridge + PCI_PRIMARY_BUS), ==,
                    0x00010100);
    g_assert_cmphex(mmix_pcie_readl(to, bridge + PCI_MEMORY_BASE), ==,
                    0x05f00400);
    g_assert_cmphex(mmix_pcie_readl(to, root + PCI_BASE_ADDRESS_0) &
                    PCI_BASE_ADDRESS_MEM_MASK, ==, root_pci_address);
    g_assert_cmphex(mmix_pcie_readl(to, child + PCI_BASE_ADDRESS_0) &
                    PCI_BASE_ADDRESS_MEM_MASK, ==, child_pci_address);
    g_assert_cmphex(mmix_intc_pending(to, source) & bit, ==, bit);
    g_assert_true(qtest_get_irq(to, 0));

    g_assert_cmpuint(mmix_intc_claim(to, 0), ==, source);
    mmix_e1000_clear_irq(to, child_bar);
    mmix_intc_complete(to, 0, source);
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_cmpuint(mmix_intc_claim(to, 0), ==, source);
    mmix_e1000_clear_irq(to, root_bar);
    mmix_intc_complete(to, 0, source);
    g_assert_cmphex(mmix_intc_pending(to, source) & bit, ==, 0);
    g_assert_false(qtest_get_irq(to, 0));

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
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

static void test_mmix_pcie_edge_irq_sources(void)
{
    static const unsigned int sources[] = {
        MMIX_PCIE_MSI_IRQ_BASE,
        MMIX_PCIE_MSI_IRQ_END - 1,
    };
    QTestState *qts = mmix_pcie_irq_start(1, "");
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(sources); i++) {
        unsigned int source = sources[i];
        uint64_t bit = mmix_intc_source_bit(source);

        mmix_intc_write_enable(qts, 0, source, bit);
        qtest_set_irq_in(qts, MMIX_INTC_QOM_PATH, "unnamed-gpio-in",
                         source, 1);
        g_assert_cmphex(mmix_intc_enable(qts, 0, source) & bit, ==, bit);
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
    qtest_add_func("/mmix/pcie/msi/single-vector",
                   test_mmix_pcie_msi_single_vector);
    qtest_add_func("/mmix/pcie/msi/invalid-programming",
                   test_mmix_pcie_msi_invalid_programming);
    qtest_add_func("/mmix/pcie/msi/independent-endpoints",
                   test_mmix_pcie_msi_independent_endpoints);
    qtest_add_func("/mmix/pcie/msi/multi-vector-masking",
                   test_mmix_pcie_msi_multi_vector_masking);
    qtest_add_func("/mmix/pcie/msix/table-and-masking",
                   test_mmix_pcie_msix_table_and_masking);
    qtest_add_func("/mmix/pcie/msix/independent-endpoints",
                   test_mmix_pcie_msix_independent_endpoints);
    qtest_add_func("/mmix/pcie/msix/intx-fallback",
                   test_mmix_pcie_msix_intx_fallback);
    qtest_add_func("/mmix/pcie/memory-mappings",
                   test_mmix_pcie_memory_mappings);
    qtest_add_func("/mmix/pcie/msi-aperture-reserved",
                   test_mmix_pcie_msi_aperture_reserved);
    qtest_add_func("/mmix/pcie/ecam-boundaries",
                   test_mmix_pcie_ecam_boundaries);
    qtest_add_func("/mmix/pcie/device-enumeration",
                   test_mmix_pcie_device_enumeration);
    qtest_add_func("/mmix/pcie/bar-access",
                   test_mmix_pcie_bar_access);
    qtest_add_func("/mmix/pcie/bridge-device",
                   test_mmix_pcie_bridge_device);
    qtest_add_func("/mmix/pcie/dma/below-4g",
                   test_mmix_pcie_dma_below_4g);
    qtest_add_func("/mmix/pcie/dma/above-4g",
                   test_mmix_pcie_dma_above_4g);
    qtest_add_func("/mmix/pcie/dma/invalid-targets",
                   test_mmix_pcie_dma_invalid_targets);
    qtest_add_func("/mmix/pcie/dma/msi-doorbell-widths",
                   test_mmix_pcie_dma_msi_doorbell_widths);
    qtest_add_func("/mmix/pcie/dma/msi-doorbell-boundaries",
                   test_mmix_pcie_dma_msi_doorbell_boundaries);
    qtest_add_func("/mmix/pcie/reset-state",
                   test_mmix_pcie_reset_state);
    qtest_add_func("/mmix/pcie/migration/empty",
                   test_mmix_pcie_empty_migration);
    qtest_add_func("/mmix/pcie/migration/populated",
                   test_mmix_pcie_populated_migration);
    qtest_add_func("/mmix/pcie/intx-swizzle",
                   test_mmix_pcie_intx_swizzle);
    qtest_add_func("/mmix/pcie/shared-intx",
                   test_mmix_pcie_shared_intx);
    qtest_add_func("/mmix/pcie/edge-irq-sources",
                   test_mmix_pcie_edge_irq_sources);

    return g_test_run();
}
