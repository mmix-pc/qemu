/*
 * QTest testcase for the MMIX virt real-time clock.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define MMIX_VIRT_RTC_BASE UINT64_C(0x0001000010010000)
#define MMIX_VIRT_RTC_REGISTER_SIZE 0x24
#define MMIX_VIRT_RTC_RESERVATION_SIZE 0x10000
#define MMIX_VIRT_RTC_IRQ 2

#define MMIX_RTC_TIME_LOW 0x00
#define MMIX_RTC_TIME_HIGH 0x04
#define MMIX_RTC_ALARM_LOW 0x08
#define MMIX_RTC_ALARM_HIGH 0x0c
#define MMIX_RTC_IRQ_ENABLED 0x10
#define MMIX_RTC_CLEAR_ALARM 0x14
#define MMIX_RTC_ALARM_STATUS 0x18
#define MMIX_RTC_CLEAR_INTERRUPT 0x1c

#define MMIX_VIRT_INTC_BASE UINT64_C(0x0001000030000000)
#define MMIX_VIRT_INTC_PENDING 0x1000
#define MMIX_VIRT_INTC_CONTEXT_BASE 0x04000000
#define MMIX_VIRT_INTC_CONTEXT_ENABLE 0x00
#define MMIX_VIRT_INTC_CONTEXT_CLAIM 0x800
#define MMIX_VIRT_INTC_CONTEXT_COMPLETE 0x808

#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ "sysbus-irq"

static uint32_t mmix_rtc_readl(QTestState *qts, uint64_t reg)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, MMIX_VIRT_RTC_BASE + reg, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static void mmix_rtc_writel(QTestState *qts, uint64_t reg, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_le_p(bytes, value);
    qtest_memwrite(qts, MMIX_VIRT_RTC_BASE + reg, bytes, sizeof(bytes));
}

static uint64_t mmix_rtc_read_time(QTestState *qts)
{
    uint64_t low = mmix_rtc_readl(qts, MMIX_RTC_TIME_LOW);
    uint64_t high = mmix_rtc_readl(qts, MMIX_RTC_TIME_HIGH);

    return low | high << 32;
}

static void mmix_rtc_write_alarm(QTestState *qts, uint64_t value)
{
    mmix_rtc_writel(qts, MMIX_RTC_ALARM_HIGH, value >> 32);
    mmix_rtc_writel(qts, MMIX_RTC_ALARM_LOW, value);
}

static uint64_t mmix_intc_context_reg(unsigned int cpu, uint64_t reg)
{
    return MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_CONTEXT_BASE +
           cpu * MMIX_VIRT_RTC_RESERVATION_SIZE + reg;
}

static uint64_t mmix_rtc_irq_mask(void)
{
    return UINT64_C(1) << MMIX_VIRT_RTC_IRQ;
}

static void mmix_intc_enable_rtc(QTestState *qts, unsigned int cpu)
{
    qtest_writeq(qts,
                 mmix_intc_context_reg(cpu,
                                       MMIX_VIRT_INTC_CONTEXT_ENABLE),
                 mmix_rtc_irq_mask());
}

static QTestState *mmix_rtc_start_cpus(unsigned int cpus)
{
    QTestState *qts = qtest_initf("-machine virt -rtc clock=vm -smp %u",
                                  cpus);

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static QTestState *mmix_rtc_start(void)
{
    return mmix_rtc_start_cpus(1);
}

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint32_t probe = 0x5aa50ff0;

    qtest_writel(qts, address, probe);
    g_assert_cmphex(qtest_readl(qts, address), !=, probe);
}

static void test_mmix_rtc_mapping_and_registers(void)
{
    QTestState *qts = mmix_rtc_start();
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *mapping =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, i/o): goldfish_rtc",
                        MMIX_VIRT_RTC_BASE,
                        MMIX_VIRT_RTC_BASE + MMIX_VIRT_RTC_REGISTER_SIZE - 1);
    const uint64_t alarm = UINT64_C(0x0123456789abcdef);
    uint8_t bytes[4];

    g_assert_nonnull(strstr(mtree, mapping));
    mmix_rtc_write_alarm(qts, alarm);
    g_assert_cmphex(mmix_rtc_readl(qts, MMIX_RTC_ALARM_LOW), ==,
                    alarm & UINT32_MAX);
    g_assert_cmphex(mmix_rtc_readl(qts, MMIX_RTC_ALARM_HIGH), ==,
                    alarm >> 32);

    qtest_memread(qts, MMIX_VIRT_RTC_BASE + MMIX_RTC_ALARM_LOW,
                  bytes, sizeof(bytes));
    g_assert_cmphex(bytes[0], ==, 0xef);
    g_assert_cmphex(bytes[1], ==, 0xcd);
    g_assert_cmphex(bytes[2], ==, 0xab);
    g_assert_cmphex(bytes[3], ==, 0x89);

    qtest_quit(qts);
}

static void test_mmix_rtc_time_and_alarm(void)
{
    QTestState *qts = mmix_rtc_start();
    uint64_t before = mmix_rtc_read_time(qts);
    uint64_t deadline = before + 1000;

    mmix_intc_enable_rtc(qts, 0);
    mmix_rtc_writel(qts, MMIX_RTC_IRQ_ENABLED, 1);
    mmix_rtc_write_alarm(qts, deadline);
    g_assert_cmpuint(mmix_rtc_readl(qts, MMIX_RTC_ALARM_STATUS), ==, 1);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(mmix_rtc_read_time(qts), >=, deadline);
    g_assert_cmphex(qtest_readq(qts, MMIX_VIRT_INTC_BASE +
                                    MMIX_VIRT_INTC_PENDING), ==,
                    mmix_rtc_irq_mask());
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(qtest_readq(
                         qts,
                         mmix_intc_context_reg(0,
                             MMIX_VIRT_INTC_CONTEXT_CLAIM)), ==,
                     MMIX_VIRT_RTC_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writeq(qts,
                 mmix_intc_context_reg(0,
                                       MMIX_VIRT_INTC_CONTEXT_COMPLETE),
                 MMIX_VIRT_RTC_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    mmix_rtc_writel(qts, MMIX_RTC_CLEAR_INTERRUPT, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readq(qts, MMIX_VIRT_INTC_BASE +
                                    MMIX_VIRT_INTC_PENDING), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_rtc_bounds_and_access_width(void)
{
    QTestState *qts = mmix_rtc_start();

    qtest_writeb(qts, MMIX_VIRT_RTC_BASE + MMIX_RTC_IRQ_ENABLED, 1);
    g_assert_cmpuint(mmix_rtc_readl(qts, MMIX_RTC_IRQ_ENABLED), ==, 0);
    mmix_assert_unassigned(qts, MMIX_VIRT_RTC_BASE +
                                MMIX_VIRT_RTC_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_VIRT_RTC_BASE +
                                MMIX_VIRT_RTC_RESERVATION_SIZE - 4);

    qtest_quit(qts);
}

static void test_mmix_rtc_reset(void)
{
    QTestState *qts = mmix_rtc_start();
    uint64_t now = mmix_rtc_read_time(qts);

    mmix_intc_enable_rtc(qts, 0);
    mmix_rtc_writel(qts, MMIX_RTC_IRQ_ENABLED, 1);
    mmix_rtc_write_alarm(qts, now);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_system_reset(qts);
    g_assert_cmpuint(mmix_rtc_readl(qts, MMIX_RTC_IRQ_ENABLED), ==, 0);
    g_assert_cmpuint(mmix_rtc_readl(qts, MMIX_RTC_ALARM_LOW), ==, 0);
    g_assert_cmpuint(mmix_rtc_readl(qts, MMIX_RTC_ALARM_HIGH), ==, 0);
    g_assert_cmpuint(mmix_rtc_readl(qts, MMIX_RTC_ALARM_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_rtc_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir = g_dir_make_tmp("mmix-rtc-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    QTestState *from;
    QTestState *to;
    uint64_t alarm;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf(
        "-machine virt -rtc clock=vm -smp 2 -incoming %s", uri);

    from = mmix_rtc_start_cpus(2);
    to = qtest_init(incoming);
    qtest_irq_intercept_out_named(to, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);

    mmix_intc_enable_rtc(from, 0);
    mmix_intc_enable_rtc(from, 1);
    mmix_rtc_writel(from, MMIX_RTC_IRQ_ENABLED, 1);
    alarm = mmix_rtc_read_time(from);
    mmix_rtc_write_alarm(from, alarm);
    g_assert_true(qtest_get_irq(from, 0));
    g_assert_true(qtest_get_irq(from, 1));
    g_assert_cmpuint(qtest_readq(
                         from,
                         mmix_intc_context_reg(
                             1, MMIX_VIRT_INTC_CONTEXT_CLAIM)), ==,
                     MMIX_VIRT_RTC_IRQ);
    g_assert_false(qtest_get_irq(from, 0));
    g_assert_false(qtest_get_irq(from, 1));

    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(from, "STOP");
    qtest_qmp_eventwait(to, "RESUME");

    g_assert_cmpuint(mmix_rtc_readl(to, MMIX_RTC_IRQ_ENABLED), ==, 1);
    g_assert_cmphex(mmix_rtc_readl(to, MMIX_RTC_ALARM_LOW), ==,
                    alarm & UINT32_MAX);
    g_assert_cmphex(mmix_rtc_readl(to, MMIX_RTC_ALARM_HIGH), ==,
                    alarm >> 32);
    g_assert_cmphex(qtest_readq(to, MMIX_VIRT_INTC_BASE +
                                   MMIX_VIRT_INTC_PENDING), ==,
                    0);
    g_assert_false(qtest_get_irq(to, 0));
    g_assert_false(qtest_get_irq(to, 1));
    qtest_writeq(to,
                 mmix_intc_context_reg(1,
                                       MMIX_VIRT_INTC_CONTEXT_COMPLETE),
                 MMIX_VIRT_RTC_IRQ);
    g_assert_cmphex(qtest_readq(to, MMIX_VIRT_INTC_BASE +
                                   MMIX_VIRT_INTC_PENDING), ==,
                    mmix_rtc_irq_mask());
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 1));
    mmix_rtc_writel(to, MMIX_RTC_CLEAR_INTERRUPT, 0);
    g_assert_false(qtest_get_irq(to, 0));
    g_assert_false(qtest_get_irq(to, 1));

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_mmix_rtc_shared_irq_routing(void)
{
    QTestState *qts = mmix_rtc_start_cpus(2);
    uint64_t now = mmix_rtc_read_time(qts);

    mmix_rtc_writel(qts, MMIX_RTC_IRQ_ENABLED, 1);
    mmix_rtc_write_alarm(qts, now);
    g_assert_cmphex(qtest_readq(qts, MMIX_VIRT_INTC_BASE +
                                    MMIX_VIRT_INTC_PENDING), ==,
                    mmix_rtc_irq_mask());
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    mmix_intc_enable_rtc(qts, 0);
    mmix_intc_enable_rtc(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(qtest_readq(
                         qts,
                         mmix_intc_context_reg(
                             1, MMIX_VIRT_INTC_CONTEXT_CLAIM)), ==,
                     MMIX_VIRT_RTC_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(qtest_readq(
                         qts,
                         mmix_intc_context_reg(
                             0, MMIX_VIRT_INTC_CONTEXT_CLAIM)), ==, 0);

    qtest_writeq(qts,
                 mmix_intc_context_reg(1,
                                       MMIX_VIRT_INTC_CONTEXT_COMPLETE),
                 MMIX_VIRT_RTC_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(qtest_readq(
                         qts,
                         mmix_intc_context_reg(
                             0, MMIX_VIRT_INTC_CONTEXT_CLAIM)), ==,
                     MMIX_VIRT_RTC_IRQ);
    mmix_rtc_writel(qts, MMIX_RTC_CLEAR_INTERRUPT, 0);
    qtest_writeq(qts,
                 mmix_intc_context_reg(0,
                                       MMIX_VIRT_INTC_CONTEXT_COMPLETE),
                 MMIX_VIRT_RTC_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/rtc/mapping-and-registers",
                   test_mmix_rtc_mapping_and_registers);
    qtest_add_func("/mmix/rtc/time-and-alarm", test_mmix_rtc_time_and_alarm);
    qtest_add_func("/mmix/rtc/bounds-and-access-width",
                   test_mmix_rtc_bounds_and_access_width);
    qtest_add_func("/mmix/rtc/reset", test_mmix_rtc_reset);
    qtest_add_func("/mmix/rtc/migration", test_mmix_rtc_migration);
    qtest_add_func("/mmix/rtc/shared-irq-routing",
                   test_mmix_rtc_shared_irq_routing);

    return g_test_run();
}
