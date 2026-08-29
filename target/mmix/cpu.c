/*
 * QEMU MMIX CPU
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "addressing.h"
#include "cpu.h"
#include "machine.h"
#include "semihosting.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"

#define MMIX_PHYS_MASK ((1ULL << 48) - 1)
#define MMIX_PTE_N_SHIFT 3
#define MMIX_PTE_N_MASK 0x3ffULL
#define MMIX_PTE_P_MASK 0x7ULL
#define MMIX_PTE_PR 0x4
#define MMIX_PTE_PW 0x2
#define MMIX_PTE_PX 0x1
#define MMIX_PTP_SIGN (1ULL << 63)
#define MMIX_PTP_C_SHIFT 13
#define MMIX_PTP_C_MASK ((1ULL << 50) - 1)
#define MMIX_PT_BLOCK_SIZE (1ULL << 13)

typedef enum MMIXTranslationCacheKind {
    MMIX_TRANSLATION_CACHE_INSTRUCTION,
    MMIX_TRANSLATION_CACHE_DATA,
} MMIXTranslationCacheKind;

typedef struct MMIXTranslationCacheEntry {
    uint64_t key;
    uint64_t pte;
    uint8_t page_shift;
} MMIXTranslationCacheEntry;

static const char * const mmix_sreg_names[MMIX_SREGS] = {
    [MMIX_SREG_RB] = "rB",
    [MMIX_SREG_RD] = "rD",
    [MMIX_SREG_RE] = "rE",
    [MMIX_SREG_RH] = "rH",
    [MMIX_SREG_RJ] = "rJ",
    [MMIX_SREG_RM] = "rM",
    [MMIX_SREG_RR] = "rR",
    [MMIX_SREG_RBB] = "rBB",
    [MMIX_SREG_RC] = "rC",
    [MMIX_SREG_RN] = "rN",
    [MMIX_SREG_RO] = "rO",
    [MMIX_SREG_RS] = "rS",
    [MMIX_SREG_RI] = "rI",
    [MMIX_SREG_RT] = "rT",
    [MMIX_SREG_RTT] = "rTT",
    [MMIX_SREG_RK] = "rK",
    [MMIX_SREG_RQ] = "rQ",
    [MMIX_SREG_RU] = "rU",
    [MMIX_SREG_RV] = "rV",
    [MMIX_SREG_RG] = "rG",
    [MMIX_SREG_RL] = "rL",
    [MMIX_SREG_RA] = "rA",
    [MMIX_SREG_RF] = "rF",
    [MMIX_SREG_RP] = "rP",
    [MMIX_SREG_RW] = "rW",
    [MMIX_SREG_RX] = "rX",
    [MMIX_SREG_RY] = "rY",
    [MMIX_SREG_RZ] = "rZ",
    [MMIX_SREG_RWW] = "rWW",
    [MMIX_SREG_RXX] = "rXX",
    [MMIX_SREG_RYY] = "rYY",
    [MMIX_SREG_RZZ] = "rZZ",
};

static void mmix_cpu_set_pc(CPUState *cs, vaddr value)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    cpu->env.pc = value;
    cpu->env.npc = value + 4;
}

static vaddr mmix_cpu_get_pc(CPUState *cs)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    return cpu->env.pc;
}

static TCGTBCPUState mmix_get_tb_cpu_state(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);
    uint64_t cs_base = 0;

    if (env->insn_replay.active) {
        cs_base = MMIX_TB_REPLAY_FLAG | env->insn_replay.insn;
        if (env->insn_replay.substitute_operands) {
            cs_base |= MMIX_TB_REPLAY_SUBSTITUTE_FLAG;
        }
    }

    return (TCGTBCPUState){
        .pc = env->pc,
        .flags = 0,
        .cs_base = cs_base,
    };
}

static void mmix_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.pc = tb->pc;
    cpu->env.npc = tb->pc + 4;
}

static void mmix_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    cpu->env.pc = data[0];
    cpu->env.npc = data[0] + 4;
    cpu->env.data_access_insn = data[1];
}

static bool mmix_cpu_has_work(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);

    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
           mmix_cpu_interrupt_enabled(env);
}

bool mmix_cpu_interrupt_enabled(CPUMMIXState *env)
{
    return env->sregs[MMIX_SREG_RQ] &
           env->sregs[MMIX_SREG_RK] &
           MMIX_RQ_HARDWARE_MASK;
}

void mmix_cpu_update_interrupt(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    if (env->interrupt_controller_level || env->ipi_level ||
        mmix_cpu_interrupt_enabled(env)) {
        cpu_set_interrupt(cs, CPU_INTERRUPT_HARD);
        if (!qemu_cpu_is_self(cs)) {
            qemu_cpu_kick(cs);
        }
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void mmix_cpu_set_interrupt_controller_work(CPUState *cs,
                                                    run_on_cpu_data data)
{
    CPUMMIXState *env = cpu_env(cs);
    int level = data.host_int;

    env->interrupt_controller_level = level;
    if (level) {
        mmix_cpu_set_rq_bits(env, MMIX_RQ_INTERRUPT_CONTROLLER);
    }
    mmix_cpu_update_interrupt(env);
}

void mmix_cpu_set_interrupt_controller(CPUState *cs, int level)
{
    run_on_cpu_data data = RUN_ON_CPU_HOST_INT(level);

    if (qemu_cpu_is_self(cs)) {
        mmix_cpu_set_interrupt_controller_work(cs, data);
    } else {
        async_run_on_cpu(cs, mmix_cpu_set_interrupt_controller_work, data);
    }
}

static void mmix_cpu_set_ipi_work(CPUState *cs, run_on_cpu_data data)
{
    CPUMMIXState *env = cpu_env(cs);
    int level = data.host_int;

    env->ipi_level = level;
    if (level) {
        mmix_cpu_set_rq_bits(env, MMIX_RQ_IPI);
    }
    mmix_cpu_update_interrupt(env);
}

void mmix_cpu_set_ipi(CPUState *cs, int level)
{
    run_on_cpu_data data = RUN_ON_CPU_HOST_INT(level);

    if (qemu_cpu_is_self(cs)) {
        mmix_cpu_set_ipi_work(cs, data);
    } else {
        async_run_on_cpu(cs, mmix_cpu_set_ipi_work, data);
    }
}

static void mmix_cpu_set_ipi_gpio(void *opaque, int irq, int level)
{
    g_assert(irq == 0);
    mmix_cpu_set_ipi(CPU(opaque), level);
}

static int mmix_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return ifetch ? 1 : 0;
}

static uint64_t mmix_access_cause(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_DATA_STORE:
        return MMIX_RQ_PROGRAM_W;
    case MMU_INST_FETCH:
        return MMIX_RQ_PROGRAM_X;
    case MMU_DATA_LOAD:
    default:
        return MMIX_RQ_PROGRAM_R;
    }
}

static int mmix_access_prot(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_DATA_STORE:
        return PAGE_WRITE;
    case MMU_INST_FETCH:
        return PAGE_EXEC;
    case MMU_DATA_LOAD:
    default:
        return PAGE_READ;
    }
}

static int mmix_pte_prot(uint64_t pte)
{
    uint64_t p = pte & MMIX_PTE_P_MASK;
    int prot = 0;

    if (p & MMIX_PTE_PR) {
        prot |= PAGE_READ;
    }
    if (p & MMIX_PTE_PW) {
        prot |= PAGE_WRITE;
    }
    if (p & MMIX_PTE_PX) {
        prot |= PAGE_EXEC;
    }
    return prot;
}

static MMIXTranslationCacheKind
mmix_translation_cache_kind(MMUAccessType access_type)
{
    return access_type == MMU_INST_FETCH ?
           MMIX_TRANSLATION_CACHE_INSTRUCTION :
           MMIX_TRANSLATION_CACHE_DATA;
}

static GArray *mmix_translation_cache(CPUMMIXState *env,
                                      MMIXTranslationCacheKind kind)
{
    MMIXCPU *cpu = env_archcpu(env);

    return kind == MMIX_TRANSLATION_CACHE_INSTRUCTION ?
           cpu->instruction_translation_cache : cpu->data_translation_cache;
}

static uint64_t mmix_translation_key(vaddr address, uint8_t page_shift,
                                     uint64_t address_space_number)
{
    uint64_t page_mask = ~((1ULL << page_shift) - 1);

    return (address & page_mask) | (address_space_number << 3);
}

static MMIXTranslationCacheEntry *
mmix_translation_cache_find(CPUMMIXState *env,
                            MMIXTranslationCacheKind kind, uint64_t key,
                            uint8_t page_shift)
{
    GArray *cache = mmix_translation_cache(env, kind);
    unsigned int i;

    for (i = 0; i < cache->len; i++) {
        MMIXTranslationCacheEntry *entry =
            &g_array_index(cache, MMIXTranslationCacheEntry, i);

        if (entry->key == key && entry->page_shift == page_shift) {
            return entry;
        }
    }
    return NULL;
}

static void mmix_translation_cache_insert(CPUMMIXState *env,
                                          MMUAccessType access_type,
                                          vaddr address, uint8_t page_shift,
                                          uint64_t address_space_number,
                                          uint64_t pte)
{
    MMIXTranslationCacheKind kind =
        mmix_translation_cache_kind(access_type);
    GArray *cache = mmix_translation_cache(env, kind);
    MMIXTranslationCacheEntry entry = {
        .key = mmix_translation_key(address, page_shift,
                                    address_space_number),
        .pte = pte,
        .page_shift = page_shift,
    };
    MMIXTranslationCacheEntry *existing =
        mmix_translation_cache_find(env, kind, entry.key, page_shift);

    if (existing != NULL) {
        *existing = entry;
    } else {
        g_array_append_val(cache, entry);
    }
}

static bool mmix_translation_cache_lookup(CPUMMIXState *env,
                                          MMUAccessType access_type,
                                          vaddr address, uint8_t page_shift,
                                          uint64_t address_space_number,
                                          MMIXAddressTranslation *translation)
{
    MMIXTranslationCacheEntry *entry = mmix_translation_cache_find(
        env, mmix_translation_cache_kind(access_type),
        mmix_translation_key(address, page_shift, address_space_number),
        page_shift);
    uint64_t page_mask;

    if (entry == NULL) {
        return false;
    }

    page_mask = ~((1ULL << entry->page_shift) - 1);
    translation->physical =
        (entry->pte & MMIX_PHYS_MASK & page_mask) | (address & ~page_mask);
    translation->page_size = 1ULL << entry->page_shift;
    translation->prot = mmix_pte_prot(entry->pte);
    return true;
}

void mmix_cpu_flush_translation_caches(CPUMMIXState *env)
{
    MMIXCPU *cpu = env_archcpu(env);

    g_array_set_size(cpu->instruction_translation_cache, 0);
    g_array_set_size(cpu->data_translation_cache, 0);
    tlb_flush(env_cpu(env));
}

uint64_t mmix_cpu_ldvts(CPUMMIXState *env, uint64_t key)
{
    uint64_t rv = env->sregs[MMIX_SREG_RV];
    uint8_t page_shift = extract64(rv, 40, 8);
    uint64_t lookup_key;
    uint64_t status = 0;
    uint8_t permissions = key & MMIX_PTE_P_MASK;
    int kind;

    if (env->flat_translation || page_shift < 13 || page_shift > 48) {
        return 0;
    }

    lookup_key = key & ~MMIX_PTE_P_MASK;
    for (kind = MMIX_TRANSLATION_CACHE_INSTRUCTION;
         kind <= MMIX_TRANSLATION_CACHE_DATA; kind++) {
        GArray *cache = mmix_translation_cache(env, kind);
        unsigned int i;

        for (i = 0; i < cache->len; i++) {
            MMIXTranslationCacheEntry *entry =
                &g_array_index(cache, MMIXTranslationCacheEntry, i);

            if (entry->key != lookup_key ||
                entry->page_shift != page_shift) {
                continue;
            }

            status |= kind == MMIX_TRANSLATION_CACHE_INSTRUCTION ? 1 : 2;
            if (permissions == 0) {
                g_array_remove_index(cache, i);
            } else {
                entry->pte = (entry->pte & ~MMIX_PTE_P_MASK) | permissions;
            }
            break;
        }
    }

    if (status != 0) {
        tlb_flush(env_cpu(env));
    }
    return status;
}

bool mmix_cpu_install_translation(CPUMMIXState *env, vaddr address,
                                  uint64_t pte,
                                  MMUAccessType access_type)
{
    uint64_t rv = env->sregs[MMIX_SREG_RV];
    uint8_t page_shift = extract64(rv, 40, 8);
    uint64_t address_space_number = extract64(rv, 3, 10);
    uint8_t function = extract64(rv, 0, 3);
    hwaddr physical_page;

    if ((access_type != MMU_INST_FETCH && access_type != MMU_DATA_LOAD) ||
        env->flat_translation || (int64_t)address < 0 ||
        page_shift < 13 || page_shift > 48 || function != 1 ||
        ((pte >> MMIX_PTE_N_SHIFT) & MMIX_PTE_N_MASK) !=
        address_space_number) {
        return false;
    }

    physical_page = pte & (MMIX_PHYS_MASK &
                           ~((1ULL << page_shift) - 1));
    if (!memory_region_present(get_system_memory(), physical_page)) {
        return false;
    }

    mmix_translation_cache_insert(env, access_type, address, page_shift,
                                  address_space_number, pte);
    tlb_flush(env_cpu(env));
    return true;
}

static bool mmix_read_phys_octa(CPUState *cs, hwaddr address, uint64_t *value)
{
    MemTxResult result;

    *value = address_space_ldq_be(cs->as, address, MEMTXATTRS_UNSPECIFIED,
                                  &result);
    return result == MEMTX_OK;
}

static bool mmix_finish_translation_fault(CPUMMIXState *env,
                                          MMIXAddressTranslation *translation,
                                          uint64_t causes, bool allow_traps)
{
    translation->causes = causes;
    if (allow_traps) {
        mmix_cpu_raise_dynamic_trap(env, causes, env->data_access_insn);
    }
    return false;
}

static bool mmix_validate_ptp(uint64_t ptp, uint64_t address_space_number)
{
    return (ptp & MMIX_PTP_SIGN) != 0 &&
           ((ptp >> MMIX_PTE_N_SHIFT) & MMIX_PTE_N_MASK) ==
           address_space_number;
}

bool mmix_translate_address(CPUMMIXState *env, vaddr address,
                            MMUAccessType access_type, bool debug,
                            bool allow_traps,
                            MMIXAddressTranslation *translation)
{
    CPUState *cs = env_cpu(env);
    uint64_t causes = mmix_access_cause(access_type);
    uint64_t rv = env->sregs[MMIX_SREG_RV];
    uint8_t b[5];
    uint8_t s;
    uint64_t root;
    uint64_t address_space_number;
    uint8_t function;
    uint8_t segment;
    int table_span;
    uint64_t segment_address;
    uint64_t page_number;
    uint64_t page_offset;
    uint16_t digit[5];
    uint64_t remaining;
    unsigned highest_digit = 0;
    hwaddr table_base;
    hwaddr entry_address;
    uint64_t entry;
    uint64_t pte;
    uint64_t page_base;
    int prot;
    int i;

    *translation = (MMIXAddressTranslation) {
        .physical = 0,
        .page_size = TARGET_PAGE_SIZE,
        .prot = 0,
        .causes = 0,
        .forced_translation = false,
    };

    if ((int64_t)address < 0) {
        if (!mmix_cpu_is_privileged(env)) {
            return mmix_finish_translation_fault(env, translation,
                                                 MMIX_RQ_PROGRAM_N,
                                                 allow_traps && !debug);
        }
        if (!mmix_negative_alias_to_phys(address, &translation->physical)) {
            return mmix_finish_translation_fault(env, translation, causes,
                                                 allow_traps && !debug);
        }
        translation->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return true;
    }

    if (env->flat_translation) {
        translation->physical = address;
        translation->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return true;
    }

    b[0] = 0;
    b[1] = extract64(rv, 60, 4);
    b[2] = extract64(rv, 56, 4);
    b[3] = extract64(rv, 52, 4);
    b[4] = extract64(rv, 48, 4);
    s = extract64(rv, 40, 8);
    root = extract64(rv, 13, 27);
    address_space_number = extract64(rv, 3, 10);
    function = extract64(rv, 0, 3);

    if (s < 13 || s > 48 || function > 1) {
        return mmix_finish_translation_fault(env, translation, causes,
                                             allow_traps && !debug);
    }

    if (mmix_translation_cache_lookup(env, access_type, address, s,
                                      address_space_number, translation)) {
        if ((translation->prot & mmix_access_prot(access_type)) == 0) {
            return mmix_finish_translation_fault(env, translation, causes,
                                                 allow_traps && !debug);
        }
        return true;
    }

    if (function == 1) {
        translation->forced_translation = true;
        return false;
    }

    segment = address >> 61;
    segment_address = address & ((1ULL << 61) - 1);
    page_number = segment_address >> s;
    page_offset = segment_address & ((1ULL << s) - 1);
    table_span = (int)b[segment + 1] - (int)b[segment];

    if (table_span < 0) {
        return mmix_finish_translation_fault(env, translation, causes,
                                             allow_traps && !debug);
    }

    remaining = page_number;
    for (i = 0; i < ARRAY_SIZE(digit); i++) {
        digit[i] = remaining & 0x3ff;
        if (digit[i] != 0) {
            highest_digit = i;
        }
        remaining >>= 10;
    }
    if (remaining != 0 ||
        (table_span == 0 && page_number != 0) ||
        (table_span > 0 && highest_digit >= table_span)) {
        return mmix_finish_translation_fault(env, translation, causes,
                                             allow_traps && !debug);
    }

    if (highest_digit == 0) {
        entry_address = ((hwaddr)root + b[segment]) * MMIX_PT_BLOCK_SIZE +
                        (hwaddr)digit[0] * 8;
    } else {
        table_base = ((hwaddr)root + b[segment] + highest_digit) *
                     MMIX_PT_BLOCK_SIZE;
        entry_address = table_base + (hwaddr)digit[highest_digit] * 8;
        if (!mmix_read_phys_octa(cs, entry_address, &entry) ||
            !mmix_validate_ptp(entry, address_space_number)) {
            return mmix_finish_translation_fault(env, translation, causes,
                                                 allow_traps && !debug);
        }

        table_base = ((entry >> MMIX_PTP_C_SHIFT) & MMIX_PTP_C_MASK) *
                     MMIX_PT_BLOCK_SIZE;
        for (i = highest_digit - 1; i >= 1; i--) {
            entry_address = table_base + (hwaddr)digit[i] * 8;
            if (!mmix_read_phys_octa(cs, entry_address, &entry) ||
                !mmix_validate_ptp(entry, address_space_number)) {
                return mmix_finish_translation_fault(env, translation, causes,
                                                     allow_traps && !debug);
            }
            table_base = ((entry >> MMIX_PTP_C_SHIFT) & MMIX_PTP_C_MASK) *
                         MMIX_PT_BLOCK_SIZE;
        }
        entry_address = table_base + (hwaddr)digit[0] * 8;
    }

    if (!mmix_read_phys_octa(cs, entry_address, &pte) ||
        ((pte >> MMIX_PTE_N_SHIFT) & MMIX_PTE_N_MASK) !=
        address_space_number) {
        return mmix_finish_translation_fault(env, translation, causes,
                                             allow_traps && !debug);
    }

    prot = mmix_pte_prot(pte);
    if ((prot & mmix_access_prot(access_type)) == 0) {
        return mmix_finish_translation_fault(env, translation, causes,
                                             allow_traps && !debug);
    }

    page_base = pte & (MMIX_PHYS_MASK & ~((1ULL << s) - 1));
    translation->physical = page_base | page_offset;
    translation->page_size = 1ULL << s;
    translation->prot = prot;
    if (!debug) {
        mmix_translation_cache_insert(env, access_type, address, s,
                                      address_space_number, pte);
    }
    return true;
}

static void mmix_cpu_reset_hold(Object *obj, ResetType type)
{
    MMIXCPU *cpu = MMIX_CPU(obj);
    MMIXCPUClass *mcc = MMIX_CPU_GET_CLASS(obj);
    CPUState *cs = CPU(obj);

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    mmix_cpu_release_semihosting_file_handles(&cpu->env);
    mmix_cpu_flush_translation_caches(&cpu->env);
    g_array_set_size(cpu->trap_restart_stack, 0);

    memset(&cpu->env, 0, offsetof(CPUMMIXState, end_reset_fields));
    cpu->env.pc = 0;
    cpu->env.npc = 4;
    cpu->env.sregs[MMIX_SREG_RK] = MMIX_INITIAL_RK;
    cpu->env.sregs[MMIX_SREG_RT] = MMIX_INITIAL_RT;
    cpu->env.sregs[MMIX_SREG_RTT] = MMIX_INITIAL_RTT;
    cpu->env.sregs[MMIX_SREG_RV] = MMIX_INITIAL_RV;
    cpu->env.sregs[MMIX_SREG_RG] = MMIX_INITIAL_RG;
    cpu->env.sregs[MMIX_SREG_RL] = MMIX_INITIAL_RL;
    cpu->env.sregs[MMIX_SREG_RO] = cpu->initial_stack;
    cpu->env.sregs[MMIX_SREG_RS] = cpu->initial_stack;
    cpu->env.flat_translation = true;
    cpu->env.lring_size = MMIX_LOCAL_REGS;
    cpu->env.lring_mask = MMIX_LOCAL_REGS - 1;
    cs->exception_index = -1;
}

static void mmix_trap_restart_clear(gpointer data)
{
    MMIXTrapRestartState *restart = data;

    g_free(restart->save_unsave);
}

static void mmix_cpu_initfn(Object *obj)
{
    MMIXCPU *cpu = MMIX_CPU(obj);

    qdev_init_gpio_in_named(DEVICE(obj), mmix_cpu_set_ipi_gpio, "ipi", 1);

    cpu->instruction_translation_cache =
        g_array_new(false, false, sizeof(MMIXTranslationCacheEntry));
    cpu->data_translation_cache =
        g_array_new(false, false, sizeof(MMIXTranslationCacheEntry));
    cpu->trap_restart_stack =
        g_array_new(false, false, sizeof(MMIXTrapRestartState));
    g_array_set_clear_func(cpu->trap_restart_stack,
                           mmix_trap_restart_clear);
}

static const Property mmix_cpu_properties[] = {
    DEFINE_PROP_UINT64("initial-stack", MMIXCPU, initial_stack,
                       MMIX_INITIAL_STACK),
};

static void mmix_cpu_finalize(Object *obj)
{
    MMIXCPU *cpu = MMIX_CPU(obj);

    g_array_unref(cpu->instruction_translation_cache);
    g_array_unref(cpu->data_translation_cache);
    g_array_unref(cpu->trap_restart_stack);
}

static ObjectClass *mmix_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    oc = object_class_by_name(cpu_model);
    if (oc != NULL && object_class_dynamic_cast(oc, TYPE_MMIX_CPU) != NULL) {
        return oc;
    }

    typename = g_strdup_printf(MMIX_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);

    return oc;
}

static void mmix_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    MMIXCPUClass *mcc = MMIX_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    mcc->parent_realize(dev, errp);
}

static const char *mmix_stack_access_name(MMIXStackAccessKind kind)
{
    switch (kind) {
    case MMIX_STACK_ACCESS_NONE:
        return "none";
    case MMIX_STACK_ACCESS_SPILL:
        return "spill";
    case MMIX_STACK_ACCESS_FILL:
        return "fill";
    case MMIX_STACK_ACCESS_SAVE:
        return "save";
    case MMIX_STACK_ACCESS_UNSAVE:
        return "unsave";
    default:
        return "unknown";
    }
}

void mmix_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUMMIXState *env = cpu_env(cs);
    int i;

    qemu_fprintf(f,
                 "pc=0x%016" PRIx64 " npc=0x%016" PRIx64
                 " rG=%" PRIu64 " rL=%" PRIu64
                 " rO=0x%016" PRIx64 " rS=0x%016" PRIx64
                 " stack-bottom=0x%016" PRIx64
                 " lring_size=%u lring_mask=0x%08x\n",
                 env->pc, env->npc, env->sregs[MMIX_SREG_RG],
                 env->sregs[MMIX_SREG_RL], env->sregs[MMIX_SREG_RO],
                 env->sregs[MMIX_SREG_RS], MMIX_CPU(cs)->initial_stack,
                 env->lring_size, env->lring_mask);
    qemu_fprintf(f, "register-stack-access=%s",
                 mmix_stack_access_name(env->stack_access.kind));
    if (env->stack_access.kind != MMIX_STACK_ACCESS_NONE) {
        qemu_fprintf(f,
                     " address=0x%016" PRIx64 " ring=%" PRIu32
                     " value=0x%016" PRIx64,
                     env->stack_access.address, env->stack_access.ring_index,
                     env->stack_access.value);
    }
    qemu_fprintf(f, "\n");
    qemu_fprintf(f, "special registers:\n");
    for (i = 0; i < MMIX_SREGS; i += 4) {
        qemu_fprintf(f,
                     "%-3s=0x%016" PRIx64 " %-3s=0x%016" PRIx64
                     " %-3s=0x%016" PRIx64 " %-3s=0x%016" PRIx64 "\n",
                     mmix_sreg_names[i], env->sregs[i],
                     mmix_sreg_names[i + 1], env->sregs[i + 1],
                     mmix_sreg_names[i + 2], env->sregs[i + 2],
                     mmix_sreg_names[i + 3], env->sregs[i + 3]);
    }
    qemu_fprintf(f, "general registers:\n");
    for (i = 0; i < MMIX_REGS; i += 4) {
        qemu_fprintf(f,
                     "r%-3d=0x%016" PRIx64 " r%-3d=0x%016" PRIx64
                     " r%-3d=0x%016" PRIx64 " r%-3d=0x%016" PRIx64 "\n",
                     i, mmix_cpu_read_reg(env, i),
                     i + 1, mmix_cpu_read_reg(env, i + 1),
                     i + 2, mmix_cpu_read_reg(env, i + 2),
                     i + 3, mmix_cpu_read_reg(env, i + 3));
    }
}

static uint64_t mmix_data_access_value(CPUMMIXState *env, uint32_t insn)
{
    uint8_t op = insn >> 24;
    uint8_t x = insn >> 16;
    uint8_t z = insn;

    if (op == 0x94 || op == 0x95 ||
        (op >= 0xa0 && op <= 0xaf) || op == 0xb6 || op == 0xb7) {
        return mmix_cpu_read_reg(env, x);
    }
    if (op == 0xb0 || op == 0xb1) {
        /* STSF[I] records its rounded binary32 value before the access. */
        return env->data_access.value;
    }
    if (op == 0xb2 || op == 0xb3) {
        return mmix_cpu_read_reg(env, x) >> 32;
    }
    if (op == 0xb4 || op == 0xb5) {
        return x;
    }

    g_assert(op >= 0x80 && op <= 0x97);
    return op & 1 ? z : mmix_cpu_read_reg(env, z);
}

