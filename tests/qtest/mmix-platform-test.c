/*
 * QTest testcase for the MMIX virt platform contract.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "elf.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "standard-headers/linux/virtio_mmio.h"

#ifndef EM_MMIX
#define EM_MMIX 80
#endif

typedef struct MMIXPlatformRegion {
    const char *name;
    uint64_t base;
    uint64_t size;
} MMIXPlatformRegion;

typedef enum MMIXPlatformRegionIndex {
    MMIX_REGION_LOW_RAM,
    MMIX_REGION_POOL,
    MMIX_REGION_DATA,
    MMIX_REGION_STACK,
    MMIX_REGION_PLATFORM_RAM,
    MMIX_REGION_FRAMEBUFFER,
    MMIX_REGION_COUNT,
} MMIXPlatformRegionIndex;

static const MMIXPlatformRegion mmix_regions[MMIX_REGION_COUNT] = {
    [MMIX_REGION_LOW_RAM] = { "low RAM", 0x00000000, 0x06000000 },
    [MMIX_REGION_POOL] = { "Pool backing", 0x06000000, 0x00800000 },
    [MMIX_REGION_DATA] = { "Data backing", 0x06800000, 0x04000000 },
    [MMIX_REGION_STACK] = { "Stack backing", 0x0a800000, 0x04000000 },
    [MMIX_REGION_PLATFORM_RAM] = {
        "platform RAM", 0x0e800000, 0x00800000
    },
    [MMIX_REGION_FRAMEBUFFER] = {
        "framebuffer RAM", 0x0f000000, 0x01000000
    },
};

typedef struct MMIXPlatformDevice {
    const char *name;
    uint64_t base;
    uint64_t size;
    unsigned irq;
} MMIXPlatformDevice;

typedef enum MMIXPlatformDeviceIndex {
    MMIX_DEVICE_UART0,
    MMIX_DEVICE_VIRTIO_BLOCK0,
    MMIX_DEVICE_FRAMEBUFFER,
    MMIX_DEVICE_TIMER0,
    MMIX_DEVICE_INTC,
    MMIX_DEVICE_IPI,
    MMIX_DEVICE_COUNT,
} MMIXPlatformDeviceIndex;

static const MMIXPlatformDevice mmix_devices[MMIX_DEVICE_COUNT] = {
    [MMIX_DEVICE_UART0] = { "UART0", 0x10000000, 0x100, 1 },
    [MMIX_DEVICE_VIRTIO_BLOCK0] = {
        "virtio block 0", 0x10001000, 0x1000, 2
    },
    [MMIX_DEVICE_FRAMEBUFFER] = {
        "framebuffer", 0x10002000, 0x1000, 3
    },
    [MMIX_DEVICE_TIMER0] = { "CPU0 timer", 0x10003000, 0x1000, 16 },
    [MMIX_DEVICE_INTC] = {
        "interrupt controller", 0x10004000, 0x2000, 0
    },
    [MMIX_DEVICE_IPI] = {
        "inter-processor interrupt", 0x10006000, 0x1000, 0
    },
};

static const uint64_t mmix_virt_mmio_base = 0x10000000ULL;
static const uint64_t mmix_virt_mmio_size = 0x10000000ULL;
static const uint64_t mmix_bootinfo_base = 0x0e800000ULL;
static const uint64_t mmix_bootinfo_magic = 0x4d4d4958424f4f54ULL;

enum {
    MMIX_BOOTINFO_VERSION = 1,
    MMIX_BOOTINFO_FLAG_KERNEL_CMDLINE = 1,
    MMIX_BOOTINFO_KERNEL_CMDLINE_MAX = 4095,
    MMIX_UART_LSR = 0x05,
    MMIX_UART_LSR_THRE = 0x20,
    MMIX_FRAMEBUFFER_REG_WIDTH = 0x00,
    MMIX_FRAMEBUFFER_REG_BASE = 0x20,
    MMIX_FRAMEBUFFER_REG_SIZE = 0x28,
    MMIX_FRAMEBUFFER_REG_FLUSH = 0x30,
    MMIX_TIMER_TIME = 0x0000,
    MMIX_TIMER_CONTEXT_BASE = 0x0100,
    MMIX_TIMER_CONTEXT_STRIDE = 0x40,
    MMIX_TIMER_CONTEXT_COMPARE = 0x00,
    MMIX_TIMER_CONTEXT_CONTROL = 0x08,
    MMIX_TIMER_CONTEXT_STATUS = 0x10,
    MMIX_TIMER_CONTROL_ENABLE = 0x01,
    MMIX_TIMER_CONTROL_IRQ_ENABLE = 0x02,
    MMIX_TIMER_STATUS_PENDING = 0x01,
    MMIX_INTC_PENDING = 0x0000,
    MMIX_INTC_CONTEXT_BASE = 0x1000,
    MMIX_INTC_CONTEXT_STRIDE = 0x100,
    MMIX_INTC_CONTEXT_ENABLE = 0x00,
    MMIX_IPI_ACTIVE_TARGETS = 0x0000,
    MMIX_IPI_SEND = 0x0008,
    MMIX_IPI_CONTEXT_BASE = 0x0100,
    MMIX_IPI_CONTEXT_STRIDE = 0x20,
    MMIX_IPI_CONTEXT_STATUS = 0x00,
    MMIX_IPI_CONTEXT_CLEAR = 0x08,
    MMIX_IPI_STATUS_PENDING = 0x01,
    MMIX_TIMER_IRQ_BASE = 16,
    MMIX_INITIAL_STACK_SLOT_COUNT = 16,
};

static const uint64_t mmix_initial_stack_base = 0x00010000;
static const uint64_t mmix_initial_stack_slot_size = 0x00008000;
static const uint64_t mmix_interrupt_controller_request = 1ULL << 8;
static const uint64_t mmix_ipi_request = 1ULL << 9;
static const char *mmix_intc_qom_path = "/machine/intc";
static const char *mmix_ipi_qom_path = "/machine/ipi";
static const char *mmix_timer_qom_path = "/machine/timer";

typedef enum MMIXBootInfoOffset {
    MMIX_BOOTINFO_MAGIC_OFFSET = 0x000,
    MMIX_BOOTINFO_VERSION_OFFSET = 0x008,
    MMIX_BOOTINFO_SIZE_OFFSET = 0x010,
    MMIX_BOOTINFO_FLAGS_OFFSET = 0x018,
    MMIX_BOOTINFO_CPU_COUNT_OFFSET = 0x020,
    MMIX_BOOTINFO_BOOT_CPU_ID_OFFSET = 0x028,
    MMIX_BOOTINFO_RAM_BASE_OFFSET = 0x030,
    MMIX_BOOTINFO_RAM_SIZE_OFFSET = 0x038,
    MMIX_BOOTINFO_LOW_RAM_BASE_OFFSET = 0x040,
    MMIX_BOOTINFO_LOW_RAM_SIZE_OFFSET = 0x048,
    MMIX_BOOTINFO_POOL_LOGICAL_BASE_OFFSET = 0x050,
    MMIX_BOOTINFO_POOL_PHYS_BASE_OFFSET = 0x058,
    MMIX_BOOTINFO_POOL_SIZE_OFFSET = 0x060,
    MMIX_BOOTINFO_DATA_LOGICAL_BASE_OFFSET = 0x068,
    MMIX_BOOTINFO_DATA_PHYS_BASE_OFFSET = 0x070,
    MMIX_BOOTINFO_DATA_SIZE_OFFSET = 0x078,
    MMIX_BOOTINFO_STACK_LOGICAL_BASE_OFFSET = 0x080,
    MMIX_BOOTINFO_STACK_PHYS_BASE_OFFSET = 0x088,
    MMIX_BOOTINFO_STACK_SIZE_OFFSET = 0x090,
    MMIX_BOOTINFO_MMIO_BASE_OFFSET = 0x098,
    MMIX_BOOTINFO_UART_BASE_OFFSET = 0x0a0,
    MMIX_BOOTINFO_UART_IRQ_OFFSET = 0x0a8,
    MMIX_BOOTINFO_TIMER_BASE_OFFSET = 0x0b0,
    MMIX_BOOTINFO_TIMER_IRQ_BASE_OFFSET = 0x0b8,
    MMIX_BOOTINFO_TIMER_IRQ_COUNT_OFFSET = 0x0c0,
    MMIX_BOOTINFO_INTC_BASE_OFFSET = 0x0c8,
    MMIX_BOOTINFO_INTC_IRQ_COUNT_OFFSET = 0x0d0,
    MMIX_BOOTINFO_VIRTIO_MMIO_BASE_OFFSET = 0x0d8,
    MMIX_BOOTINFO_VIRTIO_MMIO_IRQ_OFFSET = 0x0e0,
    MMIX_BOOTINFO_VIRTIO_MMIO_COUNT_OFFSET = 0x0e8,
    MMIX_BOOTINFO_FRAMEBUFFER_CONTROL_BASE_OFFSET = 0x0f0,
    MMIX_BOOTINFO_FRAMEBUFFER_BASE_OFFSET = 0x0f8,
    MMIX_BOOTINFO_FRAMEBUFFER_SIZE_OFFSET = 0x100,
    MMIX_BOOTINFO_FRAMEBUFFER_IRQ_OFFSET = 0x108,
    MMIX_BOOTINFO_FRAMEBUFFER_WIDTH_OFFSET = 0x110,
    MMIX_BOOTINFO_FRAMEBUFFER_HEIGHT_OFFSET = 0x118,
    MMIX_BOOTINFO_FRAMEBUFFER_STRIDE_OFFSET = 0x120,
    MMIX_BOOTINFO_FRAMEBUFFER_FORMAT_OFFSET = 0x128,
    MMIX_BOOTINFO_KERNEL_CMDLINE_ADDR_OFFSET = 0x130,
    MMIX_BOOTINFO_KERNEL_CMDLINE_SIZE_OFFSET = 0x138,
    MMIX_BOOTINFO_COMPAT_PREFIX_SIZE = 0x140,
    MMIX_BOOTINFO_IPI_BASE_OFFSET = 0x140,
    MMIX_BOOTINFO_IPI_TARGET_COUNT_OFFSET = 0x148,
    MMIX_BOOTINFO_IPI_REQUEST_MASK_OFFSET = 0x150,
    MMIX_BOOTINFO_SIZE = 0x158,
} MMIXBootInfoOffset;

static const uint64_t mmix_kernel_cmdline_base =
    0x0e800000ULL + MMIX_BOOTINFO_SIZE;

static uint64_t mmix_bootinfo_read(QTestState *qts,
                                   MMIXBootInfoOffset offset)
{
    return qtest_readq(qts, mmix_bootinfo_base + offset);
}

static QTestState *mmix_start_elf_smp(const char *elf, const char *cmdline,
                                     unsigned int cpu_count)
{
    g_autofree char *quoted_elf = g_shell_quote(elf);

    if (cmdline) {
        g_autofree char *quoted_cmdline = g_shell_quote(cmdline);

        return qtest_initf("-machine virt -display none -smp %u -kernel %s "
                           "-append %s", cpu_count, quoted_elf,
                           quoted_cmdline);
    }

    return qtest_initf("-machine virt -display none -smp %u -kernel %s",
                       cpu_count, quoted_elf);
}

static QTestState *mmix_start_elf(const char *elf, const char *cmdline)
{
    return mmix_start_elf_smp(elf, cmdline, 1);
}

static void mmix_assert_kernel_cmdline(QTestState *qts, const char *expected)
{
    uint64_t address = mmix_bootinfo_read(
        qts, MMIX_BOOTINFO_KERNEL_CMDLINE_ADDR_OFFSET);
    uint64_t size = mmix_bootinfo_read(
        qts, MMIX_BOOTINFO_KERNEL_CMDLINE_SIZE_OFFSET);
    uint64_t flags = mmix_bootinfo_read(qts, MMIX_BOOTINFO_FLAGS_OFFSET);
    size_t expected_size = strlen(expected);
    g_autofree char *actual = NULL;

    if (expected_size == 0) {
        g_assert_cmphex(flags & MMIX_BOOTINFO_FLAG_KERNEL_CMDLINE, ==, 0);
        g_assert_cmphex(address, ==, 0);
        g_assert_cmpuint(size, ==, 0);
        return;
    }

    g_assert_cmphex(flags, ==, MMIX_BOOTINFO_FLAG_KERNEL_CMDLINE);
    g_assert_cmphex(address, ==, mmix_kernel_cmdline_base);
    g_assert_cmpuint(size, ==, expected_size);

    actual = g_malloc(expected_size + 1);
    qtest_memread(qts, address, actual, expected_size + 1);
    g_assert_cmpmem(actual, expected_size + 1,
                    expected, expected_size + 1);
}

static void mmix_check_ram_region(QTestState *qts,
                                  const MMIXPlatformRegion *region,
                                  uint64_t seed)
{
    uint64_t last = region->base + region->size - sizeof(uint64_t);

    qtest_writeq(qts, region->base, seed);
    qtest_writeq(qts, last, ~seed);
    g_assert_cmphex(qtest_readq(qts, region->base), ==, seed);
    g_assert_cmphex(qtest_readq(qts, last), ==, ~seed);
}

static char *mmix_create_elf(void)
{
    struct {
        Elf64_Ehdr ehdr;
        Elf64_Phdr phdr;
        uint32_t insn;
    } image = { 0 };
    g_autofree char *path = NULL;
    GError *error = NULL;
    int fd;

    memcpy(image.ehdr.e_ident, ELFMAG, SELFMAG);
    image.ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    image.ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
    image.ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    image.ehdr.e_type = cpu_to_be16(ET_EXEC);
    image.ehdr.e_machine = cpu_to_be16(EM_MMIX);
    image.ehdr.e_version = cpu_to_be32(EV_CURRENT);
    image.ehdr.e_entry = cpu_to_be64(0);
    image.ehdr.e_phoff = cpu_to_be64(sizeof(Elf64_Ehdr));
    image.ehdr.e_ehsize = cpu_to_be16(sizeof(Elf64_Ehdr));
    image.ehdr.e_phentsize = cpu_to_be16(sizeof(Elf64_Phdr));
    image.ehdr.e_phnum = cpu_to_be16(1);

    image.phdr.p_type = cpu_to_be32(PT_LOAD);
    image.phdr.p_flags = cpu_to_be32(PF_R | PF_X);
    image.phdr.p_offset = cpu_to_be64(offsetof(typeof(image), insn));
    image.phdr.p_vaddr = cpu_to_be64(0);
    image.phdr.p_paddr = cpu_to_be64(0);
    image.phdr.p_filesz = cpu_to_be64(sizeof(image.insn));
    image.phdr.p_memsz = cpu_to_be64(sizeof(image.insn));
    image.phdr.p_align = cpu_to_be64(4);

    fd = g_file_open_tmp("mmix-platform-XXXXXX.elf", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    g_assert_true(g_file_set_contents(path, (const char *)&image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    return g_steal_pointer(&path);
}

static void test_mmix_platform_memory_layout(void)
{
    QTestState *qts = qtest_init("-machine virt");
    size_t i;

    for (i = 0; i + 1 < ARRAY_SIZE(mmix_regions); i++) {
        g_test_message("checking %s/%s boundary",
                       mmix_regions[i].name, mmix_regions[i + 1].name);
        g_assert_cmphex(mmix_regions[i].base + mmix_regions[i].size, ==,
                        mmix_regions[i + 1].base);
    }
    g_assert_cmphex(mmix_regions[MMIX_REGION_FRAMEBUFFER].base +
                    mmix_regions[MMIX_REGION_FRAMEBUFFER].size, ==,
                    mmix_virt_mmio_base);

    for (i = 0; i < ARRAY_SIZE(mmix_regions); i++) {
        uint64_t seed = 0x0102030405060708ULL + i * 0x1010101010101010ULL;

        g_test_message("checking %s endpoints", mmix_regions[i].name);
        mmix_check_ram_region(qts, &mmix_regions[i], seed);
    }

    qtest_quit(qts);
}

static void test_mmix_platform_mmio_layout(void)
{
    uint64_t aperture_end = mmix_virt_mmio_base + mmix_virt_mmio_size;
    size_t i;
    size_t j;

    g_assert_cmphex(mmix_virt_mmio_base, ==, 0x10000000);
    g_assert_cmphex(aperture_end, ==, 0x20000000);

    for (i = 0; i < ARRAY_SIZE(mmix_devices); i++) {
        const MMIXPlatformDevice *device = &mmix_devices[i];
        uint64_t device_end = device->base + device->size;

        g_test_message("checking %s MMIO window", device->name);
        g_assert_cmphex(device->size, >, 0);
        g_assert_cmphex(device->base, >=, mmix_virt_mmio_base);
        g_assert_cmphex(device_end, <=, aperture_end);

        for (j = 0; j < i; j++) {
            const MMIXPlatformDevice *other = &mmix_devices[j];
            uint64_t other_end = other->base + other->size;

            g_assert_true(device_end <= other->base ||
                          other_end <= device->base);
        }
    }
}

static void test_mmix_platform_initial_stack_layout(void)
{
    const MMIXPlatformRegion *low_ram = &mmix_regions[MMIX_REGION_LOW_RAM];
    const MMIXPlatformRegion *platform_ram =
        &mmix_regions[MMIX_REGION_PLATFORM_RAM];
    const MMIXPlatformRegion *framebuffer =
        &mmix_regions[MMIX_REGION_FRAMEBUFFER];
    uint64_t previous_end = mmix_initial_stack_base;
    uint64_t area_end;
    unsigned int i;

    for (i = 0; i < MMIX_INITIAL_STACK_SLOT_COUNT; i++) {
        uint64_t slot_base =
            mmix_initial_stack_base + i * mmix_initial_stack_slot_size;
        uint64_t slot_end = slot_base + mmix_initial_stack_slot_size;

        g_assert_cmphex(slot_base & (sizeof(uint64_t) - 1), ==, 0);
        g_assert_cmphex(slot_base, ==, previous_end);
        g_assert_cmphex(slot_base, >=, low_ram->base);
        g_assert_cmphex(slot_end, <=, low_ram->base + low_ram->size);
        previous_end = slot_end;
    }

    area_end = mmix_initial_stack_base +
               MMIX_INITIAL_STACK_SLOT_COUNT *
               mmix_initial_stack_slot_size;
    g_assert_cmphex(mmix_initial_stack_base, ==, 0x00010000);
    g_assert_cmphex(mmix_initial_stack_base + mmix_initial_stack_slot_size,
                    ==, 0x00018000);
    g_assert_cmphex(mmix_initial_stack_base +
                    15 * mmix_initial_stack_slot_size, ==, 0x00088000);
    g_assert_cmphex(previous_end, ==, area_end);
    g_assert_cmphex(area_end, ==, 0x00090000);
    g_assert_cmphex(area_end, <=, platform_ram->base);
    g_assert_cmphex(area_end, <=, framebuffer->base);
}

static void test_mmix_platform_bootinfo_headless(void)
{
    g_autofree char *elf = mmix_create_elf();
    QTestState *qts = mmix_start_elf(elf, NULL);
    const MMIXBootInfoOffset implemented_device_offsets[] = {
        MMIX_BOOTINFO_UART_BASE_OFFSET,
        MMIX_BOOTINFO_UART_IRQ_OFFSET,
        MMIX_BOOTINFO_TIMER_BASE_OFFSET,
        MMIX_BOOTINFO_TIMER_IRQ_BASE_OFFSET,
        MMIX_BOOTINFO_TIMER_IRQ_COUNT_OFFSET,
        MMIX_BOOTINFO_INTC_BASE_OFFSET,
        MMIX_BOOTINFO_INTC_IRQ_COUNT_OFFSET,
        MMIX_BOOTINFO_VIRTIO_MMIO_BASE_OFFSET,
        MMIX_BOOTINFO_VIRTIO_MMIO_IRQ_OFFSET,
        MMIX_BOOTINFO_VIRTIO_MMIO_COUNT_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_CONTROL_BASE_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_BASE_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_SIZE_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_IRQ_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_WIDTH_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_HEIGHT_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_STRIDE_OFFSET,
        MMIX_BOOTINFO_FRAMEBUFFER_FORMAT_OFFSET,
        MMIX_BOOTINFO_IPI_BASE_OFFSET,
        MMIX_BOOTINFO_IPI_TARGET_COUNT_OFFSET,
        MMIX_BOOTINFO_IPI_REQUEST_MASK_OFFSET,
    };
    uint64_t uart_base;
    uint64_t timer_base;
    uint64_t intc_base;
    uint64_t ipi_base;
    uint64_t virtio_base;
    uint64_t framebuffer_control_base;
    uint64_t uart_irq;
    uint64_t virtio_irq;
    uint64_t framebuffer_irq;
    uint64_t timer_irq;
    size_t i;

    g_assert_cmphex(mmix_bootinfo_read(qts, MMIX_BOOTINFO_MAGIC_OFFSET), ==,
                    mmix_bootinfo_magic);
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_VERSION_OFFSET), ==,
                     MMIX_BOOTINFO_VERSION);
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_SIZE_OFFSET), ==,
                     MMIX_BOOTINFO_SIZE);
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_FLAGS_OFFSET), ==,
                     0);
    mmix_assert_kernel_cmdline(qts, "");
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_CPU_COUNT_OFFSET),
                     ==, 1);
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_BOOT_CPU_ID_OFFSET),
                     ==, 0);
    g_assert_cmphex(mmix_bootinfo_read(qts, MMIX_BOOTINFO_MMIO_BASE_OFFSET),
                    ==, mmix_virt_mmio_base);
    for (i = 0; i < ARRAY_SIZE(implemented_device_offsets); i++) {
        g_assert_cmpuint(mmix_bootinfo_read(qts,
                                           implemented_device_offsets[i]),
                         >, 0);
    }

    uart_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_UART_BASE_OFFSET);
    timer_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_TIMER_BASE_OFFSET);
    intc_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_INTC_BASE_OFFSET);
    ipi_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_IPI_BASE_OFFSET);
    virtio_base = mmix_bootinfo_read(
        qts, MMIX_BOOTINFO_VIRTIO_MMIO_BASE_OFFSET);
    framebuffer_control_base = mmix_bootinfo_read(
        qts, MMIX_BOOTINFO_FRAMEBUFFER_CONTROL_BASE_OFFSET);
    g_assert_cmphex(uart_base, ==, mmix_devices[MMIX_DEVICE_UART0].base);
    g_assert_cmphex(timer_base, ==, mmix_devices[MMIX_DEVICE_TIMER0].base);
    g_assert_cmphex(intc_base, ==, mmix_devices[MMIX_DEVICE_INTC].base);
    g_assert_cmphex(ipi_base, ==, mmix_devices[MMIX_DEVICE_IPI].base);
    g_assert_cmphex(virtio_base, ==,
                    mmix_devices[MMIX_DEVICE_VIRTIO_BLOCK0].base);
    g_assert_cmphex(framebuffer_control_base, ==,
                    mmix_devices[MMIX_DEVICE_FRAMEBUFFER].base);

    g_assert_cmphex(qtest_readb(qts, uart_base + MMIX_UART_LSR) &
                    MMIX_UART_LSR_THRE, ==, MMIX_UART_LSR_THRE);
    g_assert_cmphex(qtest_readl(qts, virtio_base + VIRTIO_MMIO_MAGIC_VALUE),
                    ==, 0x74726976);
    g_assert_cmphex(qtest_readq(qts, framebuffer_control_base +
                                MMIX_FRAMEBUFFER_REG_BASE), ==,
                    mmix_bootinfo_read(qts,
                                       MMIX_BOOTINFO_FRAMEBUFFER_BASE_OFFSET));
    g_assert_cmphex(qtest_readq(qts, framebuffer_control_base +
                                MMIX_FRAMEBUFFER_REG_SIZE), ==,
                    mmix_bootinfo_read(qts,
                                       MMIX_BOOTINFO_FRAMEBUFFER_SIZE_OFFSET));
    g_assert_cmpuint(qtest_readq(qts, framebuffer_control_base +
                                 MMIX_FRAMEBUFFER_REG_WIDTH), ==,
                     mmix_bootinfo_read(
                         qts, MMIX_BOOTINFO_FRAMEBUFFER_WIDTH_OFFSET));
    g_assert_cmpuint(qtest_readq(qts, timer_base +
                                 MMIX_TIMER_CONTEXT_BASE +
                                 MMIX_TIMER_CONTEXT_CONTROL), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, intc_base + MMIX_INTC_PENDING),
                     ==, 0);
    g_assert_cmphex(qtest_readq(qts, ipi_base + MMIX_IPI_ACTIVE_TARGETS),
                    ==, 1);
    g_assert_cmphex(mmix_bootinfo_read(
                        qts, MMIX_BOOTINFO_IPI_REQUEST_MASK_OFFSET),
                    ==, mmix_ipi_request);
    g_assert_cmpuint(MMIX_BOOTINFO_IPI_BASE_OFFSET, ==,
                     MMIX_BOOTINFO_COMPAT_PREFIX_SIZE);

    uart_irq = mmix_bootinfo_read(qts, MMIX_BOOTINFO_UART_IRQ_OFFSET);
    virtio_irq = mmix_bootinfo_read(qts,
                                   MMIX_BOOTINFO_VIRTIO_MMIO_IRQ_OFFSET);
    framebuffer_irq = mmix_bootinfo_read(qts,
                                        MMIX_BOOTINFO_FRAMEBUFFER_IRQ_OFFSET);
    timer_irq = mmix_bootinfo_read(qts,
                                  MMIX_BOOTINFO_TIMER_IRQ_BASE_OFFSET);
    g_assert_cmpuint(uart_irq, ==, mmix_devices[MMIX_DEVICE_UART0].irq);
    g_assert_cmpuint(virtio_irq, ==,
                     mmix_devices[MMIX_DEVICE_VIRTIO_BLOCK0].irq);
    g_assert_cmpuint(framebuffer_irq, ==,
                     mmix_devices[MMIX_DEVICE_FRAMEBUFFER].irq);
    g_assert_cmpuint(timer_irq, ==, mmix_devices[MMIX_DEVICE_TIMER0].irq);
    g_assert_cmpuint(uart_irq, !=, virtio_irq);
    g_assert_cmpuint(uart_irq, !=, framebuffer_irq);
    g_assert_cmpuint(uart_irq, !=, timer_irq);
    g_assert_cmpuint(virtio_irq, !=, framebuffer_irq);
    g_assert_cmpuint(virtio_irq, !=, timer_irq);
    g_assert_cmpuint(framebuffer_irq, !=, timer_irq);

    qtest_writeq(qts, framebuffer_control_base +
                  MMIX_FRAMEBUFFER_REG_FLUSH, 1);
    g_assert_cmphex(qtest_readl(qts, intc_base + MMIX_INTC_PENDING) &
                    (1U << framebuffer_irq), ==, 0);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_kernel_cmdline(void)
{
    const char *cmdline = "console=ttyS0 root=/dev/vda";
    g_autofree char *elf = mmix_create_elf();
    QTestState *qts = mmix_start_elf(elf, cmdline);

    mmix_assert_kernel_cmdline(qts, cmdline);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_empty_kernel_cmdline(void)
{
    g_autofree char *elf = mmix_create_elf();
    QTestState *qts = mmix_start_elf(elf, "");

    /* MachineState normalizes absent and explicitly empty values to "". */
    mmix_assert_kernel_cmdline(qts, "");

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_max_kernel_cmdline(void)
{
    g_autofree char *cmdline =
        g_strnfill(MMIX_BOOTINFO_KERNEL_CMDLINE_MAX, 'x');
    g_autofree char *elf = mmix_create_elf();
    QTestState *qts = mmix_start_elf(elf, cmdline);

    mmix_assert_kernel_cmdline(qts, cmdline);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_smp_accepted(gconstpointer opaque)
{
    const char *smp = opaque;
    QTestState *qts = qtest_initf("-machine virt -smp %s", smp);

    qtest_quit(qts);
}

static void mmix_assert_smp_topology(QTestState *qts,
                                     unsigned int expected_count)
{
    bool seen[MMIX_INITIAL_STACK_SLOT_COUNT] = { false };
    g_autoptr(QDict) response = NULL;
    QList *cpus;
    QObject *entry;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, expected_count);

    while ((entry = qlist_pop(cpus))) {
        QDict *cpu = qobject_to(QDict, entry);
        unsigned int index = qdict_get_int(cpu, "cpu-index");
        const char *path = qdict_get_str(cpu, "qom-path");
        g_autofree char *expected_path =
            g_strdup_printf("/machine/cpu[%u]", index);
        g_autoptr(QDict) stack_response = NULL;
        uint64_t expected_stack =
            mmix_initial_stack_base + index * mmix_initial_stack_slot_size;

        g_assert_cmpuint(index, <, expected_count);
        g_assert_false(seen[index]);
        seen[index] = true;
        g_assert_cmpstr(path, ==, expected_path);

        stack_response = qtest_qmp(
            qts,
            "{ 'execute': 'qom-get', "
            "  'arguments': { 'path': %s, "
            "                 'property': 'initial-stack' } }",
            path);
        g_assert_cmphex(qdict_get_int(stack_response, "return"), ==,
                        expected_stack);
        qobject_unref(entry);
    }

    for (unsigned int i = 0; i < expected_count; i++) {
        g_assert_true(seen[i]);
    }
}

