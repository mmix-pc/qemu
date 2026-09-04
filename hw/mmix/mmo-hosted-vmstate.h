/*
 * MMIX MMO hosted-memory VMState device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_MMO_HOSTED_VMSTATE_H
#define HW_MMIX_MMO_HOSTED_VMSTATE_H

#include "qom/object.h"
#include "sparse-memory.h"

#define TYPE_MMIX_MMO_HOSTED_STATE "mmix-mmo-hosted-state"
OBJECT_DECLARE_SIMPLE_TYPE(MMIXMMOHostedState, MMIX_MMO_HOSTED_STATE)

void mmix_mmo_hosted_state_configure(MMIXMMOHostedState *state,
                                     MMIXSparseMemory **memory,
                                     CPUState **cpus,
                                     unsigned int cpu_count,
                                     uint64_t expected_budget);

#endif
