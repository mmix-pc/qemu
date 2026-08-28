/*
 * MMIX virt RAM reservation planner
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ram-reservation.h"

typedef struct MMIXRAMWorkItem {
    const MMIXRAMReservationRequest *request;
    size_t index;
    uint64_t effective_alignment;
    MMIXPhysRange content;
    MMIXPhysRange ownership;
} MMIXRAMWorkItem;

typedef struct MMIXRAMOwnedRange {
    MMIXPhysRange range;
    const MMIXRAMReservationRequest *request;
} MMIXRAMOwnedRange;

static uint64_t mmix_ram_gcd(uint64_t left, uint64_t right)
{
    while (right != 0) {
        uint64_t remainder = left % right;

        left = right;
        right = remainder;
    }
    return left;
}

static bool mmix_ram_effective_alignment(uint64_t requested,
                                         uint64_t *effective)
{
    uint64_t quotient;

    if (requested == 0) {
        return false;
    }
    quotient = requested / mmix_ram_gcd(requested, MMIX_VIRT_RAM_ALIGN);
    if (quotient > UINT64_MAX / MMIX_VIRT_RAM_ALIGN) {
        return false;
    }
    *effective = quotient * MMIX_VIRT_RAM_ALIGN;
    return true;
}

static bool mmix_ram_valid_identity(const MMIXRAMReservationRequest *request)
{
    return request->owner && request->owner[0] &&
           request->name && request->name[0];
}

static void mmix_ram_request_error(const MMIXRAMReservationRequest *request,
                                   uint64_t ram_size, Error **errp,
                                   const char *reason)
{
    error_setg(errp,
               "MMIX RAM reservation '%s/%s' %s: size 0x%" PRIx64
               ", alignment 0x%" PRIx64 ", RAM size 0x%" PRIx64,
               request->owner ? request->owner : "<invalid>",
               request->name ? request->name : "<invalid>", reason,
               request->size, request->alignment, ram_size);
}

static bool mmix_ram_validate_request(
    const MMIXRAMReservationRequest *request, uint64_t ram_size,
    MMIXRAMWorkItem *work, Error **errp)
{
    uint64_t rounded_end;

    if (!mmix_ram_valid_identity(request)) {
        mmix_ram_request_error(request, ram_size, errp,
                               "has an invalid identity");
        return false;
    }
    if (request->ownership_class >= MMIX_RAM_OWNERSHIP_CLASS_COUNT ||
        request->lifetime >= MMIX_RAM_LIFETIME_COUNT) {
        mmix_ram_request_error(request, ram_size, errp,
                               "has invalid ownership metadata");
        return false;
    }
    if (request->placement != MMIX_RAM_RESERVATION_FIXED &&
        request->placement != MMIX_RAM_RESERVATION_RELOCATABLE) {
        mmix_ram_request_error(request, ram_size, errp,
                               "has an invalid placement");
        return false;
    }
    if (request->size == 0 || request->alignment == 0) {
        mmix_ram_request_error(request, ram_size, errp,
                               "has a zero size or alignment");
        return false;
    }

    work->request = request;
    if (request->placement == MMIX_RAM_RESERVATION_FIXED) {
        if (request->address % request->alignment != 0 ||
            !mmix_phys_range_init(&work->content, request->address,
                                  request->size) ||
            work->content.end > ram_size ||
            !mmix_phys_align_down(work->content.start,
                                  MMIX_VIRT_RAM_ALIGN,
                                  &work->ownership.start) ||
            !mmix_phys_align_up(work->content.end, MMIX_VIRT_RAM_ALIGN,
                                &rounded_end) || rounded_end > ram_size) {
            mmix_ram_request_error(request, ram_size, errp,
                                   "has an invalid fixed range");
            return false;
        }
        work->ownership.end = rounded_end;
        return true;
    }

    if (request->placement_class >= MMIX_RAM_PLACEMENT_CLASS_COUNT ||
        !mmix_ram_effective_alignment(request->alignment,
                                      &work->effective_alignment) ||
        !mmix_phys_align_up(request->size, work->effective_alignment,
                            &rounded_end)) {
        mmix_ram_request_error(request, ram_size, errp,
                               "has an invalid relocatable range");
        return false;
    }
    work->ownership.start = 0;
    work->ownership.end = rounded_end;
    return true;
}

static bool mmix_ram_same_owner(const MMIXRAMReservationRequest *left,
                                const MMIXRAMReservationRequest *right)
{
    return !strcmp(left->owner, right->owner);
}

static int mmix_ram_work_compare(const void *opaque_left,
                                 const void *opaque_right)
{
    const MMIXRAMWorkItem *left = opaque_left;
    const MMIXRAMWorkItem *right = opaque_right;

    if (left->ownership.start < right->ownership.start) {
        return -1;
    }
    if (left->ownership.start > right->ownership.start) {
        return 1;
    }
    if (left->ownership.end < right->ownership.end) {
        return -1;
    }
    if (left->ownership.end > right->ownership.end) {
        return 1;
    }
    return strcmp(left->request->owner, right->request->owner);
}

static int mmix_ram_relocatable_compare(const void *opaque_left,
                                        const void *opaque_right)
{
    const MMIXRAMWorkItem *left = opaque_left;
    const MMIXRAMWorkItem *right = opaque_right;

    if (left->request->placement_class < right->request->placement_class) {
        return -1;
    }
    if (left->request->placement_class > right->request->placement_class) {
        return 1;
    }
    if (left->request->stable_id < right->request->stable_id) {
        return -1;
    }
    if (left->request->stable_id > right->request->stable_id) {
        return 1;
    }
    return 0;
}

static bool mmix_ram_validate_identities(const MMIXRAMWorkItem *work,
                                         size_t count, uint64_t ram_size,
                                         Error **errp)
{
    size_t i;
    size_t j;

    for (i = 0; i < count; i++) {
        for (j = 0; j < i; j++) {
            const MMIXRAMReservationRequest *left = work[i].request;
            const MMIXRAMReservationRequest *right = work[j].request;

            if (!strcmp(left->owner, right->owner) &&
                !strcmp(left->name, right->name)) {
                mmix_ram_request_error(left, ram_size, errp,
                                       "duplicates an object identity");
                return false;
            }
            if (left->placement == MMIX_RAM_RESERVATION_RELOCATABLE &&
                right->placement == MMIX_RAM_RESERVATION_RELOCATABLE &&
                left->placement_class == right->placement_class &&
                left->stable_id == right->stable_id) {
                mmix_ram_request_error(left, ram_size, errp,
                                       "duplicates a stable placement ID");
                return false;
            }
            if (left->placement == MMIX_RAM_RESERVATION_FIXED &&
                right->placement == MMIX_RAM_RESERVATION_FIXED &&
                mmix_phys_ranges_overlap(&work[i].content,
                                         &work[j].content)) {
                mmix_ram_request_error(left, ram_size, errp,
                                       "overlaps another fixed content range");
                return false;
            }
        }
    }
    return true;
}

static bool mmix_ram_ranges_mergeable(const MMIXRAMOwnedRange *left,
                                      const MMIXRAMWorkItem *right)
{
    return left->request->ownership_class == MMIX_RAM_OWNERSHIP_IMAGE &&
           right->request->ownership_class == MMIX_RAM_OWNERSHIP_IMAGE &&
           left->request->lifetime == right->request->lifetime &&
           mmix_ram_same_owner(left->request, right->request);
}

static bool mmix_ram_build_fixed_ranges(MMIXRAMWorkItem *fixed,
                                        size_t fixed_count, uint64_t ram_size,
                                        GArray **owned_out, Error **errp)
{
    g_autoptr(GArray) owned =
        g_array_sized_new(false, false, sizeof(MMIXRAMOwnedRange),
                          fixed_count);
    size_t i;

    qsort(fixed, fixed_count, sizeof(*fixed), mmix_ram_work_compare);
    for (i = 0; i < fixed_count; i++) {
        MMIXRAMWorkItem *item = &fixed[i];

        if (owned->len != 0) {
            MMIXRAMOwnedRange *last =
                &g_array_index(owned, MMIXRAMOwnedRange, owned->len - 1);
            bool touches = item->ownership.start <= last->range.end;

            if (touches && mmix_ram_ranges_mergeable(last, item)) {
                last->range.end = MAX(last->range.end, item->ownership.end);
                continue;
            }
            if (mmix_phys_ranges_overlap(&last->range, &item->ownership)) {
                mmix_ram_request_error(item->request, ram_size, errp,
                                       "overlaps another owner's reservation");
                return false;
            }
        }

        MMIXRAMOwnedRange range = {
            .range = item->ownership,
            .request = item->request,
        };
        g_array_append_val(owned, range);
    }

    *owned_out = g_steal_pointer(&owned);
    return true;
}

static void mmix_ram_remove_from_free(GArray **free_extents,
                                      const MMIXPhysRange *reserved)
{
    g_autoptr(GArray) next =
        g_array_sized_new(false, false, sizeof(MMIXPhysRange),
                          (*free_extents)->len + 1);
    unsigned int i;

    for (i = 0; i < (*free_extents)->len; i++) {
        MMIXPhysRange extent =
            g_array_index(*free_extents, MMIXPhysRange, i);

        if (!mmix_phys_ranges_overlap(&extent, reserved)) {
            g_array_append_val(next, extent);
            continue;
        }
        g_assert(mmix_phys_range_contains(&extent, reserved));
        if (extent.start < reserved->start) {
            MMIXPhysRange before = { extent.start, reserved->start };

            g_array_append_val(next, before);
        }
        if (reserved->end < extent.end) {
            MMIXPhysRange after = { reserved->end, extent.end };

            g_array_append_val(next, after);
        }
    }

    g_array_unref(*free_extents);
    *free_extents = g_steal_pointer(&next);
}

static bool mmix_ram_place_relocatable(MMIXRAMWorkItem *item,
                                       GArray **free_extents,
                                       uint64_t ram_size, Error **errp)
{
    uint64_t reservation_size = mmix_phys_range_size(&item->ownership);
    unsigned int i = (*free_extents)->len;

    while (i-- != 0) {
        MMIXPhysRange extent =
            g_array_index(*free_extents, MMIXPhysRange, i);
        MMIXPhysRange reserved;
        uint64_t candidate;

        if (mmix_phys_range_size(&extent) < reservation_size ||
            !mmix_phys_sub(extent.end, reservation_size, &candidate) ||
            !mmix_phys_align_down(candidate, item->effective_alignment,
                                  &candidate) ||
            candidate < extent.start ||
            !mmix_phys_range_init(&reserved, candidate, reservation_size) ||
            !mmix_phys_range_contains(&extent, &reserved)) {
            continue;
        }

        item->ownership = reserved;
        g_assert(mmix_phys_range_init(&item->content, reserved.start,
                                      item->request->size));
        mmix_ram_remove_from_free(free_extents, &reserved);
        return true;
    }

    mmix_ram_request_error(item->request, ram_size, errp,
                           "does not fit in free RAM");
    return false;
}

void mmix_ram_reservation_plan_clear(MMIXRAMReservationPlan *plan)
{
    g_free(plan->reservations);
    *plan = (MMIXRAMReservationPlan) { 0 };
}

bool mmix_ram_reservation_plan(uint64_t ram_size,
                               const MMIXRAMReservationRequest *requests,
                               size_t request_count,
                               MMIXRAMReservationPlan *plan,
                               Error **errp)
{
    g_autofree MMIXRAMWorkItem *work = NULL;
    g_autofree MMIXRAMWorkItem *fixed = NULL;
    g_autofree MMIXRAMWorkItem *relocatable = NULL;
    g_autoptr(GArray) owned = NULL;
    g_autoptr(GArray) free_extents = NULL;
    MMIXRAMReservationPlan result = { 0 };
    MMIXPhysRange ram;
    size_t fixed_count = 0;
    size_t relocatable_count = 0;
    size_t i;

    g_return_val_if_fail(plan != NULL, false);
    if ((request_count != 0 && requests == NULL) ||
        ram_size < MMIX_VIRT_RAM_MIN_SIZE ||
        ram_size > MMIX_VIRT_RAM_MAX_SIZE ||
        ram_size % MMIX_VIRT_RAM_ALIGN != 0 ||
        !mmix_phys_range_init(&ram, 0, ram_size)) {
        error_setg(errp, "invalid MMIX RAM reservation plan size 0x%" PRIx64,
                   ram_size);
        return false;
    }

    work = g_new0(MMIXRAMWorkItem, request_count);
    fixed = g_new(MMIXRAMWorkItem, request_count);
    relocatable = g_new(MMIXRAMWorkItem, request_count);
    for (i = 0; i < request_count; i++) {
        work[i].index = i;
        if (!mmix_ram_validate_request(&requests[i], ram_size, &work[i],
                                       errp)) {
            return false;
        }
        if (requests[i].placement == MMIX_RAM_RESERVATION_FIXED) {
            fixed[fixed_count++] = work[i];
        } else {
            relocatable[relocatable_count++] = work[i];
        }
    }
    if (!mmix_ram_validate_identities(work, request_count, ram_size, errp) ||
        !mmix_ram_build_fixed_ranges(fixed, fixed_count, ram_size,
                                     &owned, errp)) {
        return false;
    }

    free_extents = g_array_sized_new(false, false, sizeof(MMIXPhysRange),
                                     owned->len + 1);
    g_array_append_val(free_extents, ram);
    for (i = 0; i < owned->len; i++) {
        const MMIXRAMOwnedRange *range =
            &g_array_index(owned, MMIXRAMOwnedRange, i);

        mmix_ram_remove_from_free(&free_extents, &range->range);
    }

    qsort(relocatable, relocatable_count, sizeof(*relocatable),
          mmix_ram_relocatable_compare);
    for (i = 0; i < relocatable_count; i++) {
        if (!mmix_ram_place_relocatable(&relocatable[i], &free_extents,
                                        ram_size, errp)) {
            return false;
        }
    }

    result.count = request_count;
    result.reservations = g_new0(MMIXRAMReservation, request_count);
    for (i = 0; i < fixed_count; i++) {
        size_t j;

        for (j = 0; j < owned->len; j++) {
            const MMIXRAMOwnedRange *range =
                &g_array_index(owned, MMIXRAMOwnedRange, j);

            if (mmix_ram_same_owner(fixed[i].request, range->request) &&
                mmix_phys_range_contains(&range->range,
                                         &fixed[i].ownership)) {
                fixed[i].ownership = range->range;
                break;
            }
        }
        g_assert(j < owned->len);
        result.reservations[fixed[i].index] = (MMIXRAMReservation) {
            .content = fixed[i].content,
            .ownership = fixed[i].ownership,
        };
    }
    for (i = 0; i < relocatable_count; i++) {
        result.reservations[relocatable[i].index] = (MMIXRAMReservation) {
            .content = relocatable[i].content,
            .ownership = relocatable[i].ownership,
        };
    }

    mmix_ram_reservation_plan_clear(plan);
    *plan = result;
    return true;
}
