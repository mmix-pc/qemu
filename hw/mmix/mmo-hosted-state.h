/*
 * MMIX MMO hosted-memory migration state
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_MMO_HOSTED_STATE_H
#define HW_MMIX_MMO_HOSTED_STATE_H

#include "migration/qemu-file-types.h"
#include "qapi/error.h"
#include "sparse-memory.h"

bool mmix_mmo_hosted_state_save(QEMUFile *f,
                                const MMIXSparseMemory *memory,
                                Error **errp);
bool mmix_mmo_hosted_state_load(QEMUFile *f, bool expected_hosted,
                                uint64_t expected_budget,
                                MMIXSparseMemory **memory, Error **errp);

#endif
