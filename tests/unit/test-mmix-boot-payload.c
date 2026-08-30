/*
 * MMIX virt immutable RAM boot payload tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/boot-payload.h"
#include "qapi/error.h"
#include "system/memory.h"

static uint8_t *test_address_space_ram;
static size_t test_address_space_size;

MemTxResult address_space_write(const AddressSpace *as, hwaddr addr,
                                MemTxAttrs attrs, const void *buf,
                                hwaddr len)
{
    g_assert_nonnull(as);
    g_assert_cmphex(attrs.requester_id, ==,
                    MEMTXATTRS_UNSPECIFIED.requester_id);
    if (addr > test_address_space_size ||
        len > test_address_space_size - addr) {
        return MEMTX_ERROR;
    }
    memcpy(test_address_space_ram + addr, buf, len);
    return MEMTX_OK;
}

MemTxResult address_space_set(const AddressSpace *as, hwaddr addr,
                              uint8_t value, hwaddr len, MemTxAttrs attrs)
{
    g_assert_nonnull(as);
    g_assert_cmphex(attrs.requester_id, ==,
                    MEMTXATTRS_UNSPECIFIED.requester_id);
    if (addr > test_address_space_size ||
        len > test_address_space_size - addr) {
        return MEMTX_ERROR;
    }
    memset(test_address_space_ram + addr, value, len);
    return MEMTX_OK;
}

static void test_commit_exact_contents(void)
{
    static const uint8_t segment[] = { 0x11, 0x22, 0x33 };
    static const uint8_t fdt[] = { 0xd0, 0x0d, 0xfe, 0xed };
    g_autoptr(GBytes) segment_bytes = g_bytes_new_static(
        segment, sizeof(segment));
    g_autoptr(GBytes) fdt_bytes = g_bytes_new_static(fdt, sizeof(fdt));
    g_autofree uint8_t *ram = g_malloc0(64);
    MMIXBootPayload *payload = mmix_boot_payload_new(64);

    memset(ram, 0xaa, 64);
    g_assert_true(mmix_boot_payload_add(payload, "segment", 8,
                                       segment_bytes, 8, &error_abort));
    g_assert_true(mmix_boot_payload_add_zero(payload, "stack", 24, 8,
                                            &error_abort));
    g_assert_true(mmix_boot_payload_add(payload, "fdt", 48, fdt_bytes,
                                       sizeof(fdt), &error_abort));
    g_assert_true(mmix_boot_payload_commit(payload, ram, 64, &error_abort));

    g_assert_cmpmem(ram + 8, sizeof(segment), segment, sizeof(segment));
    g_assert_cmphex(ram[11], ==, 0);
    g_assert_cmphex(ram[15], ==, 0);
    g_assert_cmphex(ram[23], ==, 0xaa);
    g_assert_cmphex(ram[24], ==, 0);
    g_assert_cmphex(ram[31], ==, 0);
    g_assert_cmpmem(ram + 48, sizeof(fdt), fdt, sizeof(fdt));
    g_assert_cmphex(ram[52], ==, 0xaa);

    mmix_boot_payload_free(payload);
}

static void test_rejects_invalid_entries_without_writes(void)
{
    static const uint8_t bytes[] = { 1, 2, 3, 4 };
    g_autoptr(GBytes) data = g_bytes_new_static(bytes, sizeof(bytes));
    g_autofree uint8_t *ram = g_malloc0(32);
    MMIXBootPayload *payload = mmix_boot_payload_new(32);
    Error *err = NULL;

    memset(ram, 0x5a, 32);
    g_assert_true(mmix_boot_payload_add(payload, "first", 8, data, 8,
                                       &error_abort));
    g_assert_false(mmix_boot_payload_add(payload, "overlap", 12, data, 4,
                                        &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "overlaps 'first'"));
    error_free(err);
    err = NULL;

    g_assert_false(mmix_boot_payload_commit(payload, ram, 31, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "RAM size changed"));
    g_assert_cmphex(ram[8], ==, 0x5a);
    error_free(err);

    mmix_boot_payload_free(payload);
}

static void test_commit_address_space(void)
{
    static const uint8_t bytes[] = { 0x11, 0x22, 0x33 };
    g_autoptr(GBytes) data = g_bytes_new_static(bytes, sizeof(bytes));
    g_autofree uint8_t *ram = g_malloc0(32);
    MMIXBootPayload *payload = mmix_boot_payload_new(32);
    AddressSpace *address_space = (AddressSpace *)payload;

    memset(ram, 0xaa, 32);
    test_address_space_ram = ram;
    test_address_space_size = 32;
    g_assert_true(mmix_boot_payload_add(payload, "segment", 8, data, 8,
                                       &error_abort));
    g_assert_true(mmix_boot_payload_commit_address_space(
                      payload, address_space, 32, &error_abort));
    g_assert_cmpmem(ram + 8, sizeof(bytes), bytes, sizeof(bytes));
    g_assert_cmphex(ram[11], ==, 0);
    g_assert_cmphex(ram[15], ==, 0);
    g_assert_cmphex(ram[16], ==, 0xaa);

    test_address_space_ram = NULL;
    test_address_space_size = 0;
    mmix_boot_payload_free(payload);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/boot-payload/commit-exact-contents",
                    test_commit_exact_contents);
    g_test_add_func("/mmix/boot-payload/reject-before-write",
                    test_rejects_invalid_entries_without_writes);
    g_test_add_func("/mmix/boot-payload/commit-address-space",
                    test_commit_address_space);
    return g_test_run();
}
