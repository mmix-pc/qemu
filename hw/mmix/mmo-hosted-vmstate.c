/*
 * MMIX MMO hosted-memory VMState device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/tb-flush.h"
#include "hw/core/qdev.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "mmo-hosted-state.h"
#include "mmo-hosted-vmstate.h"

struct MMIXMMOHostedState {
    DeviceState parent_obj;
    MMIXSparseMemory **memory;
    CPUState **cpus;
    unsigned int cpu_count;
    uint64_t expected_budget;
};

static bool mmix_mmo_hosted_vmstate_save(
    QEMUFile *f, void *opaque, size_t size, const VMStateField *field,
    JSONWriter *vmdesc, Error **errp)
{
    MMIXMMOHostedState *state = opaque;

    return mmix_mmo_hosted_state_save(f, *state->memory, errp);
}

static bool mmix_mmo_hosted_vmstate_load(QEMUFile *f, void *opaque,
                                         size_t size,
                                         const VMStateField *field,
                                         Error **errp)
{
    MMIXMMOHostedState *state = opaque;
    bool expected_hosted = *state->memory != NULL;
    unsigned int i;

    if (!mmix_mmo_hosted_state_load(
            f, expected_hosted, state->expected_budget,
            state->memory, errp)) {
        return false;
    }
    if (expected_hosted) {
        for (i = 0; i < state->cpu_count; i++) {
            queue_tb_flush(state->cpus[i]);
        }
    }
    return true;
}

static const VMStateInfo vmstate_info_mmix_mmo_hosted = {
    .name = "mmix-mmo-hosted-memory",
    .load = mmix_mmo_hosted_vmstate_load,
    .save = mmix_mmo_hosted_vmstate_save,
};

static const VMStateDescription vmstate_mmix_mmo_hosted = {
    .name = "mmix-virt/hosted-memory",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        {
            .name = "sparse-state",
            .version_id = 1,
            .size = 0,
            .info = &vmstate_info_mmix_mmo_hosted,
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST(),
    },
};

void mmix_mmo_hosted_state_configure(MMIXMMOHostedState *state,
                                     MMIXSparseMemory **memory,
                                     CPUState **cpus,
                                     unsigned int cpu_count,
                                     uint64_t expected_budget)
{
    g_assert(memory);
    g_assert(cpus);
    g_assert(cpu_count > 0);

    state->memory = memory;
    state->cpus = cpus;
    state->cpu_count = cpu_count;
    state->expected_budget = expected_budget;
}

static void mmix_mmo_hosted_state_realize(DeviceState *dev, Error **errp)
{
    MMIXMMOHostedState *state = MMIX_MMO_HOSTED_STATE(dev);

    if (!state->memory || !state->cpus || !state->cpu_count ||
        !state->expected_budget) {
        error_setg(errp, "MMIX MMO hosted migration state is not configured");
    }
}

static void mmix_mmo_hosted_state_class_init(ObjectClass *klass,
                                             const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mmix_mmo_hosted_state_realize;
    dc->vmsd = &vmstate_mmix_mmo_hosted;
}

static const TypeInfo mmix_mmo_hosted_state_type_info = {
    .name = TYPE_MMIX_MMO_HOSTED_STATE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MMIXMMOHostedState),
    .class_init = mmix_mmo_hosted_state_class_init,
};

static void mmix_mmo_hosted_state_register_types(void)
{
    type_register_static(&mmix_mmo_hosted_state_type_info);
}

type_init(mmix_mmo_hosted_state_register_types)
