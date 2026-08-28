/*
 * MMIX virt RAM configuration tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/units.h"
#include "qobject/qdict.h"

typedef struct MMIXRAMAcceptedCase {
    const char *value;
    uint64_t expected;
    bool preconfig;
} MMIXRAMAcceptedCase;

typedef struct MMIXRAMRejectedCase {
    const char *memory;
    const char *option;
    const char *value;
    const char *diagnostic;
} MMIXRAMRejectedCase;

static void test_mmix_ram_accepted(gconstpointer opaque)
{
    const MMIXRAMAcceptedCase *test = opaque;
    QTestState *qts;
    g_autoptr(QDict) response = NULL;
    QDict *summary;

    if (test->preconfig) {
        qts = qtest_initf("-machine virt -m %s -preconfig", test->value);
    } else if (test->value) {
        qts = qtest_initf("-machine virt -m %s", test->value);
    } else {
        qts = qtest_init("-machine virt");
    }

    if (test->preconfig) {
        response = qtest_qmp(qts,
                             "{ 'execute': 'qom-get',"
                             "  'arguments': { 'path': '/machine',"
                             "                 'property': 'memory' } }");
        summary = qdict_get_qdict(response, "return");
        g_assert_cmpuint(qdict_get_int(summary, "size"), ==,
                         test->expected);
    } else {
        response = qtest_qmp(qts,
                             "{ 'execute': 'query-memory-size-summary' }");
        summary = qdict_get_qdict(response, "return");
        g_assert_cmpuint(qdict_get_int(summary, "base-memory"), ==,
                         test->expected);
        g_assert_false(qdict_haskey(summary, "plugged-memory"));
    }
    qtest_quit(qts);
}

static void test_mmix_ram_rejected(gconstpointer opaque)
{
    const MMIXRAMRejectedCase *test = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    const char *argv[14] = {
        qtest_qemu_binary(NULL),
        "-machine", "virt",
        "-m", test->memory,
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
    };
    int wait_status;

    if (test->option) {
        argv[11] = test->option;
        argv[12] = test->value;
    }

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text, test->diagnostic));
}

int main(int argc, char **argv)
{
    static const MMIXRAMAcceptedCase accepted[] = {
        { NULL, 512 * MiB, false },
        { "128M", 128 * MiB, false },
        { "131080K", 128 * MiB + 8 * KiB, false },
        { "131073K", 128 * MiB + 8 * KiB, false },
        { "8G", 8 * GiB, false },
        { "1T", 1 * TiB, true },
    };
    static const MMIXRAMRejectedCase rejected[] = {
        { "127M", NULL, NULL, "below the minimum 0x8000000" },
        { "1025G", NULL, NULL, "exceeds the maximum 0x10000000000" },
        { "512M,maxmem=1G,slots=1", NULL, NULL,
          "does not support maxmem 0x40000000" },
        { "512M,slots=1", NULL, NULL,
          "slots specified but no max-size" },
        { "512M", "-numa", "node,mem=512M",
          "NUMA is not supported by this machine-type" },
    };
    static const char * const accepted_names[] = {
        "default", "minimum", "aligned-boundary", "normalized", "above-4g",
        "maximum",
    };
    static const char * const rejected_names[] = {
        "below-minimum", "above-maximum", "maxmem", "slots", "numa",
    };
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(accepted); i++) {
        g_autofree char *path =
            g_strdup_printf("/mmix/ram/accepted/%s", accepted_names[i]);

        qtest_add_data_func(path, &accepted[i], test_mmix_ram_accepted);
    }
    for (i = 0; i < ARRAY_SIZE(rejected); i++) {
        g_autofree char *path =
            g_strdup_printf("/mmix/ram/rejected/%s", rejected_names[i]);

        qtest_add_data_func(path, &rejected[i], test_mmix_ram_rejected);
    }

    return g_test_run();
}
