/*
 * QEMU MMIX TCG translation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
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
} DisasContext;

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_npc;

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

    translator_ldl_end(ctx->env, &ctx->base, pc, MO_BE);
    ctx->base.pc_next += 4;

    tcg_gen_movi_i64(cpu_pc, pc);
    tcg_gen_movi_i64(cpu_npc, ctx->base.pc_next);
    gen_helper_raise_illegal_instruction(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
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
    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, pc), "pc");
    cpu_npc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, npc),
                                     "npc");
}
