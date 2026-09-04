/*
 * MMIX virt RAM reservation planner
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_RAM_RESERVATION_H
#define HW_MMIX_RAM_RESERVATION_H

#include "qapi/error.h"
#include "physical-layout.h"

typedef enum MMIXRAMReservationPlacement {
    MMIX_RAM_RESERVATION_FIXED,
    MMIX_RAM_RESERVATION_RELOCATABLE,
} MMIXRAMReservationPlacement;

typedef enum MMIXRAMOwnershipClass {
    MMIX_RAM_OWNERSHIP_IMAGE,
    MMIX_RAM_OWNERSHIP_MACHINE,
    MMIX_RAM_OWNERSHIP_CPU_BOOTSTRAP,
    MMIX_RAM_OWNERSHIP_DISCOVERY,
    MMIX_RAM_OWNERSHIP_CLASS_COUNT,
} MMIXRAMOwnershipClass;

typedef enum MMIXRAMReservationLifetime {
    MMIX_RAM_LIFETIME_PERMANENT,
    MMIX_RAM_LIFETIME_UNTIL_GUEST_RELEASE,
    MMIX_RAM_LIFETIME_UNTIL_CONSUMED,
    MMIX_RAM_LIFETIME_COUNT,
} MMIXRAMReservationLifetime;

typedef enum MMIXRAMPlacementClass {
    /* Earlier classes receive higher free ranges. */
    MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
    MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
    MMIX_RAM_PLACEMENT_LOADER,
    MMIX_RAM_PLACEMENT_INITRD,
    MMIX_RAM_PLACEMENT_FDT,
    MMIX_RAM_PLACEMENT_METADATA,
    MMIX_RAM_PLACEMENT_CLASS_COUNT,
} MMIXRAMPlacementClass;

typedef struct MMIXRAMReservationRequest {
    const char *owner;
    const char *name;
    /* Must be unique within a relocatable placement class. */
    uint64_t stable_id;
    MMIXRAMReservationPlacement placement;
    MMIXRAMOwnershipClass ownership_class;
    MMIXRAMReservationLifetime lifetime;
    MMIXRAMPlacementClass placement_class;
    uint64_t address;
    uint64_t size;
    uint64_t alignment;
} MMIXRAMReservationRequest;

typedef struct MMIXRAMReservation {
    MMIXPhysRange content;
    MMIXPhysRange ownership;
} MMIXRAMReservation;

typedef struct MMIXRAMReservationPlan {
    /* Entry i describes request i; merged fixed-image entries may overlap. */
    MMIXRAMReservation *reservations;
    size_t count;
} MMIXRAMReservationPlan;

void mmix_ram_reservation_plan_clear(MMIXRAMReservationPlan *plan);

/*
 * Build a complete plan without changing guest memory or CPU state. The
 * caller must zero-initialize plan. A failure leaves its previous contents
 * unchanged.
 */
bool mmix_ram_reservation_plan(uint64_t ram_size,
                               const MMIXRAMReservationRequest *requests,
                               size_t request_count,
                               MMIXRAMReservationPlan *plan,
                               Error **errp);

#endif
