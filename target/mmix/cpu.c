/*
 * QEMU MMIX CPU
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
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

    return (TCGTBCPUState){ .pc = env->pc, .flags = 0 };
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
}

static bool mmix_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}

static int mmix_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
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
        mmix_cpu_raise_dynamic_trap(env, causes);
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
    };

    if ((int64_t)address < 0) {
        if (!mmix_cpu_is_privileged(env)) {
            return mmix_finish_translation_fault(env, translation,
                                                 MMIX_RQ_PROGRAM_N,
                                                 allow_traps && !debug);
        }
        translation->physical = address & ~(1ULL << 63);
        translation->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return true;
    }

    if (env->flat_translation) {
        if (mmix_bare_data_segment_to_phys(address, &translation->physical)) {
            translation->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            return true;
        }
        if (mmix_bare_pool_segment_to_phys(address, &translation->physical)) {
            translation->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            return true;
        }
        if (mmix_bare_unsupported_high_segment(address)) {
            return mmix_finish_translation_fault(env, translation, causes,
                                                 allow_traps && !debug);
        }
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

    if (s < 13 || s > 48 || function != 0) {
        return mmix_finish_translation_fault(env, translation, causes,
                                             allow_traps && !debug);
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

    memset(&cpu->env, 0, offsetof(CPUMMIXState, end_reset_fields));
    cpu->env.pc = 0;
    cpu->env.npc = 4;
    cpu->env.sregs[MMIX_SREG_RK] = MMIX_INITIAL_RK;
    cpu->env.sregs[MMIX_SREG_RT] = MMIX_INITIAL_RT;
    cpu->env.sregs[MMIX_SREG_RTT] = MMIX_INITIAL_RTT;
    cpu->env.sregs[MMIX_SREG_RV] = MMIX_INITIAL_RV;
    cpu->env.sregs[MMIX_SREG_RG] = MMIX_INITIAL_RG;
    cpu->env.sregs[MMIX_SREG_RL] = MMIX_INITIAL_RL;
    cpu->env.sregs[MMIX_SREG_RO] = MMIX_INITIAL_STACK;
    cpu->env.sregs[MMIX_SREG_RS] = MMIX_INITIAL_STACK;
    cpu->env.flat_translation = true;
    cpu->env.lring_size = MMIX_LOCAL_REGS;
    cpu->env.lring_mask = MMIX_LOCAL_REGS - 1;
    cs->exception_index = -1;
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

void mmix_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUMMIXState *env = cpu_env(cs);
    int i;

    qemu_fprintf(f,
                 "pc=0x%016" PRIx64 " npc=0x%016" PRIx64
                 " rG=%" PRIu64 " rL=%" PRIu64
                 " rO=0x%016" PRIx64 " rS=0x%016" PRIx64
                 " lring_size=%u lring_mask=0x%08x\n",
                 env->pc, env->npc, env->sregs[MMIX_SREG_RG],
                 env->sregs[MMIX_SREG_RL], env->sregs[MMIX_SREG_RO],
                 env->sregs[MMIX_SREG_RS], env->lring_size,
                 env->lring_mask);
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
        mmix_cpu_record_program_exception(env, translation.causes);
        cs->exception_index = EXCP_MMIX_DYNAMIC_TRAP;
        cpu_loop_exit_restore(cs, retaddr);
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
    .mttcg_supported = false,

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
        .class_init = mmix_cpu_class_init,
        .abstract = true,
    },
    {
        .name = TYPE_MMIX_ANY_CPU,
        .parent = TYPE_MMIX_CPU,
    },
};

DEFINE_TYPES(mmix_cpu_type_info)
