/*
 * MMIX virt flattened device tree builder
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_FDT_BUILDER_H
#define HW_MMIX_FDT_BUILDER_H

#include "qapi/error.h"
#include "physical-layout.h"

enum {
    MMIX_FDT_COMMAND_LINE_MAX = 4095,
    MMIX_FDT_MAX_SIZE = 0x200000,
};

typedef struct MMIXFDTConfig {
    uint64_t ram_size;
    const char *command_line;
    unsigned int cpu_count;
    /* One range per CPU, indexed by the contiguous CPU ID. */
    const MMIXPhysRange *cpu_stacks;
    bool has_framebuffer;
    /* Page-rounded backing reservation, not just the visible pixel bytes. */
    MMIXPhysRange framebuffer;
    bool linux_direct;
    bool has_initrd;
    uint64_t initrd_size;
} MMIXFDTConfig;

/* A failed build leaves an existing result unchanged. */
bool mmix_fdt_build(const MMIXFDTConfig *config, GBytes **result,
                    Error **errp);

/* Finalization replaces fixed-size Linux placement placeholders. */
bool mmix_fdt_finalize_linux(GBytes *template,
                             const MMIXPhysRange *fdt_range,
                             const MMIXPhysRange *initrd_range,
                             GBytes **result, Error **errp);

#endif
