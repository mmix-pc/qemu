/*
 * QEMU MMIX helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/log.h"
#include "exec/helper-proto.h"
#include "system/runstate.h"

void helper_raise_illegal_instruction(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_MMIX_ILLEGAL;
    cpu_loop_exit(cs);
}

void helper_mmix_test_exit(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    if (!env->test_exit_seen) {
        env->test_exit_seen = true;
        qemu_log_mask(CPU_LOG_INT, "MMIX test exit at 0x%016" PRIx64 "\n",
                      env->pc);
        log_cpu_state_mask(CPU_LOG_INT, cs, 0);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                               0);
    }
    cs->halted = 1;
    cpu_loop_exit_noexc(cs);
}

void mmix_cpu_do_interrupt(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);

    switch (cs->exception_index) {
    case EXCP_MMIX_ILLEGAL:
        qemu_log_mask(CPU_LOG_INT, "MMIX illegal instruction at 0x%016" PRIx64 "\n",
                      env->pc);
        break;
    default:
        qemu_log_mask(CPU_LOG_INT, "MMIX exception %d at 0x%016" PRIx64 "\n",
                      cs->exception_index, env->pc);
        break;
    }
}

bool mmix_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        cs->exception_index = EXCP_MMIX_INTERRUPT;
        mmix_cpu_do_interrupt(cs);
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        return true;
    }

    return false;
}

hwaddr mmix_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr;
}
