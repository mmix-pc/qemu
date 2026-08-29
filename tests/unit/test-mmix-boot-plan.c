/*
 * MMIX virt direct-boot plan tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/boot-plan.h"
#include "qapi/error.h"
#include "qemu/units.h"

#define TEST_RAM_SIZE MMIX_VIRT_RAM_MIN_SIZE
#define TEST_LARGE_RAM_SIZE (8 * GiB)

static MMIXRAMReservationRequest fixed_request(
    const char *owner, const char *name, MMIXRAMOwnershipClass ownership_class,
    uint64_t address, uint64_t size)
{
    return (MMIXRAMReservationRequest) {
        .owner = owner,
        .name = name,
        .placement = MMIX_RAM_RESERVATION_FIXED,
        .ownership_class = ownership_class,
        .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
        .address = address,
        .size = size,
        .alignment = 1,
    };
}

static MMIXRAMReservationRequest framebuffer_request(void)
{
    return (MMIXRAMReservationRequest) {
        .owner = "mmix-framebuffer",
        .name = "pixels",
        .stable_id = 0,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_MACHINE,
        .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
        .placement_class = MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
        .size = 3 * MiB,
        .alignment = 8 * KiB,
    };
}

static MMIXRAMReservationRequest metadata_request(void)
{
    return (MMIXRAMReservationRequest) {
        .owner = "mmix-semihosting",
        .name = "arguments",
        .stable_id = 0,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_DISCOVERY,
        .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
        .placement_class = MMIX_RAM_PLACEMENT_METADATA,
        .size = 64,
        .alignment = 8,
    };
}

static MMIXRAMReservationRequest stack_request(unsigned int cpu,
                                                const char *name)
{
    return (MMIXRAMReservationRequest) {
        .owner = "mmix-cpu",
        .name = name,
        .stable_id = cpu,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_CPU_BOOTSTRAP,
        .lifetime = MMIX_RAM_LIFETIME_UNTIL_GUEST_RELEASE,
        .placement_class = MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
        .size = 32 * KiB,
        .alignment = 8 * KiB,
    };
}

static MMIXRAMReservationRequest initrd_request(uint64_t size)
{
    return (MMIXRAMReservationRequest) {
        .owner = "mmix-kernel",
        .name = "initrd",
        .stable_id = 0,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_IMAGE,
        .lifetime = MMIX_RAM_LIFETIME_UNTIL_CONSUMED,
        .placement_class = MMIX_RAM_PLACEMENT_INITRD,
        .size = size,
        .alignment = 8 * KiB,
    };
}

static MMIXRAMReservationRequest fdt_request(uint64_t size)
{
    return (MMIXRAMReservationRequest) {
        .owner = "mmix-fdt",
        .name = "blob",
        .stable_id = 0,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_DISCOVERY,
        .lifetime = MMIX_RAM_LIFETIME_UNTIL_CONSUMED,
        .placement_class = MMIX_RAM_PLACEMENT_FDT,
        .size = size,
        .alignment = 8,
    };
}

static void assert_range(const MMIXPhysRange *range, uint64_t start,
                         uint64_t end)
{
    g_assert_cmphex(range->start, ==, start);
    g_assert_cmphex(range->end, ==, end);
}

static void test_image_metadata_and_reservations(void)
{
    g_autofree char *owner = g_strdup("kernel");
    g_autofree char *name0 = g_strdup("text");
    g_autofree char *name1 = g_strdup("data");
    MMIXRAMReservationRequest requests[] = {
        fixed_request(owner, name0, MMIX_RAM_OWNERSHIP_IMAGE,
                      0x1000, 0x800),
        fixed_request(owner, name1, MMIX_RAM_OWNERSHIP_IMAGE,
                      0x2000, 0x800),
        framebuffer_request(),
        metadata_request(),
    };
    MMIXKernelLoadInfo image_info = {
        .entry = 0x1000,
        .image_type = MMIX_KERNEL_IMAGE_ELF,
        .boot_cpu_id = 3,
        .has_global_registers = true,
        .global_base = 254,
        .global_count = 1,
        .globals[254] = UINT64_C(0x1122334455667788),
    };
    const MMIXKernelLoadInfo *stored_info;
    const MMIXRAMReservationRequest *stored_request;
    const MMIXRAMReservation *text;
    const MMIXRAMReservation *data;
    const MMIXRAMReservation *framebuffer;
    const MMIXRAMReservation *metadata;
    MMIXBootPlan *plan = NULL;

    g_assert_true(mmix_boot_plan_build(TEST_RAM_SIZE, "kernel.elf",
                                       &image_info, NULL, requests,
                                       ARRAY_SIZE(requests), &plan,
                                       &error_abort));
    g_clear_pointer(&owner, g_free);
    g_clear_pointer(&name0, g_free);
    g_clear_pointer(&name1, g_free);

    g_assert_cmpstr(mmix_boot_plan_image_filename(plan), ==, "kernel.elf");
    stored_info = mmix_boot_plan_image_info(plan);
    g_assert_nonnull(stored_info);
    g_assert_cmphex(stored_info->entry, ==, 0x1000);
    g_assert_cmpuint(stored_info->image_type, ==, MMIX_KERNEL_IMAGE_ELF);
    g_assert_cmpuint(stored_info->boot_cpu_id, ==, 3);
    g_assert_cmphex(stored_info->globals[254], ==,
                    UINT64_C(0x1122334455667788));
    g_assert_cmpuint(mmix_boot_plan_request_count(plan), ==, 4);

    stored_request = mmix_boot_plan_request(plan, 0);
    g_assert_cmpstr(stored_request->owner, ==, "kernel");
    g_assert_cmpstr(stored_request->name, ==, "text");
    text = mmix_boot_plan_reservation(plan, 0);
    data = mmix_boot_plan_reservation(plan, 1);
    framebuffer = mmix_boot_plan_reservation(plan, 2);
    metadata = mmix_boot_plan_reservation(plan, 3);
    assert_range(&text->content, 0x1000, 0x1800);
    assert_range(&data->content, 0x2000, 0x2800);
    assert_range(&text->ownership, 0, 0x4000);
    assert_range(&data->ownership, 0, 0x4000);
    assert_range(&framebuffer->ownership,
                 TEST_RAM_SIZE - 3 * MiB, TEST_RAM_SIZE);
    assert_range(&metadata->content,
                 TEST_RAM_SIZE - 3 * MiB - 8 * KiB,
                 TEST_RAM_SIZE - 3 * MiB - 8 * KiB + 64);
    assert_range(&metadata->ownership,
                 TEST_RAM_SIZE - 3 * MiB - 8 * KiB,
                 TEST_RAM_SIZE - 3 * MiB);

    mmix_boot_plan_free(plan);
}

static void test_no_image_plan(void)
{
    MMIXRAMReservationRequest request = framebuffer_request();
    MMIXBootPlan *plan = NULL;

    g_assert_true(mmix_boot_plan_build(TEST_RAM_SIZE, NULL, NULL, NULL,
                                       &request, 1, &plan, &error_abort));
    g_assert_null(mmix_boot_plan_image_filename(plan));
    g_assert_null(mmix_boot_plan_image_info(plan));
    g_assert_null(mmix_boot_plan_linux_info(plan));
    g_assert_cmpuint(mmix_boot_plan_request_count(plan), ==, 1);
    mmix_boot_plan_free(plan);
}

static void assert_failed_rebuild_preserves_plan(
    MMIXBootPlan **plan, const MMIXRAMReservationRequest *requests,
    size_t request_count, const char *diagnostic)
{
    MMIXBootPlan *original = *plan;
    Error *err = NULL;

    g_assert_false(mmix_boot_plan_build(TEST_RAM_SIZE, "bad.elf",
                                        &(MMIXKernelLoadInfo) {
                                            .image_type =
                                                MMIX_KERNEL_IMAGE_ELF,
                                        },
                                        NULL, requests, request_count, plan,
                                        &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), diagnostic));
    g_assert_true(*plan == original);
    g_assert_cmpstr(mmix_boot_plan_image_filename(*plan), ==, "good.elf");
    error_free(err);
}

static void test_failure_is_atomic(void)
{
    MMIXRAMReservationRequest initial[] = {
        framebuffer_request(),
        stack_request(0, "initial-stack-0"),
    };
    MMIXKernelLoadInfo initial_info = {
        .image_type = MMIX_KERNEL_IMAGE_ELF,
    };
    MMIXRAMReservationRequest content_overlap[] = {
        fixed_request("kernel", "text", MMIX_RAM_OWNERSHIP_IMAGE,
                      0x1000, 0x1000),
        fixed_request("kernel", "data", MMIX_RAM_OWNERSHIP_IMAGE,
                      0x1800, 0x1000),
    };
    MMIXRAMReservationRequest page_collision[] = {
        fixed_request("kernel", "text", MMIX_RAM_OWNERSHIP_IMAGE,
                      0x1000, 0x800),
        fixed_request("machine", "state", MMIX_RAM_OWNERSHIP_MACHINE,
                      0x1800, 0x800),
    };
    MMIXRAMReservationRequest overflow =
        fixed_request("kernel", "overflow", MMIX_RAM_OWNERSHIP_IMAGE,
                      UINT64_MAX - 0x1000, 0x2000);
    MMIXRAMReservationRequest exhausted[] = {
        fixed_request("kernel", "whole-ram", MMIX_RAM_OWNERSHIP_IMAGE,
                      0, TEST_RAM_SIZE),
        framebuffer_request(),
    };
    MMIXBootPlan *plan = NULL;
    MMIXPhysRange initial_stack;

    g_assert_true(mmix_boot_plan_build(TEST_RAM_SIZE, "good.elf",
                                       &initial_info, NULL, initial,
                                       ARRAY_SIZE(initial), &plan,
                                       &error_abort));
    initial_stack = mmix_boot_plan_reservation(plan, 1)->content;
    assert_failed_rebuild_preserves_plan(&plan, content_overlap,
                                         ARRAY_SIZE(content_overlap),
                                         "overlaps another fixed content");
    assert_failed_rebuild_preserves_plan(&plan, page_collision,
                                         ARRAY_SIZE(page_collision),
                                         "another owner's reservation");
    assert_failed_rebuild_preserves_plan(&plan, &overflow, 1,
                                         "invalid fixed range");
    assert_failed_rebuild_preserves_plan(&plan, exhausted,
                                         ARRAY_SIZE(exhausted),
                                         "does not fit in free RAM");
    g_assert_cmpmem(&mmix_boot_plan_reservation(plan, 1)->content,
                    sizeof(initial_stack), &initial_stack,
                    sizeof(initial_stack));
    mmix_boot_plan_free(plan);
}

static void test_invalid_image_pair(void)
{
    MMIXKernelLoadInfo info = { 0 };
    MMIXBootPlan *plan = NULL;
    Error *err = NULL;

    g_assert_false(mmix_boot_plan_build(TEST_RAM_SIZE, "kernel.elf", NULL,
                                        NULL, NULL, 0, &plan, &err));
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "requires both image source"));
    error_free(err);
    err = NULL;
    g_assert_false(mmix_boot_plan_build(TEST_RAM_SIZE, NULL, &info, NULL,
                                        NULL, 0, &plan, &err));
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "requires both image source"));
    error_free(err);
    g_assert_null(plan);
}

static void test_linux_boot_information(void)
{
    g_autofree char *command_line = g_strdup("console=ttyS0");
    g_autofree char *initrd_filename = g_strdup("initrd.img");
    g_autoptr(GBytes) fdt = g_bytes_new_static("fdt", 3);
    MMIXRAMReservationRequest requests[] = {
        framebuffer_request(),
        stack_request(0, "initial-stack-0"),
        initrd_request(16 * MiB),
        fixed_request("mmix-kernel", "load-segment-0",
                      MMIX_RAM_OWNERSHIP_IMAGE, 4 * GiB, 8 * KiB),
        fdt_request(g_bytes_get_size(fdt)),
    };
    MMIXKernelLoadInfo image_info = {
        .entry = 4 * GiB,
        .image_type = MMIX_KERNEL_IMAGE_ELF,
    };
    MMIXLinuxBootInfo linux_info = {
        .command_line = command_line,
        .initrd_filename = initrd_filename,
        .initrd_size = 16 * MiB,
        .initrd_request_index = 2,
        .fdt = fdt,
        .fdt_request_index = 4,
        .cpu_count = 1,
        .has_initrd = true,
    };
    const MMIXLinuxBootInfo *stored;
    const MMIXRAMReservation *initrd;
    const MMIXRAMReservation *fdt_reservation;
    MMIXBootPlan *plan = NULL;

    g_assert_true(mmix_boot_plan_build(TEST_LARGE_RAM_SIZE, "kernel.elf",
                                       &image_info, &linux_info, requests,
                                       ARRAY_SIZE(requests), &plan,
                                       &error_abort));
    g_clear_pointer(&command_line, g_free);
    g_clear_pointer(&initrd_filename, g_free);

    stored = mmix_boot_plan_linux_info(plan);
    g_assert_nonnull(stored);
    g_assert_cmpstr(stored->command_line, ==, "console=ttyS0");
    g_assert_cmpstr(stored->initrd_filename, ==, "initrd.img");
    g_assert_cmpuint(stored->cpu_count, ==, 1);
    g_assert_cmpuint(stored->initrd_size, ==, 16 * MiB);
    initrd = mmix_boot_plan_reservation(plan, 2);
    fdt_reservation = mmix_boot_plan_reservation(plan, 4);
    g_assert_cmphex(stored->initrd_base, ==, initrd->content.start);
    g_assert_cmphex(stored->initrd_base, >, 4 * GiB);
    g_assert_cmphex(stored->fdt_base, ==, fdt_reservation->content.start);
    g_assert_cmphex(stored->fdt_base % 8, ==, 0);
    g_assert_cmphex(stored->fdt_base, <, stored->initrd_base);
    g_assert_cmphex(stored->fdt_base, ==,
                    TEST_LARGE_RAM_SIZE - 3 * MiB - 32 * KiB -
                    16 * MiB - 8 * KiB);
    g_assert_cmphex(stored->fdt_base, >, 4 * GiB);
    g_assert_cmphex(mmix_phys_range_size(&fdt_reservation->content), ==, 3);
    g_assert_cmphex(mmix_phys_range_size(&fdt_reservation->ownership), ==,
                    8 * KiB);
    g_assert_true(g_bytes_equal(stored->fdt, fdt));
    mmix_boot_plan_free(plan);
}

static void test_linux_fdt_failure_is_atomic(void)
{
    g_autoptr(GBytes) fdt = g_bytes_new_static("fdt", 3);
    MMIXRAMReservationRequest request = fdt_request(3);
    MMIXKernelLoadInfo image_info = {
        .image_type = MMIX_KERNEL_IMAGE_ELF,
    };
    MMIXLinuxBootInfo linux_info = {
        .command_line = "",
        .fdt = fdt,
        .fdt_request_index = 0,
        .cpu_count = 1,
    };
    MMIXBootPlan *plan = NULL;
    MMIXBootPlan *original;
    Error *err = NULL;

    g_assert_true(mmix_boot_plan_build(TEST_RAM_SIZE, "good.elf",
                                       &image_info, &linux_info, &request, 1,
                                       &plan, &error_abort));
    original = plan;
    request.size = 4;
    g_assert_false(mmix_boot_plan_build(TEST_RAM_SIZE, "bad.elf",
                                        &image_info, &linux_info, &request, 1,
                                        &plan, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "FDT reservation has the wrong size"));
    g_assert_true(plan == original);
    g_assert_true(g_bytes_equal(mmix_boot_plan_linux_info(plan)->fdt, fdt));
    error_free(err);
    mmix_boot_plan_free(plan);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/boot-plan/image-metadata",
                    test_image_metadata_and_reservations);
    g_test_add_func("/mmix/boot-plan/no-image", test_no_image_plan);
    g_test_add_func("/mmix/boot-plan/failure-atomic",
                    test_failure_is_atomic);
    g_test_add_func("/mmix/boot-plan/invalid-image-pair",
                    test_invalid_image_pair);
    g_test_add_func("/mmix/boot-plan/linux-information",
                    test_linux_boot_information);
    g_test_add_func("/mmix/boot-plan/linux-fdt-failure-atomic",
                    test_linux_fdt_failure_is_atomic);
    return g_test_run();
}
