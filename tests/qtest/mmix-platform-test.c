/*
 * QTest testcase for the MMIX virt platform boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "elf.h"
#include <glib/gstdio.h>
#include "hw/pci/pci.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "standard-headers/linux/qemu_fw_cfg.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_mmio.h"

#ifndef EM_MMIX
#define EM_MMIX 80
#endif

#define MMIX_RAM_DEFAULT_SIZE          (512 * MiB)

#define MMIX_UART_BASE                 UINT64_C(0x0001000010000000)
#define MMIX_UART_SIZE                 UINT64_C(0x8)
#define MMIX_UART_LSR                  0x5
#define MMIX_UART_LSR_THRE             0x20
#define MMIX_UART_SCRATCH              0x7

#define MMIX_RTC_BASE                  UINT64_C(0x0001000010010000)
#define MMIX_RTC_SIZE                  UINT64_C(0x24)
#define MMIX_RTC_IRQ_ENABLED           0x10
#define MMIX_RTC_ALARM_LOW             0x08
#define MMIX_RTC_ALARM_HIGH            0x0c
#define MMIX_WATCHDOG_REFRESH_BASE     UINT64_C(0x0001000010020000)
#define MMIX_WATCHDOG_CONTROL_BASE     UINT64_C(0x0001000010030000)
#define MMIX_WATCHDOG_SIZE             UINT64_C(0x1000)
#define MMIX_WATCHDOG_ID               0x1043b
#define MMIX_WATCHDOG_IIDR             0xfcc
#define MMIX_WATCHDOG_WCS              0x000
#define MMIX_WATCHDOG_WOR              0x008
#define MMIX_WATCHDOG_WCS_EN           0x1
#define MMIX_POWER_BASE                UINT64_C(0x0001000010040000)
#define MMIX_POWER_SIZE                UINT64_C(0x100)
#define MMIX_POWER_FEATURES            0x00
#define MMIX_POWER_FEATURE_CONTROL     0x1
#define MMIX_FW_CFG_BASE               UINT64_C(0x0001000014000000)
#define MMIX_FW_CFG_SIZE               UINT64_C(0x18)
#define MMIX_FW_CFG_DATA               0x00
#define MMIX_FW_CFG_SELECTOR           0x08

#define MMIX_FRAMEBUFFER_BASE          UINT64_C(0x0001000018000000)
#define MMIX_FRAMEBUFFER_SIZE          UINT64_C(0x1000)
#define MMIX_FRAMEBUFFER_WIDTH         0x00
#define MMIX_FRAMEBUFFER_WIDTH_VALUE   1024
#define MMIX_FRAMEBUFFER_RAM_BASE      0x20

#define MMIX_TIMER_BASE                UINT64_C(0x0001000020000000)
#define MMIX_TIMER_CONTEXT_BASE        UINT64_C(0x0001000020010000)
#define MMIX_TIMER_CONTEXT_COMPARE     0x00
#define MMIX_TIMER_CONTEXT_CONTROL     0x08
#define MMIX_TIMER_CONTROL_ENABLE      0x01

#define MMIX_IPI_BASE                  UINT64_C(0x0001000024000000)
#define MMIX_IPI_ACTIVE_TARGETS        0x00
#define MMIX_IPI_SEND                  0x08
#define MMIX_IPI_CONTEXT_BASE          UINT64_C(0x0001000024010000)
#define MMIX_IPI_CONTEXT_STATUS        0x00

#define MMIX_INTC_BASE                 UINT64_C(0x0001000030000000)
#define MMIX_INTC_SOURCE_COUNT         0x00
#define MMIX_INTC_CONTEXT_COUNT        0x08
#define MMIX_INTC_CONTEXT_BASE         UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_ENABLE       0x00
#define MMIX_INTC_CONTEXT_CLAIM        0x0800
#define MMIX_INTC_CONTEXT_COMPLETE     0x0808
#define MMIX_INTC_PENDING_BASE         0x1000
#define MMIX_INTC_SOURCE_COUNT_VALUE   8192

#define MMIX_VIRTIO_BASE               UINT64_C(0x0001000040000000)
#define MMIX_DISCOVERABLE_BASE         UINT64_C(0x0001000050000000)
#define MMIX_PCIE_ECAM_BASE            UINT64_C(0x0001000100000000)
#define MMIX_PCIE_MMIO32_BASE          UINT64_C(0x0001000200000000)
#define MMIX_PCIE_DEVICE_SIZE          UINT64_C(0x8000)

#define MMIX_PCIE_INTX_IRQ             6145
#define MMIX_EDU_BAR_ADDRESS           UINT32_C(0x00200000)
#define MMIX_EDU_BAR_SIZE              UINT64_C(0x100000)
#define MMIX_EDU_ID                    UINT32_C(0x010000ed)
#define MMIX_EDU_IRQ_RAISE             0x60
#define MMIX_EDU_DMA_SRC               0x80
#define MMIX_EDU_DMA_DST               0x88
#define MMIX_EDU_DMA_COUNT             0x90
#define MMIX_EDU_DMA_COMMAND           0x98
#define MMIX_EDU_DMA_BUFFER            UINT64_C(0x40000)
#define MMIX_EDU_DMA_RUN               UINT64_C(0x1)
#define MMIX_EDU_DMA_TO_PCI            UINT64_C(0x2)
#define MMIX_E1000_BAR_ADDRESS         UINT32_C(0x00800000)
#define MMIX_E1000_ICR                 0x00c0
#define MMIX_E1000_ICS                 0x00c8
#define MMIX_E1000_IMS                 0x00d0
#define MMIX_E1000_TEST_CAUSE          UINT32_C(0x1)

#define MMIX_POPULATED_SOURCE_RAM      UINT64_C(0x00100000)
#define MMIX_POPULATED_DEST_RAM        UINT64_C(0x00101000)
#define MMIX_POPULATED_RAM_MARKER      UINT64_C(0x1122334455667788)
#define MMIX_POPULATED_FB_MARKER       UINT64_C(0x8877665544332211)

#define MMIX_CONTEXT_STRIDE            UINT64_C(0x10000)
#define MMIX_CONTEXT_REGISTER_SIZE     UINT64_C(0x1000)
#define MMIX_INITIAL_CONTEXT_COUNT     64
#define MMIX_CONTEXT_CAPACITY          1023
#define MMIX_VIRTIO_STRIDE             UINT64_C(0x10000)
#define MMIX_VIRTIO_REGISTER_SIZE      UINT64_C(0x200)
#define MMIX_VIRTIO_ACTIVE_SLOTS       32
#define MMIX_VIRTIO_SLOT_CAPACITY      4096

typedef struct MMIXRAMSizeCase {
    const char *memory;
    uint64_t size;
    unsigned int cpus;
} MMIXRAMSizeCase;

typedef struct MMIXPlatformRange {
    uint64_t base;
    uint64_t size;
} MMIXPlatformRange;

typedef enum MMIXPlatformBootMode {
    MMIX_PLATFORM_NO_IMAGE,
    MMIX_PLATFORM_RAW,
    MMIX_PLATFORM_BARE_ELF,
    MMIX_PLATFORM_HOSTED_ELF,
    MMIX_PLATFORM_MMO,
    MMIX_PLATFORM_LINUX,
    MMIX_PLATFORM_FIRMWARE,
} MMIXPlatformBootMode;

typedef struct MMIXPlatformBootCase {
    MMIXPlatformBootMode mode;
    const char *memory;
    unsigned int cpus;
} MMIXPlatformBootCase;

static uint64_t mmix_context_address(uint64_t base, unsigned int context)
{
    return base + context * MMIX_CONTEXT_STRIDE;
}

static uint64_t mmix_virtio_address(unsigned int slot)
{
    return MMIX_VIRTIO_BASE + slot * MMIX_VIRTIO_STRIDE;
}

static void mmix_assert_mapping(const char *mtree, uint64_t base,
                                uint64_t size, const char *name)
{
    g_autofree char *mapping =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, i/o): %s",
                        base, base + size - 1, name);

    g_assert_nonnull(strstr(mtree, mapping));
}

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint64_t probe = UINT64_C(0x5aa55aa50ff0f00f);

    qtest_writeq(qts, address, probe);
    g_assert_cmphex(qtest_readq(qts, address), !=, probe);
}

static uint32_t mmix_readl_le(QTestState *qts, uint64_t address)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static void mmix_writel_le(QTestState *qts, uint64_t address, uint32_t value)
{
    uint8_t bytes[sizeof(value)];

    stl_le_p(bytes, value);
    qtest_memwrite(qts, address, bytes, sizeof(bytes));
}

static void mmix_writew_le(QTestState *qts, uint64_t address, uint16_t value)
{
    uint8_t bytes[sizeof(value)];

    stw_le_p(bytes, value);
    qtest_memwrite(qts, address, bytes, sizeof(bytes));
}

static uint64_t mmix_pcie_ecam_address(unsigned int device,
                                       unsigned int reg)
{
    return MMIX_PCIE_ECAM_BASE + device * MMIX_PCIE_DEVICE_SIZE + reg;
}

static uint64_t mmix_intc_source_address(uint64_t base, unsigned int source)
{
    return base + (source / 64) * sizeof(uint64_t);
}

static uint64_t mmix_intc_source_bit(unsigned int source)
{
    return UINT64_C(1) << (source % 64);
}

static void mmix_assert_fw_cfg_signature(QTestState *qts)
{
    uint8_t signature[4];
    unsigned int i;

    qtest_writew(qts, MMIX_FW_CFG_BASE + MMIX_FW_CFG_SELECTOR,
                 FW_CFG_SIGNATURE);
    for (i = 0; i < ARRAY_SIZE(signature); i++) {
        signature[i] = qtest_readb(qts, MMIX_FW_CFG_BASE + MMIX_FW_CFG_DATA);
    }
    g_assert_cmpmem(signature, sizeof(signature), "QEMU", 4);
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

static uint64_t mmix_configure_edu(QTestState *qts)
{
    uint64_t config = mmix_pcie_ecam_address(1, 0);

    mmix_writel_le(qts, config + PCI_BASE_ADDRESS_0,
                   MMIX_EDU_BAR_ADDRESS);
    mmix_writew_le(qts, config + PCI_COMMAND,
                   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return MMIX_PCIE_MMIO32_BASE + MMIX_EDU_BAR_ADDRESS;
}

static void mmix_edu_dma_copy(QTestState *qts, uint64_t bar)
{
    mmix_edu_dma_run(qts, bar, MMIX_POPULATED_SOURCE_RAM,
                     MMIX_EDU_DMA_BUFFER, sizeof(uint64_t), 0);
    mmix_edu_dma_run(qts, bar, MMIX_EDU_DMA_BUFFER,
                     MMIX_POPULATED_DEST_RAM, sizeof(uint64_t),
                     MMIX_EDU_DMA_TO_PCI);
}

static void mmix_assert_active_devices(QTestState *qts, unsigned int cpus)
{
    uint64_t active_targets = cpus == 64 ? UINT64_MAX :
                              (UINT64_C(1) << cpus) - 1;

    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_LSR) &
                    MMIX_UART_LSR_THRE, ==, MMIX_UART_LSR_THRE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_FRAMEBUFFER_BASE +
                                 MMIX_FRAMEBUFFER_WIDTH), ==,
                     MMIX_FRAMEBUFFER_WIDTH_VALUE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_IPI_BASE +
                                 MMIX_IPI_ACTIVE_TARGETS), ==,
                     active_targets);
    g_assert_cmpuint(qtest_readq(qts, MMIX_INTC_BASE +
                                 MMIX_INTC_SOURCE_COUNT), ==,
                     MMIX_INTC_SOURCE_COUNT_VALUE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_INTC_BASE +
                                 MMIX_INTC_CONTEXT_COUNT), ==, cpus);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_RTC_BASE +
                                      MMIX_RTC_IRQ_ENABLED), ==, 0);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_REFRESH_BASE +
                                      MMIX_WATCHDOG_IIDR), ==,
                    MMIX_WATCHDOG_ID);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                      MMIX_WATCHDOG_IIDR), ==,
                    MMIX_WATCHDOG_ID);
    g_assert_cmphex(qtest_readl(qts, MMIX_POWER_BASE +
                                    MMIX_POWER_FEATURES), ==,
                    MMIX_POWER_FEATURE_CONTROL);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_PCIE_ECAM_BASE), ==,
                    PCI_DEVICE_ID_REDHAT_PCIE_HOST << 16 |
                    PCI_VENDOR_ID_REDHAT);
}

static void test_mmix_platform_mappings(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    unsigned int i;

    mmix_assert_mapping(mtree, MMIX_UART_BASE, MMIX_UART_SIZE, "serial");
    mmix_assert_mapping(mtree, MMIX_RTC_BASE, MMIX_RTC_SIZE,
                        "goldfish_rtc");
    mmix_assert_mapping(mtree, MMIX_WATCHDOG_REFRESH_BASE,
                        MMIX_WATCHDOG_SIZE, "sbsa_gwdt.refresh");
    mmix_assert_mapping(mtree, MMIX_WATCHDOG_CONTROL_BASE,
                        MMIX_WATCHDOG_SIZE, "sbsa_gwdt.control");
    mmix_assert_mapping(mtree, MMIX_POWER_BASE, MMIX_POWER_SIZE,
                        "virt-ctrl");
    mmix_assert_mapping(mtree, MMIX_FW_CFG_BASE, 0x8,
                        "fwcfg.data");
    mmix_assert_mapping(mtree, MMIX_FW_CFG_BASE + 0x8, 0x2,
                        "fwcfg.ctl");
    mmix_assert_mapping(mtree, MMIX_FW_CFG_BASE + 0x10, 0x8,
                        "fwcfg.dma");
    mmix_assert_mapping(mtree, MMIX_FRAMEBUFFER_BASE,
                        MMIX_FRAMEBUFFER_SIZE, "mmix-framebuffer");
    mmix_assert_mapping(mtree, MMIX_TIMER_BASE,
                        MMIX_CONTEXT_REGISTER_SIZE, "mmix-timer-global");
    mmix_assert_mapping(mtree, MMIX_IPI_BASE,
                        MMIX_CONTEXT_REGISTER_SIZE, "mmix-ipi-global");
    mmix_assert_mapping(mtree, MMIX_INTC_BASE, MMIX_CONTEXT_STRIDE,
                        "mmix-intc-global");

    for (i = 0; i < MMIX_INITIAL_CONTEXT_COUNT; i++) {
        g_autofree char *timer =
            g_strdup_printf("mmix-timer-context[%u]", i);
        g_autofree char *ipi = g_strdup_printf("mmix-ipi-context[%u]", i);
        g_autofree char *intc =
            g_strdup_printf("mmix-intc-context[%u]", i);

        mmix_assert_mapping(mtree,
                            mmix_context_address(MMIX_TIMER_CONTEXT_BASE, i),
                            MMIX_CONTEXT_REGISTER_SIZE, timer);
        mmix_assert_mapping(mtree,
                            mmix_context_address(MMIX_IPI_CONTEXT_BASE, i),
                            MMIX_CONTEXT_REGISTER_SIZE, ipi);
        mmix_assert_mapping(mtree,
                            mmix_context_address(MMIX_INTC_CONTEXT_BASE, i),
                            MMIX_CONTEXT_REGISTER_SIZE, intc);
    }
    for (i = 0; i < MMIX_VIRTIO_ACTIVE_SLOTS; i++) {
        mmix_assert_mapping(mtree, mmix_virtio_address(i),
                            MMIX_VIRTIO_REGISTER_SIZE, "virtio-mmio");
    }

    qtest_quit(qts);
}

static void test_mmix_platform_slot_boundaries(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint64_t address;

    mmix_assert_active_devices(qts, 1);
    mmix_assert_fw_cfg_signature(qts);

    mmix_assert_unassigned(qts, MMIX_UART_BASE + MMIX_UART_SIZE);
    mmix_assert_unassigned(qts, MMIX_RTC_BASE + MMIX_RTC_SIZE);
    mmix_assert_unassigned(qts, MMIX_RTC_BASE + MMIX_CONTEXT_STRIDE - 8);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_REFRESH_BASE +
                                MMIX_WATCHDOG_SIZE);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                MMIX_WATCHDOG_SIZE);
    mmix_assert_unassigned(qts, MMIX_POWER_BASE + MMIX_POWER_SIZE);
    mmix_assert_unassigned(qts, MMIX_FW_CFG_BASE + MMIX_FW_CFG_SIZE);
    mmix_assert_unassigned(qts, MMIX_FRAMEBUFFER_BASE +
                                MMIX_FRAMEBUFFER_SIZE);
    mmix_assert_unassigned(qts, MMIX_TIMER_BASE +
                                MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_IPI_BASE +
                                MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_INTC_BASE + MMIX_CONTEXT_STRIDE);

    address = mmix_context_address(MMIX_TIMER_CONTEXT_BASE, 1);
    qtest_writeq(qts, address + MMIX_TIMER_CONTEXT_CONTROL, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, address +
                                MMIX_TIMER_CONTEXT_CONTROL), ==, 0);
    address = mmix_context_address(MMIX_IPI_CONTEXT_BASE, 1);
    g_assert_cmphex(qtest_readq(qts, address + MMIX_IPI_CONTEXT_STATUS),
                    ==, 0);
    address = mmix_context_address(MMIX_INTC_CONTEXT_BASE, 1);
    qtest_writeq(qts, address + MMIX_INTC_CONTEXT_ENABLE, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, address +
                                MMIX_INTC_CONTEXT_ENABLE), ==, 0);

    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE, 0) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT - 1) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY - 1));
    g_assert_cmpuint(qtest_readq(
                         qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                                   MMIX_CONTEXT_CAPACITY)),
                     ==, 1);

    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE, 0) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT - 1) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY - 1));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY));

    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE, 0) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT - 1) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY - 1));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY));

    g_assert_cmphex(qtest_readl(qts, mmix_virtio_address(0) +
                                VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    g_assert_cmphex(qtest_readl(
                        qts, mmix_virtio_address(
                                 MMIX_VIRTIO_ACTIVE_SLOTS - 1) +
                             VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    mmix_assert_unassigned(qts, mmix_virtio_address(0) +
                                MMIX_VIRTIO_REGISTER_SIZE);
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_ACTIVE_SLOTS - 1) +
                           MMIX_VIRTIO_REGISTER_SIZE);
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_ACTIVE_SLOTS));
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_SLOT_CAPACITY - 1));
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_SLOT_CAPACITY));

    qtest_quit(qts);
}

static void test_mmix_platform_ram_size_independence(gconstpointer opaque)
{
    const MMIXRAMSizeCase *test = opaque;
    g_autofree char *args = test->memory ?
        g_strdup_printf("-machine virt -m %s -smp %u", test->memory,
                        test->cpus) :
        g_strdup_printf("-machine virt -smp %u", test->cpus);
    QTestState *qts = qtest_init(args);
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *ram =
        g_strdup_printf("%016x-%016" PRIx64 " (prio 0, ram): mmix.ram",
                        0, test->size - 1);

    g_assert_nonnull(strstr(mtree, ram));
    mmix_assert_active_devices(qts, test->cpus);
    qtest_quit(qts);
}

static void test_mmix_platform_low_address_isolation(void)
{
    static const uint64_t low_addresses[] = {
        0x10000000,
        0x10001000,
        0x10002000,
        0x10003000,
        0x10004000,
        0x10006000,
        0x18000000,
        0x20000000,
        0x24000000,
        0x30000000,
        0x40000000,
    };
    QTestState *qts = qtest_init("-machine virt -m 8G");
    size_t i;

    for (i = 0; i < ARRAY_SIZE(low_addresses); i++) {
        uint64_t value = UINT64_C(0x1020304050607000) + i;

        qtest_writeq(qts, low_addresses[i], value);
        g_assert_cmphex(qtest_readq(qts, low_addresses[i]), ==, value);
    }
    mmix_assert_active_devices(qts, 1);

    qtest_quit(qts);
}

static void mmix_assert_deferred_apertures(QTestState *qts)
{
    static const uint64_t addresses[] = {
        UINT64_C(0x0001000000000000),
        UINT64_C(0x0001000010050000),
        UINT64_C(0x0001000014010000),
        UINT64_C(0x0001000018010000),
        MMIX_DISCOVERABLE_BASE,
        UINT64_C(0x0001000080000000),
        UINT64_C(0x0001000110000000),
        UINT64_C(0x0001000300000000),
        UINT64_C(0x0001110000000000),
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        mmix_assert_unassigned(qts, addresses[i]);
    }
}

static void test_mmix_platform_deferred_apertures(void)
{
    QTestState *qts = qtest_init("-machine virt");

    mmix_assert_deferred_apertures(qts);
    qtest_quit(qts);
}

static void test_mmix_platform_nonoverlapping_extents(void)
{
    static const MMIXPlatformRange ranges[] = {
        { MMIX_UART_BASE, MMIX_UART_SIZE },
        { MMIX_RTC_BASE, MMIX_RTC_SIZE },
        { MMIX_WATCHDOG_REFRESH_BASE, MMIX_WATCHDOG_SIZE },
        { MMIX_WATCHDOG_CONTROL_BASE, MMIX_WATCHDOG_SIZE },
        { MMIX_POWER_BASE, MMIX_POWER_SIZE },
        { MMIX_FW_CFG_BASE, MMIX_FW_CFG_SIZE },
        { MMIX_FRAMEBUFFER_BASE, MMIX_FRAMEBUFFER_SIZE },
        { MMIX_TIMER_BASE, MMIX_CONTEXT_REGISTER_SIZE },
        { MMIX_IPI_BASE, MMIX_CONTEXT_REGISTER_SIZE },
        { MMIX_INTC_BASE, MMIX_CONTEXT_STRIDE },
        { MMIX_VIRTIO_BASE,
          MMIX_VIRTIO_ACTIVE_SLOTS * MMIX_VIRTIO_STRIDE },
        { UINT64_C(0x0001000100000000), UINT64_C(0x10000000) },
        { UINT64_C(0x0001000200000000), UINT64_C(0x100000000) },
        { UINT64_C(0x0001010000000000), UINT64_C(0x100000000000) },
    };
    unsigned int i;
    unsigned int j;

    for (i = 0; i < ARRAY_SIZE(ranges); i++) {
        for (j = i + 1; j < ARRAY_SIZE(ranges); j++) {
            g_assert_true(ranges[i].base + ranges[i].size <= ranges[j].base ||
                          ranges[j].base + ranges[j].size <= ranges[i].base);
        }
    }
}

static char *mmix_write_platform_image(const char *directory,
                                       const char *name,
                                       const uint8_t *contents, size_t size)
{
    g_autoptr(GError) error = NULL;
    char *filename = g_build_filename(directory, name, NULL);

    g_assert_true(g_file_set_contents(filename, (const char *)contents,
                                      size, &error));
    g_assert_no_error(error);
    return filename;
}

static char *mmix_create_platform_elf(const char *directory)
{
    enum { CODE_OFFSET = 0x100 };
    const uint64_t entry = 0x10000;
    const uint8_t code[] = { 0xfd, 0x00, 0x00, 0x00 };
    uint8_t image[CODE_OFFSET + sizeof(code)] = { 0 };
    Elf64_Ehdr ehdr = { 0 };
    Elf64_Phdr phdr = { 0 };

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
    return mmix_write_platform_image(directory, "kernel.elf", image,
                                     sizeof(image));
}

static char *mmix_create_platform_image(const char *directory,
                                        MMIXPlatformBootMode mode)
{
    static const uint8_t mmo[] = {
        0x98, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x98, 0x0a, 0x00, 0xff,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x98, 0x0b, 0x00, 0x00,
        0x98, 0x0c, 0x00, 0x00,
    };
    uint8_t raw[0x104] = { 0 };
    const uint8_t firmware[] = { 0xfd, 0x00, 0x00, 0x00 };

    switch (mode) {
    case MMIX_PLATFORM_RAW:
        raw[0x100] = 0xfd;
        return mmix_write_platform_image(directory, "kernel.raw", raw,
                                         sizeof(raw));
    case MMIX_PLATFORM_BARE_ELF:
    case MMIX_PLATFORM_HOSTED_ELF:
    case MMIX_PLATFORM_LINUX:
        return mmix_create_platform_elf(directory);
    case MMIX_PLATFORM_MMO:
        return mmix_write_platform_image(directory, "kernel.mmo", mmo,
                                         sizeof(mmo));
    case MMIX_PLATFORM_FIRMWARE:
        return mmix_write_platform_image(directory, "firmware.bin", firmware,
                                         sizeof(firmware));
    case MMIX_PLATFORM_NO_IMAGE:
        return NULL;
    }
    g_assert_not_reached();
}

static void test_mmix_platform_boot_mode(gconstpointer opaque)
{
    const MMIXPlatformBootCase *test = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *image = NULL;
    g_autoptr(GString) args = g_string_new(NULL);
    const char *machine = test->mode == MMIX_PLATFORM_HOSTED_ELF ?
                          "virt,elf-startup-abi=argc-argv" :
                          test->mode == MMIX_PLATFORM_LINUX ?
                          "virt,elf-startup-abi=linux" : "virt";
    QTestState *qts;

    g_string_append_printf(args, "-machine %s -m %s -smp %u",
                           machine, test->memory, test->cpus);
    if (test->mode != MMIX_PLATFORM_NO_IMAGE) {
        directory = g_dir_make_tmp("mmix-platform-mode-XXXXXX", &error);
        g_assert_no_error(error);
        g_assert_nonnull(directory);
        image = mmix_create_platform_image(directory, test->mode);
    }

    switch (test->mode) {
    case MMIX_PLATFORM_NO_IMAGE:
        break;
    case MMIX_PLATFORM_RAW:
    case MMIX_PLATFORM_BARE_ELF:
    case MMIX_PLATFORM_MMO:
        g_string_append_printf(args, " -kernel %s", image);
        break;
    case MMIX_PLATFORM_HOSTED_ELF:
        g_string_append_printf(args, " -semihosting -kernel %s", image);
        break;
    case MMIX_PLATFORM_LINUX:
        g_string_append_printf(args, " -kernel %s", image);
        break;
    case MMIX_PLATFORM_FIRMWARE:
        g_string_append_printf(args, " -bios %s", image);
        break;
    }

    qts = qtest_init(args->str);
    mmix_assert_active_devices(qts, test->cpus);
    qtest_quit(qts);

    if (image) {
        g_assert_cmpint(g_unlink(image), ==, 0);
        g_assert_cmpint(g_rmdir(directory), ==, 0);
    }
}

static const char mmix_populated_devices[] =
    "-rtc clock=vm "
    "-watchdog-action none "
    "-object rng-builtin,id=rng0 "
    "-device virtio-rng-device,id=vrng,rng=rng0 "
    "-device edu,bus=pcie.0,addr=1.0,dma_mask=0xffffffffffffffff "
    "-device e1000,bus=pcie.0,addr=2.0";

static uint64_t mmix_configure_e1000(QTestState *qts)
{
    uint64_t config = mmix_pcie_ecam_address(2, 0);

    g_assert_cmphex(mmix_readl_le(qts, config), ==, 0x100e8086);
    mmix_writel_le(qts, config + PCI_BASE_ADDRESS_0,
                   MMIX_E1000_BAR_ADDRESS);
    mmix_writew_le(qts, config + PCI_COMMAND,
                   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return MMIX_PCIE_MMIO32_BASE + MMIX_E1000_BAR_ADDRESS;
}

static void mmix_populated_program_state(QTestState *qts,
                                         unsigned int target_cpu)
{
    const uint64_t timer =
        mmix_context_address(MMIX_TIMER_CONTEXT_BASE, target_cpu);
    const uint64_t ipi =
        mmix_context_address(MMIX_IPI_CONTEXT_BASE, target_cpu);
    const uint64_t intc =
        mmix_context_address(MMIX_INTC_CONTEXT_BASE, target_cpu);
    const uint64_t irq_bit = mmix_intc_source_bit(MMIX_PCIE_INTX_IRQ);
    const uint64_t irq_enable =
        mmix_intc_source_address(intc + MMIX_INTC_CONTEXT_ENABLE,
                                 MMIX_PCIE_INTX_IRQ);
    const uint64_t framebuffer =
        qtest_readq(qts, MMIX_FRAMEBUFFER_BASE + MMIX_FRAMEBUFFER_RAM_BASE);
    const uint64_t bar = mmix_configure_edu(qts);

    g_assert_cmphex(qtest_readl(qts, bar), ==, MMIX_EDU_ID);
    qtest_writeq(qts, MMIX_POPULATED_SOURCE_RAM,
                 MMIX_POPULATED_RAM_MARKER);
    mmix_edu_dma_copy(qts, bar);
    g_assert_cmphex(qtest_readq(qts, MMIX_POPULATED_DEST_RAM), ==,
                    MMIX_POPULATED_RAM_MARKER);

    qtest_writeb(qts, MMIX_UART_BASE + MMIX_UART_SCRATCH, 0x5a);
    mmix_writel_le(qts, MMIX_RTC_BASE + MMIX_RTC_ALARM_LOW, 0x89abcdef);
    mmix_writel_le(qts, MMIX_RTC_BASE + MMIX_RTC_ALARM_HIGH, 0x01234567);
    mmix_writel_le(qts, MMIX_RTC_BASE + MMIX_RTC_IRQ_ENABLED, 1);
    mmix_writel_le(qts, MMIX_WATCHDOG_CONTROL_BASE + MMIX_WATCHDOG_WOR,
                   1000);
    mmix_writel_le(qts, MMIX_WATCHDOG_CONTROL_BASE + MMIX_WATCHDOG_WCS,
                   MMIX_WATCHDOG_WCS_EN);
    qtest_writeq(qts, framebuffer, MMIX_POPULATED_FB_MARKER);
    qtest_writeq(qts, timer + MMIX_TIMER_CONTEXT_COMPARE, UINT64_MAX);
    qtest_writeq(qts, timer + MMIX_TIMER_CONTEXT_CONTROL,
                 MMIX_TIMER_CONTROL_ENABLE);
    qtest_writeq(qts, MMIX_IPI_BASE + MMIX_IPI_SEND,
                 UINT64_C(1) << target_cpu);
    qtest_writeq(qts, irq_enable, irq_bit);
    qtest_writel(qts, MMIX_VIRTIO_BASE + VIRTIO_MMIO_STATUS,
                 VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmix_writel_le(qts, bar + MMIX_EDU_IRQ_RAISE, 1);

    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_SCRATCH), ==,
                    0x5a);
    g_assert_cmphex(qtest_readq(qts, ipi + MMIX_IPI_CONTEXT_STATUS), ==, 1);
    g_assert_cmphex(qtest_readq(qts, irq_enable), ==, irq_bit);
    g_assert_cmphex(qtest_readq(
                        qts,
                        mmix_intc_source_address(
                            MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE,
                            MMIX_PCIE_INTX_IRQ)) & irq_bit, ==, irq_bit);
}

static void mmix_populated_assert_preserved(QTestState *qts,
                                            unsigned int target_cpu)
{
    const uint64_t timer =
        mmix_context_address(MMIX_TIMER_CONTEXT_BASE, target_cpu);
    const uint64_t ipi =
        mmix_context_address(MMIX_IPI_CONTEXT_BASE, target_cpu);
    const uint64_t intc =
        mmix_context_address(MMIX_INTC_CONTEXT_BASE, target_cpu);
    const uint64_t irq_bit = mmix_intc_source_bit(MMIX_PCIE_INTX_IRQ);
    const uint64_t irq_enable =
        mmix_intc_source_address(intc + MMIX_INTC_CONTEXT_ENABLE,
                                 MMIX_PCIE_INTX_IRQ);
    const uint64_t framebuffer =
        qtest_readq(qts, MMIX_FRAMEBUFFER_BASE + MMIX_FRAMEBUFFER_RAM_BASE);

    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_SCRATCH), ==,
                    0x5a);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_RTC_BASE + MMIX_RTC_ALARM_LOW),
                    ==, 0x89abcdef);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_RTC_BASE + MMIX_RTC_ALARM_HIGH),
                    ==, 0x01234567);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_RTC_BASE +
                                      MMIX_RTC_IRQ_ENABLED), ==, 1);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                      MMIX_WATCHDOG_WOR), ==, 1000);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                      MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN);
    g_assert_cmphex(qtest_readq(qts, framebuffer), ==,
                    MMIX_POPULATED_FB_MARKER);
    g_assert_cmphex(qtest_readq(qts, timer + MMIX_TIMER_CONTEXT_COMPARE), ==,
                    UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, timer + MMIX_TIMER_CONTEXT_CONTROL), ==,
                    MMIX_TIMER_CONTROL_ENABLE);
    g_assert_cmphex(qtest_readq(qts, ipi + MMIX_IPI_CONTEXT_STATUS), ==, 1);
    g_assert_cmphex(qtest_readq(qts, irq_enable), ==, irq_bit);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE + VIRTIO_MMIO_STATUS),
                    ==, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    g_assert_cmphex(qtest_readq(qts, MMIX_POPULATED_DEST_RAM), ==,
                    MMIX_POPULATED_RAM_MARKER);
}

static void test_mmix_platform_populated_reset(void)
{
    QTestState *qts = qtest_initf("-machine virt -m 512M -smp 1 %s",
                                  mmix_populated_devices);
    const uint64_t irq_bit = mmix_intc_source_bit(MMIX_PCIE_INTX_IRQ);
    const uint64_t framebuffer =
        qtest_readq(qts, MMIX_FRAMEBUFFER_BASE + MMIX_FRAMEBUFFER_RAM_BASE);

    mmix_populated_program_state(qts, 0);
    qtest_system_reset(qts);

    mmix_assert_active_devices(qts, 1);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_SCRATCH), ==,
                    0);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_RTC_BASE + MMIX_RTC_ALARM_LOW),
                    ==, 0);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                      MMIX_WATCHDOG_WCS), ==, 0);
    g_assert_cmphex(qtest_readq(qts, MMIX_TIMER_CONTEXT_BASE +
                                    MMIX_TIMER_CONTEXT_COMPARE), ==, 0);
    g_assert_cmphex(qtest_readq(qts, MMIX_TIMER_CONTEXT_BASE +
                                    MMIX_TIMER_CONTEXT_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(qts, MMIX_IPI_CONTEXT_BASE +
                                    MMIX_IPI_CONTEXT_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(
                        qts,
                        mmix_intc_source_address(
                            MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE,
                            MMIX_PCIE_INTX_IRQ)) & irq_bit, ==, 0);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE + VIRTIO_MMIO_STATUS),
                    ==, 0);
    g_assert_cmphex(mmix_readl_le(
                        qts, mmix_pcie_ecam_address(1, PCI_BASE_ADDRESS_0)) &
                    PCI_BASE_ADDRESS_MEM_MASK, ==, 0);
    g_assert_cmphex(qtest_readq(qts, framebuffer), ==,
                    MMIX_POPULATED_FB_MARKER);
    g_assert_cmphex(qtest_readq(qts, MMIX_POPULATED_DEST_RAM), ==,
                    MMIX_POPULATED_RAM_MARKER);
    mmix_assert_deferred_apertures(qts);
    qtest_quit(qts);
}

static void test_mmix_platform_populated_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir =
        g_dir_make_tmp("mmix-platform-state-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    g_autofree char *args = NULL;
    QTestState *from;
    QTestState *to;
    uint64_t e1000_bar;
    uint64_t intc;
    uint64_t irq_bit;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    args = g_strdup_printf("-machine virt -m 512M -smp 2 %s",
                           mmix_populated_devices);
    incoming = g_strdup_printf("%s -incoming %s", args, uri);
    from = qtest_init(args);
    to = qtest_init(incoming);

    mmix_populated_program_state(from, 1);
    intc = mmix_context_address(MMIX_INTC_CONTEXT_BASE, 1);
    irq_bit = mmix_intc_source_bit(MMIX_PCIE_INTX_IRQ);
    g_assert_cmpuint(qtest_readq(from, intc + MMIX_INTC_CONTEXT_CLAIM), ==,
                     MMIX_PCIE_INTX_IRQ);
    e1000_bar = mmix_configure_e1000(from);
    mmix_writel_le(from, e1000_bar + MMIX_E1000_IMS,
                   MMIX_E1000_TEST_CAUSE);
    mmix_writel_le(from, e1000_bar + MMIX_E1000_ICS,
                   MMIX_E1000_TEST_CAUSE);
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(from, "STOP");
    qtest_qmp_eventwait(to, "RESUME");

    mmix_populated_assert_preserved(to, 1);
    mmix_assert_fw_cfg_signature(to);
    g_assert_cmphex(qtest_readq(
                        to,
                        mmix_intc_source_address(
                            MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE,
                            MMIX_PCIE_INTX_IRQ)) & irq_bit, ==, 0);
    qtest_writeq(to, intc + MMIX_INTC_CONTEXT_COMPLETE,
                 MMIX_PCIE_INTX_IRQ);
    g_assert_cmphex(qtest_readq(
                        to,
                        mmix_intc_source_address(
                            MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE,
                            MMIX_PCIE_INTX_IRQ)) & irq_bit, ==, irq_bit);
    g_assert_cmphex(mmix_readl_le(
                        to, mmix_pcie_ecam_address(2, PCI_BASE_ADDRESS_0)) &
                    PCI_BASE_ADDRESS_MEM_MASK, ==, MMIX_E1000_BAR_ADDRESS);
    g_assert_cmphex(mmix_readl_le(to, e1000_bar + MMIX_E1000_ICR) &
                    MMIX_E1000_TEST_CAUSE, ==, MMIX_E1000_TEST_CAUSE);
    mmix_assert_deferred_apertures(to);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

int main(int argc, char **argv)
{
    static const MMIXRAMSizeCase ram_sizes[] = {
        { "128M", 128 * MiB, 1 },
        { NULL, MMIX_RAM_DEFAULT_SIZE, 1 },
        { "8G", 8 * GiB, 64 },
    };
    static const MMIXPlatformBootCase boot_modes[] = {
        { MMIX_PLATFORM_NO_IMAGE, "512M", 1 },
        { MMIX_PLATFORM_RAW, "128M", 1 },
        { MMIX_PLATFORM_BARE_ELF, "512M", 1 },
        { MMIX_PLATFORM_HOSTED_ELF, "512M", 1 },
        { MMIX_PLATFORM_MMO, "128M", 1 },
        { MMIX_PLATFORM_LINUX, "8G", 64 },
        { MMIX_PLATFORM_FIRMWARE, "512M", 2 },
    };
    static const char * const boot_mode_names[] = {
        "no-image", "raw", "bare-elf", "hosted-elf", "mmo", "linux",
        "firmware",
    };
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/platform/mappings",
                   test_mmix_platform_mappings);
    qtest_add_func("/mmix/platform/slot-boundaries",
                   test_mmix_platform_slot_boundaries);
    qtest_add_data_func("/mmix/platform/ram-size/minimum", &ram_sizes[0],
                        test_mmix_platform_ram_size_independence);
    qtest_add_data_func("/mmix/platform/ram-size/default", &ram_sizes[1],
                        test_mmix_platform_ram_size_independence);
    qtest_add_data_func("/mmix/platform/ram-size/above-4g", &ram_sizes[2],
                        test_mmix_platform_ram_size_independence);
    qtest_add_func("/mmix/platform/low-address-isolation",
                   test_mmix_platform_low_address_isolation);
    qtest_add_func("/mmix/platform/deferred-apertures",
                   test_mmix_platform_deferred_apertures);
    qtest_add_func("/mmix/platform/nonoverlapping-extents",
                   test_mmix_platform_nonoverlapping_extents);
    qtest_add_func("/mmix/platform/populated/reset",
                   test_mmix_platform_populated_reset);
    qtest_add_func("/mmix/platform/populated/migration",
                   test_mmix_platform_populated_migration);
    for (i = 0; i < ARRAY_SIZE(boot_modes); i++) {
        g_autofree char *name = g_strdup_printf(
            "/mmix/platform/boot-mode/%s", boot_mode_names[i]);

        qtest_add_data_func(name, &boot_modes[i],
                            test_mmix_platform_boot_mode);
    }

    return g_test_run();
}