static void mmix_assert_stack_slots_writable(QTestState *qts,
                                             unsigned int cpu_count)
{
    unsigned int i;

    for (i = 0; i < cpu_count; i++) {
        uint64_t base = mmix_initial_stack_base +
                        i * mmix_initial_stack_slot_size;
        uint64_t end = base + mmix_initial_stack_slot_size - sizeof(uint64_t);

        qtest_writeq(qts, base, 0x1000000000000000ULL + i);
        qtest_writeq(qts, end, 0xf000000000000000ULL + i);
    }
    for (i = 0; i < cpu_count; i++) {
        uint64_t base = mmix_initial_stack_base +
                        i * mmix_initial_stack_slot_size;
        uint64_t end = base + mmix_initial_stack_slot_size - sizeof(uint64_t);

        g_assert_cmphex(qtest_readq(qts, base), ==,
                        0x1000000000000000ULL + i);
        g_assert_cmphex(qtest_readq(qts, end), ==,
                        0xf000000000000000ULL + i);
    }
}

static void test_mmix_platform_smp_cpus(gconstpointer opaque)
{
    unsigned int expected_count = GPOINTER_TO_UINT(opaque);
    g_autofree char *elf = mmix_create_elf();
    QTestState *qts = mmix_start_elf_smp(elf, NULL, expected_count);

    mmix_assert_smp_topology(qts, expected_count);
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_CPU_COUNT_OFFSET),
                     ==, expected_count);
    g_assert_cmpuint(mmix_bootinfo_read(qts,
                                       MMIX_BOOTINFO_BOOT_CPU_ID_OFFSET),
                     ==, 0);
    mmix_assert_stack_slots_writable(qts, expected_count);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static uint64_t mmix_register_dump_value(const char *dump, const char *label)
{
    const char *value = strstr(dump, label);
    uint64_t result;

    g_assert_nonnull(value);
    g_assert_cmpint(sscanf(value + strlen(label), "%" SCNx64, &result), ==, 1);
    return result;
}

