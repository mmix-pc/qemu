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
        PUT rG, 231
        BNZ r0, .Lsecondary

        PUT rL, 0
        PUSHJ r31, mmix_firmware_main

.Lstop:
        SYNC 4
        JMPB .Lstop

.Lsecondary:
        SETL r240, 0x8000
        SETL r243, 1
.Lsecondary_ready:
        LDOU r241, r240, 0
        CMPU r242, r241, r243
        BNZB r242, .Lsecondary_ready
        LDOU r248, r240, 8
        LDOU r1, r240, 16
        ADDU r244, r240, 32
        ADDU r244, r244, r0
        STBU r243, r244, 0
        SYNC 1
        SETL r243, 2
.Lsecondary_go:
        LDOU r241, r240, 0
        CMPU r242, r241, r243
        BNZB r242, .Lsecondary_go
        PUT rL, 2
        GO r249, r248, 0
.Lsecondary_stop:
        SYNC 4
        JMPB .Lsecondary_stop
.size _start, .-_start

.global firmware_enter_kernel
.type firmware_enter_kernel,@function
firmware_enter_kernel:
        ADDU r0, r231, 0
        ADDU r1, r232, 0
        ADDU r248, r233, 0
        PUT rL, 2
        GO r249, r248, 0
.size firmware_enter_kernel, .-firmware_enter_kernel

.section .note.GNU-stack,"",@progbits
