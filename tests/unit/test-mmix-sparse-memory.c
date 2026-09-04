/*
 * MMIX hosted sparse logical-memory tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/sparse-memory.h"

static void assert_invalid_range(uint64_t address, size_t size,
                                 size_t alignment, const char *diagnostic)
{
    MMIXSparseSegment segment;
    Error *err = NULL;

    g_assert_false(mmix_sparse_memory_validate_range(
        address, size, alignment, &segment, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    error_free(err);
}

static void test_budget(void)
{
    MMIXSparseMemory *memory;
    Error *err = NULL;

    memory = mmix_sparse_memory_new(MMIX_SPARSE_PAGE_SIZE - 1, &err);
    g_assert_null(memory);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "not 8 KiB aligned"));
    error_free(err);

    memory = mmix_sparse_memory_new(2 * MMIX_SPARSE_PAGE_SIZE,
                                    &error_abort);
    g_assert_cmphex(mmix_sparse_memory_budget(memory), ==,
                    2 * MMIX_SPARSE_PAGE_SIZE);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 0);
    g_assert_cmphex(mmix_sparse_memory_materialized_bytes(memory), ==, 0);
    mmix_sparse_memory_free(memory);
    mmix_sparse_memory_free(NULL);
}

static void test_segment_boundaries(void)
{
    static const uint64_t bases[MMIX_SPARSE_SEGMENT_COUNT] = {
        MMIX_SPARSE_TEXT_BASE,
        MMIX_SPARSE_DATA_BASE,
        MMIX_SPARSE_POOL_BASE,
        MMIX_SPARSE_STACK_BASE,
    };
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(8 * MMIX_SPARSE_PAGE_SIZE, &error_abort);
    uint8_t value;
    unsigned int i;

    for (i = 0; i < MMIX_SPARSE_SEGMENT_COUNT; i++) {
        MMIXSparseSegment segment;
        uint64_t end = i + 1 == MMIX_SPARSE_SEGMENT_COUNT ?
                       MMIX_SPARSE_LIMIT : bases[i + 1];
        uint8_t first = 0x10 + i;
        uint8_t last = 0x20 + i;

        g_assert_true(mmix_sparse_memory_validate_range(
            bases[i], 1, 1, &segment, &error_abort));
        g_assert_cmpint(segment, ==, i);
        g_assert_true(mmix_sparse_memory_validate_range(
            end - 1, 1, 1, &segment, &error_abort));
        g_assert_cmpint(segment, ==, i);

        g_assert_true(mmix_sparse_memory_write(memory, bases[i], &first,
                                               1, 1, &error_abort));
        g_assert_true(mmix_sparse_memory_write(memory, end - 1, &last,
                                               1, 1, &error_abort));
        value = 0;
        g_assert_true(mmix_sparse_memory_read(memory, bases[i], &value,
                                              1, 1, &error_abort));
        g_assert_cmphex(value, ==, first);
        value = 0;
        g_assert_true(mmix_sparse_memory_read(memory, end - 1, &value,
                                              1, 1, &error_abort));
        g_assert_cmphex(value, ==, last);

        assert_invalid_range(end - 1, 2, 1,
                             "crosses a logical-segment boundary");
    }

    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 8);
    mmix_sparse_memory_free(memory);
}

static void test_invalid_ranges(void)
{
    assert_invalid_range(0, 0, 1, "zero size");
    assert_invalid_range(0, 1, 0, "not a power of two");
    assert_invalid_range(0, 1, 3, "not a power of two");
    assert_invalid_range(3, 1, 2, "unaligned");
    assert_invalid_range(MMIX_SPARSE_LIMIT, 1, 1,
                         "outside the nonnegative logical segments");
    if (SIZE_MAX == UINT64_MAX) {
        assert_invalid_range(1, SIZE_MAX, 1, "overflows its address space");
    }
}

static void test_holes_and_distant_pages(void)
{
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(4 * MMIX_SPARSE_PAGE_SIZE, &error_abort);
    const uint64_t distant = MMIX_SPARSE_STACK_BASE + (UINT64_C(1) << 40);
    const uint8_t text[] = { 0x11, 0x22, 0x33, 0x44 };
    const uint8_t stack[] = { 0xaa, 0xbb, 0xcc, 0xdd };
    uint8_t output[sizeof(text)] = { 0xff, 0xff, 0xff, 0xff };

    g_assert_true(mmix_sparse_memory_read(memory,
                                          MMIX_SPARSE_DATA_BASE + 0x1000,
                                          output, sizeof(output), 1,
                                          &error_abort));
    g_assert_cmpmem(output, sizeof(output),
                    (const uint8_t[sizeof(output)]) { 0 }, sizeof(output));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 0);

    g_assert_true(mmix_sparse_memory_write(memory, 0x100, text,
                                           sizeof(text), 4, &error_abort));
    g_assert_true(mmix_sparse_memory_write(memory, distant, stack,
                                           sizeof(stack), 4, &error_abort));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 2);

    memset(output, 0, sizeof(output));
    g_assert_true(mmix_sparse_memory_read(memory, 0x100, output,
                                          sizeof(output), 4, &error_abort));
    g_assert_cmpmem(output, sizeof(output), text, sizeof(text));
    memset(output, 0, sizeof(output));
    g_assert_true(mmix_sparse_memory_read(memory, distant, output,
                                          sizeof(output), 4, &error_abort));
    g_assert_cmpmem(output, sizeof(output), stack, sizeof(stack));

    mmix_sparse_memory_free(memory);
}

static void test_cross_page_and_overlap(void)
{
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(2 * MMIX_SPARSE_PAGE_SIZE, &error_abort);
    uint8_t input[32];
    uint8_t output[sizeof(input)];
    const uint8_t replacement[] = { 0xde, 0xad, 0xbe, 0xef };
    size_t i;

    for (i = 0; i < sizeof(input); i++) {
        input[i] = i;
    }
    g_assert_true(mmix_sparse_memory_write(
        memory, MMIX_SPARSE_PAGE_SIZE - 8, input, sizeof(input), 1,
        &error_abort));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 2);

    g_assert_true(mmix_sparse_memory_write(
        memory, MMIX_SPARSE_PAGE_SIZE + 4, replacement,
        sizeof(replacement), 4, &error_abort));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 2);

    g_assert_true(mmix_sparse_memory_read(
        memory, MMIX_SPARSE_PAGE_SIZE - 8, output, sizeof(output), 1,
        &error_abort));
    memcpy(input + 12, replacement, sizeof(replacement));
    g_assert_cmpmem(output, sizeof(output), input, sizeof(input));

    mmix_sparse_memory_free(memory);
}

static void test_exhaustion_is_atomic(void)
{
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(2 * MMIX_SPARSE_PAGE_SIZE, &error_abort);
    uint8_t *two_pages = g_malloc0(2 * MMIX_SPARSE_PAGE_SIZE);
    const uint8_t initial = 0x5a;
    const uint8_t final = 0xa5;
    uint8_t value;
    Error *err = NULL;

    two_pages[0] = 0x11;
    two_pages[MMIX_SPARSE_PAGE_SIZE] = 0x22;
    g_assert_true(mmix_sparse_memory_write(memory, 0, &initial, 1, 1,
                                           &error_abort));

    g_assert_false(mmix_sparse_memory_write(
        memory, MMIX_SPARSE_DATA_BASE, two_pages,
        2 * MMIX_SPARSE_PAGE_SIZE, MMIX_SPARSE_PAGE_SIZE, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "only 1 available"));
    error_free(err);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 1);

    value = 0xff;
    g_assert_true(mmix_sparse_memory_read(memory, MMIX_SPARSE_DATA_BASE,
                                          &value, 1, 1, &error_abort));
    g_assert_cmphex(value, ==, 0);
    value = 0;
    g_assert_true(mmix_sparse_memory_read(memory, 0, &value, 1, 1,
                                          &error_abort));
    g_assert_cmphex(value, ==, initial);

    g_assert_true(mmix_sparse_memory_write(memory, MMIX_SPARSE_POOL_BASE,
                                           &final, 1, 1, &error_abort));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 2);
    err = NULL;
    g_assert_false(mmix_sparse_memory_write(memory, MMIX_SPARSE_STACK_BASE,
                                            &final, 1, 1, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 2);

    g_free(two_pages);
    mmix_sparse_memory_free(memory);
}

static void test_compare_exchange_octa(void)
{
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(MMIX_SPARSE_PAGE_SIZE, &error_abort);
    const uint64_t address = MMIX_SPARSE_DATA_BASE + 0x100;
    uint64_t observed;
    Error *err = NULL;

    g_assert_true(mmix_sparse_memory_compare_exchange_octa(
        memory, address, 1, 2, &observed, &error_abort));
    g_assert_cmphex(observed, ==, 0);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 0);

    g_assert_true(mmix_sparse_memory_compare_exchange_octa(
        memory, address, 0, UINT64_C(0x1122334455667788), &observed,
        &error_abort));
    g_assert_cmphex(observed, ==, 0);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 1);

    g_assert_true(mmix_sparse_memory_compare_exchange_octa(
        memory, address, 0, 3, &observed, &error_abort));
    g_assert_cmphex(observed, ==, UINT64_C(0x1122334455667788));

    g_assert_false(mmix_sparse_memory_compare_exchange_octa(
        memory, MMIX_SPARSE_STACK_BASE, 0, 4, &observed, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "only 0 available"));
    error_free(err);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 1);

    mmix_sparse_memory_free(memory);
}

static void test_clear(void)
{
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(MMIX_SPARSE_PAGE_SIZE, &error_abort);
    const uint8_t input = 0x7f;
    uint8_t output = 0xff;

    g_assert_true(mmix_sparse_memory_write(memory, MMIX_SPARSE_POOL_BASE,
                                           &input, 1, 1, &error_abort));
    mmix_sparse_memory_clear(memory);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 0);
    g_assert_cmphex(mmix_sparse_memory_materialized_bytes(memory), ==, 0);
    g_assert_cmphex(mmix_sparse_memory_budget(memory), ==,
                    MMIX_SPARSE_PAGE_SIZE);
    g_assert_true(mmix_sparse_memory_read(memory, MMIX_SPARSE_POOL_BASE,
                                          &output, 1, 1, &error_abort));
    g_assert_cmphex(output, ==, 0);
    g_assert_true(mmix_sparse_memory_write(memory, MMIX_SPARSE_STACK_BASE,
                                           &input, 1, 1, &error_abort));
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(memory), ==, 1);

    mmix_sparse_memory_free(memory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/sparse-memory/budget", test_budget);
    g_test_add_func("/mmix/sparse-memory/segment-boundaries",
                    test_segment_boundaries);
    g_test_add_func("/mmix/sparse-memory/invalid-ranges",
                    test_invalid_ranges);
    g_test_add_func("/mmix/sparse-memory/holes-and-distant-pages",
                    test_holes_and_distant_pages);
    g_test_add_func("/mmix/sparse-memory/cross-page-and-overlap",
                    test_cross_page_and_overlap);
    g_test_add_func("/mmix/sparse-memory/exhaustion-is-atomic",
                    test_exhaustion_is_atomic);
    g_test_add_func("/mmix/sparse-memory/compare-exchange-octa",
                    test_compare_exchange_octa);
    g_test_add_func("/mmix/sparse-memory/clear", test_clear);

    return g_test_run();
}
