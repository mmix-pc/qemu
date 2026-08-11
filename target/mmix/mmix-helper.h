/*
 * QEMU MMIX internal helper declarations
 *
 * This header is private to target/mmix helper implementation files. The
 * target/mmix/helper.h name is reserved for QEMU's DEF_HELPER_* list.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MMIX_MMIX_HELPER_H
#define MMIX_MMIX_HELPER_H

#include "cpu.h"

void mmix_cpu_put_rk(CPUMMIXState *env, uint64_t val);
void mmix_update_ra_events(CPUMMIXState *env, uint32_t events,
                           uint32_t insn, uint64_t y, uint64_t z);

#endif /* MMIX_MMIX_HELPER_H */
