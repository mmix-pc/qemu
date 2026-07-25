/*
 * QEMU MMIX CPU
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"

static void mmix_cpu_set_pc(CPUState *cs, vaddr value)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    cpu->env.pc = value;
}

static vaddr mmix_cpu_get_pc(CPUState *cs)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    return cpu->env.pc;
}

static TCGTBCPUState mmix_get_tb_cpu_state(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);

    return (TCGTBCPUState){ .pc = env->pc };
}

static void mmix_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.pc = tb->pc;
}

static void mmix_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    cpu->env.pc = data[0];
}

static bool mmix_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}

static int mmix_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static void mmix_cpu_reset_hold(Object *obj, ResetType type)
{
    MMIXCPU *cpu = MMIX_CPU(obj);
    MMIXCPUClass *mcc = MMIX_CPU_GET_CLASS(obj);
    CPUState *cs = CPU(obj);

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    memset(&cpu->env, 0, offsetof(CPUMMIXState, end_reset_fields));
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

    qemu_fprintf(f, "pc=0x%016" PRIx64 "\n", env->pc);
    for (i = 0; i < MMIX_REGS; i += 4) {
        qemu_fprintf(f,
                     "r%-3d=0x%016" PRIx64 " r%-3d=0x%016" PRIx64
                     " r%-3d=0x%016" PRIx64 " r%-3d=0x%016" PRIx64 "\n",
                     i, env->regs[i],
                     i + 1, env->regs[i + 1],
                     i + 2, env->regs[i + 2],
                     i + 3, env->regs[i + 3]);
    }
}

static bool mmix_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    hwaddr physical = addr & TARGET_PAGE_MASK;
    int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    tlb_set_page(cs, physical, physical, prot, mmu_idx, TARGET_PAGE_SIZE);
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
    .mmu_index = mmix_cpu_mmu_index,
    .tlb_fill = mmix_cpu_tlb_fill,
    .cpu_exec_halt = mmix_cpu_has_work,
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
