/*
 * QTest testcase for the MMIX virt platform boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/units.h"
#include "standard-headers/linux/virtio_mmio.h"

#define MMIX_RAM_DEFAULT_SIZE          (512 * MiB)

#define MMIX_UART_BASE                 UINT64_C(0x0001000010000000)
#define MMIX_UART_SIZE                 UINT64_C(0x8)
#define MMIX_UART_LSR                  0x5
#define MMIX_UART_LSR_THRE             0x20

#define MMIX_FRAMEBUFFER_BASE          UINT64_C(0x0001000018000000)
#define MMIX_FRAMEBUFFER_SIZE          UINT64_C(0x1000)
#define MMIX_FRAMEBUFFER_WIDTH         0x00
#define MMIX_FRAMEBUFFER_WIDTH_VALUE   1024

#define MMIX_TIMER_BASE                UINT64_C(0x0001000020000000)
#define MMIX_TIMER_CONTEXT_BASE        UINT64_C(0x0001000020010000)
#define MMIX_TIMER_CONTEXT_CONTROL     0x08

#define MMIX_IPI_BASE                  UINT64_C(0x0001000024000000)
#define MMIX_IPI_ACTIVE_TARGETS        0x00
#define MMIX_IPI_CONTEXT_BASE          UINT64_C(0x0001000024010000)
#define MMIX_IPI_CONTEXT_STATUS        0x00

#define MMIX_INTC_BASE                 UINT64_C(0x0001000030000000)
#define MMIX_INTC_SOURCE_COUNT         0x00
#define MMIX_INTC_CONTEXT_COUNT        0x08
#define MMIX_INTC_CONTEXT_BASE         UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_ENABLE       0x00
#define MMIX_INTC_SOURCE_COUNT_VALUE   8192

#define MMIX_VIRTIO_BASE               UINT64_C(0x0001000040000000)
#define MMIX_DISCOVERABLE_BASE         UINT64_C(0x0001000050000000)

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
} MMIXRAMSizeCase;

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

static void mmix_assert_l3_devices(QTestState *qts)
{
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_LSR) &
                    MMIX_UART_LSR_THRE, ==, MMIX_UART_LSR_THRE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_FRAMEBUFFER_BASE +
                                 MMIX_FRAMEBUFFER_WIDTH), ==,
                     MMIX_FRAMEBUFFER_WIDTH_VALUE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_IPI_BASE +
                                 MMIX_IPI_ACTIVE_TARGETS), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, MMIX_INTC_BASE +
                                 MMIX_INTC_SOURCE_COUNT), ==,
                     MMIX_INTC_SOURCE_COUNT_VALUE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_INTC_BASE +
                                 MMIX_INTC_CONTEXT_COUNT), ==, 1);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
}

static void test_mmix_platform_mappings(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    unsigned int i;

    mmix_assert_mapping(mtree, MMIX_UART_BASE, MMIX_UART_SIZE, "serial");
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

    mmix_assert_l3_devices(qts);

    mmix_assert_unassigned(qts, MMIX_UART_BASE + MMIX_UART_SIZE);
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
        g_strdup_printf("-machine virt -m %s", test->memory) :
        g_strdup("-machine virt");
    QTestState *qts = qtest_init(args);
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *ram =
        g_strdup_printf("%016x-%016" PRIx64 " (prio 0, ram): mmix.ram",
                        0, test->size - 1);

    g_assert_nonnull(strstr(mtree, ram));
    mmix_assert_l3_devices(qts);
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
    mmix_assert_l3_devices(qts);

    qtest_quit(qts);
}

static void test_mmix_platform_deferred_apertures(void)
{
    static const uint64_t addresses[] = {
        UINT64_C(0x0001000000000000),
        UINT64_C(0x0001000010010000),
        UINT64_C(0x0001000010020000),
        UINT64_C(0x0001000010030000),
        UINT64_C(0x0001000010040000),
        UINT64_C(0x0001000014000000),
        UINT64_C(0x0001000018010000),
        MMIX_DISCOVERABLE_BASE,
        UINT64_C(0x0001000080000000),
        UINT64_C(0x0001000100000000),
        UINT64_C(0x0001000200000000),
        UINT64_C(0x0001010000000000),
    };
    QTestState *qts = qtest_init("-machine virt");
    size_t i;

    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        mmix_assert_unassigned(qts, addresses[i]);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const MMIXRAMSizeCase ram_sizes[] = {
        { "128M", 128 * MiB },
        { NULL, MMIX_RAM_DEFAULT_SIZE },
        { "8G", 8 * GiB },
    };

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

    return g_test_run();
}
