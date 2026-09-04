/*
 * MMIX architectural address conversion
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MMIX_ADDRESSING_H
#define TARGET_MMIX_ADDRESSING_H

#include <stdbool.h>
#include <stdint.h>

#define MMIX_DIRECT_PHYS_LIMIT     UINT64_C(0x8000000000000000)
#define MMIX_NEGATIVE_ALIAS_BIT    UINT64_C(0x8000000000000000)

static inline bool mmix_phys_to_negative_alias(uint64_t physical,
                                                uint64_t *alias)
{
    if (physical >= MMIX_DIRECT_PHYS_LIMIT) {
        return false;
    }

    *alias = physical | MMIX_NEGATIVE_ALIAS_BIT;
    return true;
}

static inline bool mmix_negative_alias_to_phys(uint64_t alias,
                                                uint64_t *physical)
{
    if ((alias & MMIX_NEGATIVE_ALIAS_BIT) == 0) {
        return false;
    }

    *physical = alias & ~MMIX_NEGATIVE_ALIAS_BIT;
    return true;
}

#endif
