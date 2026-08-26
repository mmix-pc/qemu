/*
 * QTest testcase for the MMIX virt timer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MMIX_VIRT_INTC_BASE 0x10004000ULL
#define MMIX_VIRT_INTC_PENDING 0x0000
#define MMIX_VIRT_INTC_CONTEXT_BASE 0x1000
#define MMIX_VIRT_INTC_CONTEXT_STRIDE 0x100
#define MMIX_VIRT_INTC_CONTEXT_ENABLE 0x00
#define MMIX_VIRT_INTC_CONTEXT_CLAIM 0x04
#define MMIX_VIRT_INTC_CONTEXT_COMPLETE 0x08

#define MMIX_VIRT_TIMER_BASE 0x10003000ULL
#define MMIX_VIRT_TIMER_TIME 0x0000
#define MMIX_VIRT_TIMER_CONTEXT_BASE 0x0100
#define MMIX_VIRT_TIMER_CONTEXT_STRIDE 0x40
#define MMIX_VIRT_TIMER_CONTEXT_COMPARE 0x00
#define MMIX_VIRT_TIMER_CONTEXT_CONTROL 0x08
#define MMIX_VIRT_TIMER_CONTEXT_STATUS 0x10
#define MMIX_VIRT_TIMER_CONTROL_ENABLE 0x01
#define MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE 0x02
#define MMIX_VIRT_TIMER_STATUS_PENDING 0x01

#define MMIX_VIRT_TIMER_CPU0_IRQ 16

#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_TIMER_QOM_PATH "/machine/timer"
#define MMIX_INTC_OUTPUT_IRQ "sysbus-irq"
#define MMIX_TIMER_OUTPUT_IRQ "sysbus-irq"

static uint64_t mmix_intc_context_reg(unsigned cpu, uint64_t reg)
{
    return MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_CONTEXT_BASE +
           cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE + reg;
}

static uint32_t mmix_intc_irq_mask(unsigned irq)
{
    return 1U << irq;
}

static uint32_t mmix_intc_read_pending(QTestState *qts)
{
    return qtest_readl(qts, MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_PENDING);
}

static uint32_t mmix_intc_claim(QTestState *qts, unsigned cpu)
{
    return qtest_readl(qts, mmix_intc_context_reg(cpu,
                                                 MMIX_VIRT_INTC_CONTEXT_CLAIM));
}

static void mmix_intc_write_enable(QTestState *qts, unsigned cpu,
                                   uint32_t value)
{
    qtest_writel(qts, mmix_intc_context_reg(cpu,
                                            MMIX_VIRT_INTC_CONTEXT_ENABLE),
                 value);
}

static void mmix_intc_complete(QTestState *qts, unsigned cpu, uint32_t irq)
{
    qtest_writel(qts, mmix_intc_context_reg(cpu,
                                            MMIX_VIRT_INTC_CONTEXT_COMPLETE),
                 irq);
}

static uint64_t mmix_timer_context_reg(unsigned cpu, uint64_t reg)
{
    return MMIX_VIRT_TIMER_BASE + MMIX_VIRT_TIMER_CONTEXT_BASE +
           cpu * MMIX_VIRT_TIMER_CONTEXT_STRIDE + reg;
}

static uint64_t mmix_timer_read_time(QTestState *qts)
{
    return qtest_readq(qts, MMIX_VIRT_TIMER_BASE + MMIX_VIRT_TIMER_TIME);
}

static uint64_t mmix_timer_read_compare(QTestState *qts, unsigned cpu)
{
    return qtest_readq(qts,
                       mmix_timer_context_reg(
                           cpu, MMIX_VIRT_TIMER_CONTEXT_COMPARE));
}

static uint64_t mmix_timer_read_control(QTestState *qts, unsigned cpu)
{
    return qtest_readq(qts,
                       mmix_timer_context_reg(
                           cpu, MMIX_VIRT_TIMER_CONTEXT_CONTROL));
}

static uint64_t mmix_timer_read_status(QTestState *qts, unsigned cpu)
{
    return qtest_readq(qts,
                       mmix_timer_context_reg(
                           cpu, MMIX_VIRT_TIMER_CONTEXT_STATUS));
}

static void mmix_timer_write_compare(QTestState *qts, unsigned cpu,
                                     uint64_t value)
{
    qtest_writeq(qts, mmix_timer_context_reg(cpu,
                                             MMIX_VIRT_TIMER_CONTEXT_COMPARE),
                 value);
}

static void mmix_timer_write_control(QTestState *qts, unsigned cpu,
                                     uint64_t value)
{
    qtest_writeq(qts, mmix_timer_context_reg(cpu,
                                             MMIX_VIRT_TIMER_CONTEXT_CONTROL),
                 value);
}

static void mmix_timer_write_status(QTestState *qts, unsigned cpu,
                                    uint64_t value)
{
    qtest_writeq(qts, mmix_timer_context_reg(cpu,
                                             MMIX_VIRT_TIMER_CONTEXT_STATUS),
                 value);
}

static QTestState *mmix_timer_start(void)
{
    QTestState *qts = qtest_init("-machine virt");

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static QTestState *mmix_timer_start_contexts(unsigned num_cpus)
{
    QTestState *qts = qtest_initf(
        "-machine virt -global mmix-timer.num-cpus=%u", num_cpus);

    qtest_irq_intercept_out_named(qts, MMIX_TIMER_QOM_PATH,
                                  MMIX_TIMER_OUTPUT_IRQ);
    return qts;
}

static void test_mmix_timer_time(void)
{
    QTestState *qts = mmix_timer_start();
    uint64_t before = mmix_timer_read_time(qts);

    qtest_clock_step(qts, 100);

    g_assert_cmpuint(mmix_timer_read_time(qts), >=, before + 100);

    qtest_quit(qts);
}

static void test_mmix_timer_registers(void)
{
    QTestState *qts = mmix_timer_start();

    g_assert_cmpuint(mmix_timer_read_compare(qts, 0), ==, 0);
    g_assert_cmpuint(mmix_timer_read_control(qts, 0), ==, 0);
    g_assert_cmpuint(mmix_timer_read_status(qts, 0), ==, 0);

    mmix_timer_write_compare(qts, 0, 0x123456789abcdef0ULL);
    g_assert_cmphex(mmix_timer_read_compare(qts, 0), ==,
                    0x123456789abcdef0ULL);

    mmix_timer_write_control(qts, 0, UINT64_MAX);
    g_assert_cmphex(mmix_timer_read_control(qts, 0), ==,
                    MMIX_VIRT_TIMER_CONTROL_ENABLE |
                    MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE);

    qtest_quit(qts);
}

static void test_mmix_timer_pending_w1c(void)
{
    QTestState *qts = mmix_timer_start();
    uint64_t now = mmix_timer_read_time(qts);

    mmix_timer_write_compare(qts, 0, now + 10);
    mmix_timer_write_control(qts, 0, MMIX_VIRT_TIMER_CONTROL_ENABLE);
    qtest_clock_step(qts, 10);

    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);

    mmix_timer_write_compare(qts, 0, mmix_timer_read_time(qts) + 10);
    mmix_timer_write_status(qts, 0, MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_timer_irq(void)
{
    QTestState *qts = mmix_timer_start();
    uint32_t timer_mask = mmix_intc_irq_mask(MMIX_VIRT_TIMER_CPU0_IRQ);
    uint64_t now = mmix_timer_read_time(qts);

    mmix_intc_write_enable(qts, 0, timer_mask);
    mmix_timer_write_compare(qts, 0, now + 10);
    mmix_timer_write_control(qts, 0,
                             MMIX_VIRT_TIMER_CONTROL_ENABLE |
                             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 10);

    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, timer_mask);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_TIMER_CPU0_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_timer_write_compare(qts, 0, mmix_timer_read_time(qts) + 10);
    mmix_timer_write_status(qts, 0, MMIX_VIRT_TIMER_STATUS_PENDING);
    mmix_intc_complete(qts, 0, MMIX_VIRT_TIMER_CPU0_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_timer_unsupported_contexts(void)
{
    QTestState *qts = mmix_timer_start();

    mmix_timer_write_compare(qts, 1, 0x1234);
    mmix_timer_write_control(qts, 1,
                             MMIX_VIRT_TIMER_CONTROL_ENABLE |
                             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE);
    mmix_timer_write_status(qts, 1, MMIX_VIRT_TIMER_STATUS_PENDING);

    g_assert_cmpuint(mmix_timer_read_compare(qts, 1), ==, 0);
    g_assert_cmpuint(mmix_timer_read_control(qts, 1), ==, 0);
    g_assert_cmpuint(mmix_timer_read_status(qts, 1), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_timer_context_property_rejected(gconstpointer opaque)
{
    unsigned num_cpus = GPOINTER_TO_UINT(opaque);
    g_autoptr(GError) error = NULL;
    g_autofree char *property =
        g_strdup_printf("mmix-timer.num-cpus=%u", num_cpus);
    g_autofree char *stderr_text = NULL;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "virt", "-global", property,
        "-display", "none", "-monitor", "none", "-serial", "none", NULL,
    };
    int wait_status;

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text,
                           "num-cpus must be between 1 and 16"));
}

static void test_mmix_timer_active_contexts(void)
{
    QTestState *qts = mmix_timer_start_contexts(2);

    mmix_timer_write_compare(qts, 0, 0x1111);
    mmix_timer_write_compare(qts, 1, 0x2222);
    mmix_timer_write_compare(qts, 2, 0x3333);
    mmix_timer_write_control(qts, 1, UINT64_MAX);

    g_assert_cmphex(mmix_timer_read_compare(qts, 0), ==, 0x1111);
    g_assert_cmphex(mmix_timer_read_compare(qts, 1), ==, 0x2222);
    g_assert_cmphex(mmix_timer_read_compare(qts, 2), ==, 0);
    g_assert_cmphex(mmix_timer_read_control(qts, 1), ==,
                    MMIX_VIRT_TIMER_CONTROL_ENABLE |
                    MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE);
    g_assert_cmphex(mmix_timer_read_control(qts, 2), ==, 0);
    g_assert_cmphex(mmix_timer_read_status(qts, 2), ==, 0);
    g_assert_false(qtest_get_irq(qts, 2));

    qtest_quit(qts);
}

static void test_mmix_timer_context_deadlines(void)
{
    QTestState *qts = mmix_timer_start_contexts(2);
    uint64_t now = mmix_timer_read_time(qts);
    uint64_t enabled = MMIX_VIRT_TIMER_CONTROL_ENABLE |
                       MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE;

    mmix_timer_write_compare(qts, 0, now + 10);
    mmix_timer_write_compare(qts, 1, now + 20);
    mmix_timer_write_control(qts, 0, enabled);
    mmix_timer_write_control(qts, 1, enabled);

    qtest_clock_step(qts, 10);
    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_cmphex(mmix_timer_read_status(qts, 1), ==, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_clock_step(qts, 10);
    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_cmphex(mmix_timer_read_status(qts, 1), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    mmix_timer_write_control(qts, 1, MMIX_VIRT_TIMER_CONTROL_ENABLE);
    g_assert_cmphex(mmix_timer_read_status(qts, 1), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_false(qtest_get_irq(qts, 1));
    mmix_timer_write_control(qts, 1, enabled);
    g_assert_true(qtest_get_irq(qts, 1));

    now = mmix_timer_read_time(qts);
    mmix_timer_write_compare(qts, 0, now + 10);
    mmix_timer_write_status(qts, 0, MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(mmix_timer_read_status(qts, 1), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_timer_simultaneous_expiry(void)
{
    QTestState *qts = mmix_timer_start_contexts(2);
    uint64_t now = mmix_timer_read_time(qts);
    uint64_t enabled = MMIX_VIRT_TIMER_CONTROL_ENABLE |
                       MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE;

    mmix_timer_write_compare(qts, 0, now + 10);
    mmix_timer_write_compare(qts, 1, now + 10);
    mmix_timer_write_control(qts, 0, enabled);
    mmix_timer_write_control(qts, 1, enabled);
    qtest_clock_step(qts, 10);

    g_assert_cmphex(mmix_timer_read_status(qts, 0), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_cmphex(mmix_timer_read_status(qts, 1), ==,
                    MMIX_VIRT_TIMER_STATUS_PENDING);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/timer/time", test_mmix_timer_time);
    qtest_add_func("/mmix/timer/registers", test_mmix_timer_registers);
    qtest_add_func("/mmix/timer/pending-w1c", test_mmix_timer_pending_w1c);
    qtest_add_func("/mmix/timer/irq", test_mmix_timer_irq);
    qtest_add_func("/mmix/timer/unsupported-contexts",
                   test_mmix_timer_unsupported_contexts);
    qtest_add_data_func("/mmix/timer/property/zero", GUINT_TO_POINTER(0),
                        test_mmix_timer_context_property_rejected);
    qtest_add_data_func("/mmix/timer/property/too-many", GUINT_TO_POINTER(17),
                        test_mmix_timer_context_property_rejected);
    qtest_add_func("/mmix/timer/active-contexts",
                   test_mmix_timer_active_contexts);
    qtest_add_func("/mmix/timer/context-deadlines",
                   test_mmix_timer_context_deadlines);
    qtest_add_func("/mmix/timer/simultaneous-expiry",
                   test_mmix_timer_simultaneous_expiry);

    return g_test_run();
}