static uint64_t mmix_cpu_rq(QTestState *qts, unsigned int cpu_index)
{
    g_autofree char *dump = qtest_hmp(qts, "info registers %u", cpu_index);

    return mmix_register_dump_value(dump, "rQ =0x");
}

static void mmix_wait_cpu_rq(QTestState *qts, unsigned int cpu_index,
                             uint64_t expected)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (mmix_cpu_rq(qts, cpu_index) != expected) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static uint64_t mmix_intc_enable_address(unsigned int cpu_index)
{
    return mmix_devices[MMIX_DEVICE_INTC].base + MMIX_INTC_CONTEXT_BASE +
           cpu_index * MMIX_INTC_CONTEXT_STRIDE + MMIX_INTC_CONTEXT_ENABLE;
}

static uint64_t mmix_timer_context_address(unsigned int cpu_index,
                                           uint64_t reg)
{
    return mmix_devices[MMIX_DEVICE_TIMER0].base + MMIX_TIMER_CONTEXT_BASE +
           cpu_index * MMIX_TIMER_CONTEXT_STRIDE + reg;
}

static uint64_t mmix_ipi_context_address(unsigned int cpu_index,
                                         uint64_t reg)
{
    return mmix_devices[MMIX_DEVICE_IPI].base + MMIX_IPI_CONTEXT_BASE +
           cpu_index * MMIX_IPI_CONTEXT_STRIDE + reg;
}

