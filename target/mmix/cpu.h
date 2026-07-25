/*
 * QEMU MMIX CPU
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MMIX_CPU_H
#define MMIX_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"

#ifdef CONFIG_USER_ONLY
#error "MMIX does not support user mode emulation"
#endif

#define MMIX_REGS 256

enum {
    EXCP_ILLEGAL = 1,
};

typedef struct CPUArchState {
    uint64_t regs[MMIX_REGS];
    uint64_t pc;

    struct {} end_reset_fields;
} CPUMMIXState;

struct ArchCPU {
    CPUState parent_obj;

    CPUMMIXState env;
};

struct MMIXCPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define CPU_RESOLVING_TYPE TYPE_MMIX_CPU

void mmix_cpu_do_interrupt(CPUState *cs);
hwaddr mmix_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr);
void mmix_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int mmix_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int mmix_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);

void mmix_translate_init(void);
void mmix_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);

#endif
