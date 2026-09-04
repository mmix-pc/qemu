/*
 * MMIX virt physical address layout tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mmix/physical-layout.h"
#include "qemu/units.h"
#include "target/mmix/addressing.h"

static void test_layout_constants(void)
{
    static const MMIXPhysRange expected[MMIX_VIRT_PHYS_REGION_COUNT] = {
        [MMIX_VIRT_PHYS_RAM] = {
            UINT64_C(0x0000000000000000), UINT64_C(0x0001000000000000),
        },
        [MMIX_VIRT_PHYS_FIRMWARE] = {
            UINT64_C(0x0001000000000000), UINT64_C(0x0001000010000000),
        },
        [MMIX_VIRT_PHYS_FIXED_DEVICES] = {
            UINT64_C(0x0001000010000000), UINT64_C(0x0001000020000000),
        },
        [MMIX_VIRT_PHYS_PER_CPU] = {
            UINT64_C(0x0001000020000000), UINT64_C(0x0001000030000000),
        },
        [MMIX_VIRT_PHYS_INTERRUPT_CONTROLLER] = {
            UINT64_C(0x0001000030000000), UINT64_C(0x0001000040000000),
        },
        [MMIX_VIRT_PHYS_DYNAMIC_PLATFORM] = {
            UINT64_C(0x0001000040000000), UINT64_C(0x0001000080000000),
        },
        [MMIX_VIRT_PHYS_SYSTEM_MMIO] = {
            UINT64_C(0x0001000080000000), UINT64_C(0x0001000100000000),
        },
        [MMIX_VIRT_PHYS_PCIE_ECAM] = {
            UINT64_C(0x0001000100000000), UINT64_C(0x0001000110000000),
        },
        [MMIX_VIRT_PHYS_PCIE_32BIT] = {
            UINT64_C(0x0001000200000000), UINT64_C(0x0001000300000000),
        },
        [MMIX_VIRT_PHYS_PCIE_64BIT] = {
            UINT64_C(0x0001010000000000), UINT64_C(0x0001110000000000),
        },
    };
    const MMIXPhysRange *ram =
        &mmix_virt_phys_regions[MMIX_VIRT_PHYS_RAM];
    MMIXPhysRange maximum_ram;
    unsigned int i;
    unsigned int j;

    g_assert_cmphex(MMIX_VIRT_RAM_MIN_SIZE, ==, 128 * MiB);
    g_assert_cmphex(MMIX_VIRT_RAM_DEFAULT_SIZE, ==, 512 * MiB);
    g_assert_cmphex(MMIX_VIRT_RAM_MAX_SIZE, ==, 1 * TiB);
    g_assert_cmphex(MMIX_VIRT_RAM_ALIGN, ==, 8 * KiB);
    g_assert_cmphex(ram->start, ==, 0);
    g_assert_cmphex(ram->end, ==, MMIX_VIRT_RAM_PHYS_LIMIT);
    g_assert_cmphex(MMIX_VIRT_RAM_MAX_SIZE, <, ram->end);
    g_assert_true(mmix_phys_range_init(&maximum_ram, 0,
                                       MMIX_VIRT_RAM_MAX_SIZE));
    g_assert_true(mmix_phys_range_contains_addr(
        &maximum_ram, MMIX_VIRT_RAM_MAX_SIZE - 1));
    g_assert_false(mmix_phys_range_contains_addr(
        &maximum_ram, MMIX_VIRT_RAM_MAX_SIZE));

    for (i = 0; i < MMIX_VIRT_PHYS_REGION_COUNT; i++) {
        const MMIXPhysRange *range = &mmix_virt_phys_regions[i];

        g_assert_cmphex(range->start, ==, expected[i].start);
        g_assert_cmphex(range->end, ==, expected[i].end);
        g_assert_true(mmix_phys_range_valid(range));
        g_assert_cmphex(range->end, <=, MMIX_DIRECT_PHYS_LIMIT);
        for (j = 0; j < i; j++) {
            g_assert_false(mmix_phys_ranges_overlap(
                range, &mmix_virt_phys_regions[j]));
        }
    }
}

static void test_checked_arithmetic(void)
{
    uint64_t result = 0;

    g_assert_true(mmix_phys_add(UINT64_C(0x1000), UINT64_C(0x2000),
                                &result));
    g_assert_cmphex(result, ==, UINT64_C(0x3000));
    g_assert_false(mmix_phys_add(UINT64_MAX, 1, &result));
    g_assert_true(mmix_phys_sub(UINT64_C(0x3000), UINT64_C(0x2000),
                                &result));
    g_assert_cmphex(result, ==, UINT64_C(0x1000));
    g_assert_false(mmix_phys_sub(0, 1, &result));
}

static void test_alignment(void)
{
    uint64_t result = 0;

    g_assert_false(mmix_phys_align_down(1, 0, &result));
    g_assert_false(mmix_phys_align_up(1, 0, &result));
    g_assert_true(mmix_phys_align_down(UINT64_C(0x3fff),
                                      MMIX_VIRT_RAM_ALIGN, &result));
    g_assert_cmphex(result, ==, UINT64_C(0x2000));
    g_assert_true(mmix_phys_align_up(UINT64_C(0x2000),
                                    MMIX_VIRT_RAM_ALIGN, &result));
    g_assert_cmphex(result, ==, UINT64_C(0x2000));
    g_assert_true(mmix_phys_align_up(UINT64_C(0x2001),
                                    MMIX_VIRT_RAM_ALIGN, &result));
    g_assert_cmphex(result, ==, UINT64_C(0x4000));
    g_assert_true(mmix_phys_align_down(UINT64_MAX, 4, &result));
    g_assert_cmphex(result, ==, UINT64_MAX - 3);
    g_assert_false(mmix_phys_align_up(UINT64_MAX, 4, &result));
}

static void test_ranges(void)
{
    MMIXPhysRange outer;
    MMIXPhysRange inner;
    MMIXPhysRange adjacent;
    MMIXPhysRange overlapping;
    MMIXPhysRange invalid = { 0 };

    g_assert_false(mmix_phys_range_init(&invalid, 0, 0));
    g_assert_false(mmix_phys_range_init(&invalid, UINT64_MAX, 1));
    g_assert_true(mmix_phys_range_init(&outer, MMIX_VIRT_RAM_PHYS_LIMIT,
                                       UINT64_C(0x10000)));
    g_assert_true(mmix_phys_range_init(&inner,
                                       MMIX_VIRT_RAM_PHYS_LIMIT, 1));
    g_assert_true(mmix_phys_range_init(&adjacent, outer.end, 1));
    g_assert_true(mmix_phys_range_init(&overlapping, outer.end - 1, 2));

    g_assert_cmphex(mmix_phys_range_size(&outer), ==, UINT64_C(0x10000));
    g_assert_true(mmix_phys_range_contains(&outer, &inner));
    g_assert_true(mmix_phys_range_contains_addr(&outer, outer.start));
    g_assert_true(mmix_phys_range_contains_addr(&outer, outer.end - 1));
    g_assert_false(mmix_phys_range_contains_addr(&outer, outer.end));
    g_assert_false(mmix_phys_ranges_overlap(&outer, &adjacent));
    g_assert_true(mmix_phys_ranges_overlap(&outer, &overlapping));
    g_assert_false(mmix_phys_range_contains(&outer, &invalid));
}

static void test_negative_alias(void)
{
    uint64_t value = 0;

    g_assert_true(mmix_phys_to_negative_alias(0, &value));
    g_assert_cmphex(value, ==, MMIX_NEGATIVE_ALIAS_BIT);
    g_assert_true(mmix_phys_to_negative_alias(MMIX_VIRT_RAM_PHYS_LIMIT,
                                              &value));
    g_assert_cmphex(value, ==,
                    MMIX_NEGATIVE_ALIAS_BIT | MMIX_VIRT_RAM_PHYS_LIMIT);
    g_assert_true(mmix_phys_to_negative_alias(
        MMIX_DIRECT_PHYS_LIMIT - 1, &value));
    g_assert_cmphex(value, ==, UINT64_MAX);
    g_assert_false(mmix_phys_to_negative_alias(
        MMIX_DIRECT_PHYS_LIMIT, &value));
    g_assert_false(mmix_phys_to_negative_alias(UINT64_MAX, &value));

    g_assert_true(mmix_negative_alias_to_phys(UINT64_MAX, &value));
    g_assert_cmphex(value, ==, MMIX_DIRECT_PHYS_LIMIT - 1);
    g_assert_true(mmix_negative_alias_to_phys(
        MMIX_NEGATIVE_ALIAS_BIT, &value));
    g_assert_cmphex(value, ==, 0);
    g_assert_false(mmix_negative_alias_to_phys(
        MMIX_DIRECT_PHYS_LIMIT - 1, &value));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mmix/physical-layout/constants", test_layout_constants);
    g_test_add_func("/mmix/physical-layout/arithmetic",
                    test_checked_arithmetic);
    g_test_add_func("/mmix/physical-layout/alignment", test_alignment);
    g_test_add_func("/mmix/physical-layout/ranges", test_ranges);
    g_test_add_func("/mmix/physical-layout/negative-alias",
                    test_negative_alias);

    return g_test_run();
}
