/*
 * QTest testcase for the MMIX virt PCI MSI receiver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define MMIX_MSI_BASE UINT64_C(0x0001000038000000)
#define MMIX_MSI_SIZE UINT64_C(0x10000)
#define MMIX_MSI_VECTOR_COUNT 1020

#define MMIX_INTC_BASE UINT64_C(0x0001000030000000)
#define MMIX_INTC_PENDING_BASE 0x1000
#define MMIX_INTC_CONTEXT_BASE UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_CLAIM 0x0800
#define MMIX_INTC_CONTEXT_COMPLETE 0x0808
#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ "sysbus-irq"

#define MMIX_MSI_IRQ_BASE 6148

static uint64_t mmix_intc_word_reg(uint64_t base, unsigned int source)
{
    return base + (source / 64) * sizeof(uint64_t);
}

static uint64_t mmix_intc_source_bit(unsigned int source)
{
    return UINT64_C(1) << (source % 64);
}

static uint64_t mmix_intc_pending(QTestState *qts, unsigned int source)
{
    return qtest_readq(qts, mmix_intc_word_reg(
                           MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE, source));
}

static void mmix_intc_enable(QTestState *qts, unsigned int source)
{
    qtest_writeq(qts, mmix_intc_word_reg(MMIX_INTC_CONTEXT_BASE, source),
                 mmix_intc_source_bit(source));
}

static uint64_t mmix_intc_claim(QTestState *qts)
{
    return qtest_readq(qts,
                       MMIX_INTC_CONTEXT_BASE + MMIX_INTC_CONTEXT_CLAIM);
}

static void mmix_intc_complete(QTestState *qts, unsigned int source)
{
    qtest_writeq(qts, MMIX_INTC_CONTEXT_BASE + MMIX_INTC_CONTEXT_COMPLETE,
                 source);
}

static void mmix_msi_write(QTestState *qts, uint64_t offset,
                           uint32_t vector)
{
    uint8_t data[sizeof(vector)];

    stl_le_p(data, vector);
    qtest_memwrite(qts, MMIX_MSI_BASE + offset, data, sizeof(data));
}

static void mmix_msi_assert_inactive(QTestState *qts, unsigned int source)
{
    g_assert_cmphex(mmix_intc_pending(qts, source) &
                    mmix_intc_source_bit(source), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
}

static QTestState *mmix_msi_start(void)
{
    QTestState *qts = qtest_init("-machine virt");

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    return qts;
}

static void test_mmix_msi_qom_topology(void)
{
    g_autoptr(QDict) response = NULL;
    QList *properties;
    const QListEntry *entry;
    bool receiver = false;
    bool intc_link = false;
    QTestState *qts = mmix_msi_start();

    response = qtest_qmp(
        qts, "{'execute':'qom-list','arguments':{'path':'/machine'}}");
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(property, "name"), "pcie-msi")) {
            g_assert_cmpstr(qdict_get_str(property, "type"), ==,
                            "child<mmix-msi-receiver>");
            receiver = true;
        }
    }
    g_assert_true(receiver);

    qobject_unref(response);
    response = NULL;
    response = qtest_qmp(
        qts,
        "{'execute':'qom-list','arguments':{'path':'/machine/pcie-msi'}}");
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(property, "name"),
                        "interrupt-controller")) {
            g_assert_cmpstr(qdict_get_str(property, "type"), ==,
                            "link<mmix-intc>");
            intc_link = true;
        }
    }
    g_assert_true(intc_link);

    qtest_quit(qts);
}

static void test_mmix_msi_vector_delivery(void)
{
    static const unsigned int vectors[] = { 0, MMIX_MSI_VECTOR_COUNT - 1 };
    QTestState *qts = mmix_msi_start();
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vectors); i++) {
        unsigned int vector = vectors[i];
        unsigned int source = MMIX_MSI_IRQ_BASE + vector;
        uint64_t bit = mmix_intc_source_bit(source);

        mmix_intc_enable(qts, source);
        mmix_msi_write(qts, 0, vector);
        g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
        g_assert_true(qtest_get_irq(qts, 0));
        g_assert_cmpuint(mmix_intc_claim(qts), ==, source);
        mmix_intc_complete(qts, source);
        g_assert_false(qtest_get_irq(qts, 0));
    }

    qtest_quit(qts);
}

static void test_mmix_msi_invalid_accesses(void)
{
    const unsigned int source = MMIX_MSI_IRQ_BASE;
    QTestState *qts = mmix_msi_start();

    mmix_intc_enable(qts, source);
    mmix_msi_write(qts, 4, 0);
    mmix_msi_assert_inactive(qts, source);
    mmix_msi_write(qts, 0, MMIX_MSI_VECTOR_COUNT);
    mmix_msi_assert_inactive(qts, source);
    mmix_msi_write(qts, 0, UINT32_MAX);
    mmix_msi_assert_inactive(qts, source);
    qtest_writeb(qts, MMIX_MSI_BASE, 0);
    mmix_msi_assert_inactive(qts, source);
    qtest_writew(qts, MMIX_MSI_BASE, 0);
    mmix_msi_assert_inactive(qts, source);
    qtest_writeq(qts, MMIX_MSI_BASE, 0);
    mmix_msi_assert_inactive(qts, source);
    qtest_writel(qts, MMIX_MSI_BASE + 1, 0);
    mmix_msi_assert_inactive(qts, source);
    qtest_writel(qts, MMIX_MSI_BASE + MMIX_MSI_SIZE, 0);
    mmix_msi_assert_inactive(qts, source);
    qtest_readl(qts, MMIX_MSI_BASE);
    mmix_msi_assert_inactive(qts, source);

    qtest_quit(qts);
}

static void test_mmix_msi_reset(void)
{
    const unsigned int source = MMIX_MSI_IRQ_BASE;
    uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_msi_start();

    mmix_intc_enable(qts, source);
    mmix_msi_write(qts, 0, 0);
    g_assert_cmpuint(mmix_intc_claim(qts), ==, source);
    mmix_msi_write(qts, 0, 0);
    qtest_system_reset(qts);

    mmix_intc_enable(qts, source);
    mmix_intc_complete(qts, source);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    mmix_msi_write(qts, 0, 0);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/msi/qom-topology", test_mmix_msi_qom_topology);
    qtest_add_func("/mmix/msi/vector-delivery",
                   test_mmix_msi_vector_delivery);
    qtest_add_func("/mmix/msi/invalid-accesses",
                   test_mmix_msi_invalid_accesses);
    qtest_add_func("/mmix/msi/reset", test_mmix_msi_reset);

    return g_test_run();
}
