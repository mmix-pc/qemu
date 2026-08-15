/*
 * QEMU MMIX exception helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "mmix-helper.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "system/runstate.h"

static uint32_t mmix_select_arithmetic_event(uint32_t events)
{
    static const uint32_t priority[] = {
        MMIX_RA_EVENT_D,
        MMIX_RA_EVENT_V,
        MMIX_RA_EVENT_W,
        MMIX_RA_EVENT_I,
        MMIX_RA_EVENT_O,
        MMIX_RA_EVENT_U,
        MMIX_RA_EVENT_Z,
        MMIX_RA_EVENT_X,
    };
    unsigned i;

    events &= MMIX_RA_EVENT_MASK;
    for (i = 0; i < ARRAY_SIZE(priority); i++) {
        if (events & priority[i]) {
            return priority[i];
        }
    }
    return 0;
}

static hwaddr mmix_arithmetic_trip_handler(uint32_t event)
{
    switch (event) {
    case MMIX_RA_EVENT_D:
        return 16;
    case MMIX_RA_EVENT_V:
        return 32;
    case MMIX_RA_EVENT_W:
        return 48;
    case MMIX_RA_EVENT_I:
        return 64;
    case MMIX_RA_EVENT_O:
        return 80;
    case MMIX_RA_EVENT_U:
        return 96;
    case MMIX_RA_EVENT_Z:
        return 112;
    case MMIX_RA_EVENT_X:
        return 128;
    default:
        g_assert_not_reached();
    }
}

static void mmix_cpu_enter_dynamic_trap(CPUState *cs, uint64_t where,
                                        uint64_t exec, uint64_t y, uint64_t z)
{
    CPUMMIXState *env = cpu_env(cs);
    hwaddr handler = env->sregs[MMIX_SREG_RTT];

    env->sregs[MMIX_SREG_RBB] = mmix_cpu_read_reg(env, 255);
    env->sregs[MMIX_SREG_RWW] = where;
    env->sregs[MMIX_SREG_RXX] = exec;
    env->sregs[MMIX_SREG_RYY] = y;
    env->sregs[MMIX_SREG_RZZ] = z;
    mmix_cpu_put_rk(env, 0);
    mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
    env->pc = handler;
    env->npc = handler + 4;
    cs->exception_index = -1;
}

static void mmix_raise_arithmetic_trip(CPUMMIXState *env, uint32_t event,
                                       uint32_t insn, uint64_t y, uint64_t z)
{
    CPUState *cs = env_cpu(env);

    env->arithmetic_trip_event = event;
    env->sregs[MMIX_SREG_RW] = env->npc;
    env->sregs[MMIX_SREG_RX] = 0x8000000000000000ULL | insn;
    env->sregs[MMIX_SREG_RY] = y;
    env->sregs[MMIX_SREG_RZ] = z;
    cs->exception_index = EXCP_MMIX_ARITHMETIC_TRIP;
    cpu_loop_exit(cs);
}

void mmix_update_ra_events(CPUMMIXState *env, uint32_t events,
                                  uint32_t insn, uint64_t y, uint64_t z)
{
    uint32_t enables;
    uint32_t disabled_events;
    uint32_t enabled_events;
    uint32_t selected_event;

    events &= MMIX_RA_EVENT_MASK;
    if (events == 0) {
        return;
    }

    enables = (env->sregs[MMIX_SREG_RA] >> MMIX_RA_ENABLE_SHIFT) &
              MMIX_RA_EVENT_MASK;
    disabled_events = events & ~enables;
    enabled_events = events & enables;

    env->sregs[MMIX_SREG_RA] |= disabled_events;

    selected_event = mmix_select_arithmetic_event(enabled_events);
    if (selected_event != 0) {
        mmix_raise_arithmetic_trip(env, selected_event, insn, y, z);
    }
}

void helper_raise_illegal_instruction(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_MMIX_ILLEGAL;
    cpu_loop_exit(cs);
}

void helper_mmix_trip(CPUMMIXState *env, uint32_t insn, uint64_t y,
                      uint64_t z)
{
    CPUState *cs = env_cpu(env);

    env->sregs[MMIX_SREG_RW] = env->npc;
    env->sregs[MMIX_SREG_RX] = 0x8000000000000000ULL | insn;
    env->sregs[MMIX_SREG_RY] = y;
    env->sregs[MMIX_SREG_RZ] = z;
    env->sregs[MMIX_SREG_RB] = mmix_cpu_read_reg(env, 255);
    mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
    env->pc = 0;
    env->npc = 4;
    qemu_log_mask(CPU_LOG_INT,
                  "MMIX trip from 0x%016" PRIx64 " to 0x%016x\n",
                  env->sregs[MMIX_SREG_RW] - 4, 0);
    cpu_loop_exit_noexc(cs);
}

void helper_mmix_trap(CPUMMIXState *env, uint32_t insn, uint64_t y,
                      uint64_t z)
{
    CPUState *cs = env_cpu(env);
    uint64_t handler = env->sregs[MMIX_SREG_RT];

    env->sregs[MMIX_SREG_RWW] = env->npc;
    env->sregs[MMIX_SREG_RXX] = 0x8000000000000000ULL | insn;
    env->sregs[MMIX_SREG_RYY] = y;
    env->sregs[MMIX_SREG_RZZ] = z;
    env->sregs[MMIX_SREG_RBB] = mmix_cpu_read_reg(env, 255);
    mmix_cpu_put_rk(env, 0);
    mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
    env->pc = handler;
    env->npc = handler + 4;
    qemu_log_mask(CPU_LOG_INT,
                  "MMIX trap from 0x%016" PRIx64 " to 0x%016" PRIx64 "\n",
                  env->sregs[MMIX_SREG_RWW] - 4, handler);
    cpu_loop_exit_noexc(cs);
}

static void mmix_resume_unsupported(CPUMMIXState *env, const char *why,
                                    uint64_t exec)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX unsupported RESUME %s exec=0x%016" PRIx64 "\n",
                  why, exec);
    helper_raise_illegal_instruction(env);
}

static void mmix_resume_state(CPUMMIXState *env, bool trap_state)
{
    CPUState *cs = env_cpu(env);
    uint64_t where;
    uint64_t exec;
    uint64_t y;
    uint64_t z;
    uint8_t ropcode;
    uint32_t insn;

    if (trap_state) {
        where = env->sregs[MMIX_SREG_RWW];
        exec = env->sregs[MMIX_SREG_RXX];
        y = env->sregs[MMIX_SREG_RYY];
        z = env->sregs[MMIX_SREG_RZZ];
        mmix_cpu_put_rk(env, mmix_cpu_read_reg(env, 255));
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RBB]);
    } else {
        where = env->sregs[MMIX_SREG_RW];
        exec = env->sregs[MMIX_SREG_RX];
        y = env->sregs[MMIX_SREG_RY];
        z = env->sregs[MMIX_SREG_RZ];
    }

    if ((int64_t)exec < 0) {
        env->pc = where;
        env->npc = where + 4;
        cpu_loop_exit_noexc(cs);
    }

    ropcode = exec >> 56;
    insn = exec;
    switch (ropcode) {
    case 2:
    {
        uint32_t events = (exec >> 40) & MMIX_RA_EVENT_MASK;
        uint32_t reg = (insn >> 16) & 0xff;

        mmix_cpu_write_reg(env, reg, z);
        if (events != 0) {
            env->pc = where - 4;
            env->npc = where;
            mmix_update_ra_events(env, events, insn, y, z);
        }
        env->pc = where;
        env->npc = where + 4;
        cpu_loop_exit_noexc(cs);
    }
    case 0:
        mmix_resume_unsupported(env, "ropcode 0 instruction replay", exec);
        break;
    case 1:
        mmix_resume_unsupported(env, "ropcode 1 operand substitution", exec);
        break;
    case 3:
        mmix_resume_unsupported(env, "ropcode 3 virtual translation", exec);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "MMIX invalid RESUME ropcode %u exec=0x%016" PRIx64
                      "\n",
                      ropcode, exec);
        helper_raise_illegal_instruction(env);
        break;
    }
    g_assert_not_reached();
}

void helper_mmix_resume(CPUMMIXState *env, uint32_t x, uint32_t y, uint32_t z)
{
    if (x != 0 || y != 0) {
        qemu_log_mask(LOG_UNIMP, "MMIX invalid RESUME x=%u y=%u z=%u\n",
                      x, y, z);
        helper_raise_illegal_instruction(env);
    }

    switch (z) {
    case 0:
        mmix_resume_state(env, false);
        break;
    case 1:
        mmix_resume_state(env, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "MMIX invalid RESUME z=%u\n", z);
        helper_raise_illegal_instruction(env);
        break;
    }
    g_assert_not_reached();
}

G_NORETURN void mmix_cpu_shutdown_with_log(CPUMMIXState *env,
                                                  const char *reason,
                                                  int exit_code)
{
    CPUState *cs = env_cpu(env);

    if (!env->test_exit_seen) {
        env->test_exit_seen = true;
        if (g_strcmp0(reason, "MMIX test exit") != 0) {
            qemu_log_mask(CPU_LOG_INT, "%s at 0x%016" PRIx64 "\n", reason,
                          env->pc);
        }
        qemu_log_mask(CPU_LOG_INT, "MMIX test exit at 0x%016" PRIx64 "\n",
                      env->pc);
        log_cpu_state_mask(CPU_LOG_INT, cs, 0);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                               exit_code);
    }
    cs->halted = 1;
    cpu_loop_exit_noexc(cs);
}

void helper_mmix_test_exit(CPUMMIXState *env)
{
    mmix_cpu_shutdown_with_log(env, "MMIX test exit", 0);
}


void mmix_cpu_do_interrupt(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);
    uint32_t event;
    uint64_t causes;
    uint64_t requests;
    hwaddr handler;

    switch (cs->exception_index) {
    case EXCP_MMIX_ILLEGAL:
        qemu_log_mask(CPU_LOG_INT, "MMIX illegal instruction at 0x%016" PRIx64 "\n",
                      env->pc);
        break;
    case EXCP_MMIX_ARITHMETIC_TRIP:
        event = env->arithmetic_trip_event;
        handler = mmix_arithmetic_trip_handler(event);
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX arithmetic trip event=0x%02x from 0x%016" PRIx64
                      " to 0x%016" HWADDR_PRIx "\n",
                      event, env->pc, handler);
        env->sregs[MMIX_SREG_RB] = mmix_cpu_read_reg(env, 255);
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
        env->pc = handler;
        env->npc = handler + 4;
        env->arithmetic_trip_event = 0;
        cs->exception_index = -1;
        break;
    case EXCP_MMIX_DYNAMIC_TRAP:
        causes = env->program_exception_causes & MMIX_RQ_PROGRAM_MASK;
        handler = env->sregs[MMIX_SREG_RTT];
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX dynamic trap causes=0x%016" PRIx64
                      " from 0x%016" PRIx64 " to 0x%016" HWADDR_PRIx "\n",
                      causes, env->pc, handler);
        mmix_cpu_enter_dynamic_trap(cs, env->npc, causes, 0, 0);
        env->program_exception_causes = 0;
        break;
    case EXCP_MMIX_INTERRUPT:
        requests = env->sregs[MMIX_SREG_RQ] & env->sregs[MMIX_SREG_RK];
        handler = env->sregs[MMIX_SREG_RTT];
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX external dynamic trap requests=0x%016" PRIx64
                      " from 0x%016" PRIx64 " to 0x%016" HWADDR_PRIx "\n",
                      requests, env->pc, handler);
        mmix_cpu_enter_dynamic_trap(cs, env->pc,
                                    MMIX_DYNAMIC_TRAP_RESUME_NEXT, 0, 0);
        break;
    default:
        qemu_log_mask(CPU_LOG_INT, "MMIX exception %d at 0x%016" PRIx64 "\n",
                      cs->exception_index, env->pc);
        break;
    }
}

bool mmix_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUMMIXState *env = cpu_env(cs);

    if (interrupt_request & CPU_INTERRUPT_HARD) {
        if (!(env->sregs[MMIX_SREG_RQ] &
              env->sregs[MMIX_SREG_RK] &
              MMIX_RK_INTERRUPT_CONTROLLER)) {
            return false;
        }
        cs->exception_index = EXCP_MMIX_INTERRUPT;
        mmix_cpu_do_interrupt(cs);
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        return true;
    }

    return false;
}

hwaddr mmix_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    CPUMMIXState *env = cpu_env(cs);
    MMIXAddressTranslation translation;

    if (mmix_translate_address(env, addr, MMU_DATA_LOAD, true, false,
                               &translation)) {
        return translation.physical;
    }
    return -1;
}
