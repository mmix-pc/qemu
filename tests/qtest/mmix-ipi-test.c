/*
 * QTest testcase for the MMIX virt inter-processor interrupt device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MMIX_VIRT_IPI_BASE UINT64_C(0x0001000024000000)
#define MMIX_VIRT_IPI_ACTIVE_TARGETS 0x0000
#define MMIX_VIRT_IPI_SEND 0x0008
#define MMIX_VIRT_IPI_CONTEXT_BASE 0x10000
#define MMIX_VIRT_IPI_CONTEXT_STRIDE 0x10000
#define MMIX_VIRT_IPI_CONTEXT_STATUS 0x00
#define MMIX_VIRT_IPI_CONTEXT_CLEAR 0x08
#define MMIX_VIRT_IPI_STATUS_PENDING 0x01
#define MMIX_VIRT_IPI_REGISTER_SIZE 0x1000
#define MMIX_VIRT_IPI_CONTEXT_CAPACITY 1023
#define MMIX_VIRT_IPI_INITIAL_CONTEXTS 64

#define MMIX_IPI_QOM_PATH "/machine/ipi"
#define MMIX_IPI_OUTPUT_IRQ "sysbus-irq"

static uint64_t mmix_ipi_context_reg(unsigned cpu, uint64_t reg)
{
    return MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_CONTEXT_BASE +
           cpu * MMIX_VIRT_IPI_CONTEXT_STRIDE + reg;
}

static uint64_t mmix_ipi_read_active_targets(QTestState *qts)
{
    return qtest_readq(qts,
                       MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_ACTIVE_TARGETS);
}

static uint64_t mmix_ipi_read_status(QTestState *qts, unsigned cpu)
{
    return qtest_readq(qts,
                       mmix_ipi_context_reg(
                           cpu, MMIX_VIRT_IPI_CONTEXT_STATUS));
}

static void mmix_ipi_send(QTestState *qts, uint64_t targets)
{
    qtest_writeq(qts, MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_SEND, targets);
}

static void mmix_ipi_clear(QTestState *qts, unsigned cpu, uint64_t value)
{
    qtest_writeq(qts,
                 mmix_ipi_context_reg(cpu, MMIX_VIRT_IPI_CONTEXT_CLEAR),
                 value);
}

static QTestState *mmix_ipi_start(unsigned num_cpus)
{
    QTestState *qts = qtest_initf("-machine virt -smp %u", num_cpus);

    qtest_irq_intercept_out_named(qts, MMIX_IPI_QOM_PATH,
                                  MMIX_IPI_OUTPUT_IRQ);
    return qts;
}

static void test_mmix_ipi_registers(void)
{
    QTestState *qts = mmix_ipi_start(2);

    g_assert_cmphex(mmix_ipi_read_active_targets(qts), ==, 0x3);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 2), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_false(qtest_get_irq(qts, 2));

    qtest_quit(qts);
}

static void test_mmix_ipi_output_isolation(void)
{
    QTestState *qts = mmix_ipi_start(2);

    mmix_ipi_send(qts, 1U << 1);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    mmix_ipi_clear(qts, 0, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 1));
    mmix_ipi_clear(qts, 1, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 1));

    mmix_ipi_send(qts, 1U << 0);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_ipi_multitarget_lifecycle(void)
{
    QTestState *qts = mmix_ipi_start(2);

    mmix_ipi_send(qts, 0x3);
    mmix_ipi_send(qts, 0x3);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    mmix_ipi_clear(qts, 0, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    mmix_ipi_send(qts, (1U << 0) | (1U << 2));
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 2), ==, 0);

    mmix_ipi_clear(qts, 0, MMIX_VIRT_IPI_STATUS_PENDING);
    mmix_ipi_clear(qts, 1, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_ipi_coalescing_retrigger(void)
{
    QTestState *qts = mmix_ipi_start(1);

    mmix_ipi_send(qts, 1);
    mmix_ipi_send(qts, 1);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 0));

    mmix_ipi_clear(qts, 0, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    mmix_ipi_clear(qts, 0, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_ipi_send(qts, 1);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_ipi_invalid_targets(void)
{
    QTestState *qts = mmix_ipi_start(2);

    mmix_ipi_send(qts, (1U << 1) | (1U << 2) | (1ULL << 63));
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 2), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_false(qtest_get_irq(qts, 2));

    mmix_ipi_clear(qts, 2, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 1));
    mmix_ipi_send(qts, 0);
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_ipi_reset(void)
{
    QTestState *qts = mmix_ipi_start(2);

    mmix_ipi_send(qts, 0x3);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_system_reset(qts);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_ipi_target_limit(void)
{
    QTestState *qts = mmix_ipi_start(64);

    g_assert_cmphex(mmix_ipi_read_active_targets(qts), ==, UINT64_MAX);
    mmix_ipi_send(qts, UINT64_C(1) << 63);
    g_assert_cmphex(mmix_ipi_read_status(qts, 63), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 62));
    g_assert_true(qtest_get_irq(qts, 63));

    mmix_ipi_clear(qts, 63, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 63));

    qtest_quit(qts);
}

static void test_mmix_ipi_reserved_boundaries(void)
{
    QTestState *qts = mmix_ipi_start(2);
    uint64_t global_tail =
        MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_REGISTER_SIZE;
    uint64_t context0_tail =
        mmix_ipi_context_reg(0, MMIX_VIRT_IPI_REGISTER_SIZE);
    uint64_t context64 =
        mmix_ipi_context_reg(MMIX_VIRT_IPI_INITIAL_CONTEXTS, 0);
    uint64_t context1022 =
        mmix_ipi_context_reg(MMIX_VIRT_IPI_CONTEXT_CAPACITY - 1, 0);
    uint64_t context_domain_end =
        mmix_ipi_context_reg(MMIX_VIRT_IPI_CONTEXT_CAPACITY, 0);

    qtest_writeq(qts, global_tail, UINT64_MAX);
    qtest_writeq(qts, context0_tail, UINT64_MAX);
    qtest_writeq(qts, context64, UINT64_MAX);
    qtest_writeq(qts, context1022, UINT64_MAX);
    qtest_writeq(qts, context_domain_end, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, global_tail), !=, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, context0_tail), !=, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, context64), !=, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, context1022), !=, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, context_domain_end), !=, UINT64_MAX);
    g_assert_cmphex(mmix_ipi_read_status(qts, 0), ==, 0);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_ipi_invalid_access_width(void)
{
    QTestState *qts = mmix_ipi_start(2);
    uint64_t send = MMIX_VIRT_IPI_BASE + MMIX_VIRT_IPI_SEND;
    uint64_t clear =
        mmix_ipi_context_reg(1, MMIX_VIRT_IPI_CONTEXT_CLEAR);

    qtest_writeb(qts, send, 0x02);
    qtest_writel(qts, send, 0x02);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==, 0);

    mmix_ipi_send(qts, UINT64_C(1) << 1);
    qtest_writeb(qts, clear, MMIX_VIRT_IPI_STATUS_PENDING);
    qtest_writel(qts, clear, MMIX_VIRT_IPI_STATUS_PENDING);
    g_assert_cmphex(mmix_ipi_read_status(qts, 1), ==,
                    MMIX_VIRT_IPI_STATUS_PENDING);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/ipi/registers", test_mmix_ipi_registers);
    qtest_add_func("/mmix/ipi/output-isolation",
                   test_mmix_ipi_output_isolation);
    qtest_add_func("/mmix/ipi/multitarget-lifecycle",
                   test_mmix_ipi_multitarget_lifecycle);
    qtest_add_func("/mmix/ipi/coalescing-retrigger",
                   test_mmix_ipi_coalescing_retrigger);
    qtest_add_func("/mmix/ipi/invalid-targets",
                   test_mmix_ipi_invalid_targets);
    qtest_add_func("/mmix/ipi/reset", test_mmix_ipi_reset);
    qtest_add_func("/mmix/ipi/target-limit", test_mmix_ipi_target_limit);
    qtest_add_func("/mmix/ipi/reserved-boundaries",
                   test_mmix_ipi_reserved_boundaries);
    qtest_add_func("/mmix/ipi/invalid-access-width",
                   test_mmix_ipi_invalid_access_width);
    return g_test_run();
}
