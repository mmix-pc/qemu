/*
 * MMIX virt immutable RAM boot payload
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "boot-payload.h"

typedef struct MMIXBootPayloadEntry {
    char *name;
    uint64_t address;
    uint64_t memory_size;
    GBytes *data;
} MMIXBootPayloadEntry;

struct MMIXBootPayload {
    GPtrArray *entries;
    uint64_t ram_size;
};

static void mmix_boot_payload_entry_free(gpointer opaque)
{
    MMIXBootPayloadEntry *entry = opaque;

    g_free(entry->name);
    g_clear_pointer(&entry->data, g_bytes_unref);
    g_free(entry);
}

MMIXBootPayload *mmix_boot_payload_new(uint64_t ram_size)
{
    MMIXBootPayload *payload = g_new0(MMIXBootPayload, 1);

    payload->entries = g_ptr_array_new_with_free_func(
        mmix_boot_payload_entry_free);
    payload->ram_size = ram_size;
    return payload;
}

void mmix_boot_payload_free(MMIXBootPayload *payload)
{
    if (!payload) {
        return;
    }

    g_ptr_array_unref(payload->entries);
    g_free(payload);
}

static bool mmix_boot_payload_range_valid(uint64_t ram_size,
                                          uint64_t address, uint64_t size)
{
    return size != 0 && address <= ram_size && size <= ram_size - address;
}

bool mmix_boot_payload_add(MMIXBootPayload *payload, const char *name,
                           uint64_t address, GBytes *data,
                           uint64_t memory_size, Error **errp)
{
    MMIXBootPayloadEntry *entry;
    gsize data_size;
    unsigned int i;

    g_return_val_if_fail(payload != NULL, false);
    g_return_val_if_fail(name != NULL, false);
    g_return_val_if_fail(data != NULL, false);

    data_size = g_bytes_get_size(data);
    if (data_size > memory_size ||
        !mmix_boot_payload_range_valid(payload->ram_size, address,
                                       memory_size)) {
        error_setg(errp, "MMIX boot payload '%s' has an invalid RAM range",
                   name);
        return false;
    }
    for (i = 0; i < payload->entries->len; i++) {
        const MMIXBootPayloadEntry *other =
            g_ptr_array_index(payload->entries, i);

        if (address < other->address + other->memory_size &&
            other->address < address + memory_size) {
            error_setg(errp, "MMIX boot payload '%s' overlaps '%s'", name,
                       other->name);
            return false;
        }
    }

    entry = g_new0(MMIXBootPayloadEntry, 1);
    entry->name = g_strdup(name);
    entry->address = address;
    entry->memory_size = memory_size;
    entry->data = g_bytes_ref(data);
    g_ptr_array_add(payload->entries, entry);
    return true;
}

bool mmix_boot_payload_add_zero(MMIXBootPayload *payload, const char *name,
                                uint64_t address, uint64_t size,
                                Error **errp)
{
    g_autoptr(GBytes) empty = g_bytes_new_static("", 0);

    return mmix_boot_payload_add(payload, name, address, empty, size, errp);
}

bool mmix_boot_payload_commit(const MMIXBootPayload *payload, void *ram,
                              uint64_t ram_size, Error **errp)
{
    unsigned int i;

    g_return_val_if_fail(payload != NULL, false);
    g_return_val_if_fail(ram != NULL, false);

    if (ram_size != payload->ram_size || ram_size > G_MAXSIZE) {
        error_setg(errp, "MMIX boot payload RAM size changed before commit");
        return false;
    }

    /* All entry ranges were validated before publication. */
    for (i = 0; i < payload->entries->len; i++) {
        const MMIXBootPayloadEntry *entry =
            g_ptr_array_index(payload->entries, i);
        gsize data_size;
        const void *data = g_bytes_get_data(entry->data, &data_size);
        uint8_t *destination = (uint8_t *)ram + entry->address;

        memcpy(destination, data, data_size);
        memset(destination + data_size, 0, entry->memory_size - data_size);
    }
    return true;
}

bool mmix_boot_payload_commit_address_space(const MMIXBootPayload *payload,
                                            AddressSpace *address_space,
                                            uint64_t ram_size, Error **errp)
{
    unsigned int i;

    g_return_val_if_fail(payload != NULL, false);
    g_return_val_if_fail(address_space != NULL, false);

    if (ram_size != payload->ram_size) {
        error_setg(errp, "MMIX boot payload RAM size changed before commit");
        return false;
    }

    /* All entry ranges were validated before publication. */
    for (i = 0; i < payload->entries->len; i++) {
        const MMIXBootPayloadEntry *entry =
            g_ptr_array_index(payload->entries, i);
        gsize data_size;
        const void *data = g_bytes_get_data(entry->data, &data_size);
        MemTxResult result;

        if (data_size != 0) {
            result = address_space_write(address_space, entry->address,
                                         MEMTXATTRS_UNSPECIFIED, data,
                                         data_size);
            if (result != MEMTX_OK) {
                error_setg(errp, "could not write MMIX boot payload '%s'",
                           entry->name);
                return false;
            }
        }
        if (data_size != entry->memory_size) {
            result = address_space_set(address_space,
                                       entry->address + data_size, 0,
                                       entry->memory_size - data_size,
                                       MEMTXATTRS_UNSPECIFIED);
            if (result != MEMTX_OK) {
                error_setg(errp, "could not clear MMIX boot payload '%s'",
                           entry->name);
                return false;
            }
        }
    }
    return true;
}
