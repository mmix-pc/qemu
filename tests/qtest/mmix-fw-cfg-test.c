/*
 * QTest testcase for the MMIX virt fw_cfg transport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "standard-headers/linux/qemu_fw_cfg.h"

#define MMIX_FW_CFG_BASE UINT64_C(0x0001000014000000)
#define MMIX_FW_CFG_DATA (MMIX_FW_CFG_BASE + 0x00)
#define MMIX_FW_CFG_SELECTOR (MMIX_FW_CFG_BASE + 0x08)
#define MMIX_FW_CFG_DMA (MMIX_FW_CFG_BASE + 0x10)

typedef struct MMIXFWCfgDirectory {
    uint32_t count;
    struct fw_cfg_file files[];
} MMIXFWCfgDirectory;

static void mmix_fw_cfg_select(QTestState *qts, uint16_t selector)
{
    qtest_writew(qts, MMIX_FW_CFG_SELECTOR, selector);
}

static void mmix_fw_cfg_read(QTestState *qts, uint16_t selector,
                             void *data, size_t size)
{
    uint8_t *bytes = data;
    size_t i;

    mmix_fw_cfg_select(qts, selector);
    for (i = 0; i < size; i++) {
        bytes[i] = qtest_readb(qts, MMIX_FW_CFG_DATA);
    }
}

static MMIXFWCfgDirectory *mmix_fw_cfg_read_directory(QTestState *qts)
{
    uint32_t count_be;
    uint32_t count;
    size_t size;
    MMIXFWCfgDirectory *directory;

    mmix_fw_cfg_read(qts, FW_CFG_FILE_DIR, &count_be, sizeof(count_be));
    count = be32_to_cpu(count_be);
    g_assert_cmpuint(count, <=, 64);
    size = sizeof(count_be) + count * sizeof(struct fw_cfg_file);
    directory = g_malloc(size);
    mmix_fw_cfg_read(qts, FW_CFG_FILE_DIR, directory, size);
    g_assert_cmpuint(be32_to_cpu(directory->count), ==, count);
    return directory;
}

static const struct fw_cfg_file *mmix_fw_cfg_find_file(
    const MMIXFWCfgDirectory *directory, const char *name)
{
    uint32_t count = be32_to_cpu(directory->count);
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (!strcmp(directory->files[i].name, name)) {
            return &directory->files[i];
        }
    }
    return NULL;
}

static void mmix_fw_cfg_assert_directory(const MMIXFWCfgDirectory *directory)
{
    uint32_t count = be32_to_cpu(directory->count);
    unsigned int i;

    for (i = 0; i < count; i++) {
        g_assert_cmpuint(be16_to_cpu(directory->files[i].select), ==,
                         FW_CFG_FILE_FIRST + i);
        if (i) {
            g_assert_cmpint(strcmp(directory->files[i - 1].name,
                                   directory->files[i].name), <, 0);
        }
    }
}

static void mmix_fw_cfg_assert_firmware_files_absent(
    const MMIXFWCfgDirectory *directory)
{
    g_assert_null(mmix_fw_cfg_find_file(directory, "etc/fdt"));
    g_assert_null(mmix_fw_cfg_find_file(directory, "opt/mmix/kernel"));
    g_assert_null(mmix_fw_cfg_find_file(directory, "opt/mmix/initrd"));
    g_assert_null(mmix_fw_cfg_find_file(directory, "opt/mmix/cmdline"));
}

static GBytes *mmix_fw_cfg_read_file(QTestState *qts,
                                     const MMIXFWCfgDirectory *directory,
                                     const char *name)
{
    const struct fw_cfg_file *file = mmix_fw_cfg_find_file(directory, name);
    uint32_t size;
    uint8_t *data;

    g_assert_nonnull(file);
    size = be32_to_cpu(file->size);
    data = g_malloc(size);
    mmix_fw_cfg_read(qts, be16_to_cpu(file->select), data, size);
    return g_bytes_new_take(data, size);
}

static char *mmix_write_file(const char *directory, const char *name,
                             const void *data, size_t size)
{
    g_autoptr(GError) error = NULL;
    char *filename = g_build_filename(directory, name, NULL);

    g_assert_true(g_file_set_contents(filename, data, size, &error));
    g_assert_no_error(error);
    return filename;
}

static void mmix_assert_file_bytes(QTestState *qts,
                                   const MMIXFWCfgDirectory *directory,
                                   const char *name, const void *expected,
                                   size_t expected_size)
{
    g_autoptr(GBytes) actual =
        mmix_fw_cfg_read_file(qts, directory, name);
    g_autoptr(GBytes) wanted = g_bytes_new(expected, expected_size);

    g_assert_true(g_bytes_equal(actual, wanted));
}

static void mmix_dump_firmware_fdt(const char *dtb, const char *bios,
                                   const char *kernel, const char *initrd,
                                   const char *command_line)
{
    g_autofree char *machine = g_strdup_printf("virt,dumpdtb=%s", dtb);
    g_autofree char *stderr_text = NULL;
    g_autoptr(GError) error = NULL;
    int wait_status;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", machine,
        "-bios", bios,
        "-kernel", kernel,
        "-initrd", initrd,
        "-append", command_line,
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        NULL,
    };

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL, NULL,
                               &stderr_text, &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, ==, 0);
}

static void test_mmix_fw_cfg_standard_entries(void)
{
    QTestState *qts = qtest_init("-machine virt -smp 2");
    g_autofree MMIXFWCfgDirectory *directory = NULL;
    uint8_t signature[FW_CFG_SIG_SIZE];
    uint8_t id;
    uint16_t cpus_le;

    mmix_fw_cfg_read(qts, FW_CFG_SIGNATURE, signature, sizeof(signature));
    g_assert_cmpmem(signature, sizeof(signature), "QEMU", 4);
    mmix_fw_cfg_read(qts, FW_CFG_ID, &id, sizeof(id));
    g_assert_cmphex(id & (FW_CFG_VERSION | FW_CFG_VERSION_DMA), ==,
                    FW_CFG_VERSION | FW_CFG_VERSION_DMA);
    mmix_fw_cfg_read(qts, FW_CFG_NB_CPUS, &cpus_le, sizeof(cpus_le));
    g_assert_cmpuint(le16_to_cpu(cpus_le), ==, 2);
    directory = mmix_fw_cfg_read_directory(qts);
    mmix_fw_cfg_assert_directory(directory);
    mmix_fw_cfg_assert_firmware_files_absent(directory);
    qtest_quit(qts);
}

static void test_mmix_fw_cfg_firmware_files(void)
{
    static const uint8_t bios[] = { 0xfc, 0x00, 0x00, 0x00 };
    static const uint8_t kernel[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    static const uint8_t initrd[] = { 0xaa, 0xbb, 0xcc };
    static const char command_line[] = "console=ttyS0 firmware-test";
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("mmix-fw-cfg-test-XXXXXX", &error);
    g_autofree char *bios_file = NULL;
    g_autofree char *kernel_file = NULL;
    g_autofree char *initrd_file = NULL;
    g_autofree char *dtb_file = NULL;
    g_autofree char *dtb = NULL;
    gsize dtb_size;
    QTestState *qts;
    g_autofree MMIXFWCfgDirectory *files = NULL;
    g_assert_no_error(error);
    bios_file = mmix_write_file(directory, "firmware.bin", bios,
                                sizeof(bios));
    kernel_file = mmix_write_file(directory, "kernel.bin", kernel,
                                  sizeof(kernel));
    initrd_file = mmix_write_file(directory, "initrd.bin", initrd,
                                  sizeof(initrd));
    dtb_file = g_build_filename(directory, "firmware.dtb", NULL);
    mmix_dump_firmware_fdt(dtb_file, bios_file, kernel_file, initrd_file,
                           command_line);
    qts = qtest_initf("-machine virt -bios %s -kernel %s -initrd %s "
                      "-append '%s'",
                      bios_file, kernel_file, initrd_file, command_line);
    g_assert_true(g_file_get_contents(dtb_file, &dtb, &dtb_size, &error));
    g_assert_no_error(error);

    files = mmix_fw_cfg_read_directory(qts);
    mmix_fw_cfg_assert_directory(files);
    mmix_assert_file_bytes(qts, files, "etc/fdt", dtb, dtb_size);
    mmix_assert_file_bytes(qts, files, "opt/mmix/kernel", kernel,
                           sizeof(kernel));
    mmix_assert_file_bytes(qts, files, "opt/mmix/initrd", initrd,
                           sizeof(initrd));
    mmix_assert_file_bytes(qts, files, "opt/mmix/cmdline", command_line,
                           sizeof(command_line));

    qtest_quit(qts);
    g_assert_cmpint(g_remove(dtb_file), ==, 0);
    g_assert_cmpint(g_remove(initrd_file), ==, 0);
    g_assert_cmpint(g_remove(kernel_file), ==, 0);
    g_assert_cmpint(g_remove(bios_file), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static uint32_t mmix_fw_cfg_dma_read(QTestState *qts, uint16_t selector,
                                     uint64_t descriptor_address,
                                     uint64_t destination, uint32_t size)
{
    struct fw_cfg_dma_access access = {
        .control = cpu_to_be32((selector << 16) |
                               FW_CFG_DMA_CTL_SELECT |
                               FW_CFG_DMA_CTL_READ),
        .length = cpu_to_be32(size),
        .address = cpu_to_be64(destination),
    };

    qtest_memwrite(qts, descriptor_address, &access, sizeof(access));
    qtest_writeq(qts, MMIX_FW_CFG_DMA, descriptor_address);
    qtest_memread(qts, descriptor_address, &access, sizeof(access));
    return be32_to_cpu(access.control);
}

static void test_mmix_fw_cfg_dma(void)
{
    static const uint8_t expected[] = { 'Q', 'E', 'M', 'U' };
    QTestState *qts = qtest_init("-machine virt -m 128M");
    uint8_t actual[sizeof(expected)];
    uint64_t ram_end = 128 * MiB;

    g_assert_cmphex(mmix_fw_cfg_dma_read(qts, FW_CFG_SIGNATURE, 0x1000,
                                         ram_end - sizeof(expected),
                                         sizeof(expected)), ==, 0);
    qtest_memread(qts, ram_end - sizeof(actual), actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(mmix_fw_cfg_dma_read(qts, FW_CFG_SIGNATURE, 0x1000,
                                         ram_end - 2,
                                         sizeof(expected)), ==,
                    FW_CFG_DMA_CTL_ERROR);
    qtest_quit(qts);
}

static void test_mmix_fw_cfg_absent_optional_files(void)
{
    static const uint8_t bios[] = { 0xfc, 0x00, 0x00, 0x00 };
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("mmix-fw-cfg-test-XXXXXX", &error);
    g_autofree char *bios_file = NULL;
    QTestState *qts;
    g_autofree MMIXFWCfgDirectory *files = NULL;

    g_assert_no_error(error);
    bios_file = mmix_write_file(directory, "firmware.bin", bios,
                                sizeof(bios));
    qts = qtest_initf("-machine virt -bios %s", bios_file);
    files = mmix_fw_cfg_read_directory(qts);
    mmix_fw_cfg_assert_directory(files);
    g_assert_nonnull(mmix_fw_cfg_find_file(files, "etc/fdt"));
    g_assert_null(mmix_fw_cfg_find_file(files, "opt/mmix/kernel"));
    g_assert_null(mmix_fw_cfg_find_file(files, "opt/mmix/initrd"));
    g_assert_null(mmix_fw_cfg_find_file(files, "opt/mmix/cmdline"));
    qtest_quit(qts);
    g_assert_cmpint(g_remove(bios_file), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_mmix_fw_cfg_dma_above_4g(void)
{
    static const uint8_t expected[] = { 'Q', 'E', 'M', 'U' };
    const uint64_t descriptor = UINT64_C(0x100001000);
    const uint64_t destination = UINT64_C(0x100002000);
    QTestState *qts = qtest_init("-machine virt -m 8G");
    uint8_t actual[sizeof(expected)];

    g_assert_cmphex(mmix_fw_cfg_dma_read(qts, FW_CFG_SIGNATURE, descriptor,
                                         destination, sizeof(expected)), ==,
                    0);
    qtest_memread(qts, destination, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_quit(qts);
}

static void test_mmix_fw_cfg_direct_boot_has_no_firmware_files(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("mmix-fw-cfg-test-XXXXXX", &error);
    g_autofree char *kernel = NULL;
    g_autofree uint8_t *image = g_malloc0(0x104);
    QTestState *qts;
    g_autofree MMIXFWCfgDirectory *files = NULL;

    g_assert_no_error(error);
    kernel = mmix_write_file(directory, "direct.bin", image, 0x104);
    qts = qtest_initf("-machine virt -kernel %s", kernel);
    files = mmix_fw_cfg_read_directory(qts);
    mmix_fw_cfg_assert_directory(files);
    mmix_fw_cfg_assert_firmware_files_absent(files);
    qtest_quit(qts);
    g_assert_cmpint(g_remove(kernel), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mmix/fw-cfg/standard-entries",
                   test_mmix_fw_cfg_standard_entries);
    qtest_add_func("/mmix/fw-cfg/firmware-files",
                   test_mmix_fw_cfg_firmware_files);
    qtest_add_func("/mmix/fw-cfg/absent-optional-files",
                   test_mmix_fw_cfg_absent_optional_files);
    qtest_add_func("/mmix/fw-cfg/dma/boundaries", test_mmix_fw_cfg_dma);
    qtest_add_func("/mmix/fw-cfg/dma/above-4g",
                   test_mmix_fw_cfg_dma_above_4g);
    qtest_add_func("/mmix/fw-cfg/direct-boot-isolation",
                   test_mmix_fw_cfg_direct_boot_has_no_firmware_files);
    return g_test_run();
}
