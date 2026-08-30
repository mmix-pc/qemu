/*
 * QTest testcase for the MMIX virt watchdog.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"

#define MMIX_WATCHDOG_REFRESH_BASE UINT64_C(0x0001000010020000)
#define MMIX_WATCHDOG_CONTROL_BASE UINT64_C(0x0001000010030000)
#define MMIX_WATCHDOG_REGISTER_SIZE 0x1000
#define MMIX_WATCHDOG_RESERVATION_SIZE 0x10000
#define MMIX_WATCHDOG_IRQ 3

#define MMIX_WATCHDOG_WRR 0x000
#define MMIX_WATCHDOG_WCS 0x000
#define MMIX_WATCHDOG_WOR 0x008
#define MMIX_WATCHDOG_WORU 0x00c
#define MMIX_WATCHDOG_WCV 0x010
#define MMIX_WATCHDOG_WCVU 0x014
#define MMIX_WATCHDOG_IIDR 0xfcc

#define MMIX_WATCHDOG_WCS_EN 0x1
#define MMIX_WATCHDOG_WCS_WS0 0x2
#define MMIX_WATCHDOG_WCS_WS1 0x4
#define MMIX_WATCHDOG_ID 0x1043b

#define MMIX_INTC_BASE UINT64_C(0x0001000030000000)
#define MMIX_INTC_PENDING 0x1000
#define MMIX_INTC_CONTEXT_BASE 0x04000000
#define MMIX_INTC_CONTEXT_ENABLE 0x00
#define MMIX_INTC_CONTEXT_CLAIM 0x800
#define MMIX_INTC_CONTEXT_COMPLETE 0x808

#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_WATCHDOG_QOM_PATH "/machine/watchdog"
#define MMIX_SYSBUS_OUTPUT_IRQ "sysbus-irq"

static uint32_t mmix_watchdog_readl(QTestState *qts, uint64_t base,
                                    uint64_t reg)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, base + reg, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static void mmix_watchdog_writel(QTestState *qts, uint64_t base,
                                 uint64_t reg, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_le_p(bytes, value);
    qtest_memwrite(qts, base + reg, bytes, sizeof(bytes));
}

static uint32_t mmix_watchdog_control_readl(QTestState *qts, uint64_t reg)
{
    return mmix_watchdog_readl(qts, MMIX_WATCHDOG_CONTROL_BASE, reg);
}

static void mmix_watchdog_control_writel(QTestState *qts, uint64_t reg,
                                         uint32_t value)
{
    mmix_watchdog_writel(qts, MMIX_WATCHDOG_CONTROL_BASE, reg, value);
}

static void mmix_watchdog_refresh(QTestState *qts)
{
    mmix_watchdog_writel(qts, MMIX_WATCHDOG_REFRESH_BASE,
                         MMIX_WATCHDOG_WRR, 0);
}

static uint64_t mmix_intc_context_reg(uint64_t reg)
{
    return MMIX_INTC_BASE + MMIX_INTC_CONTEXT_BASE + reg;
}

static uint64_t mmix_watchdog_irq_mask(void)
{
    return UINT64_C(1) << MMIX_WATCHDOG_IRQ;
}

static void mmix_intc_enable_watchdog(QTestState *qts)
{
    qtest_writeq(qts, mmix_intc_context_reg(MMIX_INTC_CONTEXT_ENABLE),
                 mmix_watchdog_irq_mask());
}

static void mmix_watchdog_program(QTestState *qts, uint64_t ticks)
{
    mmix_watchdog_control_writel(qts, MMIX_WATCHDOG_WOR, ticks);
    mmix_watchdog_control_writel(qts, MMIX_WATCHDOG_WORU, ticks >> 32);
    mmix_watchdog_control_writel(qts, MMIX_WATCHDOG_WCS,
                                 MMIX_WATCHDOG_WCS_EN);
}

static QTestState *mmix_watchdog_start(const char *action)
{
    return qtest_initf("-machine virt -watchdog-action %s", action);
}

static QTestState *mmix_watchdog_start_intc(const char *action)
{
    QTestState *qts = mmix_watchdog_start(action);

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_SYSBUS_OUTPUT_IRQ);
    return qts;
}

static QTestState *mmix_watchdog_start_output(const char *action)
{
    QTestState *qts = mmix_watchdog_start(action);

    qtest_irq_intercept_out_named(qts, MMIX_WATCHDOG_QOM_PATH,
                                  MMIX_SYSBUS_OUTPUT_IRQ);
    return qts;
}

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint32_t probe = 0x5aa50ff0;

    qtest_writel(qts, address, probe);
    g_assert_cmphex(qtest_readl(qts, address), !=, probe);
}

static void test_mmix_watchdog_mapping_and_registers(void)
{
    QTestState *qts = mmix_watchdog_start("none");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *refresh =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, i/o): sbsa_gwdt.refresh",
                        MMIX_WATCHDOG_REFRESH_BASE,
                        MMIX_WATCHDOG_REFRESH_BASE +
                        MMIX_WATCHDOG_REGISTER_SIZE - 1);
    g_autofree char *control =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, i/o): sbsa_gwdt.control",
                        MMIX_WATCHDOG_CONTROL_BASE,
                        MMIX_WATCHDOG_CONTROL_BASE +
                        MMIX_WATCHDOG_REGISTER_SIZE - 1);
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_nonnull(strstr(mtree, refresh));
    g_assert_nonnull(strstr(mtree, control));
    g_assert_cmphex(mmix_watchdog_readl(qts, MMIX_WATCHDOG_REFRESH_BASE,
                                        MMIX_WATCHDOG_IIDR), ==,
                    MMIX_WATCHDOG_ID);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_IIDR), ==,
                    MMIX_WATCHDOG_ID);

    mmix_watchdog_control_writel(qts, MMIX_WATCHDOG_WOR, 0x89abcdef);
    qtest_memread(qts, MMIX_WATCHDOG_CONTROL_BASE + MMIX_WATCHDOG_WOR,
                  bytes, sizeof(bytes));
    g_assert_cmphex(bytes[0], ==, 0xef);
    g_assert_cmphex(bytes[1], ==, 0xcd);
    g_assert_cmphex(bytes[2], ==, 0xab);
    g_assert_cmphex(bytes[3], ==, 0x89);

    qtest_quit(qts);
}

static void test_mmix_watchdog_first_stage_irq(void)
{
    QTestState *qts = mmix_watchdog_start_intc("none");

    mmix_intc_enable_watchdog(qts);
    mmix_watchdog_program(qts, 10);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 10);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN | MMIX_WATCHDOG_WCS_WS0);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE + MMIX_INTC_PENDING), ==,
                    mmix_watchdog_irq_mask());
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(qtest_readq(
                         qts,
                         mmix_intc_context_reg(MMIX_INTC_CONTEXT_CLAIM)), ==,
                     MMIX_WATCHDOG_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writeq(qts, mmix_intc_context_reg(MMIX_INTC_CONTEXT_COMPLETE),
                 MMIX_WATCHDOG_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    mmix_watchdog_refresh(qts);
    qtest_writeq(qts, mmix_intc_context_reg(MMIX_INTC_CONTEXT_COMPLETE),
                 MMIX_WATCHDOG_IRQ);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN);
    g_assert_cmphex(qtest_readq(qts, MMIX_INTC_BASE + MMIX_INTC_PENDING), ==,
                    0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_watchdog_second_stage_action(void)
{
    QTestState *qts = mmix_watchdog_start_output("none");
    QDict *event;
    QDict *data;

    mmix_watchdog_program(qts, 10);
    qtest_clock_step(qts, 10);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 10);

    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    data = qdict_get_qdict(event, "data");
    g_assert_cmpstr(qdict_get_str(data, "action"), ==, "none");
    qobject_unref(event);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN | MMIX_WATCHDOG_WCS_WS0 |
                    MMIX_WATCHDOG_WCS_WS1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_watchdog_reset_action(void)
{
    QTestState *qts = mmix_watchdog_start_output("reset");
    QDict *event;

    mmix_watchdog_program(qts, 10);
    qtest_clock_step(qts, 10);
    qtest_clock_step(qts, 10);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "reset");
    qobject_unref(event);
    qtest_qmp_eventwait(qts, "RESET");

    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WCS), ==,
                    0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_watchdog_reset_and_bounds(void)
{
    QTestState *qts = mmix_watchdog_start_output("none");

    qtest_writeb(qts, MMIX_WATCHDOG_CONTROL_BASE + MMIX_WATCHDOG_WCS,
                 MMIX_WATCHDOG_WCS_EN);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WCS), ==,
                    0);

    mmix_assert_unassigned(qts, MMIX_WATCHDOG_REFRESH_BASE +
                                MMIX_WATCHDOG_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_REFRESH_BASE +
                                MMIX_WATCHDOG_RESERVATION_SIZE - 4);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                MMIX_WATCHDOG_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                MMIX_WATCHDOG_RESERVATION_SIZE - 4);

    mmix_watchdog_program(qts, 10);
    qtest_clock_step(qts, 10);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_system_reset(qts);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WCS), ==,
                    0);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WOR), ==,
                    0);
    g_assert_cmphex(mmix_watchdog_control_readl(qts, MMIX_WATCHDOG_WORU), ==,
                    0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_mmix_watchdog_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir =
        g_dir_make_tmp("mmix-watchdog-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    QTestState *from;
    QTestState *to;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf(
        "-machine virt -watchdog-action none -incoming %s", uri);

    from = mmix_watchdog_start("none");
    to = qtest_init(incoming);
    qtest_irq_intercept_out_named(to, MMIX_WATCHDOG_QOM_PATH,
                                  MMIX_SYSBUS_OUTPUT_IRQ);
    mmix_watchdog_program(from, 1000);
    qtest_clock_step(from, 1000);
    g_assert_cmphex(mmix_watchdog_control_readl(from, MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN | MMIX_WATCHDOG_WCS_WS0);

    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(from, "STOP");
    qtest_qmp_eventwait(to, "RESUME");

    g_assert_cmphex(mmix_watchdog_control_readl(to, MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN | MMIX_WATCHDOG_WCS_WS0);
    g_assert_cmphex(mmix_watchdog_control_readl(to, MMIX_WATCHDOG_WOR), ==,
                    1000);
    g_assert_true(qtest_get_irq(to, 0));
    qtest_clock_step_next(to);
    g_assert_cmphex(mmix_watchdog_control_readl(to, MMIX_WATCHDOG_WCS), ==,
                    MMIX_WATCHDOG_WCS_EN | MMIX_WATCHDOG_WCS_WS0 |
                    MMIX_WATCHDOG_WCS_WS1);
    g_assert_true(qtest_get_irq(to, 0));

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/watchdog/mapping-and-registers",
                   test_mmix_watchdog_mapping_and_registers);
    qtest_add_func("/mmix/watchdog/first-stage-irq",
                   test_mmix_watchdog_first_stage_irq);
    qtest_add_func("/mmix/watchdog/second-stage-action",
                   test_mmix_watchdog_second_stage_action);
    qtest_add_func("/mmix/watchdog/reset-action",
                   test_mmix_watchdog_reset_action);
    qtest_add_func("/mmix/watchdog/reset-and-bounds",
                   test_mmix_watchdog_reset_and_bounds);
    qtest_add_func("/mmix/watchdog/migration",
                   test_mmix_watchdog_migration);

    return g_test_run();
}
