/*
 * MMIX virt direct-boot plan
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_BOOT_PLAN_H
#define HW_MMIX_BOOT_PLAN_H

#include "qapi/error.h"
#include "kernel-loader.h"
#include "ram-reservation.h"

typedef struct MMIXBootPlan MMIXBootPlan;

/*
 * Construct a complete side-effect-free plan. A failed build leaves the
 * caller's current plan unchanged.
 */
bool mmix_boot_plan_build(uint64_t ram_size, const char *image_filename,
                          const MMIXKernelLoadInfo *image_info,
                          const MMIXRAMReservationRequest *requests,
                          size_t request_count, MMIXBootPlan **plan,
                          Error **errp);

void mmix_boot_plan_free(MMIXBootPlan *plan);

const char *mmix_boot_plan_image_filename(const MMIXBootPlan *plan);
const MMIXKernelLoadInfo *mmix_boot_plan_image_info(const MMIXBootPlan *plan);
size_t mmix_boot_plan_request_count(const MMIXBootPlan *plan);
const MMIXRAMReservationRequest *
mmix_boot_plan_request(const MMIXBootPlan *plan, size_t index);
const MMIXRAMReservation *
mmix_boot_plan_reservation(const MMIXBootPlan *plan, size_t index);

#endif
