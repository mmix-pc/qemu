/*
 * QTest testcase for the MMIX virt interrupt controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/units.h"
#include "qobject/qdict.h"

#define MMIX_UART_BASE               UINT64_C(0x0001000010000000)
#define MMIX_UART_REGISTER_SIZE      UINT64_C(0x8)
#define MMIX_UART_RESERVATION_SIZE   UINT64_C(0x10000)
#define MMIX_UART_RETIRED_LOW_BASE   UINT64_C(0x10000000)
#define MMIX_UART_RBR                0x00
#define MMIX_UART_THR                0x00
#define MMIX_UART_IER                0x01
#define MMIX_UART_IIR                0x02
#define MMIX_UART_LSR                0x05
#define MMIX_UART_IER_RDI            0x01
#define MMIX_UART_IER_THRI           0x02
#define MMIX_UART_IIR_RDI            0x04
#define MMIX_UART_IIR_ID             0x06
#define MMIX_UART_IIR_THRI           0x02
#define MMIX_UART_LSR_DR             0x01

#define MMIX_INTC_BASE               UINT64_C(0x0001000030000000)
#define MMIX_INTC_SOURCE_COUNT       0x0000
#define MMIX_INTC_CONTEXT_COUNT      0x0008
#define MMIX_INTC_PENDING_BASE       0x1000
#define MMIX_INTC_CONTEXT_BASE       UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_STRIDE     UINT64_C(0x10000)
#define MMIX_INTC_ENABLE_BASE        0x0000
#define MMIX_INTC_CLAIM              0x0800
#define MMIX_INTC_COMPLETE           0x0808
#define MMIX_INTC_SOURCE_COUNT_VALUE 8192

#define MMIX_IRQ_UART                1
#define MMIX_IRQ_RTC                 2
#define MMIX_IRQ_WATCHDOG            3
#define MMIX_IRQ_TIMER_BASE          16
#define MMIX_IRQ_VIRTIO_BASE         2048

#define MMIX_INTC_QOM_PATH           "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ         "sysbus-irq"

#define MMIX_INITIAL_STACK_SIZE      UINT64_C(0x8000)

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
                           mmix_intc_context_reg(cpu,
                                                MMIX_INTC_ENABLE_BASE),
                           source));
}

static void mmix_intc_write_enable(QTestState *qts, unsigned int cpu,
                                   unsigned int source, uint64_t value)
{
    qtest_writeq(qts, mmix_intc_word_reg(
                     mmix_intc_context_reg(cpu, MMIX_INTC_ENABLE_BASE),
                     source), value);
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

static void mmix_intc_set_irq(QTestState *qts, unsigned int source, int level)
{
    qtest_set_irq_in(qts, MMIX_INTC_QOM_PATH, "unnamed-gpio-in",
                     source, level);
}

static QTestState *mmix_intc_start(unsigned int cpus)
{
    QTestState *qts = qtest_initf("-machine virt -smp %u", cpus);

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static QTestState *mmix_intc_start_with_serial(unsigned int cpus,
                                               int *serial_fd)
{
    g_autofree char *args = g_strdup_printf("-machine virt -smp %u", cpus);
    QTestState *qts = qtest_init_with_serial(args, serial_fd);

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static void mmix_uart_wait_for_data(QTestState *qts)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (!(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_LSR) &
             MMIX_UART_LSR_DR)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_mmix_uart_mapping_and_tx(void)
{
    const uint8_t output = 0xa5;
    uint8_t received;
    int serial_fd;
    QTestState *qts = mmix_intc_start_with_serial(1, &serial_fd);

    qtest_writeb(qts, MMIX_UART_BASE + MMIX_UART_THR, output);
    g_assert_cmpint(recv(serial_fd, &received, sizeof(received), 0), ==,
                    sizeof(received));
    g_assert_cmphex(received, ==, output);

    qtest_writeb(qts, MMIX_UART_BASE + MMIX_UART_REGISTER_SIZE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE +
                                MMIX_UART_REGISTER_SIZE), !=, 0x5a);
    qtest_writeb(qts, MMIX_UART_BASE +
                      MMIX_UART_RESERVATION_SIZE - 1, 0x5a);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE +
                                MMIX_UART_RESERVATION_SIZE - 1), !=, 0x5a);

    qtest_writeb(qts, MMIX_UART_RETIRED_LOW_BASE + MMIX_UART_IER,
                 MMIX_UART_IER_THRI);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_RETIRED_LOW_BASE +
                                MMIX_UART_IER), ==, MMIX_UART_IER_THRI);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_IER), ==, 0);
    g_assert_cmphex(mmix_intc_pending(qts, MMIX_IRQ_UART), ==, 0);

    close(serial_fd);
    qtest_quit(qts);
}

static void test_mmix_uart_rx_mask_and_deassert(void)
{
    const uint8_t input = 0x5a;
    uint64_t bit = mmix_intc_source_bit(MMIX_IRQ_UART);
    int serial_fd;
    QTestState *qts = mmix_intc_start_with_serial(1, &serial_fd);

    qtest_writeb(qts, MMIX_UART_BASE + MMIX_UART_IER, MMIX_UART_IER_RDI);
    g_assert_cmpint(qemu_write_full(serial_fd, &input, sizeof(input)), ==,
                    sizeof(input));
    mmix_uart_wait_for_data(qts);

    g_assert_cmphex(mmix_intc_pending(qts, MMIX_IRQ_UART) & bit, ==, bit);
    g_assert_false(qtest_get_irq(qts, 0));
    mmix_intc_write_enable(qts, 0, MMIX_IRQ_UART, bit);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_IIR) &
                    MMIX_UART_IIR_ID, ==, MMIX_UART_IIR_RDI);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_UART);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_RBR), ==,
                    input);
    g_assert_cmphex(mmix_intc_pending(qts, MMIX_IRQ_UART) & bit, ==, 0);
    mmix_intc_complete(qts, 0, MMIX_IRQ_UART);
    g_assert_false(qtest_get_irq(qts, 0));

    close(serial_fd);
    qtest_quit(qts);
}

static void test_mmix_uart_shared_claim_and_retrigger(void)
{
    uint64_t bit = mmix_intc_source_bit(MMIX_IRQ_UART);
    QTestState *qts = mmix_intc_start(2);

    mmix_intc_write_enable(qts, 0, MMIX_IRQ_UART, bit);
    mmix_intc_write_enable(qts, 1, MMIX_IRQ_UART, bit);
    qtest_writeb(qts, MMIX_UART_BASE + MMIX_UART_IER,
                 MMIX_UART_IER_THRI);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, MMIX_IRQ_UART);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);

    mmix_intc_complete(qts, 1, MMIX_IRQ_UART);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_UART);
    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_IIR) &
                    MMIX_UART_IIR_ID, ==, MMIX_UART_IIR_THRI);
    mmix_intc_complete(qts, 0, MMIX_IRQ_UART);
    g_assert_cmphex(mmix_intc_pending(qts, MMIX_IRQ_UART) & bit, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_intc_configuration(void)
{
    QTestState *qts = mmix_intc_start(2);

    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE +
                                MMIX_INTC_SOURCE_COUNT), ==,
                    MMIX_INTC_SOURCE_COUNT_VALUE);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE +
                                MMIX_INTC_CONTEXT_COUNT), ==, 2);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE + 0x10), ==, 0);
    qtest_writeq(qts, MMIX_INTC_BASE + MMIX_INTC_SOURCE_COUNT, 1);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE +
                                MMIX_INTC_SOURCE_COUNT), ==,
                    MMIX_INTC_SOURCE_COUNT_VALUE);

    qtest_quit(qts);
}

static void test_mmix_intc_source_namespace(void)
{
    static const unsigned int active[] = {
        MMIX_IRQ_UART, MMIX_IRQ_RTC, MMIX_IRQ_WATCHDOG,
        MMIX_IRQ_TIMER_BASE, 63, 64,
        MMIX_IRQ_TIMER_BASE + 63,
        MMIX_IRQ_VIRTIO_BASE, MMIX_IRQ_VIRTIO_BASE + 31,
    };
    static const unsigned int reserved[] = {
        0, 15, 80, 1024, MMIX_IRQ_VIRTIO_BASE - 1,
        MMIX_IRQ_VIRTIO_BASE + 32, 6144, 8191,
    };
    QTestState *qts = mmix_intc_start(64);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(active); i++) {
        unsigned int source = active[i];
        uint64_t bit = mmix_intc_source_bit(source);

        mmix_intc_set_irq(qts, source, 1);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
        mmix_intc_set_irq(qts, source, 0);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    }
    for (i = 0; i < ARRAY_SIZE(reserved); i++) {
        unsigned int source = reserved[i];
        uint64_t bit = mmix_intc_source_bit(source);

        mmix_intc_write_enable(qts, 0, source, bit);
        mmix_intc_set_irq(qts, source, 1);
        g_assert_cmphex(mmix_intc_enable(qts, 0, source) & bit, ==, 0);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    }

    qtest_quit(qts);
}

static void test_mmix_intc_claim_complete_retrigger(void)
{
    QTestState *qts = mmix_intc_start(1);
    uint64_t bit = mmix_intc_source_bit(MMIX_IRQ_UART);

    mmix_intc_set_irq(qts, MMIX_IRQ_UART, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    mmix_intc_write_enable(qts, 0, MMIX_IRQ_UART, bit);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_UART);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_intc_complete(qts, 0, MMIX_IRQ_UART);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_UART);
    mmix_intc_set_irq(qts, MMIX_IRQ_UART, 0);
    mmix_intc_complete(qts, 0, MMIX_IRQ_UART);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_intc_lowest_source(void)
{
    QTestState *qts = mmix_intc_start(1);

    mmix_intc_write_enable(qts, 0, MMIX_IRQ_UART,
                           mmix_intc_source_bit(MMIX_IRQ_UART));
    mmix_intc_write_enable(qts, 0, MMIX_IRQ_VIRTIO_BASE,
                           mmix_intc_source_bit(MMIX_IRQ_VIRTIO_BASE));
    mmix_intc_set_irq(qts, MMIX_IRQ_VIRTIO_BASE, 1);
    mmix_intc_set_irq(qts, MMIX_IRQ_UART, 1);

    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_UART);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_VIRTIO_BASE);

    qtest_quit(qts);
}

static void test_mmix_intc_shared_source_ownership(void)
{
    QTestState *qts = mmix_intc_start(2);
    uint64_t bit = mmix_intc_source_bit(MMIX_IRQ_UART);

    mmix_intc_write_enable(qts, 0, MMIX_IRQ_UART, bit);
    mmix_intc_write_enable(qts, 1, MMIX_IRQ_UART, bit);
    mmix_intc_set_irq(qts, MMIX_IRQ_UART, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, MMIX_IRQ_UART);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);

    mmix_intc_complete(qts, 0, MMIX_IRQ_UART);
    g_assert_false(qtest_get_irq(qts, 0));
    mmix_intc_complete(qts, 1, MMIX_IRQ_UART);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_mmix_intc_timer_affinity(void)
{
    QTestState *qts = mmix_intc_start(2);
    unsigned int timer0 = MMIX_IRQ_TIMER_BASE;
    unsigned int timer1 = MMIX_IRQ_TIMER_BASE + 1;
    uint64_t timer0_bit = mmix_intc_source_bit(timer0);
    uint64_t timer1_bit = mmix_intc_source_bit(timer1);

    mmix_intc_write_enable(qts, 0, timer0, timer0_bit | timer1_bit);
    mmix_intc_write_enable(qts, 1, timer0, timer0_bit | timer1_bit);
    g_assert_cmphex(mmix_intc_enable(qts, 0, timer0), ==, timer0_bit);
    g_assert_cmphex(mmix_intc_enable(qts, 1, timer0), ==, timer1_bit);

    mmix_intc_set_irq(qts, timer0, 1);
    mmix_intc_set_irq(qts, timer1, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, timer0);
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, timer1);

    qtest_quit(qts);
}

static void test_mmix_intc_inactive_contexts(void)
{
    QTestState *qts = mmix_intc_start(2);
    uint64_t bit = mmix_intc_source_bit(MMIX_IRQ_UART);
    uint64_t reserved_context =
        MMIX_INTC_CONTEXT_BASE + 64 * MMIX_INTC_CONTEXT_STRIDE;

    mmix_intc_write_enable(qts, 2, MMIX_IRQ_UART, bit);
    mmix_intc_write_enable(qts, 63, MMIX_IRQ_UART, bit);
    g_assert_cmphex(mmix_intc_enable(qts, 2, MMIX_IRQ_UART), ==, 0);
    g_assert_cmphex(mmix_intc_enable(qts, 63, MMIX_IRQ_UART), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 2), ==, 0);
    g_assert_cmpuint(mmix_intc_claim(qts, 63), ==, 0);

    qtest_writeq(qts, mmix_intc_context_reg(0, 0x810), UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, mmix_intc_context_reg(0, 0x810)), ==, 0);
    qtest_writeq(qts, reserved_context, bit);
    g_assert_cmphex(qtest_readq(qts, reserved_context), !=, bit);
    g_assert_cmphex(mmix_intc_enable(qts, 0, MMIX_IRQ_UART), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_intc_invalid_access_width(void)
{
    QTestState *qts = mmix_intc_start(1);
    uint64_t enable_reg = mmix_intc_context_reg(0, MMIX_INTC_ENABLE_BASE);

    qtest_writeb(qts, enable_reg, 0xff);
    qtest_writel(qts, enable_reg, UINT32_MAX);
    g_assert_cmphex(qtest_readq(qts, enable_reg), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MMIX_INTC_BASE +
                                MMIX_INTC_SOURCE_COUNT), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_intc_cpu_limit(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    g_autoptr(QDict) response = NULL;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "virt",
        "-smp", "65",
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        NULL,
    };
    QTestState *qts = mmix_intc_start(64);
    uint64_t expected_stack;
    int wait_status;

    response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get', "
        "  'arguments': { 'path': '/machine/cpu[63]', "
        "                 'property': 'initial-stack' } }");
    expected_stack = qdict_get_int(response, "return");
    g_assert_cmphex(expected_stack % (8 * KiB), ==, 0);
    g_assert_cmphex(expected_stack + MMIX_INITIAL_STACK_SIZE, <=,
                    512 * MiB);
    qtest_writeq(qts, expected_stack, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(qts, expected_stack), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(qts);

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text,
                           "max CPUs supported by machine 'virt' is 64"));
}

static void test_mmix_intc_reset(void)
{
    QTestState *qts = mmix_intc_start(2);
    uint64_t bit = mmix_intc_source_bit(MMIX_IRQ_UART);

    mmix_intc_write_enable(qts, 0, MMIX_IRQ_UART, bit);
    mmix_intc_set_irq(qts, MMIX_IRQ_UART, 1);
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, MMIX_IRQ_UART);
    qtest_system_reset(qts);

    g_assert_cmphex(mmix_intc_pending(qts, MMIX_IRQ_UART), ==, 0);
    g_assert_cmphex(mmix_intc_enable(qts, 0, MMIX_IRQ_UART), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/intc/configuration",
                   test_mmix_intc_configuration);
    qtest_add_func("/mmix/intc/source-namespace",
                   test_mmix_intc_source_namespace);
    qtest_add_func("/mmix/intc/claim-complete-retrigger",
                   test_mmix_intc_claim_complete_retrigger);
    qtest_add_func("/mmix/intc/lowest-source",
                   test_mmix_intc_lowest_source);
    qtest_add_func("/mmix/intc/shared-source-ownership",
                   test_mmix_intc_shared_source_ownership);
    qtest_add_func("/mmix/intc/timer-affinity",
                   test_mmix_intc_timer_affinity);
    qtest_add_func("/mmix/intc/inactive-contexts",
                   test_mmix_intc_inactive_contexts);
    qtest_add_func("/mmix/intc/invalid-access-width",
                   test_mmix_intc_invalid_access_width);
    qtest_add_func("/mmix/intc/cpu-limit", test_mmix_intc_cpu_limit);
    qtest_add_func("/mmix/intc/reset", test_mmix_intc_reset);
    qtest_add_func("/mmix/uart/mapping-and-tx",
                   test_mmix_uart_mapping_and_tx);
    qtest_add_func("/mmix/uart/rx-mask-and-deassert",
                   test_mmix_uart_rx_mask_and_deassert);
    qtest_add_func("/mmix/uart/shared-claim-and-retrigger",
                   test_mmix_uart_shared_claim_and_retrigger);

    return g_test_run();
}