static void mmix_set_intc_irq(QTestState *qts, unsigned int irq, int level)
{
    qtest_set_irq_in(qts, mmix_intc_qom_path, "unnamed-gpio-in", irq, level);
}

static void mmix_assert_cpu_startup(QTestState *qts, unsigned int cpu_index)
{
    g_autofree char *dump = qtest_hmp(qts, "info registers %u", cpu_index);
    uint64_t stack = mmix_initial_stack_base +
                     cpu_index * mmix_initial_stack_slot_size;

    g_assert_cmphex(mmix_register_dump_value(dump, "pc=0x"), ==, 0);
    g_assert_cmphex(mmix_register_dump_value(dump, "rO=0x"), ==, stack);
    g_assert_cmphex(mmix_register_dump_value(dump, "rS=0x"), ==, stack);
    g_assert_cmphex(mmix_register_dump_value(dump, "stack-bottom=0x"), ==,
                    stack);
    g_assert_cmphex(mmix_register_dump_value(dump, "r0  =0x"), ==,
                    cpu_index);
    g_assert_cmphex(mmix_register_dump_value(dump, "r1  =0x"), ==,
                    mmix_bootinfo_base);
}

static void test_mmix_platform_smp_interrupt_wiring(gconstpointer opaque)
{
    unsigned int cpu_count = GPOINTER_TO_UINT(opaque);
    unsigned int target_cpu = cpu_count - 1;
    unsigned int irq = MMIX_TIMER_IRQ_BASE + target_cpu;
    uint32_t mask = 1U << irq;
    uint64_t baseline[MMIX_INITIAL_STACK_SLOT_COUNT];
    g_autoptr(QDict) response = NULL;
    QTestState *qts = qtest_initf("-machine virt -smp %u", cpu_count);
    unsigned int i;

    response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get', "
        "  'arguments': { 'path': %s, 'property': 'num-cpus' } }",
        mmix_intc_qom_path);
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, cpu_count);

    for (i = 0; i < cpu_count; i++) {
        baseline[i] = mmix_cpu_rq(qts, i);
    }
    qtest_writel(qts, mmix_intc_enable_address(target_cpu), mask);
    mmix_set_intc_irq(qts, irq, 1);
    mmix_wait_cpu_rq(qts, target_cpu,
                     baseline[target_cpu] |
                     mmix_interrupt_controller_request);

    for (i = 0; i < cpu_count; i++) {
        g_assert_cmphex(mmix_cpu_rq(qts, i), ==,
                        i == target_cpu ?
                        baseline[i] | mmix_interrupt_controller_request :
                        baseline[i]);
    }

    mmix_set_intc_irq(qts, irq, 0);
    qtest_system_reset(qts);
    for (i = 0; i < cpu_count; i++) {
        mmix_wait_cpu_rq(qts, i, baseline[i]);
    }

    qtest_quit(qts);
}

