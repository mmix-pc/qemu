/*
 * MMIX MMO hosted startup planner
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "mmo-hosted-plan.h"
#include "sparse-memory.h"

enum {
    MMIX_MMO_ARGUMENT_ALIGN = 8,
    MMIX_MMO_ARGUMENT_MAX = 8 * MiB,
};

struct MMIXMMOHostedPlan {
    GBytes *argument_data;
    uint64_t argument_count;
    uint64_t argument_end;
    uint64_t materialized_pages;
    uint64_t sparse_budget;
};

static bool mmix_mmo_hosted_validate_options(
    const MMIXMMOHostedOptions *options, Error **errp)
{
    bool has_append = options->append && options->append[0];

    if (!options->kernel_filename || !options->kernel_filename[0]) {
        error_setg(errp, "MMIX MMO hosted startup requires a kernel file");
        return false;
    }
    if (options->cpu_count != 1) {
        error_setg(errp, "MMIX MMO hosted startup requires exactly one CPU");
        return false;
    }
    if (options->has_initrd) {
        error_setg(errp, "MMIX MMO hosted startup does not accept -initrd");
        return false;
    }
    if (options->has_explicit_elf_startup_abi) {
        error_setg(errp, "MMIX MMO hosted startup does not accept an "
                   "explicit ELF startup ABI");
        return false;
    }
    if (options->has_firmware) {
        error_setg(errp, "MMIX MMO hosted startup does not accept firmware");
        return false;
    }
    if (options->linux_handoff) {
        error_setg(errp, "MMIX MMO hosted startup does not support Linux "
                   "handoff");
        return false;
    }
    if (options->has_explicit_arguments && !options->semihosting_enabled) {
        error_setg(errp, "explicit MMIX MMO hosted arguments require "
                   "semihosting");
        return false;
    }
    if (options->has_explicit_arguments && has_append) {
        error_setg(errp, "MMIX MMO hosted startup does not allow explicit "
                   "semihosting arguments with -append");
        return false;
    }
    if (options->has_explicit_arguments !=
        (options->explicit_argument_count != 0)) {
        error_setg(errp, "MMIX MMO hosted explicit argument selection is "
                   "inconsistent");
        return false;
    }
    if (options->explicit_argument_count >
        MMIX_MMO_ARGUMENT_MAX / MMIX_MMO_ARGUMENT_ALIGN - 2) {
        error_setg(errp, "MMIX MMO hosted argument count is too large");
        return false;
    }
    if (options->explicit_argument_count != 0 &&
        !options->explicit_arguments) {
        error_setg(errp, "MMIX MMO hosted argument array is missing");
        return false;
    }
    if (options->sparse_budget % MMIX_SPARSE_PAGE_SIZE) {
        error_setg(errp, "MMIX MMO sparse budget 0x%" PRIx64
                   " is not 8 KiB aligned", options->sparse_budget);
        return false;
    }
    return true;
}

static GPtrArray *mmix_mmo_hosted_collect_arguments(
    const MMIXMMOHostedOptions *options, Error **errp)
{
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    size_t i;

    if (options->has_explicit_arguments) {
        for (i = 0; i < options->explicit_argument_count; i++) {
            if (!options->explicit_arguments[i]) {
                error_setg(errp, "explicit MMIX MMO hosted argument #%zu "
                           "is missing", i);
                g_ptr_array_unref(arguments);
                return NULL;
            }
            g_ptr_array_add(arguments,
                            g_strdup(options->explicit_arguments[i]));
        }
        return arguments;
    }

    g_ptr_array_add(arguments, g_strdup(options->kernel_filename));
    if (options->append && options->append[0]) {
        g_auto(GStrv) tokens = g_strsplit(options->append, " ", -1);

        for (i = 0; tokens[i]; i++) {
            if (tokens[i][0]) {
                g_ptr_array_add(arguments, g_strdup(tokens[i]));
            }
        }
    }
    return arguments;
}

static bool mmix_mmo_hosted_argument_size(const GPtrArray *arguments,
                                          uint64_t *size, Error **errp)
{
    uint64_t table_octa_count;
    uint64_t total;
    size_t i;

    if (uadd64_overflow(arguments->len, 2, &table_octa_count) ||
        umul64_overflow(table_octa_count, MMIX_MMO_ARGUMENT_ALIGN, &total)) {
        error_setg(errp, "MMIX MMO hosted argument table is too large");
        return false;
    }
    for (i = 0; i < arguments->len; i++) {
        const char *argument = g_ptr_array_index(arguments, i);
        uint64_t string_size = strlen(argument) + 1;
        uint64_t aligned_size;

        if (uadd64_overflow(string_size, MMIX_MMO_ARGUMENT_ALIGN - 1,
                            &aligned_size)) {
            error_setg(errp, "MMIX MMO hosted argument strings are too "
                       "large");
            return false;
        }
        aligned_size &= ~(MMIX_MMO_ARGUMENT_ALIGN - 1);
        if (uadd64_overflow(total, aligned_size, &total)) {
            error_setg(errp, "MMIX MMO hosted argument strings are too "
                       "large");
            return false;
        }
    }
    if (total > MMIX_MMO_ARGUMENT_MAX) {
        error_setg(errp, "MMIX MMO hosted argument block exceeds 8 MiB");
        return false;
    }

    *size = total;
    return true;
}

static GBytes *mmix_mmo_hosted_build_argument_data(
    const GPtrArray *arguments, uint64_t size, Error **errp)
{
    uint64_t string_offset = ((uint64_t)arguments->len + 2) *
                             MMIX_MMO_ARGUMENT_ALIGN;
    uint64_t argument_end;
    uint8_t *data = g_malloc0(size);
    size_t i;

    if (uadd64_overflow(MMIX_SPARSE_POOL_BASE, size, &argument_end)) {
        error_setg(errp, "MMIX MMO hosted argument address overflow");
        g_free(data);
        return NULL;
    }
    stq_be_p(data, argument_end);
    for (i = 0; i < arguments->len; i++) {
        const char *argument = g_ptr_array_index(arguments, i);
        size_t string_size = strlen(argument) + 1;
        uint64_t pointer = MMIX_SPARSE_POOL_BASE + string_offset;

        stq_be_p(data + (i + 1) * MMIX_MMO_ARGUMENT_ALIGN, pointer);
        memcpy(data + string_offset, argument, string_size);
        string_offset += QEMU_ALIGN_UP(string_size,
                                       MMIX_MMO_ARGUMENT_ALIGN);
    }
    g_assert(string_offset == size);
    return g_bytes_new_take(data, size);
}

static bool mmix_mmo_hosted_check_collision(const MMIXMMOPlan *mmo,
                                            uint64_t argument_end,
                                            Error **errp)
{
    size_t i;

    for (i = 0; i < mmix_mmo_plan_write_count(mmo); i++) {
        const MMIXMMOWrite *write = mmix_mmo_plan_write(mmo, i);

        if (write->address < argument_end &&
            write->address + sizeof(write->value) > MMIX_SPARSE_POOL_BASE) {
            error_setg(errp,
                       "MMIX .mmo write at 0x%016" PRIx64
                       " from tetra %" PRIu64
                       " overlaps the Pool argument block",
                       write->address, write->source_tetra);
            return false;
        }
    }
    return true;
}

static gint mmix_mmo_page_compare(gconstpointer left, gconstpointer right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : a > b;
}

static bool mmix_mmo_hosted_count_pages(const MMIXMMOPlan *mmo,
                                        uint64_t argument_end,
                                        uint64_t sparse_budget,
                                        uint64_t *materialized_pages,
                                        Error **errp)
{
    g_autoptr(GArray) pages = g_array_new(false, false, sizeof(uint64_t));
    uint64_t first_argument_page =
        MMIX_SPARSE_POOL_BASE / MMIX_SPARSE_PAGE_SIZE;
    uint64_t last_argument_page =
        (argument_end - 1) / MMIX_SPARSE_PAGE_SIZE;
    uint64_t required_pages;
    uint64_t required_bytes;
    uint64_t page;
    size_t i;

    for (i = 0; i < mmix_mmo_plan_write_count(mmo); i++) {
        const MMIXMMOWrite *write = mmix_mmo_plan_write(mmo, i);

        page = write->address / MMIX_SPARSE_PAGE_SIZE;
        g_array_append_val(pages, page);
    }
    for (page = first_argument_page; page <= last_argument_page; page++) {
        g_array_append_val(pages, page);
    }

    g_array_sort(pages, mmix_mmo_page_compare);
    required_pages = pages->len != 0;
    for (i = 1; i < pages->len; i++) {
        uint64_t previous = g_array_index(pages, uint64_t, i - 1);
        uint64_t current = g_array_index(pages, uint64_t, i);

        if (current != previous) {
            required_pages++;
        }
    }
    g_assert(!umul64_overflow(required_pages, MMIX_SPARSE_PAGE_SIZE,
                              &required_bytes));
    if (required_bytes > sparse_budget) {
        error_setg(errp,
                   "MMIX MMO initial image and arguments require 0x%" PRIx64
                   " materialized bytes, exceeding sparse budget 0x%" PRIx64,
                   required_bytes, sparse_budget);
        return false;
    }

    *materialized_pages = required_pages;
    return true;
}

bool mmix_mmo_hosted_plan_build(const MMIXMMOPlan *mmo,
                                const MMIXMMOHostedOptions *options,
                                MMIXMMOHostedPlan **plan, Error **errp)
{
    g_autoptr(GPtrArray) arguments = NULL;
    g_autoptr(GBytes) argument_data = NULL;
    MMIXMMOHostedPlan *candidate;
    uint64_t argument_size;
    uint64_t argument_end;
    uint64_t materialized_pages;

    g_return_val_if_fail(mmo != NULL, false);
    g_return_val_if_fail(options != NULL, false);
    g_return_val_if_fail(plan != NULL, false);

    if (!mmix_mmo_hosted_validate_options(options, errp)) {
        return false;
    }
    arguments = mmix_mmo_hosted_collect_arguments(options, errp);
    if (!arguments ||
        !mmix_mmo_hosted_argument_size(arguments, &argument_size, errp)) {
        return false;
    }
    argument_data = mmix_mmo_hosted_build_argument_data(
        arguments, argument_size, errp);
    if (!argument_data ||
        uadd64_overflow(MMIX_SPARSE_POOL_BASE, argument_size,
                        &argument_end) ||
        !mmix_mmo_hosted_check_collision(mmo, argument_end, errp) ||
        !mmix_mmo_hosted_count_pages(mmo, argument_end,
                                     options->sparse_budget,
                                     &materialized_pages, errp)) {
        return false;
    }

    candidate = g_new0(MMIXMMOHostedPlan, 1);
    candidate->argument_data = g_steal_pointer(&argument_data);
    candidate->argument_count = arguments->len;
    candidate->argument_end = argument_end;
    candidate->materialized_pages = materialized_pages;
    candidate->sparse_budget = options->sparse_budget;
    mmix_mmo_hosted_plan_free(*plan);
    *plan = candidate;
    return true;
}

static const char *mmix_mmo_write_kind_name(MMIXMMOWriteKind kind)
{
    switch (kind) {
    case MMIX_MMO_WRITE_DATA:
        return "data";
    case MMIX_MMO_WRITE_FIXO:
        return "lop_fixo";
    case MMIX_MMO_WRITE_FIXR:
        return "lop_fixr";
    case MMIX_MMO_WRITE_FIXRX:
        return "lop_fixrx";
    default:
        g_assert_not_reached();
    }
}

static bool mmix_mmo_hosted_xor_tetra(MMIXSparseMemory *memory,
                                      const MMIXMMOWrite *write,
                                      Error **errp)
{
    uint8_t data[sizeof(write->value)];
    uint32_t value;
    Error *local_err = NULL;

    if (!mmix_sparse_memory_read(memory, write->address, data, sizeof(data),
                                 sizeof(data), &local_err)) {
        goto fail;
    }
    value = ldl_be_p(data) ^ write->value;
    stl_be_p(data, value);
    if (!mmix_sparse_memory_write(memory, write->address, data, sizeof(data),
                                  sizeof(data), &local_err)) {
        goto fail;
    }
    return true;

fail:
    error_setg(errp,
               "could not apply MMIX .mmo %s write at 0x%016" PRIx64
               " from tetra %" PRIu64 ": %s",
               mmix_mmo_write_kind_name(write->kind), write->address,
               write->source_tetra, error_get_pretty(local_err));
    error_free(local_err);
    return false;
}

bool mmix_mmo_hosted_plan_commit(const MMIXMMOPlan *mmo,
                                 const MMIXMMOHostedPlan *plan,
                                 MMIXSparseMemory **memory, Error **errp)
{
    MMIXSparseMemory *candidate;
    const uint8_t *argument_data;
    size_t argument_size;
    size_t i;

    g_return_val_if_fail(mmo != NULL, false);
    g_return_val_if_fail(plan != NULL, false);
    g_return_val_if_fail(memory != NULL, false);

    candidate = mmix_sparse_memory_new(plan->sparse_budget, errp);
    if (!candidate) {
        return false;
    }
    for (i = 0; i < mmix_mmo_plan_write_count(mmo); i++) {
        if (!mmix_mmo_hosted_xor_tetra(
                candidate, mmix_mmo_plan_write(mmo, i), errp)) {
            goto fail;
        }
    }

    argument_data = mmix_mmo_hosted_plan_argument_data(plan,
                                                        &argument_size);
    if (!mmix_sparse_memory_write(candidate, MMIX_SPARSE_POOL_BASE,
                                  argument_data, argument_size,
                                  MMIX_MMO_ARGUMENT_ALIGN, errp)) {
        error_prepend(errp, "could not install MMIX MMO Pool arguments: ");
        goto fail;
    }
    if (mmix_sparse_memory_materialized_pages(candidate) !=
        plan->materialized_pages) {
        error_setg(errp,
                   "MMIX MMO commit materialized %" PRIu64
                   " pages instead of the planned %" PRIu64,
                   mmix_sparse_memory_materialized_pages(candidate),
                   plan->materialized_pages);
        goto fail;
    }

    mmix_sparse_memory_free(*memory);
    *memory = candidate;
    return true;

fail:
    mmix_sparse_memory_free(candidate);
    return false;
}

void mmix_mmo_hosted_plan_free(MMIXMMOHostedPlan *plan)
{
    if (!plan) {
        return;
    }

    g_bytes_unref(plan->argument_data);
    g_free(plan);
}

uint64_t mmix_mmo_hosted_plan_argument_count(
    const MMIXMMOHostedPlan *plan)
{
    g_assert(plan);
    return plan->argument_count;
}

uint64_t mmix_mmo_hosted_plan_argv(const MMIXMMOHostedPlan *plan)
{
    g_assert(plan);
    return MMIX_SPARSE_POOL_BASE + MMIX_MMO_ARGUMENT_ALIGN;
}

uint64_t mmix_mmo_hosted_plan_argument_end(const MMIXMMOHostedPlan *plan)
{
    g_assert(plan);
    return plan->argument_end;
}

const uint8_t *mmix_mmo_hosted_plan_argument_data(
    const MMIXMMOHostedPlan *plan, size_t *size)
{
    g_assert(plan);
    return g_bytes_get_data(plan->argument_data, size);
}

uint64_t mmix_mmo_hosted_plan_materialized_pages(
    const MMIXMMOHostedPlan *plan)
{
    g_assert(plan);
    return plan->materialized_pages;
}

uint64_t mmix_mmo_hosted_plan_materialized_bytes(
    const MMIXMMOHostedPlan *plan)
{
    return mmix_mmo_hosted_plan_materialized_pages(plan) *
           MMIX_SPARSE_PAGE_SIZE;
}
