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
    MMIX_ALU_ORN,
    MMIX_ALU_NOR,
    MMIX_ALU_XOR,
    MMIX_ALU_AND,
    MMIX_ALU_ANDN,
    MMIX_ALU_NAND,
    MMIX_ALU_NXOR,
} MMIXALUOp;

typedef enum MMIXCompareOp {
    MMIX_CMP_SIGNED,
    MMIX_CMP_UNSIGNED,
} MMIXCompareOp;

typedef enum MMIXPredicate {
    MMIX_PRED_NEGATIVE,
    MMIX_PRED_ZERO,
    MMIX_PRED_POSITIVE,
    MMIX_PRED_ODD,
    MMIX_PRED_NONNEGATIVE,
    MMIX_PRED_NONZERO,
    MMIX_PRED_NONPOSITIVE,
    MMIX_PRED_EVEN,
} MMIXPredicate;

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_npc;
static TCGv_i64 cpu_sregs[MMIX_SREGS];

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
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_read_reg(val, tcg_env, tcg_constant_i32(reg));
    return val;
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
    gen_helper_mmix_write_reg(tcg_env, tcg_constant_i32(reg), val);
}

static TCGv_i64 gen_load_sreg(unsigned reg)
{
    return cpu_sregs[reg];
}

static void gen_store_sreg(unsigned reg, TCGv_i64 val)
{
    tcg_gen_mov_i64(cpu_sregs[reg], val);
}

static bool mmix_put_writable(unsigned reg)
{
    switch (reg) {
    case MMIX_SREG_RB:
    case MMIX_SREG_RD:
    case MMIX_SREG_RE:
    case MMIX_SREG_RH:
    case MMIX_SREG_RJ:
    case MMIX_SREG_RM:
    case MMIX_SREG_RR:
    case MMIX_SREG_RBB:
    case MMIX_SREG_RF:
    case MMIX_SREG_RP:
    case MMIX_SREG_RW:
    case MMIX_SREG_RX:
    case MMIX_SREG_RY:
    case MMIX_SREG_RZ:
    case MMIX_SREG_RWW:
    case MMIX_SREG_RXX:
    case MMIX_SREG_RYY:
    case MMIX_SREG_RZZ:
        return true;
    default:
        return false;
    }
}

static void gen_effective_address(TCGv_i64 addr, const arg_xyz *a,
                                  bool immediate, uint64_t align_mask)
{
    tcg_gen_add_i64(addr, gen_load_reg(a->y), gen_load_z(a, immediate));
    if (align_mask != 0) {
        tcg_gen_andi_i64(addr, addr, ~align_mask);
    }
}

static vaddr mmix_branch_dest(DisasContext *ctx, const arg_xyz *a,
                              bool backward)
{
    int32_t disp = backward ? (int32_t)a->yz - 0x10000 : a->yz;

    return ctx->insn_pc + ((int64_t)disp << 2);
}

static vaddr mmix_jump_dest(DisasContext *ctx, const arg_xyz *a,
                            bool backward)
{
    int32_t disp = backward ? (int32_t)a->xyz - 0x1000000 : a->xyz;

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
    case MMIX_ALU_ORN:
        tcg_gen_orc_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_NOR:
        tcg_gen_or_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    case MMIX_ALU_XOR:
        tcg_gen_xor_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_AND:
        tcg_gen_and_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_ANDN:
        tcg_gen_andc_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_NAND:
        tcg_gen_and_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    case MMIX_ALU_NXOR:
        tcg_gen_xor_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        tcg_gen_not_i64(val, val);
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

static bool trans_ORN(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ORN, false);
}

static bool trans_ORNI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ORN, true);
}

static bool trans_NOR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NOR, false);
}

static bool trans_NORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NOR, true);
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

static bool trans_ANDN(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ANDN, false);
}

static bool trans_ANDNI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ANDN, true);
}

static bool trans_NAND(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NAND, false);
}

