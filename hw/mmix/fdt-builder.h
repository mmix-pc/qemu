/*
 * MMIX virt flattened device tree builder
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_FDT_BUILDER_H
#define HW_MMIX_FDT_BUILDER_H

#include "qapi/error.h"

enum {
    MMIX_FDT_COMMAND_LINE_MAX = 4095,
    MMIX_FDT_MAX_SIZE = 0x200000,
};

typedef struct MMIXFDTConfig {
    uint64_t ram_size;
    const char *command_line;
} MMIXFDTConfig;

/* A failed build leaves an existing result unchanged. */
bool mmix_fdt_build(const MMIXFDTConfig *config, GBytes **result,
                    Error **errp);

#endif
