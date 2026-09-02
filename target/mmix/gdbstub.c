/*
 * QEMU MMIX GDB stub
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

#define MMIX_GDB_GENERAL_REGS MMIX_REGS
#define MMIX_GDB_REGS (MMIX_REGS + MMIX_SREGS + 1)
#define MMIX_GDB_REGISTER_BYTES (MMIX_GDB_REGS * sizeof(uint64_t))

int mmix_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    CPUMMIXState *env = cpu_env(cs);

    if (n >= 0 && n < MMIX_GDB_GENERAL_REGS) {
        return gdb_get_reg64(mem_buf, mmix_cpu_read_reg(env, n));
    }
    n -= MMIX_GDB_GENERAL_REGS;

    if (n < MMIX_SREGS) {
        return gdb_get_reg64(mem_buf, env->sregs[n]);
    }
    if (n == MMIX_SREGS) {
        return gdb_get_reg64(mem_buf, env->pc);
    }

    return 0;
}

int mmix_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    CPUMMIXState *env = cpu_env(cs);

    if (n >= 0 && n < MMIX_GDB_GENERAL_REGS) {
        mmix_cpu_write_reg(env, n, ldq_be_p(mem_buf));
        return 8;
    }
    n -= MMIX_GDB_GENERAL_REGS;

    if (n < MMIX_SREGS) {
        return mmix_cpu_debug_write_sreg(env, n, ldq_be_p(mem_buf)) ? 8 : 0;
    }
    if (n == MMIX_SREGS) {
        return mmix_cpu_debug_write_pc(env, ldq_be_p(mem_buf)) ? 8 : 0;
    }

    return 0;
}

bool mmix_cpu_gdb_write_registers(CPUState *cs, const uint8_t *mem_buf,
                                  size_t len)
{
    CPUMMIXState *env = cpu_env(cs);
    uint64_t regs[MMIX_REGS];
    uint64_t sregs[MMIX_SREGS];
    uint64_t pc;
    unsigned int i;

    if (len != MMIX_GDB_REGISTER_BYTES) {
        return false;
    }

    for (i = 0; i < MMIX_REGS; i++, mem_buf += sizeof(uint64_t)) {
        regs[i] = ldq_be_p(mem_buf);
    }
    for (i = 0; i < MMIX_SREGS; i++, mem_buf += sizeof(uint64_t)) {
        sregs[i] = ldq_be_p(mem_buf);
    }
    pc = ldq_be_p(mem_buf);

    return mmix_cpu_debug_write_registers(env, regs, sregs, pc);
}
