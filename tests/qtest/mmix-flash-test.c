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
#define CFI_PROGRAM_COMMAND 0x40
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

static char *mmix_create_test_image(const char *name, uint64_t size,
                                    const uint8_t *contents,
                                    size_t contents_size, char **directory)
{
    g_autoptr(GError) error = NULL;
    char *filename;
    int fd;

    g_assert_cmpuint(contents_size, <=, size);
    *directory = g_dir_make_tmp("mmix-flash-test-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(*directory, name, NULL);
    fd = g_open(filename, O_CREAT | O_EXCL | O_RDWR, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    if (contents_size) {
        g_assert_cmpint(pwrite(fd, contents, contents_size, 0), ==,
                        contents_size);
    }
    g_assert_cmpint(close(fd), ==, 0);
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

static void mmix_program_byte(QTestState *qts, uint64_t address,
                              uint8_t value)
{
    qtest_writeb(qts, address, CFI_PROGRAM_COMMAND);
    qtest_writeb(qts, address, value);
    qtest_writeb(qts, address, CFI_READ_ARRAY_COMMAND);
}

static uint8_t mmix_read_file_byte(const char *filename, uint64_t offset)
{
    uint8_t value;
    int fd = g_open(filename, O_RDONLY, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, &value, sizeof(value), offset), ==,
                    sizeof(value));
    g_assert_cmpint(close(fd), ==, 0);
    return value;
}

static void mmix_assert_qemu_rejected(const char *const *extra_args,
                                      const char *diagnostic)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    int wait_status;
    unsigned int i;

    g_ptr_array_add(argv, (void *)qtest_qemu_binary(NULL));
    g_ptr_array_add(argv, (void *)"-display");
    g_ptr_array_add(argv, (void *)"none");
    g_ptr_array_add(argv, (void *)"-monitor");
    g_ptr_array_add(argv, (void *)"none");
    g_ptr_array_add(argv, (void *)"-serial");
    g_ptr_array_add(argv, (void *)"none");
    for (i = 0; extra_args[i]; i++) {
        g_ptr_array_add(argv, (void *)extra_args[i]);
    }
    g_ptr_array_add(argv, NULL);

    g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text, diagnostic));
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
    mmix_program_byte(qts, MMIX_FLASH0_BASE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, 0xff);
    mmix_program_byte(qts, MMIX_FLASH1_BASE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE), ==, 0x5a);
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

static void test_mmix_flash_bios_image(void)
{
    static const uint8_t image[] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    };
    uint8_t actual[sizeof(image)];
    g_autofree char *directory = NULL;
    g_autofree char *filename =
        mmix_write_test_image("firmware.bin", image, sizeof(image),
                              &directory);
    QTestState *qts = qtest_initf("-machine virt -bios %s", filename);

    qtest_memread(qts, MMIX_FLASH0_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), image, sizeof(image));
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE + sizeof(image)), ==,
                    0xff);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE +
                                    MMIX_FLASH_BANK_SIZE - 1), ==, 0xff);
    mmix_assert_cfi_geometry(qts, MMIX_FLASH0_BASE);

    mmix_program_byte(qts, MMIX_FLASH0_BASE, 0xa5);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, image[0]);
    mmix_program_byte(qts, MMIX_FLASH1_BASE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE), ==, 0x5a);

    qtest_quit(qts);
    mmix_remove_test_image(filename, directory);
}

static void test_mmix_flash_bios_size_boundaries(void)
{
    static const uint8_t minimum[] = { 0x00, 0x01, 0x02, 0x03 };
    g_autofree char *minimum_directory = NULL;
    g_autofree char *minimum_filename =
        mmix_write_test_image("minimum.bin", minimum, sizeof(minimum),
                              &minimum_directory);
    g_autofree char *maximum_directory = NULL;
    g_autofree char *maximum_filename =
        mmix_create_test_image("maximum.bin", MMIX_FLASH_BANK_SIZE,
                               minimum, sizeof(minimum), &maximum_directory);
    QTestState *qts;

    qts = qtest_initf("-machine virt -bios %s", minimum_filename);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE + sizeof(minimum)), ==,
                    0xff);
    qtest_quit(qts);

    qts = qtest_initf("-machine virt -bios %s", maximum_filename);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, minimum[0]);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE +
                                    MMIX_FLASH_BANK_SIZE - 1), ==, 0x00);
    qtest_quit(qts);

    mmix_remove_test_image(minimum_filename, minimum_directory);
    mmix_remove_test_image(maximum_filename, maximum_directory);
}

static void test_mmix_flash_bios_size_rejected(void)
{
    static const uint8_t unaligned[] = { 0x00, 0x01 };
    g_autofree char *empty_directory = NULL;
    g_autofree char *empty_filename =
        mmix_create_test_image("empty.bin", 0, NULL, 0, &empty_directory);
    g_autofree char *unaligned_directory = NULL;
    g_autofree char *unaligned_filename =
        mmix_write_test_image("unaligned.bin", unaligned,
                              sizeof(unaligned), &unaligned_directory);
    g_autofree char *oversized_directory = NULL;
    g_autofree char *oversized_filename =
        mmix_create_test_image("oversized.bin", MMIX_FLASH_BANK_SIZE + 4,
                               NULL, 0, &oversized_directory);
    const char *empty_args[] = {
        "-machine", "virt", "-bios", empty_filename, NULL,
    };
    const char *unaligned_args[] = {
        "-machine", "virt", "-bios", unaligned_filename, NULL,
    };
    const char *oversized_args[] = {
        "-machine", "virt", "-bios", oversized_filename, NULL,
    };

    mmix_assert_qemu_rejected(empty_args, "is empty");
    mmix_assert_qemu_rejected(unaligned_args,
                              "size must be a multiple of 4 bytes");
    mmix_assert_qemu_rejected(oversized_args,
                              "maximum size is 0x4000000");

    mmix_remove_test_image(empty_filename, empty_directory);
    mmix_remove_test_image(unaligned_filename, unaligned_directory);
    mmix_remove_test_image(oversized_filename, oversized_directory);
}

