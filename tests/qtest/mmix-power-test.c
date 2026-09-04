/*
 * QTest testcase for MMIX virt reset and power control.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_mmio.h"

#define MMIX_POWER_BASE UINT64_C(0x0001000010040000)
#define MMIX_POWER_REGISTER_SIZE 0x100
#define MMIX_POWER_RESERVATION_SIZE 0x10000

#define MMIX_POWER_FEATURES 0x00
#define MMIX_POWER_COMMAND 0x04

#define MMIX_POWER_FEATURE_CONTROL 0x1

#define MMIX_RTC_BASE UINT64_C(0x0001000010010000)
#define MMIX_RTC_IRQ_ENABLED 0x10
#define MMIX_WATCHDOG_CONTROL_BASE UINT64_C(0x0001000010030000)
#define MMIX_WATCHDOG_WCS 0x00
#define MMIX_WATCHDOG_WCS_EN 0x1
#define MMIX_VIRTIO_BASE UINT64_C(0x0001000040000000)

enum MMIXPowerCommand {
    MMIX_POWER_COMMAND_NOOP,
    MMIX_POWER_COMMAND_RESET,
    MMIX_POWER_COMMAND_HALT,
    MMIX_POWER_COMMAND_PANIC,
};

static uint32_t mmix_power_readl(QTestState *qts, uint64_t reg)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, MMIX_POWER_BASE + reg, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

static void mmix_power_writel(QTestState *qts, uint64_t reg, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    qtest_memwrite(qts, MMIX_POWER_BASE + reg, bytes, sizeof(bytes));
}

static uint32_t mmix_readl_le(QTestState *qts, uint64_t address)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static void mmix_writel_le(QTestState *qts, uint64_t address, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_le_p(bytes, value);
    qtest_memwrite(qts, address, bytes, sizeof(bytes));
}

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint32_t probe = 0x5aa50ff0;

    qtest_writel(qts, address, probe);
    g_assert_cmphex(qtest_readl(qts, address), !=, probe);
}

static void mmix_assert_event_reason(QDict *event, const char *reason)
{
    QDict *data = qdict_get_qdict(event, "data");

    g_assert_true(qdict_get_bool(data, "guest"));
    g_assert_cmpstr(qdict_get_str(data, "reason"), ==, reason);
}

static void test_mmix_power_mapping_and_commands(void)
{
    QTestState *qts = qtest_init("-machine virt -no-shutdown");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *mapping =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, i/o): virt-ctrl",
                        MMIX_POWER_BASE,
                        MMIX_POWER_BASE + MMIX_POWER_REGISTER_SIZE - 1);
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_nonnull(strstr(mtree, mapping));
    g_assert_cmphex(mmix_power_readl(qts, MMIX_POWER_FEATURES), ==,
                    MMIX_POWER_FEATURE_CONTROL);
    qtest_memread(qts, MMIX_POWER_BASE + MMIX_POWER_FEATURES,
                  bytes, sizeof(bytes));
    g_assert_cmphex(bytes[0], ==, 0x00);
    g_assert_cmphex(bytes[1], ==, 0x00);
    g_assert_cmphex(bytes[2], ==, 0x00);
    g_assert_cmphex(bytes[3], ==, 0x01);

    mmix_power_writel(qts, MMIX_POWER_COMMAND, MMIX_POWER_COMMAND_NOOP);
    mmix_power_writel(qts, MMIX_POWER_COMMAND, UINT32_MAX);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_null(qtest_qmp_event_ref(qts, "RESET"));
    g_assert_null(qtest_qmp_event_ref(qts, "SHUTDOWN"));
    g_assert_cmphex(mmix_power_readl(qts, MMIX_POWER_FEATURES), ==,
                    MMIX_POWER_FEATURE_CONTROL);

    mmix_assert_unassigned(qts,
                           MMIX_POWER_BASE + MMIX_POWER_REGISTER_SIZE);
    mmix_assert_unassigned(qts,
                           MMIX_POWER_BASE +
                           MMIX_POWER_RESERVATION_SIZE - sizeof(uint32_t));

    qtest_quit(qts);
}

static void test_mmix_power_reset(void)
{
    QTestState *qts = qtest_init(
        "-machine virt -no-shutdown "
        "-object rng-builtin,id=rng0 "
        "-device virtio-rng-device,rng=rng0");
    QDict *event;
    unsigned int reset;

    for (reset = 0; reset < 2; reset++) {
        mmix_writel_le(qts, MMIX_RTC_BASE + MMIX_RTC_IRQ_ENABLED, 1);
        mmix_writel_le(qts, MMIX_WATCHDOG_CONTROL_BASE + MMIX_WATCHDOG_WCS,
                       MMIX_WATCHDOG_WCS_EN);
        qtest_writel(qts, MMIX_VIRTIO_BASE + VIRTIO_MMIO_STATUS,
                     VIRTIO_CONFIG_S_ACKNOWLEDGE);

        mmix_power_writel(qts, MMIX_POWER_COMMAND, MMIX_POWER_COMMAND_RESET);
        event = qtest_qmp_eventwait_ref(qts, "RESET");
        mmix_assert_event_reason(event, "guest-reset");
        qobject_unref(event);
        g_assert_null(qtest_qmp_event_ref(qts, "RESET"));
        g_assert_cmphex(mmix_power_readl(qts, MMIX_POWER_FEATURES), ==,
                        MMIX_POWER_FEATURE_CONTROL);
        g_assert_cmphex(mmix_readl_le(
                            qts, MMIX_RTC_BASE + MMIX_RTC_IRQ_ENABLED), ==, 0);
        g_assert_cmphex(mmix_readl_le(
                            qts, MMIX_WATCHDOG_CONTROL_BASE +
                                 MMIX_WATCHDOG_WCS), ==, 0);
        g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                    VIRTIO_MMIO_DEVICE_ID), ==,
                        VIRTIO_ID_RNG);
        g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                    VIRTIO_MMIO_STATUS), ==, 0);
    }

    qtest_quit(qts);
}

static void test_mmix_power_halt(void)
{
    QTestState *qts = qtest_init("-machine virt -no-shutdown");
    QDict *event;

    mmix_power_writel(qts, MMIX_POWER_COMMAND, MMIX_POWER_COMMAND_HALT);
    event = qtest_qmp_eventwait_ref(qts, "SHUTDOWN");
    mmix_assert_event_reason(event, "guest-shutdown");
    qobject_unref(event);

    qtest_quit(qts);
}

static void test_mmix_power_panic(void)
{
    QTestState *qts = qtest_init("-machine virt -no-shutdown");
    QDict *event;

    mmix_power_writel(qts, MMIX_POWER_COMMAND, MMIX_POWER_COMMAND_PANIC);
    event = qtest_qmp_eventwait_ref(qts, "SHUTDOWN");
    mmix_assert_event_reason(event, "guest-panic");
    qobject_unref(event);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/power/mapping-and-commands",
                   test_mmix_power_mapping_and_commands);
    qtest_add_func("/mmix/power/reset", test_mmix_power_reset);
    qtest_add_func("/mmix/power/halt", test_mmix_power_halt);
    qtest_add_func("/mmix/power/panic", test_mmix_power_panic);

    return g_test_run();
}
