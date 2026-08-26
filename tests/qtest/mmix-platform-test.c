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
    unsigned irq;
} MMIXPlatformDevice;

typedef enum MMIXPlatformDeviceIndex {
    MMIX_DEVICE_UART0,
    MMIX_DEVICE_VIRTIO_BLOCK0,
    MMIX_DEVICE_FRAMEBUFFER,
    MMIX_DEVICE_TIMER0,
    MMIX_DEVICE_INTC,
    MMIX_DEVICE_COUNT,
} MMIXPlatformDeviceIndex;

static const MMIXPlatformDevice mmix_devices[MMIX_DEVICE_COUNT] = {
    [MMIX_DEVICE_UART0] = { "UART0", 0x10000000, 1 },
    [MMIX_DEVICE_VIRTIO_BLOCK0] = { "virtio block 0", 0x10001000, 2 },
    [MMIX_DEVICE_FRAMEBUFFER] = { "framebuffer", 0x10002000, 3 },
    [MMIX_DEVICE_TIMER0] = { "CPU0 timer", 0x10003000, 16 },
    [MMIX_DEVICE_INTC] = { "interrupt controller", 0x10004000, 0 },
};

static const uint64_t mmix_virt_mmio_base = 0x10000000ULL;
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
    MMIX_TIMER_CONTEXT_BASE = 0x0100,
    MMIX_TIMER_CONTEXT_CONTROL = 0x08,
    MMIX_INTC_PENDING = 0x0000,
    MMIX_INITIAL_STACK_SLOT_COUNT = 16,
};

static const uint64_t mmix_initial_stack_base = 0x00010000;
static const uint64_t mmix_initial_stack_slot_size = 0x00008000;

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
    MMIX_BOOTINFO_SIZE = 0x140,
} MMIXBootInfoOffset;

static const uint64_t mmix_kernel_cmdline_base =
    0x0e800000ULL + MMIX_BOOTINFO_SIZE;

static uint64_t mmix_bootinfo_read(QTestState *qts,
                                   MMIXBootInfoOffset offset)
{
    return qtest_readq(qts, mmix_bootinfo_base + offset);
}

static QTestState *mmix_start_elf(const char *elf, const char *cmdline)
{
    g_autofree char *quoted_elf = g_shell_quote(elf);

    if (cmdline) {
        g_autofree char *quoted_cmdline = g_shell_quote(cmdline);

        return qtest_initf("-machine virt -display none -kernel %s "
                           "-append %s", quoted_elf, quoted_cmdline);
    }

    return qtest_initf("-machine virt -display none -kernel %s", quoted_elf);
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
    };
    uint64_t uart_base;
    uint64_t timer_base;
    uint64_t intc_base;
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
    for (i = 0; i < ARRAY_SIZE(implemented_device_offsets); i++) {
        g_assert_cmpuint(mmix_bootinfo_read(qts,
                                           implemented_device_offsets[i]),
                         >, 0);
    }

    uart_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_UART_BASE_OFFSET);
    timer_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_TIMER_BASE_OFFSET);
    intc_base = mmix_bootinfo_read(qts, MMIX_BOOTINFO_INTC_BASE_OFFSET);
    virtio_base = mmix_bootinfo_read(
        qts, MMIX_BOOTINFO_VIRTIO_MMIO_BASE_OFFSET);
    framebuffer_control_base = mmix_bootinfo_read(
        qts, MMIX_BOOTINFO_FRAMEBUFFER_CONTROL_BASE_OFFSET);
    g_assert_cmphex(uart_base, ==, mmix_devices[MMIX_DEVICE_UART0].base);
    g_assert_cmphex(timer_base, ==, mmix_devices[MMIX_DEVICE_TIMER0].base);
    g_assert_cmphex(intc_base, ==, mmix_devices[MMIX_DEVICE_INTC].base);
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

static void test_mmix_platform_smp_cpus(gconstpointer opaque)
{
    unsigned int expected_count = GPOINTER_TO_UINT(opaque);
    bool seen[MMIX_INITIAL_STACK_SLOT_COUNT] = { false };
    g_autoptr(QDict) response = NULL;
    QTestState *qts = qtest_initf("-machine virt -smp %u", expected_count);
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
    qtest_quit(qts);
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
    qtest_add_data_func("/mmix/platform/smp/rejected/zero", "0",
                        test_mmix_platform_smp_rejected);
    qtest_add_data_func("/mmix/platform/smp/rejected/above-maximum", "17",
                        test_mmix_platform_smp_rejected);

    return g_test_run();
}
