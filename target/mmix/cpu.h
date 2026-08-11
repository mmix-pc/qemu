/*
 * QEMU MMIX CPU
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MMIX_CPU_H
#define MMIX_CPU_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "qemu/typedefs.h"

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/mmu-access-type.h"

#ifdef CONFIG_USER_ONLY
#error "MMIX does not support user mode emulation"
#endif

#define MMIX_REGS 256
#define MMIX_SREGS 32
#define MMIX_LOCAL_REGS 256
#define MMIX_SEMIHOSTING_HANDLES 256

#define MMIX_INITIAL_RK 0
#define MMIX_INITIAL_RT 0x8000000500000000ULL
#define MMIX_INITIAL_RTT 0x8000000600000000ULL
#define MMIX_INITIAL_RV 0x369c200400000000ULL
#define MMIX_INITIAL_RG 32
#define MMIX_INITIAL_RL 0
#define MMIX_INITIAL_STACK 0x0000000000010000ULL

#define MMIX_DATA_SEGMENT_BASE 0x2000000000000000ULL
#define MMIX_DATA_SEGMENT_PHYS_BASE 0x0000000004000000ULL
#define MMIX_DATA_SEGMENT_SIZE 0x0000000004000000ULL
#define MMIX_POOL_SEGMENT_BASE 0x4000000000000000ULL
#define MMIX_POOL_SEGMENT_PHYS_BASE 0x0000000002000000ULL
#define MMIX_POOL_SEGMENT_SIZE 0x0000000000100000ULL
#define MMIX_POSITIVE_HIGH_SEGMENT_BASE 0x2000000000000000ULL
#define MMIX_NEGATIVE_SEGMENT_BASE 0x8000000000000000ULL

static inline bool mmix_bare_segment_to_phys(uint64_t address, uint64_t base,
                                             hwaddr phys_base, uint64_t size,
                                             hwaddr *physical)
{
    uint64_t offset = address - base;

    if (address < base || offset >= size) {
        return false;
    }

    *physical = phys_base + offset;
    return true;
}

static inline bool mmix_bare_data_segment_to_phys(uint64_t address,
                                                  hwaddr *physical)
{
    return mmix_bare_segment_to_phys(address, MMIX_DATA_SEGMENT_BASE,
                                     MMIX_DATA_SEGMENT_PHYS_BASE,
                                     MMIX_DATA_SEGMENT_SIZE, physical);
}

static inline bool mmix_bare_pool_segment_to_phys(uint64_t address,
                                                  hwaddr *physical)
{
    return mmix_bare_segment_to_phys(address, MMIX_POOL_SEGMENT_BASE,
                                     MMIX_POOL_SEGMENT_PHYS_BASE,
                                     MMIX_POOL_SEGMENT_SIZE, physical);
}

static inline bool mmix_bare_unsupported_high_segment(uint64_t address)
{
    hwaddr physical;

    return address >= MMIX_POSITIVE_HIGH_SEGMENT_BASE &&
           address < MMIX_NEGATIVE_SEGMENT_BASE &&
           !mmix_bare_data_segment_to_phys(address, &physical) &&
           !mmix_bare_pool_segment_to_phys(address, &physical);
}

typedef enum MMIXSpecialReg {
    MMIX_SREG_RB = 0,
    MMIX_SREG_RD = 1,
    MMIX_SREG_RE = 2,
    MMIX_SREG_RH = 3,
    MMIX_SREG_RJ = 4,
    MMIX_SREG_RM = 5,
    MMIX_SREG_RR = 6,
    MMIX_SREG_RBB = 7,
    MMIX_SREG_RC = 8,
    MMIX_SREG_RN = 9,
    MMIX_SREG_RO = 10,
    MMIX_SREG_RS = 11,
    MMIX_SREG_RI = 12,
    MMIX_SREG_RT = 13,
    MMIX_SREG_RTT = 14,
    MMIX_SREG_RK = 15,
    MMIX_SREG_RQ = 16,
    MMIX_SREG_RU = 17,
    MMIX_SREG_RV = 18,
    MMIX_SREG_RG = 19,
    MMIX_SREG_RL = 20,
    MMIX_SREG_RA = 21,
    MMIX_SREG_RF = 22,
    MMIX_SREG_RP = 23,
    MMIX_SREG_RW = 24,
    MMIX_SREG_RX = 25,
    MMIX_SREG_RY = 26,
    MMIX_SREG_RZ = 27,
    MMIX_SREG_RWW = 28,
    MMIX_SREG_RXX = 29,
    MMIX_SREG_RYY = 30,
    MMIX_SREG_RZZ = 31,
} MMIXSpecialReg;

enum {
    EXCP_MMIX_ILLEGAL = 1,
    EXCP_MMIX_INTERRUPT = 2,
    EXCP_MMIX_ARITHMETIC_TRIP = 3,
    EXCP_MMIX_DYNAMIC_TRAP = 4,
};

#define MMIX_RQ_PROGRAM_SHIFT 32
#define MMIX_RQ_PROGRAM_R     (1ULL << 39)
#define MMIX_RQ_PROGRAM_W     (1ULL << 38)
#define MMIX_RQ_PROGRAM_X     (1ULL << 37)
#define MMIX_RQ_PROGRAM_N     (1ULL << 36)
#define MMIX_RQ_PROGRAM_K     (1ULL << 35)
#define MMIX_RQ_PROGRAM_B     (1ULL << 34)
#define MMIX_RQ_PROGRAM_S     (1ULL << 33)
#define MMIX_RQ_PROGRAM_P     (1ULL << 32)
#define MMIX_RQ_PROGRAM_MASK  (0xffULL << MMIX_RQ_PROGRAM_SHIFT)

#define MMIX_RA_EVENT_D    (1u << 7)
#define MMIX_RA_EVENT_V    (1u << 6)
#define MMIX_RA_EVENT_W    (1u << 5)
#define MMIX_RA_EVENT_I    (1u << 4)
#define MMIX_RA_EVENT_O    (1u << 3)
#define MMIX_RA_EVENT_U    (1u << 2)
#define MMIX_RA_EVENT_Z    (1u << 1)
#define MMIX_RA_EVENT_X    (1u << 0)
#define MMIX_RA_EVENT_MASK 0xffu
#define MMIX_RA_ENABLE_SHIFT 8
#define MMIX_RA_ROUND_SHIFT 16
#define MMIX_RA_VALID_MASK 0x3ffffu

typedef struct CPUArchState {
    /*
     * Architectural registers are split by rG: global registers are backed
     * directly here, while local registers are addressed through the
     * local-register ring below.
     */
    uint64_t regs[MMIX_REGS];
    uint64_t sregs[MMIX_SREGS];

    uint64_t pc;
    uint64_t npc;
    bool test_exit_seen;

    uint64_t local_regs[MMIX_LOCAL_REGS];
    uint32_t lring_size;
    uint32_t lring_mask;
    uint32_t arithmetic_trip_event;
    uint64_t program_exception_causes;
    bool flat_translation;
    uint32_t semihosting_file_guestfds[MMIX_SEMIHOSTING_HANDLES];
    uint8_t semihosting_file_modes[MMIX_SEMIHOSTING_HANDLES];
    uint8_t semihosting_pending_open_handle;
    uint8_t semihosting_pending_open_mode;
    uint64_t semihosting_pending_io_length;

    struct {} end_reset_fields;
} CPUMMIXState;

typedef struct MMIXAddressTranslation {
    hwaddr physical;
    uint64_t page_size;
    int prot;
    uint64_t causes;
} MMIXAddressTranslation;

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
bool mmix_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
hwaddr mmix_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr);
void mmix_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int mmix_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int mmix_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);

uint64_t mmix_cpu_read_reg(CPUMMIXState *env, unsigned reg);
void mmix_cpu_write_reg(CPUMMIXState *env, unsigned reg, uint64_t val);
void mmix_cpu_put_rl(CPUMMIXState *env, uint64_t val);
bool mmix_cpu_is_privileged(CPUMMIXState *env);
void mmix_cpu_shutdown_with_log(CPUMMIXState *env, const char *reason,
                                int exit_code);
void mmix_cpu_record_program_exception(CPUMMIXState *env, uint64_t causes);
void mmix_cpu_raise_dynamic_trap(CPUMMIXState *env, uint64_t causes);
bool mmix_translate_address(CPUMMIXState *env, vaddr address,
                            MMUAccessType access_type, bool debug,
                            bool allow_traps,
                            MMIXAddressTranslation *translation);

void mmix_translate_init(void);
void mmix_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);

#endif
