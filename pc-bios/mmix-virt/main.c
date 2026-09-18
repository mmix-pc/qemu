/*
 * MMIX virt firmware initial diagnostic path
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "firmware.h"

#define MMIX_UART_BASE MMIX_DIRECT_ALIAS(UINT64_C(0x0001000010000000))
#define MMIX_UART_THR 0
#define MMIX_UART_LSR 5
#define MMIX_UART_LSR_THRE 0x20

#define MMIX_POWER_BASE MMIX_DIRECT_ALIAS(UINT64_C(0x0001000010040000))
#define MMIX_POWER_COMMAND 4
#define MMIX_POWER_COMMAND_PANIC 3

static const char firmware_version[]
    __attribute__((section(".rodata.version"), used)) =
    "QEMU MMIX virt firmware 0.1.0; compatible=mmix-virt";

static uint8_t mmio_read8(uint64_t address)
{
    /* Volatile preserves each guest MMIO transaction. */
    return *(volatile uint8_t *)address;
}

static void mmio_write8(uint64_t address, uint8_t value)
{
    /* Volatile preserves each guest MMIO transaction. */
    *(volatile uint8_t *)address = value;
}

static void mmio_write32(uint64_t address, uint32_t value)
{
    /* Volatile preserves each guest MMIO transaction. */
    *(volatile uint32_t *)address = value;
}

static void uart_putc(uint8_t value)
{
    while ((mmio_read8(MMIX_UART_BASE + MMIX_UART_LSR) &
            MMIX_UART_LSR_THRE) == 0) {
    }
    mmio_write8(MMIX_UART_BASE + MMIX_UART_THR, value);
}

static void uart_puts(const char *text)
{
    while (*text != '\0') {
        uart_putc(*text++);
    }
}

static __attribute__((noreturn)) void firmware_panic(void)
{
    mmio_write32(MMIX_POWER_BASE + MMIX_POWER_COMMAND,
                 MMIX_POWER_COMMAND_PANIC);
    for (;;) {
        __asm__ volatile("SYNC 4");
    }
}

__attribute__((noreturn)) void mmix_firmware_main(void)
{
    FirmwareBootInputs inputs;
    FirmwareBootPlan plan = { 0 };
    const char *error;

    if (!firmware_discover_boot_inputs(&inputs, &error)) {
        uart_puts("MMIX firmware: ");
        uart_puts(error);
        uart_putc('\n');
    } else if (!inputs.kernel.present) {
        uart_puts("MMIX firmware: no kernel payload\n");
    } else if (!firmware_prepare_boot(&inputs, &plan, &error)) {
        uart_puts("MMIX firmware: ");
        uart_puts(error);
        uart_putc('\n');
    } else {
        firmware_commit_boot_data(&inputs, &plan);
        firmware_release_cpus(&inputs, &plan);
    }
    firmware_panic();
}