static void test_mmix_platform_smp_timer_wiring(gconstpointer opaque)
{
    unsigned int cpu_count = GPOINTER_TO_UINT(opaque);
    unsigned int target_cpu = cpu_count - 1;
    unsigned int irq = MMIX_TIMER_IRQ_BASE + target_cpu;
    uint32_t mask = 1U << irq;
    uint64_t baseline[MMIX_INITIAL_STACK_SLOT_COUNT];
    uint64_t timer_base = mmix_devices[MMIX_DEVICE_TIMER0].base;
    uint64_t now;
    g_autofree char *elf = mmix_create_elf();
    g_autoptr(QDict) response = NULL;
    QTestState *qts = mmix_start_elf_smp(elf, NULL, cpu_count);
    unsigned int i;

    response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get', "
        "  'arguments': { 'path': %s, 'property': 'num-cpus' } }",
        mmix_timer_qom_path);
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, cpu_count);
    g_assert_cmpuint(mmix_bootinfo_read(qts,
                                       MMIX_BOOTINFO_TIMER_IRQ_BASE_OFFSET),
                     ==, MMIX_TIMER_IRQ_BASE);
    g_assert_cmpuint(mmix_bootinfo_read(qts,
                                       MMIX_BOOTINFO_TIMER_IRQ_COUNT_OFFSET),
                     ==, cpu_count);

    for (i = 0; i < cpu_count; i++) {
        baseline[i] = mmix_cpu_rq(qts, i);
    }

    qtest_writel(qts, mmix_intc_enable_address(target_cpu), mask);
    now = qtest_readq(qts, timer_base + MMIX_TIMER_TIME);
    qtest_writeq(qts,
                 mmix_timer_context_address(
                     target_cpu, MMIX_TIMER_CONTEXT_COMPARE),
                 now + 10);
    qtest_writeq(qts,
                 mmix_timer_context_address(
                     target_cpu, MMIX_TIMER_CONTEXT_CONTROL),
                 MMIX_TIMER_CONTROL_ENABLE |
                 MMIX_TIMER_CONTROL_IRQ_ENABLE);
    qtest_clock_step(qts, 10);

    g_assert_cmphex(qtest_readq(
                        qts,
                        mmix_timer_context_address(
                            target_cpu, MMIX_TIMER_CONTEXT_STATUS)),
                    ==, MMIX_TIMER_STATUS_PENDING);
    g_assert_cmphex(qtest_readl(
                        qts,
                        mmix_devices[MMIX_DEVICE_INTC].base +
                        MMIX_INTC_PENDING),
                    ==, mask);
    mmix_wait_cpu_rq(qts, target_cpu,
                     baseline[target_cpu] |
                     mmix_interrupt_controller_request);

    for (i = 0; i < cpu_count; i++) {
        g_assert_cmphex(mmix_cpu_rq(qts, i), ==,
                        i == target_cpu ?
                        baseline[i] | mmix_interrupt_controller_request :
                        baseline[i]);
    }

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_smp_ipi_wiring(gconstpointer opaque)
{
    unsigned int cpu_count = GPOINTER_TO_UINT(opaque);
    unsigned int target_cpu = cpu_count - 1;
    uint64_t active_targets = (1ULL << cpu_count) - 1;
    uint64_t invalid_target = 1ULL << cpu_count;
    uint64_t baseline[MMIX_INITIAL_STACK_SLOT_COUNT];
    uint64_t ipi_base = mmix_devices[MMIX_DEVICE_IPI].base;
    g_autofree char *elf = mmix_create_elf();
    g_autoptr(QDict) response = NULL;
    QTestState *qts = mmix_start_elf_smp(elf, NULL, cpu_count);
    unsigned int i;

    response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get', "
        "  'arguments': { 'path': %s, 'property': 'num-cpus' } }",
        mmix_ipi_qom_path);
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, cpu_count);
    g_assert_cmphex(qtest_readq(qts, ipi_base + MMIX_IPI_ACTIVE_TARGETS),
                    ==, active_targets);
    g_assert_cmphex(mmix_bootinfo_read(qts,
                                      MMIX_BOOTINFO_IPI_BASE_OFFSET),
                    ==, ipi_base);
    g_assert_cmpuint(mmix_bootinfo_read(
                         qts, MMIX_BOOTINFO_IPI_TARGET_COUNT_OFFSET),
                     ==, cpu_count);
    g_assert_cmphex(mmix_bootinfo_read(
                        qts, MMIX_BOOTINFO_IPI_REQUEST_MASK_OFFSET),
                    ==, mmix_ipi_request);

    for (i = 0; i < cpu_count; i++) {
        baseline[i] = mmix_cpu_rq(qts, i);
    }

    qtest_writeq(qts, ipi_base + MMIX_IPI_SEND,
                 (1ULL << target_cpu) | invalid_target);
    mmix_wait_cpu_rq(qts, target_cpu,
                     baseline[target_cpu] | mmix_ipi_request);
    for (i = 0; i < cpu_count; i++) {
        g_assert_cmphex(mmix_cpu_rq(qts, i), ==,
                        i == target_cpu ?
                        baseline[i] | mmix_ipi_request : baseline[i]);
    }
    g_assert_cmphex(qtest_readq(
                        qts,
                        mmix_ipi_context_address(
                            target_cpu, MMIX_IPI_CONTEXT_STATUS)),
                    ==, MMIX_IPI_STATUS_PENDING);
    if (cpu_count < MMIX_INITIAL_STACK_SLOT_COUNT) {
        g_assert_cmphex(qtest_readq(
                            qts,
                            mmix_ipi_context_address(
                                cpu_count, MMIX_IPI_CONTEXT_STATUS)),
                        ==, 0);
    }

    qtest_writeq(qts,
                 mmix_ipi_context_address(target_cpu,
                                          MMIX_IPI_CONTEXT_CLEAR),
                 MMIX_IPI_STATUS_PENDING);
    g_assert_cmphex(qtest_readq(
                        qts,
                        mmix_ipi_context_address(
                            target_cpu, MMIX_IPI_CONTEXT_STATUS)),
                    ==, 0);
    qtest_system_reset(qts);
    for (i = 0; i < cpu_count; i++) {
        mmix_wait_cpu_rq(qts, i, baseline[i]);
    }

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_smp_reset(void)
{
    g_autofree char *elf = mmix_create_elf();
    QTestState *qts = mmix_start_elf_smp(elf, NULL, 2);
    uint64_t baseline_rq[2];
    unsigned int i;

    for (i = 0; i < 2; i++) {
        mmix_assert_cpu_startup(qts, i);
        baseline_rq[i] = mmix_cpu_rq(qts, i);
    }

    qtest_writel(qts, mmix_intc_enable_address(1),
                 1U << (MMIX_TIMER_IRQ_BASE + 1));
    mmix_set_intc_irq(qts, MMIX_TIMER_IRQ_BASE + 1, 1);
    mmix_wait_cpu_rq(qts, 1,
                     baseline_rq[1] | mmix_interrupt_controller_request);
    g_assert_cmphex(mmix_cpu_rq(qts, 0), ==, baseline_rq[0]);

    mmix_set_intc_irq(qts, MMIX_TIMER_IRQ_BASE + 1, 0);
    qtest_system_reset(qts);
    mmix_assert_smp_topology(qts, 2);
    g_assert_cmpuint(mmix_bootinfo_read(qts, MMIX_BOOTINFO_CPU_COUNT_OFFSET),
                     ==, 2);
    g_assert_cmpuint(mmix_bootinfo_read(qts,
                                       MMIX_BOOTINFO_BOOT_CPU_ID_OFFSET),
                     ==, 0);
    for (i = 0; i < 2; i++) {
        mmix_assert_cpu_startup(qts, i);
        mmix_wait_cpu_rq(qts, i, baseline_rq[i]);
    }

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(elf), ==, 0);
}

