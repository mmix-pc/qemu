/*
 * MMIX MMO record-plan tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/mmo-hosted-plan.h"
#include "hw/mmix/mmo-loader.h"
#include "hw/mmix/sparse-memory.h"
#include "qemu/bswap.h"
#include "qemu/units.h"

enum {
    MMO_ESCAPE = 0x98,
    LOP_QUOTE = 0x00,
    LOP_LOC = 0x01,
    LOP_SKIP = 0x02,
    LOP_FIXO = 0x03,
    LOP_FIXR = 0x04,
    LOP_FIXRX = 0x05,
    LOP_FILE = 0x06,
    LOP_LINE = 0x07,
    LOP_SPEC = 0x08,
    LOP_PRE = 0x09,
    LOP_POST = 0x0a,
    LOP_STAB = 0x0b,
    LOP_END = 0x0c,
};

static void image_append_tetra(GByteArray *image, uint32_t value)
{
    uint8_t tetra[4];

    stl_be_p(tetra, value);
    g_byte_array_append(image, tetra, sizeof(tetra));
}

static void image_append_octa(GByteArray *image, uint64_t value)
{
    image_append_tetra(image, value >> 32);
    image_append_tetra(image, value);
}

static void image_append_lop(GByteArray *image, uint8_t lop,
                             uint8_t y, uint8_t z)
{
    const uint8_t tetra[] = { MMO_ESCAPE, lop, y, z };

    g_byte_array_append(image, tetra, sizeof(tetra));
}

static void image_append_address_lop(GByteArray *image, uint8_t lop,
                                     uint64_t address)
{
    uint32_t high = address >> 32;
    uint32_t low = address;

    if (high & 0x00ffffff) {
        image_append_lop(image, lop, high >> 24, 2);
        image_append_tetra(image, high & 0x00ffffff);
    } else {
        image_append_lop(image, lop, high >> 24, 1);
    }
    image_append_tetra(image, low);
}

static void image_begin(GByteArray *image, uint8_t header_tetras)
{
    image_append_lop(image, LOP_PRE, 1, header_tetras);
}

static void image_append_post(GByteArray *image, uint8_t global_base,
                              uint64_t global_254, uint64_t entry)
{
    unsigned int reg;

    image_append_lop(image, LOP_POST, 0, global_base);
    for (reg = global_base; reg < 256; reg++) {
        uint64_t value = 0;

        if (reg == 254) {
            value = global_254;
        } else if (reg == 255) {
            value = entry;
        }
        image_append_octa(image, value);
    }
}

static void image_append_symbol_tail(GByteArray *image,
                                     const uint32_t *symbols,
                                     size_t symbol_count)
{
    size_t i;

    image_append_lop(image, LOP_STAB, 0, 0);
    for (i = 0; i < symbol_count; i++) {
        image_append_tetra(image, symbols[i]);
    }
    image_append_lop(image, LOP_END, symbol_count >> 8, symbol_count);
}

static GByteArray *minimal_image(uint64_t entry)
{
    GByteArray *image = g_byte_array_new();

    image_begin(image, 0);
    image_append_tetra(image, 0xf0000000);
    image_append_post(image, 255, 0, entry);
    image_append_symbol_tail(image, NULL, 0);
    return image;
}

static bool parse_image(const GByteArray *image, MMIXMMOPlan **plan,
                        Error **errp)
{
    g_autofree char *filename = NULL;
    g_autoptr(GError) gerror = NULL;
    bool result;
    int fd;

    fd = g_file_open_tmp("mmix-mmo-plan-XXXXXX", &filename, &gerror);
    g_assert_no_error(gerror);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(qemu_write_full(fd, image->data, image->len), ==,
                    image->len);
    g_assert_cmpint(close(fd), ==, 0);

    result = mmix_mmo_plan_parse(filename, plan, errp);
    g_assert_cmpint(unlink(filename), ==, 0);
    return result;
}

static void assert_write(const MMIXMMOPlan *plan, size_t index,
                         uint64_t address, uint32_t value,
                         MMIXMMOWriteKind kind, uint64_t source_tetra)
{
    const MMIXMMOWrite *write = mmix_mmo_plan_write(plan, index);

    g_assert_cmphex(write->address, ==, address);
    g_assert_cmphex(write->value, ==, value);
    g_assert_cmpint(write->kind, ==, kind);
    g_assert_cmpuint(write->source_tetra, ==, source_tetra);
}

static void assert_parse_fails(const GByteArray *image,
                               const char *diagnostic)
{
    MMIXMMOPlan *plan = NULL;
    Error *err = NULL;

    g_assert_false(parse_image(image, &plan, &err));
    g_assert_null(plan);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    error_free(err);
}

static void test_complete_record_plan(void)
{
    static const uint32_t symbols[] = { 0x12345678 };
    GByteArray *image = g_byte_array_new();
    MMIXMMOPlan *plan = NULL;
    const MMIXKernelLoadInfo *info;

    image_begin(image, 1);
    image_append_tetra(image, 0xdeadbeef);
    image_append_tetra(image, 0x11223344);
    image_append_lop(image, LOP_QUOTE, 0, 1);
    image_append_tetra(image, 0x98090000);
    image_append_lop(image, LOP_FILE, 7, 1);
    image_append_tetra(image, 0x666f6f00);
    image_append_lop(image, LOP_LINE, 0x12, 0x34);
    image_append_lop(image, LOP_SPEC, 0, 1);
    image_append_tetra(image, 0xaabbccdd);
    image_append_lop(image, LOP_QUOTE, 0, 1);
    image_append_tetra(image, 0x980a0000);
    image_append_address_lop(image, LOP_LOC,
                             MMIX_SPARSE_DATA_BASE + 0x100);
    image_append_tetra(image, 0x55667788);
    image_append_lop(image, LOP_SKIP, 0, 4);
    image_append_tetra(image, 0x99aabbcc);
    image_append_address_lop(image, LOP_LOC, 0x20);
    image_append_address_lop(image, LOP_FIXO,
                             MMIX_SPARSE_POOL_BASE + 0x100);
    image_append_address_lop(image, LOP_LOC, 0x10);
    image_append_lop(image, LOP_FIXR, 0, 2);
    image_append_lop(image, LOP_FIXRX, 0, 16);
    image_append_tetra(image, 1);
    image_append_post(image, 254, UINT64_C(0x0123456789abcdef), 0);
    image_append_symbol_tail(image, symbols, ARRAY_SIZE(symbols));

    g_assert_true(parse_image(image, &plan, &error_abort));
    g_assert_cmpuint(mmix_mmo_plan_write_count(plan), ==, 8);
    assert_write(plan, 0, 0, 0x11223344, MMIX_MMO_WRITE_DATA, 3);
    assert_write(plan, 1, 4, 0x98090000, MMIX_MMO_WRITE_DATA, 4);
    assert_write(plan, 2, MMIX_SPARSE_DATA_BASE + 0x100, 0x55667788,
                 MMIX_MMO_WRITE_DATA, 15);
    assert_write(plan, 3, MMIX_SPARSE_DATA_BASE + 0x108, 0x99aabbcc,
                 MMIX_MMO_WRITE_DATA, 17);
    assert_write(plan, 4, MMIX_SPARSE_POOL_BASE + 0x100, 0,
                 MMIX_MMO_WRITE_FIXO, 20);
    assert_write(plan, 5, MMIX_SPARSE_POOL_BASE + 0x104, 0x20,
                 MMIX_MMO_WRITE_FIXO, 20);
    assert_write(plan, 6, 8, 2, MMIX_MMO_WRITE_FIXR, 24);
    assert_write(plan, 7, 0xc, 1, MMIX_MMO_WRITE_FIXRX, 25);

    info = mmix_mmo_plan_load_info(plan);
    g_assert_cmpint(info->image_type, ==, MMIX_KERNEL_IMAGE_MMO);
    g_assert_cmpuint(info->boot_cpu_id, ==, 0);
    g_assert_true(info->has_global_registers);
    g_assert_cmpuint(info->global_base, ==, 254);
    g_assert_cmpuint(info->global_count, ==, 2);
    g_assert_cmphex(info->globals[254], ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(info->globals[255], ==, 0);
    g_assert_cmphex(info->entry, ==, 0);

    mmix_mmo_plan_free(plan);
    g_byte_array_unref(image);
}

static void test_full_logical_segments(void)
{
    static const uint64_t addresses[] = {
        MMIX_SPARSE_DATA_BASE + (UINT64_C(1) << 40),
        MMIX_SPARSE_POOL_BASE + (UINT64_C(1) << 48),
        MMIX_SPARSE_LIMIT - 4,
    };
    GByteArray *image = g_byte_array_new();
    MMIXMMOPlan *plan = NULL;
    size_t i;

    image_begin(image, 0);
    image_append_tetra(image, 0x01020304);
    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        image_append_address_lop(image, LOP_LOC, addresses[i]);
        image_append_tetra(image, 0x11111111 + i);
    }
    image_append_post(image, 255, 0, 0);
    image_append_symbol_tail(image, NULL, 0);

    g_assert_true(parse_image(image, &plan, &error_abort));
    g_assert_cmpuint(mmix_mmo_plan_write_count(plan), ==, 4);
    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        assert_write(plan, i + 1, addresses[i], 0x11111111 + i,
                     MMIX_MMO_WRITE_DATA, 6 + 4 * i);
    }

    mmix_mmo_plan_free(plan);
    g_byte_array_unref(image);
}

static void test_location_and_fixup_failures(void)
{
    GByteArray *image;

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_address_lop(image, LOP_LOC, MMIX_SPARSE_LIMIT);
    assert_parse_fails(image, "outside the nonnegative logical segments");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_address_lop(image, LOP_LOC, 3);
    image_append_tetra(image, 0);
    assert_parse_fails(image, "unaligned MMIX sparse-memory access");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_address_lop(image, LOP_LOC,
                             MMIX_SPARSE_DATA_BASE - 4);
    image_append_lop(image, LOP_SKIP, 0, 8);
    assert_parse_fails(image, "crosses a logical-segment boundary");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_FIXR, 0, 1);
    assert_parse_fails(image, "lop_fixr target before address 0");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_FIXO, 0, 0);
    assert_parse_fails(image, "invalid MMIX .mmo lop_fixo z=0");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_FIXRX, 0, 8);
    assert_parse_fails(image, "invalid MMIX .mmo lop_fixrx yz=8");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_FIXRX, 0, 16);
    assert_parse_fails(image, "truncated MMIX .mmo object");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_FIXRX, 0, 24);
    image_append_tetra(image, 0x02000000);
    assert_parse_fails(image, "lop_fixrx delta 0x02000000");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_FIXRX, 0, 16);
    image_append_tetra(image, 0x00010000);
    assert_parse_fails(image, "delta 0x00010000 for 16-bit relative fixup");
    g_byte_array_unref(image);
}

static void test_entry_failures(void)
{
    GByteArray *image;

    image = minimal_image(1);
    assert_parse_fails(image, "unaligned MMIX .mmo Main entry");
    g_byte_array_unref(image);

    image = minimal_image(MMIX_SPARSE_DATA_BASE);
    assert_parse_fails(image, "is outside Text");
    g_byte_array_unref(image);

    image = minimal_image(4);
    assert_parse_fails(image, "does not identify an initialized Text");
    g_byte_array_unref(image);
}

static void test_structure_failures(void)
{
    GByteArray *image;

    image = g_byte_array_new();
    image_append_lop(image, LOP_PRE, 2, 0);
    assert_parse_fails(image, "unsupported MMIX .mmo preamble version 2");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 1);
    assert_parse_fails(image, "truncated MMIX .mmo object");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    assert_parse_fails(image, "missing MMIX .mmo lop_post");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_QUOTE, 0, 2);
    assert_parse_fails(image, "invalid MMIX .mmo lop_quote yz=2");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_QUOTE, 0, 1);
    assert_parse_fails(image, "truncated MMIX .mmo object");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_SPEC, 0, 1);
    assert_parse_fails(image, "unterminated MMIX .mmo lop_spec");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_SPEC, 0, 1);
    image_append_lop(image, LOP_QUOTE, 0, 2);
    assert_parse_fails(image, "lop_quote yz=2 in lop_spec");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, LOP_SPEC, 0, 1);
    image_append_lop(image, LOP_QUOTE, 0, 1);
    assert_parse_fails(image, "truncated MMIX .mmo object");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_lop(image, LOP_POST, 1, 255);
    assert_parse_fails(image, "invalid MMIX .mmo lop_post y=1");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_lop(image, LOP_POST, 0, 31);
    assert_parse_fails(image, "invalid MMIX .mmo lop_post z=31");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_lop(image, LOP_POST, 0, 255);
    image_append_tetra(image, 0);
    assert_parse_fails(image, "truncated MMIX .mmo object");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_post(image, 255, 0, 0);
    image_append_tetra(image, 0);
    assert_parse_fails(image, "expected MMIX .mmo lop_stab");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_post(image, 255, 0, 0);
    image_append_lop(image, LOP_STAB, 0, 1);
    assert_parse_fails(image, "invalid MMIX .mmo lop_stab yz=1");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_post(image, 255, 0, 0);
    image_append_lop(image, LOP_STAB, 0, 0);
    assert_parse_fails(image, "missing MMIX .mmo lop_end");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_tetra(image, 0);
    image_append_post(image, 255, 0, 0);
    image_append_lop(image, LOP_STAB, 0, 0);
    image_append_tetra(image, 0x12345678);
    image_append_lop(image, LOP_END, 0, 0);
    assert_parse_fails(image, "expected 1");
    g_byte_array_unref(image);

    image = minimal_image(0);
    image_append_tetra(image, 0);
    assert_parse_fails(image, "data after lop_end");
    g_byte_array_unref(image);

    image = g_byte_array_new();
    image_begin(image, 0);
    image_append_lop(image, 0x7f, 0, 0);
    assert_parse_fails(image, "unsupported MMIX .mmo lopcode 0x7f");
    g_byte_array_unref(image);
}

static void test_failure_preserves_plan(void)
{
    GByteArray *valid = minimal_image(0);
    GByteArray *invalid = g_byte_array_new();
    MMIXMMOPlan *plan = NULL;
    MMIXMMOPlan *original;
    Error *err = NULL;

    g_assert_true(parse_image(valid, &plan, &error_abort));
    original = plan;
    image_begin(invalid, 0);
    image_append_lop(invalid, LOP_POST, 1, 255);

    g_assert_false(parse_image(invalid, &plan, &err));
    g_assert_true(plan == original);
    g_assert_cmpuint(mmix_mmo_plan_write_count(plan), ==, 1);
    g_assert_nonnull(strstr(error_get_pretty(err), "lop_post y=1"));

    error_free(err);
    mmix_mmo_plan_free(plan);
    g_byte_array_unref(invalid);
    g_byte_array_unref(valid);
}

static MMIXMMOHostedOptions hosted_options(void)
{
    return (MMIXMMOHostedOptions) {
        .kernel_filename = "program.mmo",
        .sparse_budget = 8 * MMIX_SPARSE_PAGE_SIZE,
        .cpu_count = 1,
    };
}

static void assert_hosted_plan_fails(const MMIXMMOPlan *mmo,
                                     const MMIXMMOHostedOptions *options,
                                     const char *diagnostic)
{
    MMIXMMOHostedPlan *hosted = NULL;
    Error *err = NULL;

    g_assert_false(mmix_mmo_hosted_plan_build(mmo, options, &hosted, &err));
    g_assert_null(hosted);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    error_free(err);
}

static void test_hosted_fallback_arguments(void)
{
    GByteArray *image = minimal_image(0);
    MMIXMMOHostedOptions options = hosted_options();
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXMMOPlan *mmo = NULL;
    const uint8_t *data;
    size_t size;

    options.append = "alpha  beta";
    g_assert_true(parse_image(image, &mmo, &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_build(mmo, &options, &hosted,
                                             &error_abort));

    data = mmix_mmo_hosted_plan_argument_data(hosted, &size);
    g_assert_cmpuint(mmix_mmo_hosted_plan_argument_count(hosted), ==, 3);
    g_assert_cmphex(mmix_mmo_hosted_plan_argv(hosted), ==,
                    MMIX_SPARSE_POOL_BASE + 8);
    g_assert_cmphex(mmix_mmo_hosted_plan_argument_end(hosted), ==,
                    MMIX_SPARSE_POOL_BASE + 72);
    g_assert_cmpuint(size, ==, 72);
    g_assert_cmphex(ldq_be_p(data), ==, MMIX_SPARSE_POOL_BASE + 72);
    g_assert_cmphex(ldq_be_p(data + 8), ==,
                    MMIX_SPARSE_POOL_BASE + 40);
    g_assert_cmphex(ldq_be_p(data + 16), ==,
                    MMIX_SPARSE_POOL_BASE + 56);
    g_assert_cmphex(ldq_be_p(data + 24), ==,
                    MMIX_SPARSE_POOL_BASE + 64);
    g_assert_cmphex(ldq_be_p(data + 32), ==, 0);
    g_assert_cmpstr((const char *)data + 40, ==, "program.mmo");
    g_assert_cmpstr((const char *)data + 56, ==, "alpha");
    g_assert_cmpstr((const char *)data + 64, ==, "beta");
    g_assert_cmpuint(mmix_mmo_hosted_plan_materialized_pages(hosted), ==, 2);
    g_assert_cmphex(mmix_mmo_hosted_plan_materialized_bytes(hosted), ==,
                    2 * MMIX_SPARSE_PAGE_SIZE);

    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static void test_hosted_explicit_arguments(void)
{
    static const char *const arguments[] = { "", "two words" };
    GByteArray *image = minimal_image(0);
    MMIXMMOHostedOptions options = hosted_options();
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXMMOPlan *mmo = NULL;
    const uint8_t *data;
    size_t size;

    options.explicit_arguments = arguments;
    options.explicit_argument_count = ARRAY_SIZE(arguments);
    options.has_explicit_arguments = true;
    options.semihosting_enabled = true;
    g_assert_true(parse_image(image, &mmo, &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_build(mmo, &options, &hosted,
                                             &error_abort));

    data = mmix_mmo_hosted_plan_argument_data(hosted, &size);
    g_assert_cmpuint(mmix_mmo_hosted_plan_argument_count(hosted), ==, 2);
    g_assert_cmpuint(size, ==, 56);
    g_assert_cmphex(ldq_be_p(data), ==, MMIX_SPARSE_POOL_BASE + 56);
    g_assert_cmphex(ldq_be_p(data + 8), ==,
                    MMIX_SPARSE_POOL_BASE + 32);
    g_assert_cmphex(ldq_be_p(data + 16), ==,
                    MMIX_SPARSE_POOL_BASE + 40);
    g_assert_cmphex(ldq_be_p(data + 24), ==, 0);
    g_assert_cmpstr((const char *)data + 32, ==, "");
    g_assert_cmpstr((const char *)data + 40, ==, "two words");

    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static void test_hosted_option_policy(void)
{
    static const char *const argument[] = { "program.mmo" };
    GByteArray *image = minimal_image(0);
    MMIXMMOHostedOptions options;
    MMIXMMOPlan *mmo = NULL;

    g_assert_true(parse_image(image, &mmo, &error_abort));

    options = hosted_options();
    options.cpu_count = 2;
    assert_hosted_plan_fails(mmo, &options, "exactly one CPU");
    options = hosted_options();
    options.has_initrd = true;
    assert_hosted_plan_fails(mmo, &options, "does not accept -initrd");
    options = hosted_options();
    options.has_explicit_elf_startup_abi = true;
    assert_hosted_plan_fails(mmo, &options, "explicit ELF startup ABI");
    options = hosted_options();
    options.has_firmware = true;
    assert_hosted_plan_fails(mmo, &options, "does not accept firmware");
    options = hosted_options();
    options.linux_handoff = true;
    assert_hosted_plan_fails(mmo, &options, "Linux handoff");
    options = hosted_options();
    options.explicit_arguments = argument;
    options.explicit_argument_count = 1;
    options.has_explicit_arguments = true;
    assert_hosted_plan_fails(mmo, &options, "require semihosting");
    options.semihosting_enabled = true;
    options.append = "conflict";
    assert_hosted_plan_fails(mmo, &options,
                             "explicit semihosting arguments with -append");
    options = hosted_options();
    options.has_explicit_arguments = true;
    options.semihosting_enabled = true;
    assert_hosted_plan_fails(mmo, &options,
                             "argument selection is inconsistent");
    options = hosted_options();
    options.sparse_budget--;
    assert_hosted_plan_fails(mmo, &options, "not 8 KiB aligned");

    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static void test_hosted_argument_limit(void)
{
    GByteArray *image = minimal_image(0);
    MMIXMMOHostedOptions options = hosted_options();
    g_autofree char *large = g_malloc0(8 * MiB + 1);
    const char *arguments[] = { large };
    MMIXMMOPlan *mmo = NULL;

    memset(large, 'x', 8 * MiB);
    options.explicit_arguments = arguments;
    options.explicit_argument_count = ARRAY_SIZE(arguments);
    options.has_explicit_arguments = true;
    options.semihosting_enabled = true;
    g_assert_true(parse_image(image, &mmo, &error_abort));
    assert_hosted_plan_fails(mmo, &options, "exceeds 8 MiB");

    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static GByteArray *image_with_write(uint64_t address)
{
    GByteArray *image = g_byte_array_new();

    image_begin(image, 0);
    image_append_tetra(image, 0xf0000000);
    image_append_address_lop(image, LOP_LOC, address);
    image_append_tetra(image, 0x11223344);
    image_append_post(image, 255, 0, 0);
    image_append_symbol_tail(image, NULL, 0);
    return image;
}

static void test_hosted_collision_and_budget(void)
{
    MMIXMMOHostedOptions options = hosted_options();
    GByteArray *image;
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXMMOPlan *mmo = NULL;

    image = image_with_write(MMIX_SPARSE_POOL_BASE);
    g_assert_true(parse_image(image, &mmo, &error_abort));
    assert_hosted_plan_fails(mmo, &options,
                             "overlaps the Pool argument block");
    mmix_mmo_plan_free(mmo);
    mmo = NULL;
    g_byte_array_unref(image);

    image = image_with_write(MMIX_SPARSE_POOL_BASE + 40);
    g_assert_true(parse_image(image, &mmo, &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_build(mmo, &options, &hosted,
                                             &error_abort));
    g_assert_cmpuint(mmix_mmo_hosted_plan_materialized_pages(hosted), ==, 2);
    mmix_mmo_hosted_plan_free(hosted);
    hosted = NULL;
    mmix_mmo_plan_free(mmo);
    mmo = NULL;
    g_byte_array_unref(image);

    image = image_with_write(MMIX_SPARSE_DATA_BASE +
                             4 * MMIX_SPARSE_PAGE_SIZE);
    g_assert_true(parse_image(image, &mmo, &error_abort));
    options.sparse_budget = 2 * MMIX_SPARSE_PAGE_SIZE;
    assert_hosted_plan_fails(mmo, &options, "exceeding sparse budget");
    options.sparse_budget = 3 * MMIX_SPARSE_PAGE_SIZE;
    g_assert_true(mmix_mmo_hosted_plan_build(mmo, &options, &hosted,
                                             &error_abort));
    g_assert_cmpuint(mmix_mmo_hosted_plan_materialized_pages(hosted), ==, 3);

    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static void test_hosted_failure_preserves_plan(void)
{
    GByteArray *image = minimal_image(0);
    MMIXMMOHostedOptions options = hosted_options();
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXMMOHostedPlan *original;
    MMIXMMOPlan *mmo = NULL;
    Error *err = NULL;

    g_assert_true(parse_image(image, &mmo, &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_build(mmo, &options, &hosted,
                                             &error_abort));
    original = hosted;
    options.cpu_count = 0;
    g_assert_false(mmix_mmo_hosted_plan_build(mmo, &options, &hosted, &err));
    g_assert_true(hosted == original);
    g_assert_nonnull(err);
    error_free(err);

    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static uint32_t sparse_read_tetra(MMIXSparseMemory *memory,
                                  uint64_t address)
{
    uint8_t data[4];

    g_assert_true(mmix_sparse_memory_read(memory, address, data,
                                          sizeof(data), sizeof(data),
                                          &error_abort));
    return ldl_be_p(data);
}

static uint64_t sparse_read_octa(MMIXSparseMemory *memory,
                                 uint64_t address)
{
    uint8_t data[8];

    g_assert_true(mmix_sparse_memory_read(memory, address, data,
                                          sizeof(data), sizeof(data),
                                          &error_abort));
    return ldq_be_p(data);
}

static GByteArray *hosted_commit_image(void)
{
    const uint64_t data = MMIX_SPARSE_DATA_BASE +
                          4 * MMIX_SPARSE_PAGE_SIZE;
    const uint64_t pool = MMIX_SPARSE_POOL_BASE + 0x100;
    const uint64_t stack = MMIX_SPARSE_STACK_BASE + (UINT64_C(1) << 40);
    GByteArray *image = g_byte_array_new();

    image_begin(image, 0);
    image_append_tetra(image, 0x01020304);
    image_append_lop(image, LOP_FIXR, 0, 1);
    image_append_lop(image, LOP_FIXRX, 0, 16);
    image_append_tetra(image, 1);
    image_append_address_lop(image, LOP_LOC, data);
    image_append_tetra(image, 0x11223344);
    image_append_address_lop(image, LOP_LOC, data);
    image_append_tetra(image, 0x01010101);
    image_append_address_lop(image, LOP_LOC, pool);
    image_append_tetra(image, 0xaabbccdd);
    image_append_tetra(image, 0x55667788);
    image_append_address_lop(image, LOP_LOC,
                             MMIX_SPARSE_DATA_BASE + 0x200);
    image_append_address_lop(image, LOP_FIXO, pool);
    image_append_address_lop(image, LOP_FIXO, pool);
    image_append_address_lop(image, LOP_LOC, stack);
    image_append_tetra(image, 0xdeadbeef);
    image_append_post(image, 255, 0, 0);
    image_append_symbol_tail(image, NULL, 0);
    return image;
}

static void test_hosted_commit_sparse_memory(void)
{
    const uint64_t data_address = MMIX_SPARSE_DATA_BASE +
                                  4 * MMIX_SPARSE_PAGE_SIZE;
    const uint64_t pool_address = MMIX_SPARSE_POOL_BASE + 0x100;
    const uint64_t stack_address = MMIX_SPARSE_STACK_BASE +
                                   (UINT64_C(1) << 40);
    GByteArray *image = hosted_commit_image();
    MMIXMMOHostedOptions options = hosted_options();
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXSparseMemory *memory = NULL;
    MMIXMMOPlan *mmo = NULL;
    const uint8_t *argument_data;
    g_autofree uint8_t *loaded_arguments = NULL;
    size_t argument_size;
    uint8_t hole[16];

    options.sparse_budget = 4 * MMIX_SPARSE_PAGE_SIZE;
    g_assert_true(parse_image(image, &mmo, &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_build(mmo, &options, &hosted,
                                             &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_commit(mmo, hosted, &memory,
                                              &error_abort));

    g_assert_cmphex(sparse_read_tetra(memory, 0), ==, 0x01020304);
    g_assert_cmphex(sparse_read_tetra(memory, data_address), ==,
                    0x10233245);
    g_assert_cmphex(sparse_read_octa(memory, pool_address), ==,
                    UINT64_C(0xaabbccdd55667788));
    g_assert_cmphex(sparse_read_tetra(memory, stack_address), ==,
                    0xdeadbeef);
    memset(hole, 0xff, sizeof(hole));
    g_assert_true(mmix_sparse_memory_read(
        memory, MMIX_SPARSE_DATA_BASE + 8 * MMIX_SPARSE_PAGE_SIZE,
        hole, sizeof(hole), 1, &error_abort));
    g_assert_cmpmem(hole, sizeof(hole),
                    (const uint8_t[sizeof(hole)]) { 0 }, sizeof(hole));

    argument_data = mmix_mmo_hosted_plan_argument_data(hosted,
                                                        &argument_size);
    loaded_arguments = g_malloc(argument_size);
    g_assert_true(mmix_sparse_memory_read(
        memory, MMIX_SPARSE_POOL_BASE, loaded_arguments, argument_size,
        sizeof(uint64_t), &error_abort));
    g_assert_cmpmem(loaded_arguments, argument_size,
                    argument_data, argument_size);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 4);

    mmix_sparse_memory_free(memory);
    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

static void test_hosted_commit_failure_preserves_store(void)
{
    const uint8_t marker[] = { 0x5a, 0xa5 };
    GByteArray *small_image = minimal_image(0);
    GByteArray *large_image = hosted_commit_image();
    MMIXMMOHostedOptions options = hosted_options();
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXSparseMemory *memory;
    MMIXSparseMemory *original;
    MMIXMMOPlan *small_mmo = NULL;
    MMIXMMOPlan *large_mmo = NULL;
    uint8_t output[sizeof(marker)] = { 0 };
    Error *err = NULL;

    options.sparse_budget = 2 * MMIX_SPARSE_PAGE_SIZE;
    g_assert_true(parse_image(small_image, &small_mmo, &error_abort));
    g_assert_true(parse_image(large_image, &large_mmo, &error_abort));
    g_assert_true(mmix_mmo_hosted_plan_build(
        small_mmo, &options, &hosted, &error_abort));

    memory = mmix_sparse_memory_new(MMIX_SPARSE_PAGE_SIZE, &error_abort);
    g_assert_true(mmix_sparse_memory_write(memory, 0x100, marker,
                                           sizeof(marker), 1,
                                           &error_abort));
    original = memory;
    g_assert_false(mmix_mmo_hosted_plan_commit(large_mmo, hosted,
                                               &memory, &err));
    g_assert_true(memory == original);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "only 0 available"));
    error_free(err);
    g_assert_true(mmix_sparse_memory_read(memory, 0x100, output,
                                          sizeof(output), 1,
                                          &error_abort));
    g_assert_cmpmem(output, sizeof(output), marker, sizeof(marker));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 1);

    mmix_sparse_memory_free(memory);
    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(large_mmo);
    mmix_mmo_plan_free(small_mmo);
    g_byte_array_unref(large_image);
    g_byte_array_unref(small_image);
}

static void test_hosted_commit_machine_budgets(void)
{
    static const uint64_t budgets[] = {
        128 * MiB,
        512 * MiB,
        4 * GiB + 128 * MiB,
    };
    GByteArray *image = hosted_commit_image();
    MMIXMMOHostedOptions options = hosted_options();
    MMIXMMOHostedPlan *hosted = NULL;
    MMIXSparseMemory *memory = NULL;
    MMIXMMOPlan *mmo = NULL;
    size_t i;

    g_assert_true(parse_image(image, &mmo, &error_abort));
    for (i = 0; i < ARRAY_SIZE(budgets); i++) {
        options.sparse_budget = budgets[i];
        g_assert_true(mmix_mmo_hosted_plan_build(
            mmo, &options, &hosted, &error_abort));
        g_assert_true(mmix_mmo_hosted_plan_commit(
            mmo, hosted, &memory, &error_abort));
        g_assert_cmphex(mmix_sparse_memory_budget(memory), ==, budgets[i]);
        g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==,
                         4);
        g_assert_cmphex(sparse_read_tetra(
                            memory, MMIX_SPARSE_STACK_BASE +
                                    (UINT64_C(1) << 40)),
                        ==, 0xdeadbeef);
    }

    mmix_sparse_memory_free(memory);
    mmix_mmo_hosted_plan_free(hosted);
    mmix_mmo_plan_free(mmo);
    g_byte_array_unref(image);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/mmo-plan/complete-record-plan",
                    test_complete_record_plan);
    g_test_add_func("/mmix/mmo-plan/full-logical-segments",
                    test_full_logical_segments);
    g_test_add_func("/mmix/mmo-plan/location-and-fixup-failures",
                    test_location_and_fixup_failures);
    g_test_add_func("/mmix/mmo-plan/entry-failures", test_entry_failures);
    g_test_add_func("/mmix/mmo-plan/structure-failures",
                    test_structure_failures);
    g_test_add_func("/mmix/mmo-plan/failure-preserves-plan",
                    test_failure_preserves_plan);
    g_test_add_func("/mmix/mmo-hosted/fallback-arguments",
                    test_hosted_fallback_arguments);
    g_test_add_func("/mmix/mmo-hosted/explicit-arguments",
                    test_hosted_explicit_arguments);
    g_test_add_func("/mmix/mmo-hosted/option-policy",
                    test_hosted_option_policy);
    g_test_add_func("/mmix/mmo-hosted/argument-limit",
                    test_hosted_argument_limit);
    g_test_add_func("/mmix/mmo-hosted/collision-and-budget",
                    test_hosted_collision_and_budget);
    g_test_add_func("/mmix/mmo-hosted/failure-preserves-plan",
                    test_hosted_failure_preserves_plan);
    g_test_add_func("/mmix/mmo-hosted/commit-sparse-memory",
                    test_hosted_commit_sparse_memory);
    g_test_add_func("/mmix/mmo-hosted/commit-failure-preserves-store",
                    test_hosted_commit_failure_preserves_store);
    g_test_add_func("/mmix/mmo-hosted/commit-machine-budgets",
                    test_hosted_commit_machine_budgets);

    return g_test_run();
}
