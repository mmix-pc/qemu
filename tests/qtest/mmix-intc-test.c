/*
 * QTest testcase for the MMIX virt interrupt controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MMIX_VIRT_UART0_BASE 0x10000000ULL
#define MMIX_UART_RBR 0x00
#define MMIX_UART_IER 0x01
#define MMIX_UART_IIR 0x02
#define MMIX_UART_LSR 0x05
#define MMIX_UART_IER_RDI 0x01
#define MMIX_UART_IER_THRI 0x02
#define MMIX_UART_IIR_RDI 0x04
#define MMIX_UART_IIR_ID 0x06
#define MMIX_UART_IIR_THRI 0x02
#define MMIX_UART_LSR_DR 0x01

#define MMIX_VIRT_INTC_BASE 0x10004000ULL
#define MMIX_VIRT_INTC_PENDING 0x0000
#define MMIX_VIRT_INTC_CONTEXT_BASE 0x1000
#define MMIX_VIRT_INTC_CONTEXT_STRIDE 0x100
#define MMIX_VIRT_INTC_CONTEXT_ENABLE 0x00
#define MMIX_VIRT_INTC_CONTEXT_CLAIM 0x04
#define MMIX_VIRT_INTC_CONTEXT_COMPLETE 0x08
#define MMIX_VIRT_INTC_CONTEXT_COUNT 16
#define MMIX_VIRT_INTC_SIZE \
    (MMIX_VIRT_INTC_CONTEXT_BASE + \
     MMIX_VIRT_INTC_CONTEXT_COUNT * MMIX_VIRT_INTC_CONTEXT_STRIDE)

#define MMIX_VIRT_UART0_IRQ 1
#define MMIX_VIRT_VIRTIO_BLOCK0_IRQ 2
#define MMIX_VIRT_FRAMEBUFFER_IRQ 3
#define MMIX_VIRT_TEST_SYNTHETIC_IRQ 4
#define MMIX_VIRT_TIMER_IRQ_BASE 16

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
    return qtest_readl(
        qts, mmix_intc_context_reg(cpu, MMIX_VIRT_INTC_CONTEXT_ENABLE));
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

static void mmix_intc_intercept_output(QTestState *qts)
{
    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
}

static QTestState *mmix_intc_start(void)
{
    QTestState *qts = qtest_init("-machine virt");

    mmix_intc_intercept_output(qts);
    return qts;
}

static QTestState *mmix_intc_start_with_contexts(unsigned int num_cpus)
{
    QTestState *qts = qtest_initf(
        "-machine virt -smp %u -global mmix-intc.num-cpus=%u",
        num_cpus, num_cpus);

    mmix_intc_intercept_output(qts);
    return qts;
}

static QTestState *mmix_intc_start_with_serial(int *serial_fd)
{
    QTestState *qts = qtest_init_with_serial("-machine virt", serial_fd);

    mmix_intc_intercept_output(qts);
    return qts;
}

static void mmix_uart_wait_for_data(QTestState *qts)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (!(qtest_readb(qts, MMIX_VIRT_UART0_BASE + MMIX_UART_LSR) &
             MMIX_UART_LSR_DR)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
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

static void test_mmix_intc_uart_irq(void)
{
    QTestState *qts = mmix_intc_start();
    uint32_t uart_mask = mmix_intc_irq_mask(MMIX_VIRT_UART0_IRQ);

    mmix_intc_write_enable(qts, 0, uart_mask);
    qtest_writeb(qts, MMIX_VIRT_UART0_BASE + MMIX_UART_IER,
                 MMIX_UART_IER_THRI);

    g_assert_cmphex(mmix_intc_read_pending(qts), ==, uart_mask);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_UART0_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    g_assert_cmphex(qtest_readb(qts, MMIX_VIRT_UART0_BASE + MMIX_UART_IIR) &
                    MMIX_UART_IIR_ID, ==, MMIX_UART_IIR_THRI);
    mmix_intc_complete(qts, 0, MMIX_VIRT_UART0_IRQ);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_intc_uart_rx_irq(void)
{
    const uint8_t input = 0xa5;
    QTestState *qts;
    uint32_t uart_mask = mmix_intc_irq_mask(MMIX_VIRT_UART0_IRQ);
    int serial_fd;

    qts = mmix_intc_start_with_serial(&serial_fd);
    mmix_intc_write_enable(qts, 0, uart_mask);
    qtest_writeb(qts, MMIX_VIRT_UART0_BASE + MMIX_UART_IER,
                 MMIX_UART_IER_RDI);

    g_assert_cmpint(qemu_write_full(serial_fd, &input, sizeof(input)), ==,
                    sizeof(input));
    mmix_uart_wait_for_data(qts);

    g_assert_cmphex(mmix_intc_read_pending(qts), ==, uart_mask);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readb(qts, MMIX_VIRT_UART0_BASE + MMIX_UART_IIR) &
                    MMIX_UART_IIR_ID, ==, MMIX_UART_IIR_RDI);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_UART0_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    g_assert_cmphex(qtest_readb(qts, MMIX_VIRT_UART0_BASE + MMIX_UART_RBR),
                    ==, input);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);
    mmix_intc_complete(qts, 0, MMIX_VIRT_UART0_IRQ);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    close(serial_fd);
    qtest_quit(qts);
}

static void test_mmix_intc_claim_complete(void)
{
    QTestState *qts = mmix_intc_start();
    uint32_t enabled = mmix_intc_irq_mask(MMIX_VIRT_FRAMEBUFFER_IRQ) |
                       mmix_intc_irq_mask(MMIX_VIRT_TEST_SYNTHETIC_IRQ);

    mmix_intc_write_enable(qts, 0, enabled);
    mmix_intc_set_irq(qts, MMIX_VIRT_TEST_SYNTHETIC_IRQ, 1);
    mmix_intc_set_irq(qts, MMIX_VIRT_FRAMEBUFFER_IRQ, 1);

    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_FRAMEBUFFER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_TEST_SYNTHETIC_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);

    mmix_intc_complete(qts, 0, MMIX_VIRT_FRAMEBUFFER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_FRAMEBUFFER_IRQ);

    mmix_intc_set_irq(qts, MMIX_VIRT_FRAMEBUFFER_IRQ, 0);
    mmix_intc_complete(qts, 0, MMIX_VIRT_FRAMEBUFFER_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_complete(qts, 0, MMIX_VIRT_TEST_SYNTHETIC_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    mmix_intc_set_irq(qts, MMIX_VIRT_TEST_SYNTHETIC_IRQ, 0);
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

static void test_mmix_intc_active_contexts(void)
{
    QTestState *qts = mmix_intc_start_with_contexts(2);
    uint32_t uart_mask = mmix_intc_irq_mask(MMIX_VIRT_UART0_IRQ);
    uint32_t virtio_mask = mmix_intc_irq_mask(MMIX_VIRT_VIRTIO_BLOCK0_IRQ);

    mmix_intc_write_enable(qts, 0, uart_mask);
    mmix_intc_write_enable(qts, 1, virtio_mask);

    g_assert_cmphex(mmix_intc_read_enable(qts, 0), ==, uart_mask);
    g_assert_cmphex(mmix_intc_read_enable(qts, 1), ==, virtio_mask);

    mmix_intc_write_enable(qts, 2, UINT32_MAX);
    g_assert_cmphex(mmix_intc_read_enable(qts, 2), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_intc_context_limit(void)
{
    QTestState *qts = mmix_intc_start_with_contexts(
        MMIX_VIRT_INTC_CONTEXT_COUNT);
    uint64_t out_of_range = MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_SIZE;
    uint32_t valid_mask = UINT32_MAX & ~1U;

    mmix_intc_write_enable(qts, MMIX_VIRT_INTC_CONTEXT_COUNT - 1,
                           UINT32_MAX);
    g_assert_cmphex(
        mmix_intc_read_enable(qts, MMIX_VIRT_INTC_CONTEXT_COUNT - 1),
        ==, valid_mask);

    qtest_writel(qts, out_of_range, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, out_of_range), ==, 0);
    g_assert_cmphex(
        mmix_intc_read_enable(qts, MMIX_VIRT_INTC_CONTEXT_COUNT - 1),
        ==, valid_mask);

    qtest_quit(qts);
}

static void test_mmix_intc_fixed_affinity(void)
{
    QTestState *qts = mmix_intc_start_with_contexts(2);
    uint32_t timer0_irq = MMIX_VIRT_TIMER_IRQ_BASE;
    uint32_t timer1_irq = MMIX_VIRT_TIMER_IRQ_BASE + 1;
    uint32_t timer0_mask = mmix_intc_irq_mask(timer0_irq);
    uint32_t timer1_mask = mmix_intc_irq_mask(timer1_irq);
    uint32_t enabled = timer0_mask | timer1_mask;

    mmix_intc_write_enable(qts, 0, enabled);
    mmix_intc_write_enable(qts, 1, enabled);

    mmix_intc_set_irq(qts, timer0_irq, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, timer0_irq);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_complete(qts, 1, timer0_irq);
    g_assert_false(qtest_get_irq(qts, 0));
    mmix_intc_complete(qts, 0, timer0_irq);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    mmix_intc_set_irq(qts, timer0_irq, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_set_irq(qts, timer1_irq, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, timer1_irq);
    g_assert_false(qtest_get_irq(qts, 1));

    mmix_intc_complete(qts, 0, timer1_irq);
    g_assert_false(qtest_get_irq(qts, 1));
    mmix_intc_set_irq(qts, timer1_irq, 0);
    mmix_intc_complete(qts, 1, timer1_irq);
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_intc_simultaneous_fixed_irqs(void)
{
    QTestState *qts = mmix_intc_start_with_contexts(2);
    uint32_t timer0_irq = MMIX_VIRT_TIMER_IRQ_BASE;
    uint32_t timer1_irq = MMIX_VIRT_TIMER_IRQ_BASE + 1;

    mmix_intc_write_enable(qts, 0, mmix_intc_irq_mask(timer0_irq));
    mmix_intc_write_enable(qts, 1, mmix_intc_irq_mask(timer1_irq));
    mmix_intc_set_irq(qts, timer0_irq, 1);
    mmix_intc_set_irq(qts, timer1_irq, 1);

    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_false(qtest_get_irq(qts, 2));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, timer0_irq);
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, timer1_irq);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    mmix_intc_complete(qts, 0, timer0_irq);
    mmix_intc_complete(qts, 1, timer1_irq);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_system_reset(qts);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);
    g_assert_cmphex(mmix_intc_read_enable(qts, 0), ==, 0);
    g_assert_cmphex(mmix_intc_read_enable(qts, 1), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_false(qtest_get_irq(qts, MMIX_VIRT_INTC_CONTEXT_COUNT - 1));

    qtest_quit(qts);
}

static void test_mmix_intc_shared_irq_cpu0_only(void)
{
    QTestState *qts = mmix_intc_start_with_contexts(2);
    uint32_t uart_mask = mmix_intc_irq_mask(MMIX_VIRT_UART0_IRQ);

    mmix_intc_write_enable(qts, 0, uart_mask);
    mmix_intc_write_enable(qts, 1, uart_mask);
    mmix_intc_set_irq(qts, MMIX_VIRT_UART0_IRQ, 1);

    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_VIRT_UART0_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_set_irq(qts, MMIX_VIRT_UART0_IRQ, 0);
    mmix_intc_complete(qts, 0, MMIX_VIRT_UART0_IRQ);
    qtest_quit(qts);
}

static void test_mmix_intc_invalid_context_count(gconstpointer opaque)
{
    const char *num_cpus = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *property = g_strdup_printf(
        "mmix-intc.num-cpus=%s", num_cpus);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/intc/pending-enable",
                   test_mmix_intc_pending_enable);
    qtest_add_func("/mmix/intc/uart-irq", test_mmix_intc_uart_irq);
    qtest_add_func("/mmix/intc/uart-rx-irq", test_mmix_intc_uart_rx_irq);
    qtest_add_func("/mmix/intc/claim-complete",
                   test_mmix_intc_claim_complete);
    qtest_add_func("/mmix/intc/unsupported-contexts",
                   test_mmix_intc_unsupported_contexts);
    qtest_add_func("/mmix/intc/active-contexts",
                   test_mmix_intc_active_contexts);
    qtest_add_func("/mmix/intc/context-limit",
                   test_mmix_intc_context_limit);
    qtest_add_func("/mmix/intc/fixed-affinity",
                   test_mmix_intc_fixed_affinity);
    qtest_add_func("/mmix/intc/simultaneous-fixed-irqs",
                   test_mmix_intc_simultaneous_fixed_irqs);
    qtest_add_func("/mmix/intc/shared-irq-cpu0-only",
                   test_mmix_intc_shared_irq_cpu0_only);
    qtest_add_data_func("/mmix/intc/invalid-context-count/zero", "0",
                        test_mmix_intc_invalid_context_count);
    qtest_add_data_func("/mmix/intc/invalid-context-count/above-maximum", "17",
                        test_mmix_intc_invalid_context_count);

    return g_test_run();
}