static bool trans_NANDI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NAND, true);
}

static bool trans_NXOR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NXOR, false);
}

static bool trans_NXORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NXOR, true);
}

static bool gen_scaled_addu(DisasContext *ctx, arg_xyz *a, unsigned shift,
                            bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_shli_i64(val, gen_load_reg(a->y), shift);
    tcg_gen_add_i64(val, val, gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_TWO_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 1, false);
}

static bool trans_TWO_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 1, true);
}

static bool trans_FOUR_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 2, false);
}

static bool trans_FOUR_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 2, true);
}

static bool trans_EIGHT_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 3, false);
}

static bool trans_EIGHT_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 3, true);
}

static bool trans_SIXTEEN_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 4, false);
}

static bool trans_SIXTEEN_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 4, true);
}

static uint64_t mmix_wyde_value(const arg_xyz *a, unsigned shift)
{
    return (uint64_t)a->yz << shift;
}

static bool gen_set_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    gen_store_reg(a->x, tcg_constant_i64(mmix_wyde_value(a, shift)));
    return true;
}

static bool gen_inc_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_add_i64(val, gen_load_reg(a->x),
                    tcg_constant_i64(mmix_wyde_value(a, shift)));
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_SETH(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 48);
}

static bool trans_SETMH(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 32);
}

static bool trans_SETML(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 16);
}

static bool trans_SETL(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 0);
}

static bool trans_INCH(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 48);
}

static bool trans_INCMH(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 32);
}

static bool trans_INCML(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 16);
}

static bool trans_INCL(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 0);
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

static void gen_predicate(TCGv_i64 pred, TCGv_i64 val, MMIXPredicate predicate)
{
    TCGv_i64 tmp;

    switch (predicate) {
    case MMIX_PRED_NEGATIVE:
        tcg_gen_setcondi_i64(TCG_COND_LT, pred, val, 0);
        break;
    case MMIX_PRED_ZERO:
        tcg_gen_setcondi_i64(TCG_COND_EQ, pred, val, 0);
        break;
    case MMIX_PRED_POSITIVE:
        tcg_gen_setcondi_i64(TCG_COND_GT, pred, val, 0);
        break;
    case MMIX_PRED_ODD:
        tmp = tcg_temp_new_i64();
        tcg_gen_andi_i64(tmp, val, 1);
        tcg_gen_setcondi_i64(TCG_COND_NE, pred, tmp, 0);
        break;
    case MMIX_PRED_NONNEGATIVE:
        tcg_gen_setcondi_i64(TCG_COND_GE, pred, val, 0);
        break;
    case MMIX_PRED_NONZERO:
        tcg_gen_setcondi_i64(TCG_COND_NE, pred, val, 0);
        break;
    case MMIX_PRED_NONPOSITIVE:
        tcg_gen_setcondi_i64(TCG_COND_LE, pred, val, 0);
        break;
    case MMIX_PRED_EVEN:
        tmp = tcg_temp_new_i64();
        tcg_gen_andi_i64(tmp, val, 1);
        tcg_gen_setcondi_i64(TCG_COND_EQ, pred, tmp, 0);
        break;
    default:
        g_assert_not_reached();
    }
}

static bool gen_cond_result(arg_xyz *a, MMIXPredicate predicate, bool immediate,
                            bool zero_false)
{
    TCGv_i64 pred = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_predicate(pred, gen_load_reg(a->y), predicate);
    tcg_gen_movcond_i64(TCG_COND_NE, val, pred, tcg_constant_i64(0),
                        gen_load_z(a, immediate),
                        zero_false ? tcg_constant_i64(0) : gen_load_reg(a->x));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_cs(DisasContext *ctx, arg_xyz *a, MMIXPredicate predicate,
                   bool immediate)
{
    return gen_cond_result(a, predicate, immediate, false);
}

static bool gen_zs(DisasContext *ctx, arg_xyz *a, MMIXPredicate predicate,
                   bool immediate)
{
    return gen_cond_result(a, predicate, immediate, true);
}

#define TRANS_CS(NAME, PREDICATE, IMMEDIATE) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_cs(ctx, a, PREDICATE, IMMEDIATE); \
    }