static bool mmix_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    CPUMMIXState *env = cpu_env(cs);
    MMIXAddressTranslation translation;
    hwaddr vpage = addr & TARGET_PAGE_MASK;
    hwaddr ppage;

    if (!mmix_translate_address(env, addr, access_type, false, false,
                                &translation)) {
        if (probe) {
            return false;
        }
        if (access_type != MMU_INST_FETCH) {
            cpu_restore_state(cs, retaddr);
        }
        if (translation.forced_translation) {
            env->forced_translation_insn = access_type == MMU_INST_FETCH ?
                                           MMIX_SWYM_INSN :
                                           env->data_access_insn;
            env->forced_translation_address = addr;
            env->forced_translation_access = access_type;
            cs->exception_index = EXCP_MMIX_FORCED_TRANSLATION;
            cpu_loop_exit(cs);
        }
        env->program_exception_data_access =
            access_type != MMU_INST_FETCH &&
            env->stack_access.kind == MMIX_STACK_ACCESS_NONE;
        if (env->program_exception_data_access) {
            env->data_access.address = addr;
            env->data_access.value =
                mmix_data_access_value(env, env->data_access_insn);
        }
        env->program_exception_insn = access_type == MMU_INST_FETCH ?
                                      MMIX_SWYM_INSN :
                                      env->data_access_insn;
        mmix_cpu_record_program_exception(env, translation.causes);
        cs->exception_index = EXCP_MMIX_DYNAMIC_TRAP;
        cpu_loop_exit(cs);
    }

    ppage = translation.physical & TARGET_PAGE_MASK;
    tlb_set_page(cs, vpage, ppage, translation.prot, mmu_idx,
                 TARGET_PAGE_SIZE);
    return true;
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps mmix_sysemu_ops = {
    .has_work = mmix_cpu_has_work,
    .get_phys_addr_debug = mmix_cpu_get_phys_addr_debug,
};

