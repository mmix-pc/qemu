/*
 * MMIX virt RAM reservation planner tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/ram-reservation.h"
#include "hw/mmix/virt.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "target/mmix/addressing.h"

#define TEST_RAM_SIZE MMIX_VIRT_RAM_MIN_SIZE

static MMIXRAMReservationRequest fixed_request(const char *owner,
                                               const char *name,
                                               uint64_t address,
                                               uint64_t size)
{
    return (MMIXRAMReservationRequest) {
        .owner = owner,
        .name = name,
        .placement = MMIX_RAM_RESERVATION_FIXED,
        .ownership_class = MMIX_RAM_OWNERSHIP_IMAGE,
        .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
        .address = address,
        .size = size,
        .alignment = 1,
    };
}

static MMIXRAMReservationRequest relocatable_request(
    const char *owner, const char *name, uint64_t stable_id,
    MMIXRAMPlacementClass placement_class, uint64_t size, uint64_t alignment)
{
    MMIXRAMOwnershipClass ownership_class = MMIX_RAM_OWNERSHIP_DISCOVERY;
    MMIXRAMReservationLifetime lifetime = MMIX_RAM_LIFETIME_UNTIL_CONSUMED;

    switch (placement_class) {
    case MMIX_RAM_PLACEMENT_PERMANENT_MACHINE:
        ownership_class = MMIX_RAM_OWNERSHIP_MACHINE;
        lifetime = MMIX_RAM_LIFETIME_PERMANENT;
        break;
    case MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP:
        ownership_class = MMIX_RAM_OWNERSHIP_CPU_BOOTSTRAP;
        lifetime = MMIX_RAM_LIFETIME_UNTIL_GUEST_RELEASE;
        break;
    case MMIX_RAM_PLACEMENT_LOADER:
    case MMIX_RAM_PLACEMENT_INITRD:
        ownership_class = MMIX_RAM_OWNERSHIP_IMAGE;
        lifetime = MMIX_RAM_LIFETIME_PERMANENT;
        break;
    case MMIX_RAM_PLACEMENT_FDT:
    case MMIX_RAM_PLACEMENT_METADATA:
        break;
    default:
        g_assert_not_reached();
    }

    return (MMIXRAMReservationRequest) {
        .owner = owner,
        .name = name,
        .stable_id = stable_id,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = ownership_class,
        .lifetime = lifetime,
        .placement_class = placement_class,
        .size = size,
        .alignment = alignment,
    };
}

static void assert_range(const MMIXPhysRange *range, uint64_t start,
                         uint64_t end)
{
    g_assert_cmphex(range->start, ==, start);
    g_assert_cmphex(range->end, ==, end);
}

static void assert_reservations_do_not_overlap(
    const MMIXRAMReservationPlan *plan)
{
    size_t i;
    size_t j;

    for (i = 0; i < plan->count; i++) {
        for (j = 0; j < i; j++) {
            g_assert_false(mmix_phys_ranges_overlap(
                &plan->reservations[i].ownership,
                &plan->reservations[j].ownership));
        }
    }
}

static void assert_reservation_aliases_round_trip(
    const MMIXRAMReservationPlan *plan)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        const MMIXPhysRange *range = &plan->reservations[i].ownership;
        uint64_t addresses[] = { range->start, range->end - 1 };
        size_t j;

        for (j = 0; j < ARRAY_SIZE(addresses); j++) {
            uint64_t alias = 0;
            uint64_t physical = 0;

            g_assert_true(mmix_phys_to_negative_alias(addresses[j], &alias));
            g_assert_true(mmix_negative_alias_to_phys(alias, &physical));
            g_assert_cmphex(physical, ==, addresses[j]);
        }
    }
}

typedef struct MMIXPlatformBoundaryCase {
    uint64_t ram_size;
    unsigned int cpu_count;
} MMIXPlatformBoundaryCase;

static void test_platform_boundary(gconstpointer opaque)
{
    const MMIXPlatformBoundaryCase *test = opaque;
    const uint64_t image_size = 8 * MiB;
    const uint64_t initrd_size = 16 * MiB;
    const uint64_t fdt_size = 2 * MiB;
    size_t request_count = test->cpu_count + 5;
    g_autofree MMIXRAMReservationRequest *requests =
        g_new0(MMIXRAMReservationRequest, request_count);
    g_auto(GStrv) stack_names = g_new0(char *, test->cpu_count + 1);
    MMIXRAMReservationPlan plan = { 0 };
    const MMIXRAMReservation *reservation;
    uint64_t cursor = test->ram_size;
    size_t index;
    unsigned int cpu;

    requests[0] = fixed_request("kernel", "image", 0, image_size);
    requests[1] = relocatable_request(
        "framebuffer", "pixels", 0,
        MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
        MMIX_VIRT_FRAMEBUFFER_SIZE, MMIX_VIRT_FRAMEBUFFER_ALIGN);
    for (cpu = 0; cpu < test->cpu_count; cpu++) {
        stack_names[cpu] = g_strdup_printf("initial-stack-%u", cpu);
        requests[2 + cpu] = relocatable_request(
            "cpu", stack_names[cpu], cpu,
            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
            MMIX_VIRT_INITIAL_STACK_SIZE,
            MMIX_VIRT_INITIAL_STACK_ALIGN);
    }
    index = 2 + test->cpu_count;
    requests[index++] = relocatable_request(
        "kernel", "initrd", 0, MMIX_RAM_PLACEMENT_INITRD,
        initrd_size, MMIX_VIRT_RAM_ALIGN);
    requests[index++] = relocatable_request(
        "fdt", "blob", 0, MMIX_RAM_PLACEMENT_FDT,
        fdt_size, 8);
    requests[index] = relocatable_request(
        "semihosting", "arguments", 0, MMIX_RAM_PLACEMENT_METADATA,
        4096, 8);

    g_assert_true(mmix_ram_reservation_plan(
        test->ram_size, requests, request_count, &plan, &error_abort));

    reservation = &plan.reservations[1];
    assert_range(&reservation->ownership,
                 cursor - MMIX_VIRT_FRAMEBUFFER_SIZE, cursor);
    cursor = reservation->ownership.start;
    for (cpu = 0; cpu < test->cpu_count; cpu++) {
        reservation = &plan.reservations[2 + cpu];
        assert_range(&reservation->ownership,
                     cursor - MMIX_VIRT_INITIAL_STACK_SIZE, cursor);
        cursor = reservation->ownership.start;
    }
    reservation = &plan.reservations[2 + test->cpu_count];
    assert_range(&reservation->ownership, cursor - initrd_size, cursor);
    cursor = reservation->ownership.start;
    reservation = &plan.reservations[3 + test->cpu_count];
    assert_range(&reservation->ownership, cursor - fdt_size, cursor);
    cursor = reservation->ownership.start;
    reservation = &plan.reservations[4 + test->cpu_count];
    assert_range(&reservation->ownership,
                 cursor - MMIX_VIRT_RAM_ALIGN, cursor);
    g_assert_cmphex(plan.reservations[0].ownership.end, <=,
                    reservation->ownership.start);
    assert_reservations_do_not_overlap(&plan);
    assert_reservation_aliases_round_trip(&plan);
    mmix_ram_reservation_plan_clear(&plan);
}

static void test_platform_near_capacity_is_atomic(void)
{
    enum { CPU_COUNT = MMIX_VIRT_MAX_CPUS };
    const uint64_t dynamic_size =
        MMIX_VIRT_FRAMEBUFFER_SIZE +
        CPU_COUNT * MMIX_VIRT_INITIAL_STACK_SIZE + MiB +
        2 * MMIX_VIRT_RAM_ALIGN;
    const uint64_t image_size = TEST_RAM_SIZE - dynamic_size;
    const size_t request_count = CPU_COUNT + 5;
    g_autofree MMIXRAMReservationRequest *requests =
        g_new0(MMIXRAMReservationRequest, request_count);
    g_auto(GStrv) stack_names = g_new0(char *, CPU_COUNT + 1);
    MMIXRAMReservationPlan plan = { 0 };
    MMIXRAMReservation *original;
    g_autofree MMIXRAMReservation *saved = NULL;
    Error *err = NULL;
    size_t index;
    unsigned int cpu;

    requests[0] = fixed_request("kernel", "image", 0, image_size);
    requests[1] = relocatable_request(
        "framebuffer", "pixels", 0,
        MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
        MMIX_VIRT_FRAMEBUFFER_SIZE, MMIX_VIRT_FRAMEBUFFER_ALIGN);
    for (cpu = 0; cpu < CPU_COUNT; cpu++) {
        stack_names[cpu] = g_strdup_printf("initial-stack-%u", cpu);
        requests[2 + cpu] = relocatable_request(
            "cpu", stack_names[cpu], cpu,
            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
            MMIX_VIRT_INITIAL_STACK_SIZE,
            MMIX_VIRT_INITIAL_STACK_ALIGN);
    }
    index = 2 + CPU_COUNT;
    requests[index++] = relocatable_request(
        "kernel", "initrd", 0, MMIX_RAM_PLACEMENT_INITRD,
        MiB, MMIX_VIRT_RAM_ALIGN);
    requests[index++] = relocatable_request(
        "fdt", "blob", 0, MMIX_RAM_PLACEMENT_FDT,
        MMIX_VIRT_RAM_ALIGN, 8);
    requests[index] = relocatable_request(
        "semihosting", "arguments", 0, MMIX_RAM_PLACEMENT_METADATA,
        MMIX_VIRT_RAM_ALIGN, 8);

    g_assert_true(mmix_ram_reservation_plan(
        TEST_RAM_SIZE, requests, request_count, &plan, &error_abort));
    g_assert_cmphex(plan.reservations[0].ownership.end, ==,
                    plan.reservations[request_count - 1].ownership.start);
    assert_reservations_do_not_overlap(&plan);
    assert_reservation_aliases_round_trip(&plan);

    original = plan.reservations;
    saved = g_memdup2(plan.reservations,
                      plan.count * sizeof(*plan.reservations));
    requests[2 + CPU_COUNT].size += MMIX_VIRT_RAM_ALIGN;
    g_assert_false(mmix_ram_reservation_plan(
        TEST_RAM_SIZE, requests, request_count, &plan, &err));
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "does not fit in free RAM"));
    g_assert_true(plan.reservations == original);
    g_assert_cmpmem(plan.reservations,
                    plan.count * sizeof(*plan.reservations),
                    saved, plan.count * sizeof(*saved));
    error_free(err);
    mmix_ram_reservation_plan_clear(&plan);
}

static const MMIXRAMReservation *find_reservation(
    const MMIXRAMReservationRequest *requests,
    const MMIXRAMReservationPlan *plan, const char *name)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        if (!strcmp(requests[i].name, name)) {
            return &plan->reservations[i];
        }
    }
    g_assert_not_reached();
}

static void assert_plan_fails(const MMIXRAMReservationRequest *requests,
                              size_t count, const char *diagnostic)
{
    MMIXRAMReservationPlan plan = { 0 };
    Error *err = NULL;

    g_assert_false(mmix_ram_reservation_plan(TEST_RAM_SIZE, requests, count,
                                             &plan, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    g_assert_null(plan.reservations);
    g_assert_cmpuint(plan.count, ==, 0);
    error_free(err);
}

static void test_fixed_image_merge(void)
{
    MMIXRAMReservationRequest requests[] = {
        fixed_request("kernel", "text", 0x1000, 0x800),
        fixed_request("kernel", "data", 0x2000, 0x800),
        fixed_request("initrd", "payload", 0x8000, 0x1000),
    };
    MMIXRAMReservationPlan plan = { 0 };

    g_assert_true(mmix_ram_reservation_plan(TEST_RAM_SIZE, requests,
                                            ARRAY_SIZE(requests), &plan,
                                            &error_abort));
    assert_range(&plan.reservations[0].content, 0x1000, 0x1800);
    assert_range(&plan.reservations[0].ownership, 0, 0x4000);
    assert_range(&plan.reservations[1].content, 0x2000, 0x2800);
    assert_range(&plan.reservations[1].ownership, 0, 0x4000);
    assert_range(&plan.reservations[2].ownership, 0x8000, 0xa000);
    mmix_ram_reservation_plan_clear(&plan);
}

static void test_fixed_conflicts(void)
{
    MMIXRAMReservationRequest page_conflict[] = {
        fixed_request("kernel", "text", 0x1000, 0x800),
        fixed_request("initrd", "payload", 0x1800, 0x800),
    };
    MMIXRAMReservationRequest content_conflict[] = {
        fixed_request("kernel", "text", 0x1000, 0x1000),
        fixed_request("kernel", "data", 0x1800, 0x1000),
    };

    assert_plan_fails(page_conflict, ARRAY_SIZE(page_conflict),
                      "another owner's reservation");
    assert_plan_fails(content_conflict, ARRAY_SIZE(content_conflict),
                      "another fixed content range");
}

static void test_stable_class_order(void)
{
    MMIXRAMReservationRequest requests[] = {
        relocatable_request("metadata", "arguments", 0,
                            MMIX_RAM_PLACEMENT_METADATA, 1, 1),
        relocatable_request("cpu", "stack-1", 1,
                            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP, 1, 1),
        relocatable_request("framebuffer", "pixels", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE, 1, 1),
        relocatable_request("cpu", "stack-0", 0,
                            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP, 1, 1),
        relocatable_request("fdt", "blob", 0,
                            MMIX_RAM_PLACEMENT_FDT, 1, 1),
    };
    MMIXRAMReservationRequest reversed[ARRAY_SIZE(requests)];
    MMIXRAMReservationPlan plan = { 0 };
    MMIXRAMReservationPlan reversed_plan = { 0 };
    const MMIXRAMReservation *reservation;
    size_t i;

    g_assert_true(mmix_ram_reservation_plan(TEST_RAM_SIZE, requests,
                                            ARRAY_SIZE(requests), &plan,
                                            &error_abort));
    reservation = find_reservation(requests, &plan, "pixels");
    assert_range(&reservation->ownership,
                 TEST_RAM_SIZE - 8 * KiB, TEST_RAM_SIZE);
    reservation = find_reservation(requests, &plan, "stack-0");
    assert_range(&reservation->ownership,
                 TEST_RAM_SIZE - 16 * KiB, TEST_RAM_SIZE - 8 * KiB);
    reservation = find_reservation(requests, &plan, "stack-1");
    assert_range(&reservation->ownership,
                 TEST_RAM_SIZE - 24 * KiB, TEST_RAM_SIZE - 16 * KiB);
    reservation = find_reservation(requests, &plan, "blob");
    assert_range(&reservation->ownership,
                 TEST_RAM_SIZE - 32 * KiB, TEST_RAM_SIZE - 24 * KiB);
    reservation = find_reservation(requests, &plan, "arguments");
    assert_range(&reservation->ownership,
                 TEST_RAM_SIZE - 40 * KiB, TEST_RAM_SIZE - 32 * KiB);

    for (i = 0; i < ARRAY_SIZE(requests); i++) {
        reversed[i] = requests[ARRAY_SIZE(requests) - i - 1];
    }
    g_assert_true(mmix_ram_reservation_plan(TEST_RAM_SIZE, reversed,
                                            ARRAY_SIZE(reversed),
                                            &reversed_plan, &error_abort));
    for (i = 0; i < ARRAY_SIZE(requests); i++) {
        const MMIXRAMReservation *original =
            find_reservation(requests, &plan, requests[i].name);
        const MMIXRAMReservation *reordered =
            find_reservation(reversed, &reversed_plan, requests[i].name);

        g_assert_cmpmem(original, sizeof(*original),
                        reordered, sizeof(*reordered));
    }

    mmix_ram_reservation_plan_clear(&reversed_plan);
    mmix_ram_reservation_plan_clear(&plan);
}

static void test_highest_fit_around_hole(void)
{
    MMIXRAMReservationRequest requests[] = {
        fixed_request("kernel", "high-segment",
                      TEST_RAM_SIZE - 32 * KiB, 8 * KiB),
        relocatable_request("machine", "first", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
                            16 * KiB, 8 * KiB),
        relocatable_request("cpu", "second", 0,
                            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
                            16 * KiB, 8 * KiB),
    };
    MMIXRAMReservationPlan plan = { 0 };

    g_assert_true(mmix_ram_reservation_plan(TEST_RAM_SIZE, requests,
                                            ARRAY_SIZE(requests), &plan,
                                            &error_abort));
    assert_range(&plan.reservations[1].ownership,
                 TEST_RAM_SIZE - 16 * KiB, TEST_RAM_SIZE);
    assert_range(&plan.reservations[2].ownership,
                 TEST_RAM_SIZE - 48 * KiB, TEST_RAM_SIZE - 32 * KiB);
    mmix_ram_reservation_plan_clear(&plan);
}

static void test_exact_fit(void)
{
    MMIXRAMReservationRequest requests[] = {
        fixed_request("kernel", "image", 0,
                      TEST_RAM_SIZE - 32 * KiB),
        relocatable_request("machine", "exact", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
                            24 * KiB, 16 * KiB),
    };
    MMIXRAMReservationPlan plan = { 0 };

    g_assert_true(mmix_ram_reservation_plan(TEST_RAM_SIZE, requests,
                                            ARRAY_SIZE(requests), &plan,
                                            &error_abort));
    assert_range(&plan.reservations[1].content,
                 TEST_RAM_SIZE - 32 * KiB, TEST_RAM_SIZE - 8 * KiB);
    assert_range(&plan.reservations[1].ownership,
                 TEST_RAM_SIZE - 32 * KiB, TEST_RAM_SIZE);
    mmix_ram_reservation_plan_clear(&plan);
}

static void test_alignment_loss(void)
{
    MMIXRAMReservationRequest requests[] = {
        fixed_request("low", "image", 0,
                      TEST_RAM_SIZE - 24 * KiB),
        fixed_request("high", "image", TEST_RAM_SIZE - 8 * KiB,
                      8 * KiB),
        relocatable_request("machine", "aligned", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
                            16 * KiB, 16 * KiB),
    };

    assert_plan_fails(requests, ARRAY_SIZE(requests),
                      "does not fit in free RAM");
}

static void test_invalid_and_duplicate_requests(void)
{
    MMIXRAMReservationRequest overflow[] = {
        relocatable_request("machine", "overflow", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
                            UINT64_MAX, 8 * KiB),
    };
    MMIXRAMReservationRequest duplicate_ids[] = {
        relocatable_request("cpu", "stack-0", 0,
                            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
                            8 * KiB, 8 * KiB),
        relocatable_request("cpu", "stack-other", 0,
                            MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
                            8 * KiB, 8 * KiB),
    };

    assert_plan_fails(overflow, ARRAY_SIZE(overflow),
                      "invalid relocatable range");
    assert_plan_fails(duplicate_ids, ARRAY_SIZE(duplicate_ids),
                      "duplicates a stable placement ID");
}

static void test_failure_is_atomic(void)
{
    MMIXRAMReservationRequest initial[] = {
        relocatable_request("machine", "backing", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
                            8 * KiB, 8 * KiB),
    };
    MMIXRAMReservationRequest exhausted[] = {
        fixed_request("kernel", "image", 0, TEST_RAM_SIZE),
        relocatable_request("machine", "backing", 0,
                            MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
                            8 * KiB, 8 * KiB),
    };
    MMIXRAMReservationPlan plan = { 0 };
    MMIXRAMReservation *original;
    MMIXRAMReservation saved;
    Error *err = NULL;

    g_assert_true(mmix_ram_reservation_plan(TEST_RAM_SIZE, initial,
                                            ARRAY_SIZE(initial), &plan,
                                            &error_abort));
    original = plan.reservations;
    saved = plan.reservations[0];

    g_assert_false(mmix_ram_reservation_plan(TEST_RAM_SIZE, exhausted,
                                             ARRAY_SIZE(exhausted), &plan,
                                             &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "does not fit in free RAM"));
    g_assert_nonnull(strstr(error_get_pretty(err), "machine/backing"));
    g_assert_nonnull(strstr(error_get_pretty(err), "alignment 0x2000"));
    g_assert_nonnull(strstr(error_get_pretty(err), "RAM size 0x8000000"));
    g_assert_true(plan.reservations == original);
    g_assert_cmpuint(plan.count, ==, ARRAY_SIZE(initial));
    g_assert_cmpmem(&plan.reservations[0], sizeof(plan.reservations[0]),
                    &saved, sizeof(saved));
    error_free(err);
    mmix_ram_reservation_plan_clear(&plan);
}

int main(int argc, char **argv)
{
    static const MMIXPlatformBoundaryCase boundary_cases[] = {
        { MMIX_VIRT_RAM_MIN_SIZE, 1 },
        { 512 * MiB, 2 },
        { 8 * GiB, MMIX_VIRT_MAX_CPUS },
        { MMIX_VIRT_RAM_MAX_SIZE, MMIX_VIRT_MAX_CPUS },
    };
    static const char * const boundary_names[] = {
        "minimum-one-cpu",
        "default-two-cpus",
        "above-4g-maximum-cpus",
        "maximum-ram-and-cpus",
    };
    size_t i;

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/ram-reservation/fixed-merge",
                    test_fixed_image_merge);
    g_test_add_func("/mmix/ram-reservation/fixed-conflicts",
                    test_fixed_conflicts);
    g_test_add_func("/mmix/ram-reservation/stable-class-order",
                    test_stable_class_order);
    g_test_add_func("/mmix/ram-reservation/highest-fit-hole",
                    test_highest_fit_around_hole);
    g_test_add_func("/mmix/ram-reservation/exact-fit", test_exact_fit);
    g_test_add_func("/mmix/ram-reservation/alignment-loss",
                    test_alignment_loss);
    g_test_add_func("/mmix/ram-reservation/invalid-requests",
                    test_invalid_and_duplicate_requests);
    g_test_add_func("/mmix/ram-reservation/failure-atomic",
                    test_failure_is_atomic);
    for (i = 0; i < ARRAY_SIZE(boundary_cases); i++) {
        g_autofree char *path = g_strdup_printf(
            "/mmix/ram-reservation/platform-boundary/%s",
            boundary_names[i]);

        g_test_add_data_func(path, &boundary_cases[i],
                             test_platform_boundary);
    }
    g_test_add_func("/mmix/ram-reservation/platform-near-capacity-atomic",
                    test_platform_near_capacity_is_atomic);
    return g_test_run();
}
