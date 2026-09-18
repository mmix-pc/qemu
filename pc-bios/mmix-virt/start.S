/*
 * MMIX virt firmware entry
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

.section .text.entry,"ax",@progbits
.global _start
.type _start,@function
_start:
        GET r254, rO
        SETL r1, 0x8000
        ADDU r254, r254, r1
        BNZ r0, .Lsecondary

        PUT rG, 231
        PUT rL, 0
        PUSHJ r31, mmix_firmware_main

.Lstop:
        SYNC 4
        JMPB .Lstop

.Lsecondary:
        SYNC 4
        JMPB .Lsecondary
.size _start, .-_start

.section .note.GNU-stack,"",@progbits
