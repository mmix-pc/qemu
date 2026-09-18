/*
 * MMIX virt firmware initial diagnostic path
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

_Static_assert(sizeof(uint8_t) == 1, "unexpected MMIX byte size");
_Static_assert(sizeof(uint32_t) == 4, "unexpected MMIX tetra size");
_Static_assert(sizeof(uint64_t) == 8, "unexpected MMIX octa size");

#define UINT64_C(value) value##ULL
#define MMIX_DIRECT_ALIAS(address) (UINT64_C(0x8000000000000000) | (address))

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
    uart_puts("MMIX firmware: boot services unavailable\n");
    firmware_panic();
}