#define TRANS_ZS(NAME, PREDICATE, IMMEDIATE) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_zs(ctx, a, PREDICATE, IMMEDIATE); \
    }

TRANS_CS(CSN, MMIX_PRED_NEGATIVE, false)
TRANS_CS(CSNI, MMIX_PRED_NEGATIVE, true)
TRANS_CS(CSZ, MMIX_PRED_ZERO, false)
TRANS_CS(CSZI, MMIX_PRED_ZERO, true)
TRANS_CS(CSP, MMIX_PRED_POSITIVE, false)
TRANS_CS(CSPI, MMIX_PRED_POSITIVE, true)
TRANS_CS(CSOD, MMIX_PRED_ODD, false)
TRANS_CS(CSODI, MMIX_PRED_ODD, true)
TRANS_CS(CSNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_CS(CSNNI, MMIX_PRED_NONNEGATIVE, true)
TRANS_CS(CSNZ, MMIX_PRED_NONZERO, false)
TRANS_CS(CSNZI, MMIX_PRED_NONZERO, true)
TRANS_CS(CSNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_CS(CSNPI, MMIX_PRED_NONPOSITIVE, true)
TRANS_CS(CSEV, MMIX_PRED_EVEN, false)
TRANS_CS(CSEVI, MMIX_PRED_EVEN, true)

TRANS_ZS(ZSN, MMIX_PRED_NEGATIVE, false)
TRANS_ZS(ZSNI, MMIX_PRED_NEGATIVE, true)
TRANS_ZS(ZSZ, MMIX_PRED_ZERO, false)
TRANS_ZS(ZSZI, MMIX_PRED_ZERO, true)
TRANS_ZS(ZSP, MMIX_PRED_POSITIVE, false)
TRANS_ZS(ZSPI, MMIX_PRED_POSITIVE, true)
TRANS_ZS(ZSOD, MMIX_PRED_ODD, false)
TRANS_ZS(ZSODI, MMIX_PRED_ODD, true)
TRANS_ZS(ZSNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_ZS(ZSNNI, MMIX_PRED_NONNEGATIVE, true)
TRANS_ZS(ZSNZ, MMIX_PRED_NONZERO, false)
TRANS_ZS(ZSNZI, MMIX_PRED_NONZERO, true)
TRANS_ZS(ZSNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_ZS(ZSNPI, MMIX_PRED_NONPOSITIVE, true)
TRANS_ZS(ZSEV, MMIX_PRED_EVEN, false)
TRANS_ZS(ZSEVI, MMIX_PRED_EVEN, true)

#undef TRANS_CS
#undef TRANS_ZS

static bool gen_branch(DisasContext *ctx, arg_xyz *a, MMIXPredicate predicate,
                       bool backward)
{
    TCGLabel *not_taken = gen_new_label();
    TCGv_i64 pred = tcg_temp_new_i64();
    vaddr dest = mmix_branch_dest(ctx, a, backward);
    vaddr next = ctx->base.pc_next;

    gen_predicate(pred, gen_load_reg(a->x), predicate);
    tcg_gen_brcondi_i64(TCG_COND_EQ, pred, 0, not_taken);
    gen_goto_tb(ctx, 0, dest);
    gen_set_label(not_taken);
    gen_goto_tb(ctx, 1, next);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_geta(DisasContext *ctx, arg_xyz *a, bool backward)
{
    gen_store_reg(a->x, tcg_constant_i64(mmix_branch_dest(ctx, a, backward)));
    return true;
}

static bool gen_jmp(DisasContext *ctx, arg_xyz *a, bool backward)
{
    gen_goto_tb(ctx, 0, mmix_jump_dest(ctx, a, backward));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_go(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 dest = tcg_temp_new_i64();

    tcg_gen_add_i64(dest, gen_load_reg(a->y), gen_load_z(a, immediate));
    tcg_gen_andi_i64(dest, dest, ~3ULL);
    gen_store_reg(a->x, tcg_constant_i64(ctx->base.pc_next));
    tcg_gen_mov_i64(cpu_pc, dest);
    tcg_gen_addi_i64(cpu_npc, dest, 4);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

#define TRANS_BRANCH(NAME, PREDICATE, BACKWARD) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_branch(ctx, a, PREDICATE, BACKWARD); \
    }

TRANS_BRANCH(BN, MMIX_PRED_NEGATIVE, false)
TRANS_BRANCH(BNB, MMIX_PRED_NEGATIVE, true)
TRANS_BRANCH(BZ, MMIX_PRED_ZERO, false)
TRANS_BRANCH(BZB, MMIX_PRED_ZERO, true)
TRANS_BRANCH(BP, MMIX_PRED_POSITIVE, false)
TRANS_BRANCH(BPB, MMIX_PRED_POSITIVE, true)
TRANS_BRANCH(BOD, MMIX_PRED_ODD, false)
TRANS_BRANCH(BODB, MMIX_PRED_ODD, true)
TRANS_BRANCH(BNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_BRANCH(BNNB, MMIX_PRED_NONNEGATIVE, true)
TRANS_BRANCH(BNZ, MMIX_PRED_NONZERO, false)
TRANS_BRANCH(BNZB, MMIX_PRED_NONZERO, true)
TRANS_BRANCH(BNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_BRANCH(BNPB, MMIX_PRED_NONPOSITIVE, true)
TRANS_BRANCH(BEV, MMIX_PRED_EVEN, false)
TRANS_BRANCH(BEVB, MMIX_PRED_EVEN, true)
TRANS_BRANCH(PBN, MMIX_PRED_NEGATIVE, false)
TRANS_BRANCH(PBNB, MMIX_PRED_NEGATIVE, true)
TRANS_BRANCH(PBZ, MMIX_PRED_ZERO, false)
TRANS_BRANCH(PBZB, MMIX_PRED_ZERO, true)
TRANS_BRANCH(PBP, MMIX_PRED_POSITIVE, false)
TRANS_BRANCH(PBPB, MMIX_PRED_POSITIVE, true)
TRANS_BRANCH(PBOD, MMIX_PRED_ODD, false)
TRANS_BRANCH(PBODB, MMIX_PRED_ODD, true)
TRANS_BRANCH(PBNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_BRANCH(PBNNB, MMIX_PRED_NONNEGATIVE, true)
TRANS_BRANCH(PBNZ, MMIX_PRED_NONZERO, false)
TRANS_BRANCH(PBNZB, MMIX_PRED_NONZERO, true)
TRANS_BRANCH(PBNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_BRANCH(PBNPB, MMIX_PRED_NONPOSITIVE, true)
TRANS_BRANCH(PBEV, MMIX_PRED_EVEN, false)
TRANS_BRANCH(PBEVB, MMIX_PRED_EVEN, true)

#undef TRANS_BRANCH

static bool trans_GETA(DisasContext *ctx, arg_xyz *a)
{
    return gen_geta(ctx, a, false);
}

static bool trans_GETAB(DisasContext *ctx, arg_xyz *a)
{
    return gen_geta(ctx, a, true);
}

static bool trans_GET(DisasContext *ctx, arg_xyz *a)
{
    if (a->y != 0 || a->z >= MMIX_SREGS) {
        return gen_mmix_unsupported(ctx, "GET", a);
    }

    gen_store_reg(a->x, gen_load_sreg(a->z));
    return true;
}

static bool gen_put(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    if (a->y != 0 || a->x >= MMIX_SREGS) {
        return gen_mmix_unsupported(ctx, immediate ? "PUTI" : "PUT", a);
    }
    if (a->x == MMIX_SREG_RL) {
        gen_helper_mmix_put_rl(tcg_env, gen_load_z(a, immediate));
        return true;
    }
    if (!mmix_put_writable(a->x)) {
        return gen_mmix_unsupported(ctx, immediate ? "PUTI" : "PUT", a);
    }

    gen_store_sreg(a->x, gen_load_z(a, immediate));
    return true;
}

static bool trans_PUT(DisasContext *ctx, arg_xyz *a)
{
    return gen_put(ctx, a, false);
}

static bool trans_PUTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_put(ctx, a, true);
}

static bool trans_JMP(DisasContext *ctx, arg_xyz *a)
{
    return gen_jmp(ctx, a, false);
}

static bool trans_JMPB(DisasContext *ctx, arg_xyz *a)
{
    return gen_jmp(ctx, a, true);
}

static bool trans_GO(DisasContext *ctx, arg_xyz *a)
{
    return gen_go(ctx, a, false);
}

static bool trans_GOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_go(ctx, a, true);
}

static bool gen_load_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                         MemOp memop, uint64_t align_mask)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, align_mask);
    tcg_gen_qemu_ld_i64(val, addr, 0, memop);
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_ldht(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    tcg_gen_qemu_ld_i64(val, addr, 0, MO_BEUL);
    tcg_gen_shli_i64(val, val, 32);
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_LDB(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_SB, 0);
}

static bool trans_LDBI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_SB, 0);
}

static bool trans_LDBU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_UB, 0);
}

static bool trans_LDBUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_UB, 0);
}

