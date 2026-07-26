/*
 * QEMU MMIX TCG translation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/log.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "exec/translation-block.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPUMMIXState *env;
    vaddr insn_pc;
} DisasContext;

typedef enum MMIXALUOp {
    MMIX_ALU_ADD,
    MMIX_ALU_SUB,
    MMIX_ALU_OR,
    MMIX_ALU_XOR,
    MMIX_ALU_AND,
} MMIXALUOp;

typedef enum MMIXBranchCond {
    MMIX_BRANCH_ZERO,
    MMIX_BRANCH_NONZERO,
} MMIXBranchCond;

typedef enum MMIXCompareOp {
    MMIX_CMP_SIGNED,
    MMIX_CMP_UNSIGNED,
} MMIXCompareOp;

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_npc;
static TCGv_i64 cpu_regs[MMIX_REGS];

/* Include the auto-generated decoder. */
#include "decode-insns.c.inc"

static void gen_raise_illegal(DisasContext *ctx)
{
    gen_helper_raise_illegal_instruction(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_goto_tb(DisasContext *ctx, unsigned tb_slot_idx, vaddr dest)
{
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(tb_slot_idx);
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_movi_i64(cpu_npc, dest + 4);
        tcg_gen_exit_tb(ctx->base.tb, tb_slot_idx);
    } else {
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_movi_i64(cpu_npc, dest + 4);
        tcg_gen_lookup_and_goto_ptr();
    }
}

static TCGv_i64 gen_load_reg(unsigned reg)
{
    return cpu_regs[reg];
}

static TCGv_i64 gen_load_z(const arg_xyz *a, bool immediate)
{
    if (immediate) {
        return tcg_constant_i64(a->z);
    }
    return gen_load_reg(a->z);
}

static void gen_store_reg(unsigned reg, TCGv_i64 val)
{
    tcg_gen_mov_i64(cpu_regs[reg], val);
}

static void gen_effective_address(TCGv_i64 addr, const arg_xyz *a,
                                  bool immediate)
{
    tcg_gen_add_i64(addr, gen_load_reg(a->y), gen_load_z(a, immediate));
    tcg_gen_andi_i64(addr, addr, ~7ULL);
}

static vaddr mmix_branch_dest(DisasContext *ctx, const arg_xyz *a,
                              bool backward)
{
    int32_t disp = backward ? (int32_t)a->yz - 0x10000 : a->yz;

    return ctx->insn_pc + ((int64_t)disp << 2);
}

static bool gen_mmix_unsupported(DisasContext *ctx, const char *mnemonic,
                                 const arg_xyz *a)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX decoded unimplemented %s at 0x%016" VADDR_PRIx
                  " x=%u y=%u z=%u yz=0x%04x xyz=0x%06x\n",
                  mnemonic, ctx->insn_pc, a->x, a->y, a->z, a->yz,
                  a->xyz);
    gen_raise_illegal(ctx);
    return true;
}

#define TRANS_UNSUPPORTED(NAME) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_mmix_unsupported(ctx, #NAME, a); \
    }

TRANS_UNSUPPORTED(TRIP)
TRANS_UNSUPPORTED(PBZ)
TRANS_UNSUPPORTED(PBZB)
TRANS_UNSUPPORTED(PBNZ)
TRANS_UNSUPPORTED(PBNZB)

#undef TRANS_UNSUPPORTED

static bool trans_TRAP(DisasContext *ctx, arg_xyz *a)
{
    if (a->x == 0 && a->y == 0 && a->z == 0) {
        gen_helper_mmix_test_exit(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
        return true;
    }

    return gen_mmix_unsupported(ctx, "TRAP", a);
}

static bool trans_SWYM(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool gen_alu(DisasContext *ctx, arg_xyz *a, MMIXALUOp op,
                    bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    switch (op) {
    case MMIX_ALU_ADD:
        tcg_gen_add_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_SUB:
        tcg_gen_sub_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_OR:
        tcg_gen_or_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_XOR:
        tcg_gen_xor_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_AND:
        tcg_gen_and_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    default:
        g_assert_not_reached();
    }

    gen_store_reg(a->x, val);
    return true;
}

static bool trans_ADD(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ADD, false);
}

static bool trans_ADDI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ADD, true);
}

static bool trans_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ADD, false);
}

static bool trans_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ADD, true);
}

static bool trans_SUB(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_SUB, false);
}

static bool trans_SUBI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_SUB, true);
}

static bool trans_SUBU(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_SUB, false);
}

static bool trans_SUBUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_SUB, true);
}

static bool trans_OR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_OR, false);
}

static bool trans_ORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_OR, true);
}

static bool trans_XOR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_XOR, false);
}

static bool trans_XORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_XOR, true);
}

static bool trans_AND(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_AND, false);
}

static bool trans_ANDI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_AND, true);
}