static const TCGCPUOps mmix_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    /*
     * Architectural, register-stack, replay, trap, and translation-cache
     * state is owned by one MMIXCPU. Device IRQ callbacks queue changes to
     * that CPU, while shared RAM and MMIO use the common TCG and BQL paths.
     * MMIX semihosting retains QEMU's common semihosting concurrency limits.
     */
    .mttcg_supported = true,

    .initialize = mmix_translate_init,
    .translate_code = mmix_translate_code,
    .get_tb_cpu_state = mmix_get_tb_cpu_state,
    .synchronize_from_tb = mmix_cpu_synchronize_from_tb,
    .restore_state_to_opc = mmix_restore_state_to_opc,
    .pointer_wrap = cpu_pointer_wrap_notreached,
    .mmu_index = mmix_cpu_mmu_index,
    .tlb_fill = mmix_cpu_tlb_fill,
    .cpu_exec_halt = mmix_cpu_has_work,
    .cpu_exec_interrupt = mmix_cpu_exec_interrupt,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = mmix_cpu_do_interrupt,
};

static void mmix_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    MMIXCPUClass *mcc = MMIX_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, mmix_cpu_realize, &mcc->parent_realize);
    device_class_set_props(dc, mmix_cpu_properties);
    dc->vmsd = &vmstate_mmix_cpu;
    resettable_class_set_parent_phases(rc, NULL, mmix_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);

    cc->class_by_name = mmix_cpu_class_by_name;
    cc->dump_state = mmix_cpu_dump_state;
    cc->set_pc = mmix_cpu_set_pc;
    cc->get_pc = mmix_cpu_get_pc;
    cc->sysemu_ops = &mmix_sysemu_ops;
    cc->gdb_read_register = mmix_cpu_gdb_read_register;
    cc->gdb_write_register = mmix_cpu_gdb_write_register;
    cc->gdb_core_xml_file = "mmix-core.xml";
    cc->tcg_ops = &mmix_tcg_ops;
}

static const TypeInfo mmix_cpu_type_info[] = {
    {
        .name = TYPE_MMIX_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(MMIXCPU),
        .instance_align = __alignof(MMIXCPU),
        .class_size = sizeof(MMIXCPUClass),
        .instance_init = mmix_cpu_initfn,
        .instance_finalize = mmix_cpu_finalize,
        .class_init = mmix_cpu_class_init,
        .abstract = true,
    },
    {
        .name = TYPE_MMIX_ANY_CPU,
        .parent = TYPE_MMIX_CPU,
    },
};

DEFINE_TYPES(mmix_cpu_type_info)
