/*
 * MMIX virt direct-boot plan
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "boot-plan.h"

struct MMIXBootPlan {
    char *image_filename;
    MMIXKernelLoadInfo image_info;
    MMIXRAMReservationRequest *requests;
    MMIXRAMReservationPlan ram;
    size_t request_count;
    bool has_image;
};

static void mmix_boot_plan_clear_requests(MMIXBootPlan *plan)
{
    size_t i;

    for (i = 0; i < plan->request_count; i++) {
        g_free((char *)plan->requests[i].owner);
        g_free((char *)plan->requests[i].name);
    }
    g_free(plan->requests);
}

void mmix_boot_plan_free(MMIXBootPlan *plan)
{
    if (!plan) {
        return;
    }

    mmix_ram_reservation_plan_clear(&plan->ram);
    mmix_boot_plan_clear_requests(plan);
    g_free(plan->image_filename);
    g_free(plan);
}

static void mmix_boot_plan_copy_requests(
    MMIXBootPlan *plan, const MMIXRAMReservationRequest *requests,
    size_t request_count)
{
    size_t i;

    plan->requests = g_new0(MMIXRAMReservationRequest, request_count);
    plan->request_count = request_count;
    for (i = 0; i < request_count; i++) {
        plan->requests[i] = requests[i];
        plan->requests[i].owner = g_strdup(requests[i].owner);
        plan->requests[i].name = g_strdup(requests[i].name);
    }
}

bool mmix_boot_plan_build(uint64_t ram_size, const char *image_filename,
                          const MMIXKernelLoadInfo *image_info,
                          const MMIXRAMReservationRequest *requests,
                          size_t request_count, MMIXBootPlan **plan,
                          Error **errp)
{
    MMIXBootPlan *result;

    g_return_val_if_fail(plan != NULL, false);
    if ((image_filename == NULL) != (image_info == NULL)) {
        error_setg(errp, "MMIX boot plan requires both image source and "
                   "image information");
        return false;
    }
    if (image_info && image_info->image_type > MMIX_KERNEL_IMAGE_ELF) {
        error_setg(errp, "MMIX boot plan has an invalid image type");
        return false;
    }
    if (request_count != 0 && requests == NULL) {
        error_setg(errp, "MMIX boot plan reservation request array is "
                   "missing");
        return false;
    }

    result = g_new0(MMIXBootPlan, 1);
    result->has_image = image_info != NULL;
    if (result->has_image) {
        result->image_filename = g_strdup(image_filename);
        result->image_info = *image_info;
    }
    mmix_boot_plan_copy_requests(result, requests, request_count);
    if (!mmix_ram_reservation_plan(ram_size, result->requests,
                                   request_count, &result->ram, errp)) {
        mmix_boot_plan_free(result);
        return false;
    }

    mmix_boot_plan_free(*plan);
    *plan = result;
    return true;
}

const char *mmix_boot_plan_image_filename(const MMIXBootPlan *plan)
{
    return plan && plan->has_image ? plan->image_filename : NULL;
}

const MMIXKernelLoadInfo *mmix_boot_plan_image_info(const MMIXBootPlan *plan)
{
    return plan && plan->has_image ? &plan->image_info : NULL;
}

size_t mmix_boot_plan_request_count(const MMIXBootPlan *plan)
{
    return plan ? plan->request_count : 0;
}

const MMIXRAMReservationRequest *
mmix_boot_plan_request(const MMIXBootPlan *plan, size_t index)
{
    g_return_val_if_fail(plan != NULL, NULL);
    g_return_val_if_fail(index < plan->request_count, NULL);

    return &plan->requests[index];
}

const MMIXRAMReservation *
mmix_boot_plan_reservation(const MMIXBootPlan *plan, size_t index)
{
    g_return_val_if_fail(plan != NULL, NULL);
    g_return_val_if_fail(index < plan->ram.count, NULL);

    return &plan->ram.reservations[index];
}
