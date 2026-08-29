/*
 * MMIX virt flattened device tree builder tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/fdt-builder.h"
#include "hw/mmix/physical-layout.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include <libfdt.h>

static GBytes *build_fdt(uint64_t ram_size, const char *command_line)
{
    MMIXFDTConfig config = {
        .ram_size = ram_size,
        .command_line = command_line,
    };
    GBytes *blob = NULL;
    Error *err = NULL;

    g_assert_true(mmix_fdt_build(&config, &blob, &err));
    g_assert_null(err);
    return blob;
}

static int node_offset(const void *fdt, const char *path)
{
    int node = fdt_path_offset(fdt, path);

    g_assert_cmpint(node, >=, 0);
    return node;
}

static void assert_string(const void *fdt, const char *path,
                          const char *name, const char *expected)
{
    int length;
    const char *actual = fdt_getprop(fdt, node_offset(fdt, path), name,
                                     &length);

    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, strlen(expected) + 1);
    g_assert_cmpstr(actual, ==, expected);
}

static void assert_u32(const void *fdt, const char *path,
                       const char *name, uint32_t expected)
{
    int length;
    const fdt32_t *actual = fdt_getprop(fdt, node_offset(fdt, path), name,
                                        &length);

    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, sizeof(*actual));
    g_assert_cmpuint(be32_to_cpu(*actual), ==, expected);
}

static void assert_empty(const void *fdt, const char *path, const char *name)
{
    int length;
    const void *actual = fdt_getprop(fdt, node_offset(fdt, path), name,
                                     &length);

    g_assert_nonnull(actual);
    g_assert_cmpint(length, ==, 0);
}

static void assert_foundation(const void *fdt, size_t size,
                              uint64_t ram_size, const char *command_line)
{
    const fdt64_t *reg;
    int length;

    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    g_assert_cmpuint(fdt_version(fdt), ==, 17);
    g_assert_cmpuint(fdt_totalsize(fdt), ==, size);
    assert_string(fdt, "/", "compatible", "qemu,mmix-virt");
    assert_string(fdt, "/", "model", "QEMU MMIX Virt Machine");
    assert_u32(fdt, "/", "#address-cells", 2);
    assert_u32(fdt, "/", "#size-cells", 2);
    node_offset(fdt, "/aliases");
    assert_string(fdt, "/chosen", "bootargs", command_line);
    assert_string(fdt, "/memory@0", "device_type", "memory");
    reg = fdt_getprop(fdt, node_offset(fdt, "/memory@0"), "reg", &length);
    g_assert_nonnull(reg);
    g_assert_cmpint(length, ==, 2 * sizeof(*reg));
    g_assert_cmphex(be64_to_cpu(reg[0]), ==, 0);
    g_assert_cmphex(be64_to_cpu(reg[1]), ==, ram_size);
    assert_u32(fdt, "/reserved-memory", "#address-cells", 2);
    assert_u32(fdt, "/reserved-memory", "#size-cells", 2);
    assert_empty(fdt, "/reserved-memory", "ranges");
    assert_string(fdt, "/soc", "compatible", "simple-bus");
    assert_u32(fdt, "/soc", "#address-cells", 2);
    assert_u32(fdt, "/soc", "#size-cells", 2);
    assert_empty(fdt, "/soc", "ranges");
}

static void test_default_foundation(void)
{
    g_autoptr(GBytes) blob = build_fdt(MMIX_VIRT_RAM_DEFAULT_SIZE, "");
    gsize size;
    const void *fdt = g_bytes_get_data(blob, &size);

    assert_foundation(fdt, size, MMIX_VIRT_RAM_DEFAULT_SIZE, "");
}

static void test_large_ram_and_command_line(void)
{
    const char *command_line = "console=ttyS0 root=/dev/vda";
    g_autoptr(GBytes) blob = build_fdt(8 * GiB, command_line);
    gsize size;
    const void *fdt = g_bytes_get_data(blob, &size);

    assert_foundation(fdt, size, 8 * GiB, command_line);
}

static void test_deterministic_output(void)
{
    g_autoptr(GBytes) first = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "test");
    g_autoptr(GBytes) second = build_fdt(MMIX_VIRT_RAM_MIN_SIZE, "test");

    g_assert_true(g_bytes_equal(first, second));
}

static void assert_invalid_config(const MMIXFDTConfig *config,
                                  const char *diagnostic)
{
    g_autoptr(GBytes) original = build_fdt(MMIX_VIRT_RAM_DEFAULT_SIZE, "");
    GBytes *result = g_bytes_ref(original);
    Error *err = NULL;

    g_assert_false(mmix_fdt_build(config, &result, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    g_assert_true(result == original);
    error_free(err);
    g_bytes_unref(result);
}

static void test_invalid_inputs_preserve_result(void)
{
    MMIXFDTConfig below_minimum = {
        .ram_size = MMIX_VIRT_RAM_MIN_SIZE - MMIX_VIRT_RAM_ALIGN,
        .command_line = "",
    };
    MMIXFDTConfig above_maximum = {
        .ram_size = MMIX_VIRT_RAM_MAX_SIZE + MMIX_VIRT_RAM_ALIGN,
        .command_line = "",
    };
    MMIXFDTConfig unaligned = {
        .ram_size = MMIX_VIRT_RAM_MIN_SIZE + 1,
        .command_line = "",
    };
    MMIXFDTConfig missing_command_line = {
        .ram_size = MMIX_VIRT_RAM_MIN_SIZE,
    };
    g_autofree char *long_command_line =
        g_strnfill(MMIX_FDT_COMMAND_LINE_MAX + 1, 'x');
    MMIXFDTConfig oversized_command_line = {
        .ram_size = MMIX_VIRT_RAM_MIN_SIZE,
        .command_line = long_command_line,
    };

    assert_invalid_config(NULL, "configuration is missing");
    assert_invalid_config(&below_minimum, "RAM size");
    assert_invalid_config(&above_maximum, "RAM size");
    assert_invalid_config(&unaligned, "RAM size");
    assert_invalid_config(&missing_command_line, "command line is missing");
    assert_invalid_config(&oversized_command_line,
                          "command line exceeds 4095 bytes");
}

static void test_missing_result_pointer(void)
{
    MMIXFDTConfig config = {
        .ram_size = MMIX_VIRT_RAM_MIN_SIZE,
        .command_line = "",
    };
    Error *err = NULL;

    g_assert_false(mmix_fdt_build(&config, NULL, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "result pointer is missing"));
    error_free(err);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/fdt-builder/default-foundation",
                    test_default_foundation);
    g_test_add_func("/mmix/fdt-builder/large-ram-command-line",
                    test_large_ram_and_command_line);
    g_test_add_func("/mmix/fdt-builder/deterministic-output",
                    test_deterministic_output);
    g_test_add_func("/mmix/fdt-builder/invalid-inputs-preserve-result",
                    test_invalid_inputs_preserve_result);
    g_test_add_func("/mmix/fdt-builder/missing-result-pointer",
                    test_missing_result_pointer);
    return g_test_run();
}
