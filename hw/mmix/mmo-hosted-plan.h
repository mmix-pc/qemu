/*
 * MMIX MMO hosted startup planner
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_MMO_HOSTED_PLAN_H
#define HW_MMIX_MMO_HOSTED_PLAN_H

#include "qapi/error.h"
#include "mmo-loader.h"
#include "sparse-memory.h"

typedef struct MMIXMMOHostedOptions {
    const char *kernel_filename;
    const char *append;
    const char *const *explicit_arguments;
    size_t explicit_argument_count;
    uint64_t sparse_budget;
    unsigned int cpu_count;
    bool has_explicit_arguments;
    bool semihosting_enabled;
    bool has_initrd;
    bool has_explicit_elf_startup_abi;
    bool has_firmware;
    bool linux_handoff;
} MMIXMMOHostedOptions;

typedef struct MMIXMMOHostedPlan MMIXMMOHostedPlan;

/* A failed build leaves the caller's current plan unchanged. */
bool mmix_mmo_hosted_plan_build(const MMIXMMOPlan *mmo,
                                const MMIXMMOHostedOptions *options,
                                MMIXMMOHostedPlan **plan, Error **errp);
void mmix_mmo_hosted_plan_free(MMIXMMOHostedPlan *plan);

/* A failed commit leaves the caller's current sparse store unchanged. */
bool mmix_mmo_hosted_plan_commit(const MMIXMMOPlan *mmo,
                                 const MMIXMMOHostedPlan *plan,
                                 MMIXSparseMemory **memory, Error **errp);

uint64_t mmix_mmo_hosted_plan_argument_count(
    const MMIXMMOHostedPlan *plan);
uint64_t mmix_mmo_hosted_plan_argv(const MMIXMMOHostedPlan *plan);
uint64_t mmix_mmo_hosted_plan_argument_end(
    const MMIXMMOHostedPlan *plan);
const uint8_t *mmix_mmo_hosted_plan_argument_data(
    const MMIXMMOHostedPlan *plan, size_t *size);
uint64_t mmix_mmo_hosted_plan_materialized_pages(
    const MMIXMMOHostedPlan *plan);
uint64_t mmix_mmo_hosted_plan_materialized_bytes(
    const MMIXMMOHostedPlan *plan);

#endif
