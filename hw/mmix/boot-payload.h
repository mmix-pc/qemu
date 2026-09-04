/*
 * MMIX virt immutable RAM boot payload
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_BOOT_PAYLOAD_H
#define HW_MMIX_BOOT_PAYLOAD_H

#include "qemu/typedefs.h"
#include "qapi/error.h"

typedef struct MMIXBootPayload MMIXBootPayload;

MMIXBootPayload *mmix_boot_payload_new(uint64_t ram_size);
void mmix_boot_payload_free(MMIXBootPayload *payload);

bool mmix_boot_payload_add(MMIXBootPayload *payload, const char *name,
                           uint64_t address, GBytes *data,
                           uint64_t memory_size, Error **errp);
bool mmix_boot_payload_add_zero(MMIXBootPayload *payload, const char *name,
                                uint64_t address, uint64_t size,
                                Error **errp);

bool mmix_boot_payload_commit(const MMIXBootPayload *payload, void *ram,
                              uint64_t ram_size, Error **errp);
bool mmix_boot_payload_commit_address_space(const MMIXBootPayload *payload,
                                            AddressSpace *address_space,
                                            uint64_t ram_size, Error **errp);

#endif