static bool trans_LDW(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BESW, 1);
}

static bool trans_LDWI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BESW, 1);
}

static bool trans_LDWU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUW, 1);
}

static bool trans_LDWUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUW, 1);
}

static bool trans_LDT(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BESL, 3);
}

static bool trans_LDTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BESL, 3);
}

static bool trans_LDTU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUL, 3);
}

static bool trans_LDTUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUL, 3);
}

static bool trans_LDO(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_LDOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_LDOU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_LDOUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_LDHT(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldht(ctx, a, false);
}

static bool trans_LDHTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldht(ctx, a, true);
}

static bool gen_store_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                          MemOp memop, uint64_t align_mask)
{
    TCGv_i64 addr = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, align_mask);
    tcg_gen_qemu_st_i64(gen_load_reg(a->x), addr, 0, memop);
    return true;
}

static bool gen_stht(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    tcg_gen_shri_i64(val, gen_load_reg(a->x), 32);
    tcg_gen_qemu_st_i64(val, addr, 0, MO_BEUL);
    return true;
}

static bool trans_STB(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_UB, 0);
}

static bool trans_STBI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_UB, 0);
}

static bool trans_STBU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_UB, 0);
}

static bool trans_STBUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_UB, 0);
}

static bool trans_STW(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUW, 1);
}

static bool trans_STWI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUW, 1);
}

static bool trans_STWU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUW, 1);
}

static bool trans_STWUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUW, 1);
}

static bool trans_STT(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUL, 3);
}

static bool trans_STTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUL, 3);
}

static bool trans_STTU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUL, 3);
}

static bool trans_STTUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUL, 3);
}

static bool trans_STO(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_STOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_STOU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_STOUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_STHT(DisasContext *ctx, arg_xyz *a)
{
    return gen_stht(ctx, a, false);
}

static bool trans_STHTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_stht(ctx, a, true);
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
    static char sregnames[MMIX_SREGS][8];
    int i;

    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, pc), "pc");
    cpu_npc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, npc),
                                     "npc");
    for (i = 0; i < MMIX_SREGS; i++) {
        snprintf(sregnames[i], sizeof(sregnames[i]), "s%d", i);
        cpu_sregs[i] = tcg_global_mem_new_i64(tcg_env,
                                              offsetof(CPUMMIXState, sregs[i]),
                                              sregnames[i]);
    }
}
