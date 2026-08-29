#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def _hosted_image(items, *, global_base=R255, globals_=None):
    globals_ = {R255: 0} if globals_ is None else globals_
    return mmo_image([*items, mmo_post(global_base, globals_), mmo_stab_end()])


def _hosted_stack_spill_fill_image(depth):
    subroutine = 0x20
    program = [
        wyde(SETL, R250, depth),
        branch(PUSHJ, R31, (subroutine - 4) // 4),
        insn(ADDI, R60, R31, 0),
        insn(GET, R50, R0, SR_O),
        insn(GET, R51, R0, SR_S),
        halt(),
        insn(SWYM),
        insn(SWYM),
        insn(SUBUI, R250, R250, 1),
        insn(GET, R0, R0, SR_J),
        branch(BZ, R250, 5),
        branch(PUSHJB, R31, 0xfffd),
        insn(PUT, SR_J, R0, R0),
        insn(ADDI, R1, R31, 0),
        insn(POP, 2, 0, 0),
        wyde(SETL, R1, 0x55),
        insn(POP, 2, 0, 0),
    ]
    return _hosted_image(program, global_base=R240, globals_={R255: 0})


def _padded_bytes(data):
    return data + b"\0" * (-len(data) % 4)


def _hosted_semihosting_console_test():
    addresses = (
        0x200,
        MMIX_DATA_SEGMENT_BASE + 0x200,
        MMIX_POOL_SEGMENT_BASE + 0x200,
        MMIX_STACK_SEGMENT_BASE + 0x200,
    )
    strings = (b"Text ", b"Data ", b"Pool ", b"Stack\n")
    program = []

    for address in addresses:
        program.extend(
            [
                *set_octa(R255, address),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
            ]
        )
    program.extend([*set_octa(R255, 0), halt()])
    pc = len(b"".join(program)) - 4
    items = [*program]
    for address, string in zip(addresses, strings):
        items.extend([mmo_loc(address), _padded_bytes(string + b"\0")])

    return MMIXSerialTest(
        "mmo-hosted-semihosting-console-segments",
        _hosted_image(items),
        pc=pc,
        output=b"".join(strings),
    )


def _hosted_semihosting_stdin_test():
    read_args = MMIX_DATA_SEGMENT_BASE + 0x200
    write_args = MMIX_POOL_SEGMENT_BASE + 0x200
    buffer = MMIX_STACK_SEGMENT_BASE + 0x200
    size = 5
    program = [
        *set_octa(R255, read_args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD, MMIX_SEMIHOSTING_STDIN),
        *set_octa(R255, write_args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE, MMIX_SEMIHOSTING_STDOUT),
        *set_octa(R255, 0),
        halt(),
    ]

    return MMIXSerialTest(
        "mmo-hosted-semihosting-stdin",
        _hosted_image(
            [
                *program,
                mmo_loc(read_args),
                struct.pack(">QQ", buffer, size),
                mmo_loc(write_args),
                struct.pack(">QQ", buffer, size),
            ]
        ),
        pc=len(b"".join(program)) - 4,
        output=b"input",
        stdin_data=b"input",
    )


def _hosted_semihosting_fgets_test():
    args = MMIX_DATA_SEGMENT_BASE + 0x240
    buffer = MMIX_STACK_SEGMENT_BASE + 0x240
    program = [
        *set_octa(R255, args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FGETS, MMIX_SEMIHOSTING_STDIN),
        *set_octa(R255, buffer),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS, MMIX_SEMIHOSTING_STDOUT),
        *set_octa(R255, 0),
        halt(),
    ]

    return MMIXSerialTest(
        "mmo-hosted-semihosting-fgets",
        _hosted_image(
            [
                *program,
                mmo_loc(args),
                struct.pack(">QQ", buffer, 8),
            ]
        ),
        pc=len(b"".join(program)) - 4,
        output=b"line\n",
        stdin_data=b"line\nextra",
    )


MMO_HOSTED_SEMIHOSTING_CONSOLE_TESTS = [
    _hosted_semihosting_console_test(),
]


MMO_HOSTED_SEMIHOSTING_STDIN_TESTS = [
    _hosted_semihosting_stdin_test(),
    _hosted_semihosting_fgets_test(),
]


