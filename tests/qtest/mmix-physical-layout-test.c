/*
 * MMIX virt replacement physical layout tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "elf.h"
#include "libqtest.h"
#include "qemu/units.h"
#include "qobject/qdict.h"

#define MMIX_DEFAULT_RAM_SIZE (512 * MiB)
#define MMIX_INITIAL_STACK_SIZE (32 * KiB)
#define MMIX_INITIAL_STACK_ALIGN (8 * KiB)
#define MMIX_FRAMEBUFFER_CONTROL_BASE UINT64_C(0x0001000018000000)
#define MMIX_FRAMEBUFFER_REG_BASE 0x20

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

typedef struct MMIXRAMEndpointCase {
    const char *value;
    uint64_t size;
} MMIXRAMEndpointCase;

typedef struct MMIXKernelClassificationCase {
    const char *name;
    const uint8_t *data;
    size_t size;
    const char *diagnostic;
} MMIXKernelClassificationCase;

typedef struct MMIXRawRejectionCase {
    uint64_t image_size;
    const char *memory;
    const char *machine;
    const char *option;
    const char *value;
    const char *diagnostic;
} MMIXRawRejectionCase;

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

static void mmix_assert_ram_bytes(QTestState *qts, uint64_t address,
                                  const uint8_t *expected, size_t size)
{
    g_autofree uint8_t *actual = g_malloc(size);

    qtest_memwrite(qts, address, expected, size);
    qtest_memread(qts, address, actual, size);
    g_assert_cmpmem(actual, size, expected, size);
}

static void test_mmix_ram_exact_endpoint(gconstpointer opaque)
{
    const MMIXRAMEndpointCase *test = opaque;
    const uint8_t pattern[] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    QTestState *qts = qtest_initf("-machine virt -m %s", test->value);

    mmix_assert_ram_bytes(qts, 0, pattern, sizeof(pattern));
    mmix_assert_ram_bytes(qts, test->size - sizeof(pattern), pattern,
                          sizeof(pattern));
    qtest_writeb(qts, test->size, 0xa5);
    g_assert_cmphex(qtest_readb(qts, test->size), !=, 0xa5);
    qtest_quit(qts);
}

static void test_mmix_ram_contiguous_default(void)
{
    static const uint64_t legacy_addresses[] = {
        0x06000000,
        0x06800000,
        0x0a800000,
        0x0e800000,
        0x0f000000,
        0x10000000,
        0x10001000,
        0x10002000,
        0x10003000,
        0x10004000,
        0x10006000,
    };
    const uint8_t pattern[] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    QTestState *qts = qtest_init("-machine virt");
    unsigned int i;

    mmix_assert_ram_bytes(qts, 0, pattern, sizeof(pattern));
    for (i = 0; i < ARRAY_SIZE(legacy_addresses); i++) {
        mmix_assert_ram_bytes(qts, legacy_addresses[i], pattern,
                              sizeof(pattern));
    }
    mmix_assert_ram_bytes(qts, 512 * MiB - sizeof(pattern), pattern,
                          sizeof(pattern));

    qtest_writeb(qts, 512 * MiB, 0xa5);
    g_assert_cmphex(qtest_readb(qts, 512 * MiB), !=, 0xa5);
    qtest_quit(qts);
}

static void test_mmix_ram_crosses_4g(void)
{
    const uint8_t pattern[] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
    };
    const uint64_t ram_size = 8 * GiB;
    QTestState *qts = qtest_init("-machine virt -m 8G");

    mmix_assert_ram_bytes(qts, 4 * GiB - sizeof(pattern) / 2,
                          pattern, sizeof(pattern));
    mmix_assert_ram_bytes(qts, ram_size - sizeof(pattern), pattern,
                          sizeof(pattern));
    qtest_writeb(qts, ram_size, 0xa5);
    g_assert_cmphex(qtest_readb(qts, ram_size), !=, 0xa5);
    qtest_quit(qts);
}

static void test_mmix_ram_survives_reset(void)
{
    const uint8_t pattern[] = {
        0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe,
    };
    uint8_t actual[sizeof(pattern)];
    QTestState *qts = qtest_init("-machine virt");

    qtest_memwrite(qts, 0x10000000, pattern, sizeof(pattern));
    qtest_system_reset(qts);
    qtest_memread(qts, 0x10000000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), pattern, sizeof(pattern));
    qtest_quit(qts);
}

static uint64_t mmix_cpu_initial_stack(QTestState *qts, unsigned int cpu)
{
    g_autofree char *path = g_strdup_printf("/machine/cpu[%u]", cpu);
    g_autoptr(QDict) response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get',"
        "  'arguments': { 'path': %s,"
        "                 'property': 'initial-stack' } }",
        path);

    return qdict_get_int(response, "return");
}

static uint64_t mmix_framebuffer_base(QTestState *qts)
{
    return qtest_readq(qts, MMIX_FRAMEBUFFER_CONTROL_BASE +
                       MMIX_FRAMEBUFFER_REG_BASE);
}

static bool mmix_test_ranges_overlap(uint64_t left, uint64_t left_size,
                                     uint64_t right, uint64_t right_size)
{
    return left < right + right_size && right < left + left_size;
}

static void test_mmix_initial_stack_single_cpu(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint64_t stack = mmix_cpu_initial_stack(qts, 0);
    uint64_t framebuffer = mmix_framebuffer_base(qts);

    g_assert_cmphex(stack % MMIX_INITIAL_STACK_ALIGN, ==, 0);
    g_assert_cmphex(stack + MMIX_INITIAL_STACK_SIZE, <=, framebuffer);
    qtest_writeq(qts, stack, UINT64_C(0x1122334455667788));
    qtest_writeq(qts, stack + MMIX_INITIAL_STACK_SIZE - sizeof(uint64_t),
                 UINT64_C(0x8877665544332211));
    qtest_system_reset(qts);
    g_assert_cmphex(mmix_cpu_initial_stack(qts, 0), ==, stack);
    g_assert_cmphex(qtest_readq(qts, stack), ==, 0);
    g_assert_cmphex(qtest_readq(
                        qts, stack + MMIX_INITIAL_STACK_SIZE -
                             sizeof(uint64_t)), ==, 0);
    qtest_quit(qts);
}

static void test_mmix_initial_stack_cpu_limit(void)
{
    uint64_t stacks[64];
    QTestState *qts = qtest_init("-machine virt -smp 64");
    uint64_t framebuffer = mmix_framebuffer_base(qts);
    unsigned int i;
    unsigned int j;

    for (i = 0; i < ARRAY_SIZE(stacks); i++) {
        stacks[i] = mmix_cpu_initial_stack(qts, i);
        g_assert_cmphex(stacks[i] % MMIX_INITIAL_STACK_ALIGN, ==, 0);
        g_assert_cmphex(stacks[i] + MMIX_INITIAL_STACK_SIZE, <=,
                        MMIX_DEFAULT_RAM_SIZE);
        g_assert_false(mmix_test_ranges_overlap(
            stacks[i], MMIX_INITIAL_STACK_SIZE, framebuffer,
            MMIX_DEFAULT_RAM_SIZE - framebuffer));
        for (j = 0; j < i; j++) {
            g_assert_false(mmix_test_ranges_overlap(
                stacks[i], MMIX_INITIAL_STACK_SIZE, stacks[j],
                MMIX_INITIAL_STACK_SIZE));
        }
    }
    qtest_quit(qts);
}

static void mmix_assert_debug_translation(QTestState *qts, uint64_t virtual,
                                          uint64_t physical)
{
    g_autofree char *expected =
        g_strdup_printf("gpa: 0x%" PRIx64, physical);
    g_autofree char *response =
        qtest_hmp(qts, "gva2gpa 0x%016" PRIx64, virtual);

    g_assert_nonnull(strstr(response, expected));
}

static void test_mmix_flat_translation_identity(void)
{
    static const uint64_t addresses[] = {
        UINT64_C(0x2000000000000000),
        UINT64_C(0x4000000000000000),
        UINT64_C(0x6000000000000000),
    };
    QTestState *qts = qtest_init("-machine virt");
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        mmix_assert_debug_translation(qts, addresses[i], addresses[i]);
    }
    qtest_quit(qts);
}

static void test_mmix_negative_alias_translation(void)
{
    static const struct {
        uint64_t start;
        uint64_t end;
    } ranges[] = {
        { UINT64_C(0x0001000000000000), UINT64_C(0x0001000010000000) },
        { UINT64_C(0x0001000010000000), UINT64_C(0x0001000020000000) },
        { UINT64_C(0x0001000020000000), UINT64_C(0x0001000030000000) },
        { UINT64_C(0x0001000030000000), UINT64_C(0x0001000040000000) },
        { UINT64_C(0x0001000040000000), UINT64_C(0x0001000080000000) },
        { UINT64_C(0x0001000080000000), UINT64_C(0x0001000100000000) },
        { UINT64_C(0x0001000100000000), UINT64_C(0x0001000110000000) },
        { UINT64_C(0x0001000200000000), UINT64_C(0x0001000300000000) },
        { UINT64_C(0x0001010000000000), UINT64_C(0x0001110000000000) },
    };
    QTestState *qts = qtest_init("-machine virt");
    unsigned int i;

    mmix_assert_debug_translation(qts, UINT64_C(0x8000000000000000), 0);
    mmix_assert_debug_translation(qts, UINT64_C(0x800000001fffffff),
                                  UINT64_C(0x000000001fffffff));
    for (i = 0; i < ARRAY_SIZE(ranges); i++) {
        uint64_t addresses[] = {
            ranges[i].start,
            ranges[i].end - 1,
            ranges[i].end,
        };
        unsigned int j;

        for (j = 0; j < ARRAY_SIZE(addresses); j++) {
            mmix_assert_debug_translation(
                qts, addresses[j] | UINT64_C(0x8000000000000000),
                addresses[j]);
        }
    }
    mmix_assert_debug_translation(qts, UINT64_MAX,
                                  UINT64_C(0x7fffffffffffffff));
    qtest_quit(qts);
}

static void test_mmix_high_physical_addresses_do_not_alias(void)
{
    static const uint64_t absent_addresses[] = {
        UINT64_C(0x0001000000000000),
        UINT64_C(0x0001000010000000),
        UINT64_C(0x0001000020000000),
        UINT64_C(0x0001000040000000),
        UINT64_C(0x0001000080000000),
        UINT64_C(0x0001000100000000),
        UINT64_C(0x0001000110000000),
        UINT64_C(0x0001000200000000),
        UINT64_C(0x0001010000000000),
        UINT64_C(0x8000000000000000),
    };
    static const struct {
        uint64_t high;
        uint64_t low;
    } addresses[] = {
        { UINT64_C(0x0001000000000000), UINT64_C(0x0000000000000000) },
        { UINT64_C(0x0001000010000000), UINT64_C(0x0000000010000000) },
    };
    QTestState *qts = qtest_init("-machine virt");
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(absent_addresses); i++) {
        qtest_writeb(qts, absent_addresses[i], 0xa5);
        g_assert_cmphex(qtest_readb(qts, absent_addresses[i]), !=, 0xa5);
    }
    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        qtest_writeb(qts, addresses[i].low, 0x5a);
        qtest_writeb(qts, addresses[i].high, 0xa5);
        g_assert_cmphex(qtest_readb(qts, addresses[i].low), ==, 0x5a);
    }
    g_assert_cmphex(qtest_readb(qts, 0), ==, 0x5a);
    qtest_quit(qts);
}

static void test_mmix_kernel_classification(gconstpointer opaque)
{
    const MMIXKernelClassificationCase *test = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *stderr_text = NULL;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "virt",
        "-kernel", NULL,
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        NULL,
    };
    int wait_status;

    directory = g_dir_make_tmp("mmix-kernel-classification-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(directory);
    filename = g_build_filename(directory, test->name, NULL);
    g_assert_true(g_file_set_contents(filename, (const char *)test->data,
                                      test->size, &error));
    g_assert_no_error(error);
    argv[4] = filename;

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text, test->diagnostic));

    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static char *mmix_create_sparse_raw(const char *directory, uint64_t size)
{
    g_autofree char *filename = g_build_filename(directory, "image.raw", NULL);
    int fd = g_open(filename, O_CREAT | O_EXCL | O_WRONLY, 0600);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return g_steal_pointer(&filename);
}

static void test_mmix_raw_minimum_and_reset(void)
{
    uint8_t image[0x104] = { 0 };
    uint8_t actual[sizeof(image)];
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("mmix-raw-direct-XXXXXX", &error);
    g_autofree char *filename = NULL;
    g_autofree char *registers = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_nonnull(directory);
    filename = g_build_filename(directory, "minimum.raw", NULL);
    image[0] = 0x11;
    image[0xff] = 0x22;
    image[0x100] = 0x33;
    image[0x103] = 0x44;
    g_assert_true(g_file_set_contents(filename, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    qts = qtest_initf("-machine virt -kernel %s", filename);
    qtest_memread(qts, 0, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), image, sizeof(image));
    registers = qtest_hmp(qts, "info registers");
    g_assert_nonnull(strstr(registers, "pc=0x0000000000000100"));

    qtest_memset(qts, 0, 0xa5, sizeof(image));
    qtest_system_reset(qts);
    qtest_memread(qts, 0, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), image, sizeof(image));
    g_clear_pointer(&registers, g_free);
    registers = qtest_hmp(qts, "info registers");
    g_assert_nonnull(strstr(registers, "pc=0x0000000000000100"));
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_mmix_raw_rejected(gconstpointer opaque)
{
    const MMIXRawRejectionCase *test = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("mmix-raw-rejected-XXXXXX", &error);
    g_autofree char *filename = NULL;
    g_autofree char *stderr_text = NULL;
    const char *argv[18] = {
        qtest_qemu_binary(NULL),
        "-machine", test->machine ?: "virt",
        "-m", test->memory ?: "128M",
        "-kernel", NULL,
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
    };
    int wait_status;

    g_assert_no_error(error);
    g_assert_nonnull(directory);
    filename = mmix_create_sparse_raw(directory, test->image_size);
    argv[6] = filename;
    if (test->option) {
        argv[13] = test->option;
        argv[14] = !strcmp(test->value, "$IMAGE") ? filename : test->value;
    }

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text, test->diagnostic));

    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
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
    static const MMIXRAMEndpointCase endpoints[] = {
        { "128M", 128 * MiB },
        { "512M", 512 * MiB },
        { "8G", 8 * GiB },
    };
    static const char * const accepted_names[] = {
        "default", "minimum", "aligned-boundary", "normalized", "above-4g",
        "maximum",
    };
    static const char * const rejected_names[] = {
        "below-minimum", "above-maximum", "maxmem", "slots", "numa",
    };
    static const char * const endpoint_names[] = {
        "minimum", "default", "above-4g",
    };
    static const uint8_t raw_image[] = { 0, 0, 0, 0 };
    static const uint8_t elf_image[EI_NIDENT] = {
        ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3,
    };
    static const uint8_t mmo_image[] = { 0x98, 0x09, 0x01, 0x00 };
    static const uint8_t truncated_elf[] = {
        ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3,
    };
    static const uint8_t truncated_mmo[] = { 0x98, 0x09 };
    static const MMIXKernelClassificationCase kernel_cases[] = {
        {
            "raw.img", raw_image, sizeof(raw_image),
            "MMIX raw kernel",
        },
        {
            "image.elf", elf_image, sizeof(elf_image),
            "MMIX ELF -kernel loading is not yet implemented",
        },
        {
            "image.mmo", mmo_image, sizeof(mmo_image),
            "MMIX MMO -kernel loading is unavailable",
        },
        {
            "truncated.elf", truncated_elf, sizeof(truncated_elf),
            "truncated MMIX ELF header",
        },
        {
            "truncated.mmo", truncated_mmo, sizeof(truncated_mmo),
            "truncated MMIX .mmo preamble",
        },
    };
    static const char * const kernel_case_names[] = {
        "raw", "elf", "mmo", "truncated-elf", "truncated-mmo",
    };
    static const MMIXRawRejectionCase raw_rejections[] = {
        {
            128 * MiB, "128M", NULL, NULL, NULL,
            "MMIX RAM reservation 'mmix-framebuffer/pixels' does not fit",
        },
        {
            128 * MiB + 1, "128M", NULL, NULL, NULL,
            "does not fit in physical RAM",
        },
        {
            125 * MiB, "128M", NULL, NULL, NULL,
            "MMIX RAM reservation 'mmix-cpu/initial-stack-0' does not fit",
        },
        {
            0x104, NULL, NULL, "-smp", "2",
            "requires exactly one CPU",
        },
        {
            0x104, NULL, "virt,elf-startup-abi=argc-argv", NULL, NULL,
            "does not support ELF startup ABI 'argc-argv'",
        },
        {
            0x104, NULL, NULL, "-semihosting-config", "enable=on,arg=x",
            "does not accept semihosting arguments",
        },
        {
            0x104, NULL, NULL, "-append", "x",
            "does not accept -append",
        },
        {
            0x104, NULL, NULL, "-initrd", "$IMAGE",
            "does not accept -initrd",
        },
    };
    static const char * const raw_rejection_names[] = {
        "ram-endpoint", "oversized", "startup-collision", "smp",
        "argc-argv", "semihosting-args", "append", "initrd",
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
    for (i = 0; i < ARRAY_SIZE(endpoints); i++) {
        g_autofree char *path =
            g_strdup_printf("/mmix/ram/endpoint/%s", endpoint_names[i]);

        qtest_add_data_func(path, &endpoints[i],
                            test_mmix_ram_exact_endpoint);
    }
    qtest_add_func("/mmix/ram/contiguous/default",
                   test_mmix_ram_contiguous_default);
    qtest_add_func("/mmix/ram/contiguous/crosses-4g",
                   test_mmix_ram_crosses_4g);
    qtest_add_func("/mmix/ram/reset/cold", test_mmix_ram_survives_reset);
    qtest_add_func("/mmix/ram/initial-stack/single-cpu",
                   test_mmix_initial_stack_single_cpu);
    qtest_add_func("/mmix/ram/initial-stack/cpu-limit",
                   test_mmix_initial_stack_cpu_limit);
    qtest_add_func("/mmix/kernel/raw/minimum-reset",
                   test_mmix_raw_minimum_and_reset);
    qtest_add_func("/mmix/translation/flat-identity",
                   test_mmix_flat_translation_identity);
    qtest_add_func("/mmix/translation/negative-alias",
                   test_mmix_negative_alias_translation);
    qtest_add_func("/mmix/translation/high-physical-no-alias",
                   test_mmix_high_physical_addresses_do_not_alias);
    for (i = 0; i < ARRAY_SIZE(kernel_cases); i++) {
        g_autofree char *path =
            g_strdup_printf("/mmix/kernel/classification/%s",
                            kernel_case_names[i]);

        qtest_add_data_func(path, &kernel_cases[i],
                            test_mmix_kernel_classification);
    }
    for (i = 0; i < ARRAY_SIZE(raw_rejections); i++) {
        g_autofree char *path =
            g_strdup_printf("/mmix/kernel/raw/rejected/%s",
                            raw_rejection_names[i]);

        qtest_add_data_func(path, &raw_rejections[i],
                            test_mmix_raw_rejected);
    }

    return g_test_run();
}
