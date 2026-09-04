/*
 * MMIX MMO hosted-memory migration state tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/mmo-hosted-state.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

static QEMUFile *open_output(QIOChannelBuffer *buffer)
{
    buffer->offset = 0;
    buffer->usage = 0;
    return qemu_file_new_output(QIO_CHANNEL(buffer));
}

static QEMUFile *open_input(QIOChannelBuffer *buffer)
{
    buffer->offset = 0;
    return qemu_file_new_input(QIO_CHANNEL(buffer));
}

static void finish_output(QEMUFile *file, QIOChannelBuffer *buffer)
{
    g_autofree uint8_t *data = NULL;
    size_t size;

    g_assert_cmpint(qemu_fflush(file), ==, 0);
    size = buffer->usage;
    data = g_memdup2(buffer->data, size);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    buffer->data = g_steal_pointer(&data);
    buffer->capacity = size;
    buffer->usage = size;
    buffer->offset = 0;
}

static void assert_memory_byte(MMIXSparseMemory *memory, uint64_t address,
                               uint8_t expected)
{
    uint8_t value = 0;

    g_assert_true(mmix_sparse_memory_read(memory, address, &value, 1, 1,
                                          &error_abort));
    g_assert_cmphex(value, ==, expected);
}

static void test_round_trip(void)
{
    const uint64_t budget = 4 * MMIX_SPARSE_PAGE_SIZE;
    const uint64_t addresses[] = {
        MMIX_SPARSE_STACK_BASE + 0x6000,
        MMIX_SPARSE_TEXT_BASE,
        MMIX_SPARSE_DATA_BASE + 0x2000,
    };
    const uint8_t values[] = { 0x33, 0x11, 0x22 };
    QIOChannelBuffer *buffer = qio_channel_buffer_new(0);
    MMIXSparseMemory *source =
        mmix_sparse_memory_new(budget, &error_abort);
    MMIXSparseMemory *destination =
        mmix_sparse_memory_new(budget, &error_abort);
    QEMUFile *file;
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(addresses); i++) {
        g_assert_true(mmix_sparse_memory_write(
            source, addresses[i], &values[i], 1, 1, &error_abort));
    }
    file = open_output(buffer);
    g_assert_true(mmix_mmo_hosted_state_save(file, source, &error_abort));
    finish_output(file, buffer);

    file = open_input(buffer);
    g_assert_true(mmix_mmo_hosted_state_load(
        file, true, budget, &destination, &error_abort));
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_cmpuint(mmix_sparse_memory_materialized_pages(destination), ==,
                     G_N_ELEMENTS(addresses));
    for (i = 0; i < G_N_ELEMENTS(addresses); i++) {
        assert_memory_byte(destination, addresses[i], values[i]);
    }
    assert_memory_byte(destination, MMIX_SPARSE_POOL_BASE + 0x4000, 0);

    mmix_sparse_memory_free(destination);
    mmix_sparse_memory_free(source);
    object_unref(OBJECT(buffer));
}

static void test_non_hosted(void)
{
    QIOChannelBuffer *buffer = qio_channel_buffer_new(0);
    MMIXSparseMemory *memory = NULL;
    QEMUFile *file = open_output(buffer);

    g_assert_true(mmix_mmo_hosted_state_save(file, NULL, &error_abort));
    finish_output(file, buffer);
    file = open_input(buffer);
    g_assert_true(mmix_mmo_hosted_state_load(
        file, false, 4 * MMIX_SPARSE_PAGE_SIZE, &memory, &error_abort));
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_null(memory);
    object_unref(OBJECT(buffer));
}

static void write_wire(QIOChannelBuffer *buffer, uint8_t hosted,
                       uint64_t budget, uint64_t accounting,
                       const uint64_t *addresses, uint64_t pages,
                       uint64_t payload_pages)
{
    g_autofree uint8_t *data = g_malloc0(MMIX_SPARSE_PAGE_SIZE);
    QEMUFile *file = open_output(buffer);
    uint64_t i;

    qemu_put_byte(file, hosted);
    qemu_put_be64(file, budget);
    qemu_put_be64(file, accounting);
    qemu_put_be64(file, pages);
    for (i = 0; i < payload_pages; i++) {
        qemu_put_be64(file, addresses[i]);
        qemu_put_buffer(file, data, MMIX_SPARSE_PAGE_SIZE);
    }
    finish_output(file, buffer);
}

static void assert_load_fails(QIOChannelBuffer *buffer,
                              bool expected_hosted,
                              uint64_t expected_budget,
                              MMIXSparseMemory **memory,
                              const char *diagnostic)
{
    MMIXSparseMemory *original = *memory;
    QEMUFile *file = open_input(buffer);
    Error *err = NULL;

    g_assert_false(mmix_mmo_hosted_state_load(
        file, expected_hosted, expected_budget, memory, &err));
    g_assert_true(*memory == original);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    error_free(err);
    qemu_fclose(file);
}

static void test_invalid_headers(void)
{
    const uint64_t budget = 2 * MMIX_SPARSE_PAGE_SIZE;
    const uint64_t address = MMIX_SPARSE_TEXT_BASE;
    QIOChannelBuffer *buffer = qio_channel_buffer_new(0);
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(budget, &error_abort);

    write_wire(buffer, 2, 0, 0, NULL, 0, 0);
    assert_load_fails(buffer, true, budget, &memory, "hosted-mode flag");
    write_wire(buffer, 0, budget, 0, NULL, 0, 0);
    assert_load_fails(buffer, false, budget, &memory, "non-hosted");
    write_wire(buffer, 0, 0, 0, NULL, 0, 0);
    assert_load_fails(buffer, true, budget, &memory, "does not match");
    write_wire(buffer, 1, budget * 2, 0, NULL, 0, 0);
    assert_load_fails(buffer, true, budget, &memory, "does not match");
    write_wire(buffer, 1, budget, 1, &address, 1, 1);
    assert_load_fails(buffer, true, budget, &memory, "count or accounting");
    write_wire(buffer, 1, budget, 3 * MMIX_SPARSE_PAGE_SIZE,
               NULL, 3, 0);
    assert_load_fails(buffer, true, budget, &memory, "count or accounting");

    mmix_sparse_memory_free(memory);
    object_unref(OBJECT(buffer));
}

static void test_invalid_pages(void)
{
    const uint64_t budget = 2 * MMIX_SPARSE_PAGE_SIZE;
    const uint64_t unaligned[] = { 1 };
    const uint64_t duplicate[] = {
        MMIX_SPARSE_DATA_BASE,
        MMIX_SPARSE_DATA_BASE,
    };
    const uint64_t reversed[] = {
        MMIX_SPARSE_DATA_BASE,
        MMIX_SPARSE_TEXT_BASE,
    };
    const uint64_t truncated[] = { MMIX_SPARSE_TEXT_BASE };
    QIOChannelBuffer *buffer = qio_channel_buffer_new(0);
    MMIXSparseMemory *memory =
        mmix_sparse_memory_new(budget, &error_abort);

    write_wire(buffer, 1, budget, MMIX_SPARSE_PAGE_SIZE,
               unaligned, 1, 1);
    assert_load_fails(buffer, true, budget, &memory, "unaligned");
    write_wire(buffer, 1, budget, budget, duplicate, 2, 2);
    assert_load_fails(buffer, true, budget, &memory, "strictly sorted");
    write_wire(buffer, 1, budget, budget, reversed, 2, 2);
    assert_load_fails(buffer, true, budget, &memory, "strictly sorted");
    write_wire(buffer, 1, budget, MMIX_SPARSE_PAGE_SIZE,
               truncated, 1, 0);
    assert_load_fails(buffer, true, budget, &memory, "truncated");

    mmix_sparse_memory_free(memory);
    object_unref(OBJECT(buffer));
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/mmo-hosted-state/round-trip", test_round_trip);
    g_test_add_func("/mmix/mmo-hosted-state/non-hosted", test_non_hosted);
    g_test_add_func("/mmix/mmo-hosted-state/invalid-headers",
                    test_invalid_headers);
    g_test_add_func("/mmix/mmo-hosted-state/invalid-pages",
                    test_invalid_pages);
    return g_test_run();
}