static bool gen_cmp(DisasContext *ctx, arg_xyz *a, MMIXCompareOp op,
                    bool immediate)
{
    TCGv_i64 gt = tcg_temp_new_i64();
    TCGv_i64 lt = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i64 lhs = gen_load_reg(a->y);
    TCGv_i64 rhs = gen_load_z(a, immediate);

    if (op == MMIX_CMP_SIGNED) {
        tcg_gen_setcond_i64(TCG_COND_GT, gt, lhs, rhs);
        tcg_gen_setcond_i64(TCG_COND_LT, lt, lhs, rhs);
    } else {
        tcg_gen_setcond_i64(TCG_COND_GTU, gt, lhs, rhs);
        tcg_gen_setcond_i64(TCG_COND_LTU, lt, lhs, rhs);
    }
    tcg_gen_sub_i64(val, gt, lt);
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_CMP(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_SIGNED, false);
}

static bool trans_CMPI(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_SIGNED, true);
}

static bool trans_CMPU(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_UNSIGNED, false);
}

static bool trans_CMPUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_UNSIGNED, true);
}

static bool gen_branch(DisasContext *ctx, arg_xyz *a, MMIXBranchCond cond,
                       bool backward)
{
    TCGLabel *not_taken = gen_new_label();
    TCGv_i64 val = gen_load_reg(a->x);
    vaddr dest = mmix_branch_dest(ctx, a, backward);
    vaddr next = ctx->base.pc_next;

    if (cond == MMIX_BRANCH_ZERO) {
        tcg_gen_brcondi_i64(TCG_COND_NE, val, 0, not_taken);
    } else {
        tcg_gen_brcondi_i64(TCG_COND_EQ, val, 0, not_taken);
    }
    gen_goto_tb(ctx, 0, dest);
    gen_set_label(not_taken);
    gen_goto_tb(ctx, 1, next);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_BZ(DisasContext *ctx, arg_xyz *a)
{
    return gen_branch(ctx, a, MMIX_BRANCH_ZERO, false);
}

static bool trans_BZB(DisasContext *ctx, arg_xyz *a)
{
    return gen_branch(ctx, a, MMIX_BRANCH_ZERO, true);
}

static bool trans_BNZ(DisasContext *ctx, arg_xyz *a)
{
    return gen_branch(ctx, a, MMIX_BRANCH_NONZERO, false);
}

static bool trans_BNZB(DisasContext *ctx, arg_xyz *a)
{
    return gen_branch(ctx, a, MMIX_BRANCH_NONZERO, true);
}

static bool gen_ldo(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate);
    tcg_gen_qemu_ld_i64(val, addr, 0, MO_BEUQ);
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_LDO(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldo(ctx, a, false);
}

static bool trans_LDOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldo(ctx, a, true);
}

static bool trans_LDOU(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldo(ctx, a, false);
}

static bool trans_LDOUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldo(ctx, a, true);
}

static bool gen_sto(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate);
    tcg_gen_qemu_st_i64(gen_load_reg(a->x), addr, 0, MO_BEUQ);
    return true;
}

static bool trans_STO(DisasContext *ctx, arg_xyz *a)
{
    return gen_sto(ctx, a, false);
}

static bool trans_STOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_sto(ctx, a, true);
}

static bool trans_STOU(DisasContext *ctx, arg_xyz *a)
{
    return gen_sto(ctx, a, false);
}

static bool trans_STOUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_sto(ctx, a, true);
}

static void mmix_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->env = cpu_env(cs);
}

static void mmix_tr_tb_start(DisasContextBase *dcbase, CPUState *cs)
{
}

static void mmix_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    tcg_gen_insn_start(dcbase->pc_next, 0, 0);
}

static void mmix_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    vaddr pc = ctx->base.pc_next;
    uint32_t insn;

    insn = translator_ldl_end(ctx->env, &ctx->base, pc, MO_BE);
    ctx->insn_pc = pc;
    ctx->base.pc_next += 4;

    tcg_gen_movi_i64(cpu_pc, pc);
    tcg_gen_movi_i64(cpu_npc, ctx->base.pc_next);
    if (!decode(ctx, insn)) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX unknown opcode 0x%02x at 0x%016" VADDR_PRIx
                      " insn=0x%08x\n",
                      insn >> 24, pc, insn);
        gen_raise_illegal(ctx);
    }
}

static void mmix_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    switch (dcbase->is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        tcg_gen_movi_i64(cpu_pc, dcbase->pc_next);
        tcg_gen_movi_i64(cpu_npc, dcbase->pc_next + 4);
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps mmix_tr_ops = {
    .init_disas_context = mmix_tr_init_disas_context,
    .tb_start           = mmix_tr_tb_start,
    .insn_start         = mmix_tr_insn_start,
    .translate_insn     = mmix_tr_translate_insn,
    .tb_stop            = mmix_tr_tb_stop,
};

void mmix_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = { };

    translator_loop(cs, tb, max_insns, pc, host_pc, &mmix_tr_ops, &dc.base,
                    TCG_TYPE_VA);
}

void mmix_translate_init(void)
{
    static char regnames[MMIX_REGS][8];
    int i;

    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, pc), "pc");
    cpu_npc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, npc),
                                     "npc");
    for (i = 0; i < MMIX_REGS; i++) {
        snprintf(regnames[i], sizeof(regnames[i]), "r%d", i);
        cpu_regs[i] = tcg_global_mem_new_i64(tcg_env,
                                             offsetof(CPUMMIXState, regs[i]),
                                             regnames[i]);
    }
}
