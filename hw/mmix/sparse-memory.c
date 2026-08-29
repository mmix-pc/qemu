/*
 * MMIX hosted sparse logical memory
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "sparse-memory.h"

typedef struct MMIXSparsePage {
    uint8_t data[MMIX_SPARSE_PAGE_SIZE];
} MMIXSparsePage;

typedef struct MMIXSparsePendingPage {
    uint64_t *key;
    MMIXSparsePage *page;
} MMIXSparsePendingPage;

struct MMIXSparseMemory {
    GTree *pages;
    uint64_t budget;
    uint64_t materialized_pages;
};

static gint mmix_sparse_page_compare(gconstpointer left, gconstpointer right,
                                     gpointer user_data)
{
    const uint64_t left_page = *(const uint64_t *)left;
    const uint64_t right_page = *(const uint64_t *)right;

    return left_page < right_page ? -1 : left_page > right_page;
}

static GTree *mmix_sparse_page_tree_new(void)
{
    return g_tree_new_full(mmix_sparse_page_compare, NULL, g_free, g_free);
}

MMIXSparseMemory *mmix_sparse_memory_new(uint64_t budget, Error **errp)
{
    MMIXSparseMemory *memory;

    if (budget % MMIX_SPARSE_PAGE_SIZE) {
        error_setg(errp,
                   "MMIX sparse-memory budget 0x%" PRIx64
                   " is not 8 KiB aligned",
                   budget);
        return NULL;
    }

    memory = g_try_new0(MMIXSparseMemory, 1);
    if (!memory) {
        error_setg(errp, "could not allocate MMIX sparse-memory state");
        return NULL;
    }
    memory->pages = mmix_sparse_page_tree_new();
    memory->budget = budget;
    return memory;
}

void mmix_sparse_memory_free(MMIXSparseMemory *memory)
{
    if (!memory) {
        return;
    }

    g_tree_destroy(memory->pages);
    g_free(memory);
}

void mmix_sparse_memory_clear(MMIXSparseMemory *memory)
{
    g_assert(memory);

    g_tree_destroy(memory->pages);
    memory->pages = mmix_sparse_page_tree_new();
    memory->materialized_pages = 0;
}

bool mmix_sparse_memory_validate_range(uint64_t address, size_t size,
                                       size_t alignment,
                                       MMIXSparseSegment *segment,
                                       Error **errp)
{
    uint64_t end;
    uint64_t segment_end;
    MMIXSparseSegment selected;

    if (size == 0) {
        error_setg(errp, "MMIX sparse-memory access has zero size");
        return false;
    }
    if (!is_power_of_2(alignment)) {
        error_setg(errp,
                   "MMIX sparse-memory alignment 0x%zx is not a power of two",
                   alignment);
        return false;
    }
    if (address & (alignment - 1)) {
        error_setg(errp,
                   "unaligned MMIX sparse-memory access at 0x%016" PRIx64
                   " for alignment 0x%zx",
                   address, alignment);
        return false;
    }
    if (address >= MMIX_SPARSE_LIMIT) {
        error_setg(errp,
                   "MMIX sparse-memory address 0x%016" PRIx64
                   " is outside the nonnegative logical segments",
                   address);
        return false;
    }
    if (uadd64_overflow(address, size, &end)) {
        error_setg(errp,
                   "MMIX sparse-memory range at 0x%016" PRIx64
                   " overflows its address space",
                   address);
        return false;
    }

    selected = address >> 61;
    segment_end = ((uint64_t)selected + 1) << 61;
    if (end > segment_end) {
        error_setg(errp,
                   "MMIX sparse-memory range at 0x%016" PRIx64
                   " crosses a logical-segment boundary",
                   address);
        return false;
    }

    if (segment) {
        *segment = selected;
    }
    return true;
}

static MMIXSparsePage *mmix_sparse_memory_lookup(
    const MMIXSparseMemory *memory, uint64_t page_number)
{
    return g_tree_lookup(memory->pages, &page_number);
}

bool mmix_sparse_memory_read(MMIXSparseMemory *memory, uint64_t address,
                             void *buffer, size_t size, size_t alignment,
                             Error **errp)
{
    uint8_t *output = buffer;
    size_t remaining = size;

    g_assert(memory);
    g_assert(buffer);

    if (!mmix_sparse_memory_validate_range(address, size, alignment, NULL,
                                           errp)) {
        return false;
    }

    while (remaining) {
        uint64_t page_number = address / MMIX_SPARSE_PAGE_SIZE;
        size_t page_offset = address % MMIX_SPARSE_PAGE_SIZE;
        size_t chunk = MIN(remaining, MMIX_SPARSE_PAGE_SIZE - page_offset);
        MMIXSparsePage *page =
            mmix_sparse_memory_lookup(memory, page_number);

        if (page) {
            memcpy(output, page->data + page_offset, chunk);
        } else {
            memset(output, 0, chunk);
        }
        address += chunk;
        output += chunk;
        remaining -= chunk;
    }
    return true;
}

static uint64_t mmix_sparse_memory_missing_pages(
    const MMIXSparseMemory *memory, uint64_t first_page, uint64_t last_page)
{
    uint64_t page_number;
    uint64_t missing = 0;

    for (page_number = first_page; ; page_number++) {
        if (!mmix_sparse_memory_lookup(memory, page_number)) {
            missing++;
        }
        if (page_number == last_page) {
            return missing;
        }
    }
}

static void mmix_sparse_pending_pages_free(MMIXSparsePendingPage *pages,
                                           uint64_t count)
{
    uint64_t i;

    for (i = 0; i < count; i++) {
        g_free(pages[i].key);
        g_free(pages[i].page);
    }
    g_free(pages);
}

static bool mmix_sparse_memory_allocate_pages(MMIXSparseMemory *memory,
                                              uint64_t first_page,
                                              uint64_t last_page,
                                              uint64_t missing,
                                              Error **errp)
{
    MMIXSparsePendingPage *pending;
    uint64_t page_number;
    uint64_t index = 0;

    if (!missing) {
        return true;
    }
    pending = g_try_new0(MMIXSparsePendingPage, missing);
    if (!pending) {
        error_setg(errp,
                   "could not allocate metadata for %" PRIu64
                   " MMIX sparse-memory pages", missing);
        return false;
    }

    for (page_number = first_page; ; page_number++) {
        if (!mmix_sparse_memory_lookup(memory, page_number)) {
            pending[index].key = g_try_new(uint64_t, 1);
            pending[index].page = g_try_new0(MMIXSparsePage, 1);
            if (!pending[index].key || !pending[index].page) {
                error_setg(errp,
                           "could not allocate MMIX sparse-memory page "
                           "0x%" PRIx64, page_number);
                mmix_sparse_pending_pages_free(pending, missing);
                return false;
            }
            *pending[index].key = page_number;
            index++;
        }
        if (page_number == last_page) {
            break;
        }
    }
    g_assert(index == missing);

    for (index = 0; index < missing; index++) {
        g_tree_insert(memory->pages, pending[index].key,
                      pending[index].page);
        pending[index].key = NULL;
        pending[index].page = NULL;
    }
    memory->materialized_pages += missing;
    mmix_sparse_pending_pages_free(pending, missing);
    return true;
}

bool mmix_sparse_memory_write(MMIXSparseMemory *memory, uint64_t address,
                              const void *buffer, size_t size,
                              size_t alignment, Error **errp)
{
    const uint8_t *input = buffer;
    uint64_t first_page;
    uint64_t last_page;
    uint64_t missing;
    uint64_t available;
    uint64_t page_count;
    size_t remaining = size;

    g_assert(memory);
    g_assert(buffer);

    if (!mmix_sparse_memory_validate_range(address, size, alignment, NULL,
                                           errp)) {
        return false;
    }

    first_page = address / MMIX_SPARSE_PAGE_SIZE;
    last_page = (address + size - 1) / MMIX_SPARSE_PAGE_SIZE;
    page_count = last_page - first_page + 1;
    available = memory->budget / MMIX_SPARSE_PAGE_SIZE -
                memory->materialized_pages;

    if (page_count > memory->budget / MMIX_SPARSE_PAGE_SIZE) {
        error_setg(errp,
                   "MMIX sparse-memory write spans %" PRIu64
                   " pages, exceeding the %" PRIu64 "-page budget",
                   page_count, memory->budget / MMIX_SPARSE_PAGE_SIZE);
        return false;
    }
    missing = mmix_sparse_memory_missing_pages(memory, first_page,
                                               last_page);
    if (missing > available) {
        error_setg(errp,
                   "MMIX sparse-memory write requires %" PRIu64
                   " new 8 KiB pages with only %" PRIu64 " available",
                   missing, available);
        return false;
    }

    if (!mmix_sparse_memory_allocate_pages(memory, first_page, last_page,
                                           missing, errp)) {
        return false;
    }

    while (remaining) {
        uint64_t current_page = address / MMIX_SPARSE_PAGE_SIZE;
        size_t page_offset = address % MMIX_SPARSE_PAGE_SIZE;
        size_t chunk = MIN(remaining, MMIX_SPARSE_PAGE_SIZE - page_offset);
        MMIXSparsePage *page =
            mmix_sparse_memory_lookup(memory, current_page);

        g_assert(page);
        memcpy(page->data + page_offset, input, chunk);
        address += chunk;
        input += chunk;
        remaining -= chunk;
    }
    return true;
}

bool mmix_sparse_memory_compare_exchange_octa(MMIXSparseMemory *memory,
                                              uint64_t address,
                                              uint64_t expected,
                                              uint64_t desired,
                                              uint64_t *observed,
                                              Error **errp)
{
    uint8_t data[sizeof(uint64_t)];

    g_assert(observed);

    if (!mmix_sparse_memory_read(memory, address, data, sizeof(data),
                                 sizeof(data), errp)) {
        return false;
    }
    *observed = ldq_be_p(data);
    if (*observed != expected) {
        return true;
    }

    stq_be_p(data, desired);
    return mmix_sparse_memory_write(memory, address, data, sizeof(data),
                                    sizeof(data), errp);
}

uint64_t mmix_sparse_memory_budget(const MMIXSparseMemory *memory)
{
    g_assert(memory);
    return memory->budget;
}

uint64_t mmix_sparse_memory_materialized_bytes(
    const MMIXSparseMemory *memory)
{
    g_assert(memory);
    return memory->materialized_pages * MMIX_SPARSE_PAGE_SIZE;
}

uint64_t mmix_sparse_memory_materialized_pages(
    const MMIXSparseMemory *memory)
{
    g_assert(memory);
    return memory->materialized_pages;
}
