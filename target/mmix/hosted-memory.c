/*
 * MMIX hosted logical-memory access
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "exec/memop.h"
#include "exec/tb-flush.h"

static G_NORETURN void mmix_hosted_memory_failure(CPUMMIXState *env,
                                                   Error *err)
{
    if (!env->test_exit_seen) {
        error_report_err(err);
    } else {
        error_free(err);
    }
    mmix_cpu_shutdown_with_log(env, "MMIX hosted memory access failed",
                               EXIT_FAILURE);
}

bool mmix_cpu_hosted_memory_enabled(CPUMMIXState *env)
{
    return MMIX_CPU(env_cpu(env))->hosted_memory_ops != NULL;
}

void mmix_cpu_set_hosted_memory(CPUState *cs,
                                const MMIXHostedMemoryOps *ops,
                                void *opaque)
{
    MMIXCPU *cpu = MMIX_CPU(cs);

    g_assert((ops == NULL) == (opaque == NULL));
    cpu->hosted_memory_ops = ops;
    cpu->hosted_memory_opaque = opaque;
}

static bool mmix_hosted_memory_read(CPUMMIXState *env, uint64_t address,
                                    void *buffer, size_t size,
                                    size_t alignment, Error **errp)
{
    MMIXCPU *cpu = MMIX_CPU(env_cpu(env));

    g_assert(cpu->hosted_memory_ops);
    return cpu->hosted_memory_ops->read(cpu->hosted_memory_opaque, address,
                                        buffer, size, alignment, errp);
}

uint32_t mmix_cpu_hosted_fetch(CPUMMIXState *env, vaddr address)
{
    uint8_t data[sizeof(uint32_t)];
    Error *err = NULL;

    if (address >= MMIX_HOSTED_DATA_BASE ||
        !mmix_hosted_memory_read(env, address, data, sizeof(data),
                                 sizeof(data), &err)) {
        if (!err) {
            error_setg(&err, "MMIX hosted instruction fetch outside Text "
                       "at 0x%016" PRIx64, address);
        }
        mmix_hosted_memory_failure(env, err);
    }
    return ldl_be_p(data);
}

uint64_t helper_mmix_hosted_load(CPUMMIXState *env, uint64_t address,
                                 uint32_t memop)
{
    uint8_t data[sizeof(uint64_t)] = { 0 };
    size_t size = memop_size(memop);
    uint64_t value;
    Error *err = NULL;

    if (!mmix_hosted_memory_read(env, address, data, size, size, &err)) {
        mmix_hosted_memory_failure(env, err);
    }
    switch (size) {
    case 1:
        value = data[0];
        break;
    case 2:
        value = lduw_be_p(data);
        break;
    case 4:
        value = ldl_be_p(data);
        break;
    case 8:
        value = ldq_be_p(data);
        break;
    default:
        g_assert_not_reached();
    }
    if (memop & MO_SIGN) {
        value = sextract64(value, 0, size * 8);
    }
    return value;
}

void helper_mmix_hosted_store(CPUMMIXState *env, uint64_t address,
                              uint64_t value, uint32_t memop)
{
    MMIXCPU *cpu = MMIX_CPU(env_cpu(env));
    uint8_t data[sizeof(uint64_t)];
    size_t size = memop_size(memop);
    Error *err = NULL;

    switch (size) {
    case 1:
        data[0] = value;
        break;
    case 2:
        stw_be_p(data, value);
        break;
    case 4:
        stl_be_p(data, value);
        break;
    case 8:
        stq_be_p(data, value);
        break;
    default:
        g_assert_not_reached();
    }
    if (!cpu->hosted_memory_ops->write(cpu->hosted_memory_opaque, address,
                                       data, size, size, &err)) {
        mmix_hosted_memory_failure(env, err);
    }
    if (address < MMIX_HOSTED_DATA_BASE) {
        queue_tb_flush(env_cpu(env));
    }
}

G_NORETURN void helper_mmix_hosted_unsupported(CPUMMIXState *env,
                                               uint32_t insn)
{
    error_report("MMIX hosted execution does not yet support instruction "
                 "0x%08x at 0x%016" PRIx64, insn, env->pc);
    mmix_cpu_shutdown_with_log(env, "unsupported MMIX hosted memory access",
                               EXIT_FAILURE);
}
