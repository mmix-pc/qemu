/*
 * MMIX virt firmware compiler runtime support
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "firmware.h"

#ifndef MMIX_FIRMWARE_TEST
void firmware_physical_write8(uint64_t address, uint8_t value)
{
    /* Volatile preserves each guest RAM transaction. */
    *(volatile uint8_t *)address = value;
}
#endif

void *memset(void *destination, int value, unsigned long size)
{
    /*
     * Prevent the compiler from replacing this freestanding loop with memset.
     */
    volatile uint8_t *bytes = destination;
    unsigned long i;

    for (i = 0; i < size; i++) {
        bytes[i] = value;
    }
    return destination;
}
