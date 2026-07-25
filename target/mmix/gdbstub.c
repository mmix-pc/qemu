/*
 * QEMU MMIX GDB stub
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

int mmix_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    CPUMMIXState *env = cpu_env(cs);

    if (n < MMIX_REGS) {
        return gdb_get_reg64(mem_buf, env->regs[n]);
    }
    if (n == MMIX_REGS) {
        return gdb_get_reg64(mem_buf, env->pc);
    }

    return 0;
}

int mmix_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    CPUMMIXState *env = cpu_env(cs);

    if (n < MMIX_REGS) {
        env->regs[n] = ldq_be_p(mem_buf);
        return 8;
    }
    if (n == MMIX_REGS) {
        env->pc = ldq_be_p(mem_buf);
        return 8;
    }

    return 0;
}
