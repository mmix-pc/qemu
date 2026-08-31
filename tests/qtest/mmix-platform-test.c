/*
 * QTest testcase for the MMIX virt platform boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "elf.h"
#include <glib/gstdio.h>
#include "hw/pci/pci.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_mmio.h"

#ifndef EM_MMIX
#define EM_MMIX 80
#endif

#define MMIX_RAM_DEFAULT_SIZE          (512 * MiB)

#define MMIX_UART_BASE                 UINT64_C(0x0001000010000000)
#define MMIX_UART_SIZE                 UINT64_C(0x8)
#define MMIX_UART_LSR                  0x5
#define MMIX_UART_LSR_THRE             0x20

#define MMIX_RTC_BASE                  UINT64_C(0x0001000010010000)
#define MMIX_RTC_SIZE                  UINT64_C(0x24)
#define MMIX_RTC_IRQ_ENABLED           0x10
#define MMIX_WATCHDOG_REFRESH_BASE     UINT64_C(0x0001000010020000)
#define MMIX_WATCHDOG_CONTROL_BASE     UINT64_C(0x0001000010030000)
#define MMIX_WATCHDOG_SIZE             UINT64_C(0x1000)
#define MMIX_WATCHDOG_ID               0x1043b
#define MMIX_WATCHDOG_IIDR             0xfcc
#define MMIX_POWER_BASE                UINT64_C(0x0001000010040000)
#define MMIX_POWER_SIZE                UINT64_C(0x100)
#define MMIX_POWER_FEATURES            0x00
#define MMIX_POWER_FEATURE_CONTROL     0x1
#define MMIX_FW_CFG_BASE               UINT64_C(0x0001000014000000)
#define MMIX_FW_CFG_SIZE               UINT64_C(0x18)

#define MMIX_FRAMEBUFFER_BASE          UINT64_C(0x0001000018000000)
#define MMIX_FRAMEBUFFER_SIZE          UINT64_C(0x1000)
#define MMIX_FRAMEBUFFER_WIDTH         0x00
#define MMIX_FRAMEBUFFER_WIDTH_VALUE   1024

#define MMIX_TIMER_BASE                UINT64_C(0x0001000020000000)
#define MMIX_TIMER_CONTEXT_BASE        UINT64_C(0x0001000020010000)
#define MMIX_TIMER_CONTEXT_CONTROL     0x08

#define MMIX_IPI_BASE                  UINT64_C(0x0001000024000000)
#define MMIX_IPI_ACTIVE_TARGETS        0x00
#define MMIX_IPI_CONTEXT_BASE          UINT64_C(0x0001000024010000)
#define MMIX_IPI_CONTEXT_STATUS        0x00

#define MMIX_INTC_BASE                 UINT64_C(0x0001000030000000)
#define MMIX_INTC_SOURCE_COUNT         0x00
#define MMIX_INTC_CONTEXT_COUNT        0x08
#define MMIX_INTC_CONTEXT_BASE         UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_ENABLE       0x00
#define MMIX_INTC_SOURCE_COUNT_VALUE   8192

#define MMIX_VIRTIO_BASE               UINT64_C(0x0001000040000000)
#define MMIX_DISCOVERABLE_BASE         UINT64_C(0x0001000050000000)
#define MMIX_PCIE_ECAM_BASE             UINT64_C(0x0001000100000000)

#define MMIX_CONTEXT_STRIDE            UINT64_C(0x10000)
#define MMIX_CONTEXT_REGISTER_SIZE     UINT64_C(0x1000)
#define MMIX_INITIAL_CONTEXT_COUNT     64
#define MMIX_CONTEXT_CAPACITY          1023
#define MMIX_VIRTIO_STRIDE             UINT64_C(0x10000)
#define MMIX_VIRTIO_REGISTER_SIZE      UINT64_C(0x200)
#define MMIX_VIRTIO_ACTIVE_SLOTS       32
#define MMIX_VIRTIO_SLOT_CAPACITY      4096

typedef struct MMIXRAMSizeCase {
    const char *memory;
    uint64_t size;
    unsigned int cpus;
} MMIXRAMSizeCase;

typedef struct MMIXPlatformRange {
    uint64_t base;
    uint64_t size;
} MMIXPlatformRange;

typedef enum MMIXPlatformBootMode {
    MMIX_PLATFORM_NO_IMAGE,
    MMIX_PLATFORM_RAW,
    MMIX_PLATFORM_BARE_ELF,
    MMIX_PLATFORM_HOSTED_ELF,
    MMIX_PLATFORM_MMO,
    MMIX_PLATFORM_LINUX,
    MMIX_PLATFORM_FIRMWARE,
} MMIXPlatformBootMode;

typedef struct MMIXPlatformBootCase {
    MMIXPlatformBootMode mode;
    const char *memory;
    unsigned int cpus;
} MMIXPlatformBootCase;

static uint64_t mmix_context_address(uint64_t base, unsigned int context)
{
    return base + context * MMIX_CONTEXT_STRIDE;
}

static uint64_t mmix_virtio_address(unsigned int slot)
{
    return MMIX_VIRTIO_BASE + slot * MMIX_VIRTIO_STRIDE;
}

static void mmix_assert_mapping(const char *mtree, uint64_t base,
                                uint64_t size, const char *name)
{
    g_autofree char *mapping =
        g_strdup_printf("%016" PRIx64 "-%016" PRIx64
                        " (prio 0, i/o): %s",
                        base, base + size - 1, name);

    g_assert_nonnull(strstr(mtree, mapping));
}

static void mmix_assert_unassigned(QTestState *qts, uint64_t address)
{
    const uint64_t probe = UINT64_C(0x5aa55aa50ff0f00f);

    qtest_writeq(qts, address, probe);
    g_assert_cmphex(qtest_readq(qts, address), !=, probe);
}

static uint32_t mmix_readl_le(QTestState *qts, uint64_t address)
{
    uint8_t bytes[sizeof(uint32_t)];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static void mmix_assert_active_devices(QTestState *qts, unsigned int cpus)
{
    uint64_t active_targets = cpus == 64 ? UINT64_MAX :
                              (UINT64_C(1) << cpus) - 1;

    g_assert_cmphex(qtest_readb(qts, MMIX_UART_BASE + MMIX_UART_LSR) &
                    MMIX_UART_LSR_THRE, ==, MMIX_UART_LSR_THRE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_FRAMEBUFFER_BASE +
                                 MMIX_FRAMEBUFFER_WIDTH), ==,
                     MMIX_FRAMEBUFFER_WIDTH_VALUE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_IPI_BASE +
                                 MMIX_IPI_ACTIVE_TARGETS), ==,
                     active_targets);
    g_assert_cmpuint(qtest_readq(qts, MMIX_INTC_BASE +
                                 MMIX_INTC_SOURCE_COUNT), ==,
                     MMIX_INTC_SOURCE_COUNT_VALUE);
    g_assert_cmpuint(qtest_readq(qts, MMIX_INTC_BASE +
                                 MMIX_INTC_CONTEXT_COUNT), ==, cpus);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_RTC_BASE +
                                      MMIX_RTC_IRQ_ENABLED), ==, 0);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_REFRESH_BASE +
                                      MMIX_WATCHDOG_IIDR), ==,
                    MMIX_WATCHDOG_ID);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                      MMIX_WATCHDOG_IIDR), ==,
                    MMIX_WATCHDOG_ID);
    g_assert_cmphex(qtest_readl(qts, MMIX_POWER_BASE +
                                    MMIX_POWER_FEATURES), ==,
                    MMIX_POWER_FEATURE_CONTROL);
    g_assert_cmphex(mmix_readl_le(qts, MMIX_PCIE_ECAM_BASE), ==,
                    PCI_DEVICE_ID_REDHAT_PCIE_HOST << 16 |
                    PCI_VENDOR_ID_REDHAT);
}

static void test_mmix_platform_mappings(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    unsigned int i;

    mmix_assert_mapping(mtree, MMIX_UART_BASE, MMIX_UART_SIZE, "serial");
    mmix_assert_mapping(mtree, MMIX_RTC_BASE, MMIX_RTC_SIZE,
                        "goldfish_rtc");
    mmix_assert_mapping(mtree, MMIX_WATCHDOG_REFRESH_BASE,
                        MMIX_WATCHDOG_SIZE, "sbsa_gwdt.refresh");
    mmix_assert_mapping(mtree, MMIX_WATCHDOG_CONTROL_BASE,
                        MMIX_WATCHDOG_SIZE, "sbsa_gwdt.control");
    mmix_assert_mapping(mtree, MMIX_POWER_BASE, MMIX_POWER_SIZE,
                        "virt-ctrl");
    mmix_assert_mapping(mtree, MMIX_FW_CFG_BASE, 0x8,
                        "fwcfg.data");
    mmix_assert_mapping(mtree, MMIX_FW_CFG_BASE + 0x8, 0x2,
                        "fwcfg.ctl");
    mmix_assert_mapping(mtree, MMIX_FW_CFG_BASE + 0x10, 0x8,
                        "fwcfg.dma");
    mmix_assert_mapping(mtree, MMIX_FRAMEBUFFER_BASE,
                        MMIX_FRAMEBUFFER_SIZE, "mmix-framebuffer");
    mmix_assert_mapping(mtree, MMIX_TIMER_BASE,
                        MMIX_CONTEXT_REGISTER_SIZE, "mmix-timer-global");
    mmix_assert_mapping(mtree, MMIX_IPI_BASE,
                        MMIX_CONTEXT_REGISTER_SIZE, "mmix-ipi-global");
    mmix_assert_mapping(mtree, MMIX_INTC_BASE, MMIX_CONTEXT_STRIDE,
                        "mmix-intc-global");

    for (i = 0; i < MMIX_INITIAL_CONTEXT_COUNT; i++) {
        g_autofree char *timer =
            g_strdup_printf("mmix-timer-context[%u]", i);
        g_autofree char *ipi = g_strdup_printf("mmix-ipi-context[%u]", i);
        g_autofree char *intc =
            g_strdup_printf("mmix-intc-context[%u]", i);

        mmix_assert_mapping(mtree,
                            mmix_context_address(MMIX_TIMER_CONTEXT_BASE, i),
                            MMIX_CONTEXT_REGISTER_SIZE, timer);
        mmix_assert_mapping(mtree,
                            mmix_context_address(MMIX_IPI_CONTEXT_BASE, i),
                            MMIX_CONTEXT_REGISTER_SIZE, ipi);
        mmix_assert_mapping(mtree,
                            mmix_context_address(MMIX_INTC_CONTEXT_BASE, i),
                            MMIX_CONTEXT_REGISTER_SIZE, intc);
    }
    for (i = 0; i < MMIX_VIRTIO_ACTIVE_SLOTS; i++) {
        mmix_assert_mapping(mtree, mmix_virtio_address(i),
                            MMIX_VIRTIO_REGISTER_SIZE, "virtio-mmio");
    }

    qtest_quit(qts);
}

static void test_mmix_platform_slot_boundaries(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint64_t address;

    mmix_assert_active_devices(qts, 1);

    mmix_assert_unassigned(qts, MMIX_UART_BASE + MMIX_UART_SIZE);
    mmix_assert_unassigned(qts, MMIX_RTC_BASE + MMIX_RTC_SIZE);
    mmix_assert_unassigned(qts, MMIX_RTC_BASE + MMIX_CONTEXT_STRIDE - 8);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_REFRESH_BASE +
                                MMIX_WATCHDOG_SIZE);
    mmix_assert_unassigned(qts, MMIX_WATCHDOG_CONTROL_BASE +
                                MMIX_WATCHDOG_SIZE);
    mmix_assert_unassigned(qts, MMIX_POWER_BASE + MMIX_POWER_SIZE);
    mmix_assert_unassigned(qts, MMIX_FW_CFG_BASE + MMIX_FW_CFG_SIZE);
    mmix_assert_unassigned(qts, MMIX_FRAMEBUFFER_BASE +
                                MMIX_FRAMEBUFFER_SIZE);
    mmix_assert_unassigned(qts, MMIX_TIMER_BASE +
                                MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_IPI_BASE +
                                MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(qts, MMIX_INTC_BASE + MMIX_CONTEXT_STRIDE);

    address = mmix_context_address(MMIX_TIMER_CONTEXT_BASE, 1);
    qtest_writeq(qts, address + MMIX_TIMER_CONTEXT_CONTROL, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, address +
                                MMIX_TIMER_CONTEXT_CONTROL), ==, 0);
    address = mmix_context_address(MMIX_IPI_CONTEXT_BASE, 1);
    g_assert_cmphex(qtest_readq(qts, address + MMIX_IPI_CONTEXT_STATUS),
                    ==, 0);
    address = mmix_context_address(MMIX_INTC_CONTEXT_BASE, 1);
    qtest_writeq(qts, address + MMIX_INTC_CONTEXT_ENABLE, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, address +
                                MMIX_INTC_CONTEXT_ENABLE), ==, 0);

    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE, 0) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT - 1) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY - 1));
    g_assert_cmpuint(qtest_readq(
                         qts, mmix_context_address(MMIX_TIMER_CONTEXT_BASE,
                                                   MMIX_CONTEXT_CAPACITY)),
                     ==, 1);

    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE, 0) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT - 1) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY - 1));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_IPI_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY));

    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE, 0) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT - 1) +
             MMIX_CONTEXT_REGISTER_SIZE);
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_INITIAL_CONTEXT_COUNT));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY - 1));
    mmix_assert_unassigned(
        qts, mmix_context_address(MMIX_INTC_CONTEXT_BASE,
                                  MMIX_CONTEXT_CAPACITY));

    g_assert_cmphex(qtest_readl(qts, mmix_virtio_address(0) +
                                VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    g_assert_cmphex(qtest_readl(
                        qts, mmix_virtio_address(
                                 MMIX_VIRTIO_ACTIVE_SLOTS - 1) +
                             VIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    mmix_assert_unassigned(qts, mmix_virtio_address(0) +
                                MMIX_VIRTIO_REGISTER_SIZE);
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_ACTIVE_SLOTS - 1) +
                           MMIX_VIRTIO_REGISTER_SIZE);
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_ACTIVE_SLOTS));
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_SLOT_CAPACITY - 1));
    mmix_assert_unassigned(qts,
                           mmix_virtio_address(MMIX_VIRTIO_SLOT_CAPACITY));

    qtest_quit(qts);
}

static void test_mmix_platform_ram_size_independence(gconstpointer opaque)
{
    const MMIXRAMSizeCase *test = opaque;
    g_autofree char *args = test->memory ?
        g_strdup_printf("-machine virt -m %s -smp %u", test->memory,
                        test->cpus) :
        g_strdup_printf("-machine virt -smp %u", test->cpus);
    QTestState *qts = qtest_init(args);
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *ram =
        g_strdup_printf("%016x-%016" PRIx64 " (prio 0, ram): mmix.ram",
                        0, test->size - 1);

    g_assert_nonnull(strstr(mtree, ram));
    mmix_assert_active_devices(qts, test->cpus);
    qtest_quit(qts);
}

static void test_mmix_platform_low_address_isolation(void)
{
    static const uint64_t low_addresses[] = {
        0x10000000,
        0x10001000,
        0x10002000,
        0x10003000,
        0x10004000,
        0x10006000,
        0x18000000,
        0x20000000,
        0x24000000,
        0x30000000,
        0x40000000,
    };
    QTestState *qts = qtest_init("-machine virt -m 8G");
    size_t i;

    for (i = 0; i < ARRAY_SIZE(low_addresses); i++) {
        uint64_t value = UINT64_C(0x1020304050607000) + i;

        qtest_writeq(qts, low_addresses[i], value);
        g_assert_cmphex(qtest_readq(qts, low_addresses[i]), ==, value);
    }
    mmix_assert_active_devices(qts, 1);

    qtest_quit(qts);
}

static void test_mmix_platform_deferred_apertures(void)
{
    static const uint64_t addresses[] = {
        UINT64_C(0x0001000000000000),
        UINT64_C(0x0001000010050000),
        UINT64_C(0x0001000014010000),
        UINT64_C(0x0001000018010000),
        MMIX_DISCOVERABLE_BASE,
        UINT64_C(0x0001000080000000),
        UINT64_C(0x0001000110000000),
        UINT64_C(0x0001000300000000),
        UINT64_C(0x0001110000000000),
    };
    QTestState *qts = qtest_init("-machine virt");
    size_t i;

    for (i = 0; i < ARRAY_SIZE(addresses); i++) {
        mmix_assert_unassigned(qts, addresses[i]);
    }

    qtest_quit(qts);
}

static void test_mmix_platform_nonoverlapping_extents(void)
{
    static const MMIXPlatformRange ranges[] = {
        { MMIX_UART_BASE, MMIX_UART_SIZE },
        { MMIX_RTC_BASE, MMIX_RTC_SIZE },
        { MMIX_WATCHDOG_REFRESH_BASE, MMIX_WATCHDOG_SIZE },
        { MMIX_WATCHDOG_CONTROL_BASE, MMIX_WATCHDOG_SIZE },
        { MMIX_POWER_BASE, MMIX_POWER_SIZE },
        { MMIX_FW_CFG_BASE, MMIX_FW_CFG_SIZE },
        { MMIX_FRAMEBUFFER_BASE, MMIX_FRAMEBUFFER_SIZE },
        { MMIX_TIMER_BASE, MMIX_CONTEXT_REGISTER_SIZE },
        { MMIX_IPI_BASE, MMIX_CONTEXT_REGISTER_SIZE },
        { MMIX_INTC_BASE, MMIX_CONTEXT_STRIDE },
        { MMIX_VIRTIO_BASE,
          MMIX_VIRTIO_ACTIVE_SLOTS * MMIX_VIRTIO_STRIDE },
        { UINT64_C(0x0001000100000000), UINT64_C(0x10000000) },
        { UINT64_C(0x0001000200000000), UINT64_C(0x100000000) },
        { UINT64_C(0x0001010000000000), UINT64_C(0x100000000000) },
    };
    unsigned int i;
    unsigned int j;

    for (i = 0; i < ARRAY_SIZE(ranges); i++) {
        for (j = i + 1; j < ARRAY_SIZE(ranges); j++) {
            g_assert_true(ranges[i].base + ranges[i].size <= ranges[j].base ||
                          ranges[j].base + ranges[j].size <= ranges[i].base);
        }
    }
}

static char *mmix_write_platform_image(const char *directory,
                                       const char *name,
                                       const uint8_t *contents, size_t size)
{
    g_autoptr(GError) error = NULL;
    char *filename = g_build_filename(directory, name, NULL);

    g_assert_true(g_file_set_contents(filename, (const char *)contents,
                                      size, &error));
    g_assert_no_error(error);
    return filename;
}

static char *mmix_create_platform_elf(const char *directory)
{
    enum { CODE_OFFSET = 0x100 };
    const uint64_t entry = 0x10000;
    const uint8_t code[] = { 0xfd, 0x00, 0x00, 0x00 };
    uint8_t image[CODE_OFFSET + sizeof(code)] = { 0 };
    Elf64_Ehdr ehdr = { 0 };
    Elf64_Phdr phdr = { 0 };

    memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = cpu_to_be16(ET_EXEC);
    ehdr.e_machine = cpu_to_be16(EM_MMIX);
    ehdr.e_version = cpu_to_be32(EV_CURRENT);
    ehdr.e_entry = cpu_to_be64(entry);
    ehdr.e_phoff = cpu_to_be64(sizeof(ehdr));
    ehdr.e_ehsize = cpu_to_be16(sizeof(ehdr));
    ehdr.e_phentsize = cpu_to_be16(sizeof(phdr));
    ehdr.e_phnum = cpu_to_be16(1);

    phdr.p_type = cpu_to_be32(PT_LOAD);
    phdr.p_flags = cpu_to_be32(PF_R | PF_X);
    phdr.p_offset = cpu_to_be64(CODE_OFFSET);
    phdr.p_vaddr = cpu_to_be64(entry);
    phdr.p_paddr = cpu_to_be64(entry);
    phdr.p_filesz = cpu_to_be64(sizeof(code));
    phdr.p_memsz = cpu_to_be64(sizeof(code));
    phdr.p_align = cpu_to_be64(1);

    memcpy(image, &ehdr, sizeof(ehdr));
    memcpy(image + sizeof(ehdr), &phdr, sizeof(phdr));
    memcpy(image + CODE_OFFSET, code, sizeof(code));
    return mmix_write_platform_image(directory, "kernel.elf", image,
                                     sizeof(image));
}

static char *mmix_create_platform_image(const char *directory,
                                        MMIXPlatformBootMode mode)
{
    static const uint8_t mmo[] = {
        0x98, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x98, 0x0a, 0x00, 0xff,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x98, 0x0b, 0x00, 0x00,
        0x98, 0x0c, 0x00, 0x00,
    };
    uint8_t raw[0x104] = { 0 };
    const uint8_t firmware[] = { 0xfd, 0x00, 0x00, 0x00 };

    switch (mode) {
    case MMIX_PLATFORM_RAW:
        raw[0x100] = 0xfd;
        return mmix_write_platform_image(directory, "kernel.raw", raw,
                                         sizeof(raw));
    case MMIX_PLATFORM_BARE_ELF:
    case MMIX_PLATFORM_HOSTED_ELF:
    case MMIX_PLATFORM_LINUX:
        return mmix_create_platform_elf(directory);
    case MMIX_PLATFORM_MMO:
        return mmix_write_platform_image(directory, "kernel.mmo", mmo,
                                         sizeof(mmo));
    case MMIX_PLATFORM_FIRMWARE:
        return mmix_write_platform_image(directory, "firmware.bin", firmware,
                                         sizeof(firmware));
    case MMIX_PLATFORM_NO_IMAGE:
        return NULL;
    }
    g_assert_not_reached();
}

static void test_mmix_platform_boot_mode(gconstpointer opaque)
{
    const MMIXPlatformBootCase *test = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *image = NULL;
    g_autoptr(GString) args = g_string_new(NULL);
    const char *machine = test->mode == MMIX_PLATFORM_HOSTED_ELF ?
                          "virt,elf-startup-abi=argc-argv" :
                          test->mode == MMIX_PLATFORM_LINUX ?
                          "virt,elf-startup-abi=linux" : "virt";
    QTestState *qts;

    g_string_append_printf(args, "-machine %s -m %s -smp %u",
                           machine, test->memory, test->cpus);
    if (test->mode != MMIX_PLATFORM_NO_IMAGE) {
        directory = g_dir_make_tmp("mmix-platform-mode-XXXXXX", &error);
        g_assert_no_error(error);
        g_assert_nonnull(directory);
        image = mmix_create_platform_image(directory, test->mode);
    }

    switch (test->mode) {
    case MMIX_PLATFORM_NO_IMAGE:
        break;
    case MMIX_PLATFORM_RAW:
    case MMIX_PLATFORM_BARE_ELF:
    case MMIX_PLATFORM_MMO:
        g_string_append_printf(args, " -kernel %s", image);
        break;
    case MMIX_PLATFORM_HOSTED_ELF:
        g_string_append_printf(args, " -semihosting -kernel %s", image);
        break;
    case MMIX_PLATFORM_LINUX:
        g_string_append_printf(args, " -kernel %s", image);
        break;
    case MMIX_PLATFORM_FIRMWARE:
        g_string_append_printf(args, " -bios %s", image);
        break;
    }

    qts = qtest_init(args->str);
    mmix_assert_active_devices(qts, test->cpus);
    qtest_quit(qts);

    if (image) {
        g_assert_cmpint(g_unlink(image), ==, 0);
        g_assert_cmpint(g_rmdir(directory), ==, 0);
    }
}

int main(int argc, char **argv)
{
    static const MMIXRAMSizeCase ram_sizes[] = {
        { "128M", 128 * MiB, 1 },
        { NULL, MMIX_RAM_DEFAULT_SIZE, 1 },
        { "8G", 8 * GiB, 64 },
    };
    static const MMIXPlatformBootCase boot_modes[] = {
        { MMIX_PLATFORM_NO_IMAGE, "512M", 1 },
        { MMIX_PLATFORM_RAW, "128M", 1 },
        { MMIX_PLATFORM_BARE_ELF, "512M", 1 },
        { MMIX_PLATFORM_HOSTED_ELF, "512M", 1 },
        { MMIX_PLATFORM_MMO, "128M", 1 },
        { MMIX_PLATFORM_LINUX, "8G", 64 },
        { MMIX_PLATFORM_FIRMWARE, "512M", 2 },
    };
    static const char * const boot_mode_names[] = {
        "no-image", "raw", "bare-elf", "hosted-elf", "mmo", "linux",
        "firmware",
    };
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/platform/mappings",
                   test_mmix_platform_mappings);
    qtest_add_func("/mmix/platform/slot-boundaries",
                   test_mmix_platform_slot_boundaries);
    qtest_add_data_func("/mmix/platform/ram-size/minimum", &ram_sizes[0],
                        test_mmix_platform_ram_size_independence);
    qtest_add_data_func("/mmix/platform/ram-size/default", &ram_sizes[1],
                        test_mmix_platform_ram_size_independence);
    qtest_add_data_func("/mmix/platform/ram-size/above-4g", &ram_sizes[2],
                        test_mmix_platform_ram_size_independence);
    qtest_add_func("/mmix/platform/low-address-isolation",
                   test_mmix_platform_low_address_isolation);
    qtest_add_func("/mmix/platform/deferred-apertures",
                   test_mmix_platform_deferred_apertures);
    qtest_add_func("/mmix/platform/nonoverlapping-extents",
                   test_mmix_platform_nonoverlapping_extents);
    for (i = 0; i < ARRAY_SIZE(boot_modes); i++) {
        g_autofree char *name = g_strdup_printf(
            "/mmix/platform/boot-mode/%s", boot_mode_names[i]);

        qtest_add_data_func(name, &boot_modes[i],
                            test_mmix_platform_boot_mode);
    }

    return g_test_run();
}
