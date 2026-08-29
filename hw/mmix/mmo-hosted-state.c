/*
 * MMIX MMO hosted-memory migration state
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "migration/qemu-file-types.h"
#include "qapi/error.h"
#include "mmo-hosted-state.h"

typedef struct MMIXMMOHostedSaveContext {
    QEMUFile *file;
} MMIXMMOHostedSaveContext;

static bool mmix_mmo_hosted_state_save_page(uint64_t address,
                                            const uint8_t *data,
                                            void *opaque, Error **errp)
{
    MMIXMMOHostedSaveContext *context = opaque;

    qemu_put_be64(context->file, address);
    qemu_put_buffer(context->file, data, MMIX_SPARSE_PAGE_SIZE);
    return true;
}

bool mmix_mmo_hosted_state_save(QEMUFile *f,
                                const MMIXSparseMemory *memory,
                                Error **errp)
{
    MMIXMMOHostedSaveContext context = { .file = f };
    uint64_t budget = memory ? mmix_sparse_memory_budget(memory) : 0;
    uint64_t pages = memory ?
        mmix_sparse_memory_materialized_pages(memory) : 0;

    qemu_put_byte(f, memory != NULL);
    qemu_put_be64(f, budget);
    qemu_put_be64(f, pages * MMIX_SPARSE_PAGE_SIZE);
    qemu_put_be64(f, pages);
    if (memory &&
        !mmix_sparse_memory_foreach_page(
            memory, mmix_mmo_hosted_state_save_page, &context, errp)) {
        return false;
    }
    return qemu_file_get_error(f) == 0;
}

static bool mmix_mmo_hosted_state_read_header(
    QEMUFile *f, bool *hosted, uint64_t *budget, uint64_t *accounting,
    uint64_t *pages, Error **errp)
{
    int hosted_value = qemu_get_byte(f);

    *budget = qemu_get_be64(f);
    *accounting = qemu_get_be64(f);
    *pages = qemu_get_be64(f);
    if (qemu_file_get_error(f)) {
        error_setg(errp, "truncated MMIX MMO hosted-memory state header");
        return false;
    }
    if (hosted_value != 0 && hosted_value != 1) {
        error_setg(errp, "invalid MMIX MMO hosted-mode flag %d",
                   hosted_value);
        return false;
    }
    *hosted = hosted_value;
    return true;
}

bool mmix_mmo_hosted_state_load(QEMUFile *f, bool expected_hosted,
                                uint64_t expected_budget,
                                MMIXSparseMemory **memory, Error **errp)
{
    g_autofree uint8_t *data = NULL;
    MMIXSparseMemory *candidate = NULL;
    uint64_t budget;
    uint64_t accounting;
    uint64_t pages;
    uint64_t previous = 0;
    bool hosted;
    uint64_t i;

    g_return_val_if_fail(memory != NULL, false);

    if (!mmix_mmo_hosted_state_read_header(
            f, &hosted, &budget, &accounting, &pages, errp)) {
        return false;
    }
    if (hosted != expected_hosted) {
        error_setg(errp,
                   "incoming MMIX MMO hosted mode does not match machine");
        return false;
    }
    if (!hosted) {
        if (budget || accounting || pages) {
            error_setg(errp,
                       "non-hosted MMIX state contains sparse-memory data");
            return false;
        }
        return true;
    }
    if (budget != expected_budget) {
        error_setg(errp,
                   "incoming MMIX MMO sparse budget 0x%" PRIx64
                   " does not match machine budget 0x%" PRIx64,
                   budget, expected_budget);
        return false;
    }
    if (pages > budget / MMIX_SPARSE_PAGE_SIZE ||
        accounting != pages * MMIX_SPARSE_PAGE_SIZE) {
        error_setg(errp,
                   "invalid MMIX MMO sparse page count or accounting");
        return false;
    }

    candidate = mmix_sparse_memory_new(budget, errp);
    if (!candidate) {
        return false;
    }
    data = g_try_malloc(MMIX_SPARSE_PAGE_SIZE);
    if (!data) {
        error_setg(errp, "could not allocate MMIX MMO migration page");
        goto fail;
    }
    for (i = 0; i < pages; i++) {
        uint64_t address = qemu_get_be64(f);

        if (qemu_get_buffer(f, data, MMIX_SPARSE_PAGE_SIZE) !=
            MMIX_SPARSE_PAGE_SIZE || qemu_file_get_error(f)) {
            error_setg(errp,
                       "truncated MMIX MMO sparse page %" PRIu64, i);
            goto fail;
        }
        if (i && address <= previous) {
            error_setg(errp,
                       "MMIX MMO sparse pages are not strictly sorted");
            goto fail;
        }
        if (!mmix_sparse_memory_validate_range(
                address, MMIX_SPARSE_PAGE_SIZE, MMIX_SPARSE_PAGE_SIZE,
                NULL, errp)) {
            goto fail;
        }
        if (!mmix_sparse_memory_write(
                candidate, address, data, MMIX_SPARSE_PAGE_SIZE,
                MMIX_SPARSE_PAGE_SIZE, errp)) {
            goto fail;
        }
        previous = address;
    }
    if (mmix_sparse_memory_materialized_pages(candidate) != pages) {
        error_setg(errp,
                   "MMIX MMO sparse accounting changed during load");
        goto fail;
    }

    mmix_sparse_memory_free(*memory);
    *memory = candidate;
    return true;

fail:
    mmix_sparse_memory_free(candidate);
    return false;
}