def mmo_hosted_semihosting_file_test(pathname):
    open_write_args = MMIX_DATA_SEGMENT_BASE + 0x200
    write_args = MMIX_POOL_SEGMENT_BASE + 0x200
    open_read_args = MMIX_DATA_SEGMENT_BASE + 0x220
    read_args = MMIX_POOL_SEGMENT_BASE + 0x220
    pathname_address = MMIX_POOL_SEGMENT_BASE + 0x300
    write_buffer = MMIX_STACK_SEGMENT_BASE + 0x200
    read_buffer = 0x300
    contents = b"hosted-file"
    handle = MMIX_SEMIHOSTING_FIRST_FILE_HANDLE
    program = [
        *set_octa(R255, open_write_args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN, handle),
        insn(ADDI, R4, R255, 0),
        *set_octa(R255, write_args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE, handle),
        insn(ADDI, R5, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE, handle),
        insn(ADDI, R6, R255, 0),
        *set_octa(R255, open_read_args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN, handle),
        insn(ADDI, R7, R255, 0),
        *set_octa(R255, read_args),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD, handle),
        insn(ADDI, R8, R255, 0),
        *set_octa(R9, read_buffer),
        insn(LDOU, R10, R9, R0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE, handle),
        insn(ADDI, R11, R255, 0),
        *set_octa(R255, 0),
        halt(),
    ]
    items = [
        *program,
        mmo_loc(open_write_args),
        struct.pack(">QQ", pathname_address, MMIX_SEMIHOSTING_TEXT_WRITE),
        mmo_loc(open_read_args),
        struct.pack(">QQ", pathname_address, MMIX_SEMIHOSTING_TEXT_READ),
        mmo_loc(write_args),
        struct.pack(">QQ", write_buffer, len(contents)),
        mmo_loc(read_args),
        struct.pack(">QQ", read_buffer, len(contents) + 4),
        mmo_loc(pathname_address),
        _padded_bytes(str(pathname).encode("utf-8") + b"\0"),
        mmo_loc(write_buffer),
        _padded_bytes(contents),
    ]

    return MMIXMMOTest(
        "mmo-hosted-semihosting-file",
        _hosted_image(items),
        pc=len(b"".join(program)) - 4,
        regs={
            R4: 0,
            R5: 0,
            R6: 0,
            R7: 0,
            R8: MASK64 - 3,
            R10: int.from_bytes(contents[:8], "big"),
            R11: 0,
        },
        qemu_args=("-semihosting",),
    )


def mmo_hosted_debug_image(*, fill_budget=False):
    items = [jump(JMP, 0)]

    if fill_budget:
        page_size = 0x2000
        minimum_budget_pages = 128 * 1024 * 1024 // page_size
        # Text and the fallback Pool argument block consume one page each.
        for page in range(minimum_budget_pages - 2):
            items.extend(
                [
                    mmo_loc(MMIX_DATA_SEGMENT_BASE + page * page_size),
                    struct.pack(">I", page + 1),
                ]
            )
    else:
        items.extend(
            [
                mmo_loc(MMIX_DATA_SEGMENT_BASE + 0x100),
                bytes.fromhex("1122334455667788"),
            ]
        )
    return _hosted_image(items)