static void test_mmix_flash_backends(void)
{
    static const uint8_t firmware[] = { 0x12, 0x34, 0x56, 0x78 };
    static const uint8_t variable[] = { 0xff };
    g_autofree char *firmware_directory = NULL;
    g_autofree char *firmware_filename =
        mmix_create_test_image("firmware.fd", MMIX_FLASH_BANK_SIZE,
                               firmware, sizeof(firmware),
                               &firmware_directory);
    g_autofree char *variable_directory = NULL;
    g_autofree char *variable_filename =
        mmix_create_test_image("variables.fd", MMIX_FLASH_BANK_SIZE,
                               variable, sizeof(variable),
                               &variable_directory);
    QTestState *qts = qtest_initf(
        "-machine virt,pflash0=flash0,pflash1=flash1 "
        "-drive if=none,id=flash0,file=%s,format=raw,readonly=on "
        "-drive if=none,id=flash1,file=%s,format=raw",
        firmware_filename, variable_filename);

    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, firmware[0]);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE), ==, variable[0]);
    mmix_program_byte(qts, MMIX_FLASH0_BASE, 0xa5);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH0_BASE), ==, firmware[0]);
    mmix_program_byte(qts, MMIX_FLASH1_BASE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, MMIX_FLASH1_BASE), ==, 0x5a);
    qtest_quit(qts);

    g_assert_cmphex(mmix_read_file_byte(firmware_filename, 0), ==,
                    firmware[0]);
    g_assert_cmphex(mmix_read_file_byte(variable_filename, 0), ==, 0x5a);
    mmix_remove_test_image(firmware_filename, firmware_directory);
    mmix_remove_test_image(variable_filename, variable_directory);
}

static void test_mmix_flash_backend_policy_rejected(void)
{
    static const uint8_t firmware[] = { 0x12 };
    static const uint8_t variable[] = { 0xa5 };
    g_autofree char *firmware_directory = NULL;
    g_autofree char *firmware_filename =
        mmix_create_test_image("firmware.fd", MMIX_FLASH_BANK_SIZE,
                               firmware, sizeof(firmware),
                               &firmware_directory);
    g_autofree char *variable_directory = NULL;
    g_autofree char *variable_filename =
        mmix_create_test_image("variables.fd", MMIX_FLASH_BANK_SIZE,
                               variable, sizeof(variable),
                               &variable_directory);
    g_autofree char *short_directory = NULL;
    g_autofree char *short_filename =
        mmix_create_test_image("short.fd", MMIX_FLASH_BANK_SIZE - 512,
                               NULL, 0, &short_directory);
    const char *writable_firmware_args[] = {
        "-drive", NULL, "-machine", "virt,pflash0=flash0", NULL,
    };
    const char *readonly_variable_args[] = {
        "-drive", NULL, "-drive", NULL, "-machine",
        "virt,pflash0=flash0,pflash1=flash1", NULL,
    };
    const char *short_firmware_args[] = {
        "-drive", NULL, "-machine", "virt,pflash0=flash0", NULL,
    };
    g_autofree char *writable_firmware =
        g_strdup_printf("if=none,id=flash0,file=%s,format=raw",
                        firmware_filename);
    g_autofree char *readonly_firmware =
        g_strdup_printf("if=none,id=flash0,file=%s,format=raw,readonly=on",
                        firmware_filename);
    g_autofree char *readonly_variable =
        g_strdup_printf("if=none,id=flash1,file=%s,format=raw,readonly=on",
                        variable_filename);
    g_autofree char *short_firmware =
        g_strdup_printf("if=none,id=flash0,file=%s,format=raw,readonly=on",
                        short_filename);

    writable_firmware_args[1] = writable_firmware;
    readonly_variable_args[1] = readonly_firmware;
    readonly_variable_args[3] = readonly_variable;
    short_firmware_args[1] = short_firmware;

    mmix_assert_qemu_rejected(writable_firmware_args,
                              "pflash0 backend must be read-only");
    mmix_assert_qemu_rejected(readonly_variable_args,
                              "pflash1 backend must be writable");
    mmix_assert_qemu_rejected(short_firmware_args,
                              "required size is 0x4000000");

    g_assert_cmphex(mmix_read_file_byte(firmware_filename, 0), ==,
                    firmware[0]);
    g_assert_cmphex(mmix_read_file_byte(variable_filename, 0), ==,
                    variable[0]);
    mmix_remove_test_image(firmware_filename, firmware_directory);
    mmix_remove_test_image(variable_filename, variable_directory);
    mmix_remove_test_image(short_filename, short_directory);
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
    qtest_add_func("/mmix/flash/bios/image", test_mmix_flash_bios_image);
    qtest_add_func("/mmix/flash/bios/size-boundaries",
                   test_mmix_flash_bios_size_boundaries);
    qtest_add_func("/mmix/flash/bios/size-rejected",
                   test_mmix_flash_bios_size_rejected);
    qtest_add_func("/mmix/flash/backends/accepted",
                   test_mmix_flash_backends);
    qtest_add_func("/mmix/flash/backends/policy-rejected",
                   test_mmix_flash_backend_policy_rejected);
    return g_test_run();
}