static void test_mmix_platform_smp_rejected(gconstpointer opaque)
{
    const char *smp = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "virt", "-smp", smp,
        "-display", "none", "-monitor", "none", "-serial", "none", NULL,
    };
    int wait_status;

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);

    if (!strcmp(smp, "17")) {
        g_assert_nonnull(strstr(stderr_text,
                               "max CPUs supported by machine 'virt' is 16"));
    } else {
        g_assert_nonnull(strstr(stderr_text,
                               "CPU topology parameters must be greater "
                               "than zero"));
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/platform/memory-layout",
                   test_mmix_platform_memory_layout);
    qtest_add_func("/mmix/platform/mmio-layout",
                   test_mmix_platform_mmio_layout);
    qtest_add_func("/mmix/platform/initial-stack-layout",
                   test_mmix_platform_initial_stack_layout);
    qtest_add_func("/mmix/platform/bootinfo-headless",
                   test_mmix_platform_bootinfo_headless);
    qtest_add_func("/mmix/platform/kernel-cmdline",
                   test_mmix_platform_kernel_cmdline);
    qtest_add_func("/mmix/platform/empty-kernel-cmdline",
                   test_mmix_platform_empty_kernel_cmdline);
    qtest_add_func("/mmix/platform/max-kernel-cmdline",
                   test_mmix_platform_max_kernel_cmdline);
    qtest_add_data_func("/mmix/platform/smp/accepted/1",
                        "1",
                        test_mmix_platform_smp_accepted);
    qtest_add_data_func("/mmix/platform/smp/accepted/2",
                        "2",
                        test_mmix_platform_smp_accepted);
    qtest_add_data_func("/mmix/platform/smp/accepted/16",
                        "16",
                        test_mmix_platform_smp_accepted);
    qtest_add_data_func("/mmix/platform/smp/accepted/actual-below-maximum",
                        "cpus=2,maxcpus=16",
                        test_mmix_platform_smp_accepted);
    qtest_add_data_func("/mmix/platform/smp/cpus/1",
                        GUINT_TO_POINTER(1),
                        test_mmix_platform_smp_cpus);
    qtest_add_data_func("/mmix/platform/smp/cpus/2",
                        GUINT_TO_POINTER(2),
                        test_mmix_platform_smp_cpus);
    qtest_add_data_func("/mmix/platform/smp/cpus/16",
                        GUINT_TO_POINTER(16),
                        test_mmix_platform_smp_cpus);
    qtest_add_data_func("/mmix/platform/smp/interrupt-wiring/1",
                        GUINT_TO_POINTER(1),
                        test_mmix_platform_smp_interrupt_wiring);
    qtest_add_data_func("/mmix/platform/smp/interrupt-wiring/2",
                        GUINT_TO_POINTER(2),
                        test_mmix_platform_smp_interrupt_wiring);
    qtest_add_data_func("/mmix/platform/smp/interrupt-wiring/16",
                        GUINT_TO_POINTER(16),
                        test_mmix_platform_smp_interrupt_wiring);
    qtest_add_data_func("/mmix/platform/smp/timer-wiring/1",
                        GUINT_TO_POINTER(1),
                        test_mmix_platform_smp_timer_wiring);
    qtest_add_data_func("/mmix/platform/smp/timer-wiring/2",
                        GUINT_TO_POINTER(2),
                        test_mmix_platform_smp_timer_wiring);
    qtest_add_data_func("/mmix/platform/smp/timer-wiring/16",
                        GUINT_TO_POINTER(16),
                        test_mmix_platform_smp_timer_wiring);
    qtest_add_data_func("/mmix/platform/smp/ipi-wiring/1",
                        GUINT_TO_POINTER(1),
                        test_mmix_platform_smp_ipi_wiring);
    qtest_add_data_func("/mmix/platform/smp/ipi-wiring/2",
                        GUINT_TO_POINTER(2),
                        test_mmix_platform_smp_ipi_wiring);
    qtest_add_data_func("/mmix/platform/smp/ipi-wiring/16",
                        GUINT_TO_POINTER(16),
                        test_mmix_platform_smp_ipi_wiring);
    qtest_add_func("/mmix/platform/smp/reset",
                   test_mmix_platform_smp_reset);
    qtest_add_data_func("/mmix/platform/smp/rejected/zero", "0",
                        test_mmix_platform_smp_rejected);
    qtest_add_data_func("/mmix/platform/smp/rejected/above-maximum", "17",
                        test_mmix_platform_smp_rejected);

    return g_test_run();
}