MMO_HOSTED_TESTS = [
    MMIXMMOTest(
        "mmo-hosted-startup",
        _hosted_image(
            [
                insn(GET, R2, R0, SR_L),
                insn(GET, R3, R0, SR_O),
                insn(GET, R4, R0, SR_S),
                insn(GET, R5, R0, SR_K),
                insn(GET, R6, R0, SR_Q),
                insn(GET, R7, R0, SR_T),
                insn(GET, R8, R0, SR_TT),
                insn(GET, R9, R0, SR_V),
                insn(GET, R10, R0, SR_G),
                insn(ADDI, R11, R0, 0),
                insn(ADDI, R12, R1, 0),
                insn(ADDI, R13, R254, 0),
                halt(),
            ],
            global_base=R254,
            globals_={R254: 0x123456789ABCDEF0, R255: 0},
        ),
        pc=0x30,
        regs={
            R0: 1,
            R1: MMIX_POOL_SEGMENT_BASE + 8,
            R2: 2,
            R3: MMIX_STACK_SEGMENT_BASE,
            R4: MMIX_STACK_SEGMENT_BASE,
            R5: MASK64,
            R6: 0,
            R7: 0x8000000500000000,
            R8: 0x8000000600000000,
            R9: 0x369C200400000000,
            R10: R254,
            R11: 1,
            R12: MMIX_POOL_SEGMENT_BASE + 8,
            R13: 0x123456789ABCDEF0,
            R254: 0x123456789ABCDEF0,
        },
    ),
    MMIXMMOTest(
        "mmo-hosted-data-store-load",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE + 0x100),
                *set_octa(R3, 0x8899AABBCCDDEEFF),
                insn(STOU, R3, R2, R0),
                insn(LDOU, R4, R2, R0),
                wyde(SETL, R255, 0),
                halt(),
            ]
        ),
        pc=0x2c,
        regs={R4: 0x8899AABBCCDDEEFF},
    ),
    MMIXMMOTest(
        "mmo-hosted-text-write-fetch",
        _hosted_image(
            [
                *set_octa(R2, 0x100),
                *set_octa(R3, int.from_bytes(wyde(SETL, R5, 0x77), "big")),
                insn(STTU, R3, R2, R0),
                insn(GO, R7, R2, R0),
                mmo_loc(0x104),
                halt(),
            ]
        ),
        pc=0x104,
        regs={R5: 0x77},
    ),
    MMIXMMOTest(
        "mmo-hosted-access-widths",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE + 0x100),
                *set_octa(R3, 0x80000000FFFFFFFF),
                insn(STBU, R3, R2, R0),
                insn(LDB, R4, R2, R0),
                insn(LDBU, R5, R2, R0),
                insn(STWUI, R3, R2, 2),
                insn(LDWI, R6, R2, 2),
                insn(LDWUI, R7, R2, 2),
                insn(STTUI, R3, R2, 4),
                insn(LDTI, R8, R2, 4),
                insn(LDTUI, R9, R2, 4),
                insn(STOUI, R3, R2, 8),
                insn(LDOUI, R10, R2, 8),
                halt(),
            ]
        ),
        pc=0x4c,
        regs={
            R4: MASK64,
            R5: 0xff,
            R6: MASK64,
            R7: 0xffff,
            R8: MASK64,
            R9: 0xffffffff,
            R10: 0x80000000FFFFFFFF,
        },
    ),
    MMIXMMOTest(
        "mmo-hosted-cswap",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE + 0x100),
                *set_octa(R3, 0x1122334455667788),
                insn(PUTI, SR_P, R0, 0),
                insn(CSWAP, R3, R2, R0),
                insn(LDOU, R4, R2, R0),
                wyde(SETL, R5, 0x99),
                insn(PUT, SR_P, R0, R5),
                wyde(SETL, R6, 0xaa),
                insn(CSWAP, R6, R2, R0),
                insn(GET, R7, R0, SR_P),
                insn(LDOU, R8, R2, R0),
                halt(),
            ]
        ),
        pc=0x44,
        regs={
            R3: 1,
            R4: 0x1122334455667788,
            R6: 0,
            R7: 0x1122334455667788,
            R8: 0x1122334455667788,
        },
    ),
    MMIXMMOTest(
        "mmo-hosted-mmio-isolation",
        _hosted_image(
            [
                *set_octa(R2, MMIX_VIRT_UART0_BASE),
                wyde(SETL, R3, 0x5a),
                insn(STBU, R3, R2, R0),
                insn(LDBU, R4, R2, R0),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={R4: 0x5a},
    ),
    MMIXMMOTest(
        "mmo-hosted-semihosting-invalid-buffer",
        _hosted_image(
            [
                *set_octa(R255, MMIX_DATA_SEGMENT_BASE + 0x200),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE,
                     MMIX_SEMIHOSTING_STDOUT),
                insn(ADDI, R4, R255, 0),
                *set_octa(R255, 0),
                halt(),
                mmo_loc(MMIX_DATA_SEGMENT_BASE + 0x200),
                struct.pack(">QQ", MMIX_HOSTED_LIMIT, 4),
            ]
        ),
        pc=0x28,
        regs={R4: MASK64 - 4},
        qemu_args=("-semihosting",),
    ),
    MMIXMMOTest(
        "mmo-hosted-stack-spill-fill-cross-page",
        _hosted_stack_spill_fill_image(48),
        pc=0x14,
        regs={
            R50: MMIX_STACK_SEGMENT_BASE,
            R51: MMIX_STACK_SEGMENT_BASE,
            R60: 0x55,
        },
    ),
    MMIXMMOTest(
        "mmo-hosted-save-unsave",
        _hosted_image(
            [SAVE_UNSAVE_ROUNDTRIP[0]],
            global_base=R32,
            globals_={R255: 0},
        ),
        pc=SAVE_UNSAVE_ROUNDTRIP[1],
        regs={
            R50: 0x11,
            R51: 0x22,
            R52: 0x33,
            R53: 0x1234,
            R54: 0x5a,
            R55: 0x6b,
            R56: 0x3ffff,
            R57: 3,
            R58: MMIX_STACK_SEGMENT_BASE,
            R59: MMIX_STACK_SEGMENT_BASE,
            R60: 0x1111222233334444,
            R61: 0x5555666677778888,
            R62: 0,
            R63: 0,
        },
    ),
]


MMO_HOSTED_FAILURE_TESTS = [
    MMIXLoaderFailure(
        "mmo-hosted-explicit-elf-abi",
        _hosted_image([halt()]),
        ("does not accept an explicit ELF startup ABI",),
        qemu_args=("-machine", "elf-startup-abi=argc-argv"),
    ),
    MMIXLoaderFailure(
        "mmo-hosted-fetch-outside-text",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE),
                insn(GO, R7, R2, R0),
            ]
        ),
        ("MMIX hosted instruction fetch outside Text",),
    ),
    MMIXLoaderFailure(
        "mmo-hosted-privileged-translation",
        _hosted_image([insn(LDVTSI, R3, R0, 0)]),
        ("MMIX hosted instruction fetch outside Text",),
    ),
    MMIXLoaderFailure(
        "mmo-hosted-runtime-exhaustion",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE),
                *set_octa(R3, 0x2000),
                wyde(SETL, R4, 0x3fff),
                wyde(SETL, R5, 1),
                insn(STBU, R5, R2, R0),
                insn(ADDU, R2, R2, R3),
                insn(SUBUI, R4, R4, 1),
                branch(BNZB, R4, 0xfffd),
            ]
        ),
        ("only 0 available",),
        qemu_args=("-m", "128M"),
    ),
]
