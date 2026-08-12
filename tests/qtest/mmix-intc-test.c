/*
 * QTest testcase for the MMIX virt interrupt controller.
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

#define MMIX_VIRT_UART0_IRQ 1
#define MMIX_VIRT_VIRTIO_BLOCK0_IRQ 2
#define MMIX_VIRT_FRAMEBUFFER_IRQ 3
#define MMIX_VIRT_TIMER_CPU0_IRQ 16

#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ "sysbus-irq"

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

static uint32_t mmix_intc_read_enable(QTestState *qts, unsigned cpu)
{
    return qtest_readl(qts, mmix_intc_context_reg(cpu,
                                                  MMIX_VIRT_INTC_CONTEXT_ENABLE));
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

static void mmix_intc_set_irq(QTestState *qts, unsigned irq, int level)
{
    qtest_set_irq_in(qts, MMIX_INTC_QOM_PATH, "unnamed-gpio-in", irq, level);
}

static QTestState *mmix_intc_start(void)
{
    QTestState *qts = qtest_init("-machine virt");

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static void test_mmix_intc_pending_enable(void)
{
    QTestState *qts = mmix_intc_start();
    uint32_t uart_mask = mmix_intc_irq_mask(MMIX_VIRT_UART0_IRQ);

    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);
    g_assert_cmphex(mmix_intc_read_enable(qts, 0), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_set_irq(qts, MMIX_VIRT_UART0_IRQ, 1);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, uart_mask);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_write_enable(qts, 0, uart_mask);
    g_assert_cmphex(mmix_intc_read_enable(qts, 0), ==, uart_mask);
    g_assert_true(qtest_get_irq(qts, 0));

    mmix_intc_write_enable(qts, 0, 0);
    g_assert_cmphex(mmix_intc_read_enable(qts, 0), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_set_irq(qts, MMIX_VIRT_UART0_IRQ, 0);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_intc_claim_complete(void)
{
    QTestState *qts = mmix_intc_start();
    uint32_t enabled = mmix_intc_irq_mask(MMIX_VIRT_FRAMEBUFFER_IRQ) |
                       mmix_intc_irq_mask(MMIX_VIRT_TIMER_CPU0_IRQ);

    mmix_intc_write_enable(qts, 0, enabled);
    mmix_intc_set_irq(qts, MMIX_VIRT_TIMER_CPU0_IRQ, 1);
    mmix_intc_set_irq(qts, MMIX_VIRT_FRAMEBUFFER_IRQ, 1);

    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_FRAMEBUFFER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_TIMER_CPU0_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);

    mmix_intc_complete(qts, 0, MMIX_VIRT_FRAMEBUFFER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_FRAMEBUFFER_IRQ);

    mmix_intc_set_irq(qts, MMIX_VIRT_FRAMEBUFFER_IRQ, 0);
    mmix_intc_complete(qts, 0, MMIX_VIRT_FRAMEBUFFER_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_complete(qts, 0, MMIX_VIRT_TIMER_CPU0_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    mmix_intc_set_irq(qts, MMIX_VIRT_TIMER_CPU0_IRQ, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_intc_unsupported_contexts(void)
{
    QTestState *qts = mmix_intc_start();
    uint32_t virtio_mask = mmix_intc_irq_mask(MMIX_VIRT_VIRTIO_BLOCK0_IRQ);

    mmix_intc_write_enable(qts, 1, virtio_mask);
    g_assert_cmphex(mmix_intc_read_enable(qts, 1), ==, 0);

    mmix_intc_set_irq(qts, MMIX_VIRT_VIRTIO_BLOCK0_IRQ, 1);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, virtio_mask);
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_write_enable(qts, 0, virtio_mask);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_VIRTIO_BLOCK0_IRQ);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/intc/pending-enable",
                   test_mmix_intc_pending_enable);
    qtest_add_func("/mmix/intc/claim-complete",
                   test_mmix_intc_claim_complete);
    qtest_add_func("/mmix/intc/unsupported-contexts",
                   test_mmix_intc_unsupported_contexts);

    return g_test_run();
}
