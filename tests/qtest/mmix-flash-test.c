/*
 * QTest testcase for the MMIX virt firmware flash aperture.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"

#define MMIX_FLASH0_BASE UINT64_C(0x0001000000000000)
#define MMIX_FLASH1_BASE UINT64_C(0x0001000004000000)
#define MMIX_FLASH_BANK_SIZE UINT64_C(0x04000000)
#define MMIX_FLASH_RESERVED_BASE UINT64_C(0x0001000008000000)
#define MMIX_FLASH_APERTURE_END UINT64_C(0x0001000010000000)

#define CFI_QUERY_COMMAND 0x98
#define CFI_READ_ARRAY_COMMAND 0xff
#define CFI_DEVICE_ID_COMMAND 0x90
#define CFI_QUERY_Q_OFFSET 0x40
#define CFI_QUERY_R_OFFSET 0x44
#define CFI_QUERY_Y_OFFSET 0x48
#define CFI_DEVICE_ID_OFFSET 0x04

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint8_t value = 0x5a;

    qtest_writeb(qts, address, value);
    g_assert_cmphex(qtest_readb(qts, address), !=, value);
}

static void mmix_assert_mapping(const char *mtree, uint64_t base,
                                const char *name)
{
    g_autofree char *mapping =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, romd): %s",
                        base, base + MMIX_FLASH_BANK_SIZE - 1, name);

    g_assert_nonnull(strstr(mtree, mapping));
}

static char *mmix_write_test_image(const char *name, const uint8_t *contents,
                                   size_t size, char **directory)
{
    g_autoptr(GError) error = NULL;
    char *filename;

    *directory = g_dir_make_tmp("mmix-flash-test-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(*directory, name, NULL);
    g_file_set_contents(filename, (const char *)contents, size, &error);
    g_assert_no_error(error);
    return filename;
}

static void mmix_remove_test_image(const char *filename, const char *directory)
{
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void mmix_assert_erased_banks(QTestState *qts)
{
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE), ==, 0xff);
}

static void test_mmix_flash_boundaries(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");

    mmix_assert_mapping(mtree, MMIX_FLASH0_BASE, "mmix.flash0");
    mmix_assert_mapping(mtree, MMIX_FLASH1_BASE, "mmix.flash1");
    g_assert_null(strstr(mtree, "mmix.flash2"));

    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE +
                                    MMIX_FLASH_BANK_SIZE - 1), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE +
                                    MMIX_FLASH_BANK_SIZE - 1), ==, 0xff);

    mmix_assert_unassigned(qts, MMIX_FLASH0_BASE - 1);
    mmix_assert_unassigned(qts, MMIX_FLASH_RESERVED_BASE);
    mmix_assert_unassigned(qts, MMIX_FLASH_APERTURE_END - 1);
    qtest_quit(qts);
}

static void mmix_assert_cfi_geometry(QTestState *qts, uint64_t base)
{
    qtest_writeb(qts, base, CFI_QUERY_COMMAND);
    g_assert_cmphex(qtest_readl(qts, base + CFI_QUERY_Q_OFFSET), ==,
                    UINT32_C(0x00510051));
    g_assert_cmphex(qtest_readl(qts, base + CFI_QUERY_R_OFFSET), ==,
                    UINT32_C(0x00520052));
    g_assert_cmphex(qtest_readl(qts, base + CFI_QUERY_Y_OFFSET), ==,
                    UINT32_C(0x00590059));
    qtest_writeb(qts, base, CFI_READ_ARRAY_COMMAND);

    qtest_writeb(qts, base, CFI_DEVICE_ID_COMMAND);
    g_assert_cmphex(qtest_readl(qts, base), ==, UINT32_C(0x00890089));
    g_assert_cmphex(qtest_readl(qts, base + CFI_DEVICE_ID_OFFSET), ==,
                    UINT32_C(0x00180018));
    qtest_writeb(qts, base, CFI_READ_ARRAY_COMMAND);
}

static void test_mmix_flash_cfi_geometry(void)
{
    QTestState *qts = qtest_init("-machine virt");

    mmix_assert_cfi_geometry(qts, MMIX_FLASH0_BASE);
    mmix_assert_cfi_geometry(qts, MMIX_FLASH1_BASE);
    qtest_quit(qts);
}

static void test_mmix_flash_erased_without_image(void)
{
    QTestState *qts = qtest_init("-machine virt");

    mmix_assert_erased_banks(qts);
    qtest_quit(qts);
}

static void test_mmix_flash_erased_during_raw_boot(void)
{
    uint8_t image[0x104] = { 0 };
    g_autofree char *directory = NULL;
    g_autofree char *filename =
        mmix_write_test_image("kernel.bin", image, sizeof(image), &directory);
    QTestState *qts = qtest_initf("-machine virt -kernel %s", filename);

    mmix_assert_erased_banks(qts);
    qtest_quit(qts);
    mmix_remove_test_image(filename, directory);
}

static void test_mmix_flash_erased_during_hosted_mmo_boot(void)
{
    static const uint8_t image[] = {
        0x98, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x98, 0x0a, 0x00, 0xff,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x98, 0x0b, 0x00, 0x00,
        0x98, 0x0c, 0x00, 0x00,
    };
    g_autofree char *directory = NULL;
    g_autofree char *filename =
        mmix_write_test_image("kernel.mmo", image, sizeof(image), &directory);
    QTestState *qts = qtest_initf("-machine virt -kernel %s", filename);

    mmix_assert_erased_banks(qts);
    qtest_quit(qts);
    mmix_remove_test_image(filename, directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mmix/flash/boundaries", test_mmix_flash_boundaries);
    qtest_add_func("/mmix/flash/cfi-geometry",
                   test_mmix_flash_cfi_geometry);
    qtest_add_func("/mmix/flash/no-image",
                   test_mmix_flash_erased_without_image);
    qtest_add_func("/mmix/flash/raw-boot",
                   test_mmix_flash_erased_during_raw_boot);
    qtest_add_func("/mmix/flash/hosted-mmo-boot",
                   test_mmix_flash_erased_during_hosted_mmo_boot);
    return g_test_run();
}
