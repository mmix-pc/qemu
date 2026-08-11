#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def _pad_to_address(image, address):
    if len(image) > address:
        raise ValueError("image prefix overlaps data")
    if (address - len(image)) % 4 != 0:
        raise ValueError("data address is not instruction-aligned")
    return image + insn(SWYM, 0, 0, 0) * ((address - len(image)) // 4)


def _set_args3(arg_address, arg2, arg3):
    return [
        *set_octa(R1, arg_address),
        *set_octa(R2, arg2),
        *set_octa(R3, arg3),
        insn(STOUI, R2, R1, 0),
        insn(STOUI, R3, R1, 8),
        *set_octa(R255, arg_address),
    ]


def argv_layout_program(argv_indices, byte_checks=()):
    program = [
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_L),
        *set_octa(R1, MMIX_POOL_SEGMENT_BASE),
        insn(LDOU, R2, R1, R0),
    ]

    for reg, index in argv_indices:
        program.append(insn(LDOUI, reg, R1, 8 * (index + 1)))
    for reg, base_reg, offset in byte_checks:
        program.append(insn(LDBUI, reg, base_reg, offset))
    program.extend([wyde(SETL, R255, 0), halt()])
    return b"".join(program)


def _fopen_program(pathname, mode, *, handle=MMIX_SEMIHOSTING_FIRST_FILE_HANDLE,
                   close=False):
    arg_address = 0x100
    pathname_address = 0x120
    pathname_bytes = str(pathname).encode("utf-8") + b"\0"
    arg_block = struct.pack(">QQ", pathname_address, mode)

    instructions = [
        *set_octa(R255, arg_address),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN, handle),
        insn(ADDI, R32, R255, 0),
    ]
    if close:
        instructions.extend(
            [
                insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE, handle),
                insn(ADDI, R33, R255, 0),
            ]
        )
    instructions.extend([*set_octa(R255, 0), halt()])

    program = b"".join(instructions)
    program = _pad_to_address(program, arg_address) + arg_block
    program = _pad_to_address(program, pathname_address) + pathname_bytes
    return program


def fopen_fclose_test(pathname):
    program = _fopen_program(pathname, MMIX_SEMIHOSTING_TEXT_READ, close=True)

    return MMIXTest(
        "semihosting-fopen-fclose",
        program,
        pc=0x30,
        regs={R32: 0, R33: 0},
    )


def fopen_failure_test(name, pathname, mode,
                       handle=MMIX_SEMIHOSTING_FIRST_FILE_HANDLE):
    program = _fopen_program(pathname, mode, handle=handle)

    return MMIXTest(
        name,
        program,
        pc=0x28,
        regs={R32: MASK64},
    )


def fclose_failure_test(name, handle):
    program = b"".join(
        [
            insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE, handle),
            insn(ADDI, R32, R255, 0),
            *set_octa(R255, 0),
            halt(),
        ]
    )

    return MMIXTest(
        name,
        program,
        pc=0x18,
        regs={R32: MASK64},
    )


def fread_file_test(pathname, request_size, expected_bytes, expected_return):
    arg_address = 0x180
    buffer_address = 0x1c0
    pathname_address = 0x200
    pathname_bytes = str(pathname).encode("utf-8") + b"\0"

    instructions = [
        *_set_args3(arg_address, pathname_address,
                    MMIX_SEMIHOSTING_TEXT_READ),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R32, R255, 0),
        *_set_args3(arg_address, buffer_address, request_size),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R33, R255, 0),
        *set_octa(R4, buffer_address),
    ]
    for i in range(len(expected_bytes)):
        instructions.append(insn(LDBUI, R34 + i, R4, i))
    instructions.extend(
        [
            insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE,
                 MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
            insn(ADDI, R40, R255, 0),
            *set_octa(R255, 0),
            halt(),
        ]
    )

    prefix = b"".join(instructions)
    program = _pad_to_address(prefix, pathname_address) + pathname_bytes
    regs = {R32: 0, R33: expected_return, R40: 0}
    regs.update({R34 + i: byte for i, byte in enumerate(expected_bytes)})

    return MMIXTest(
        f"semihosting-fread-{request_size}",
        program,
        pc=len(prefix) - 4,
        regs=regs,
    )


def fread_failure_test(name, handle, buffer_address, size):
    arg_address = 0x100
    prefix = b"".join(
        [
            *_set_args3(arg_address, buffer_address, size),
            insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD, handle),
            insn(ADDI, R32, R255, 0),
            *set_octa(R255, 0),
            halt(),
        ]
    )

    return MMIXTest(
        name,
        prefix,
        pc=len(prefix) - 4,
        regs={R32: (MASK64 - size)},
    )


def fread_stdin_test(name, request_size, input_data, expected_bytes,
                     expected_return):
    arg_address = 0x100
    buffer_address = 0x140
    instructions = [
        *_set_args3(arg_address, buffer_address, request_size),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD, MMIX_SEMIHOSTING_STDIN),
        insn(ADDI, R32, R255, 0),
        *set_octa(R4, buffer_address),
    ]

    for i in range(len(expected_bytes)):
        instructions.append(insn(LDBUI, R33 + i, R4, i))
    instructions.extend([*set_octa(R255, 0), halt()])

    prefix = b"".join(instructions)
    regs = {R32: expected_return}
    regs.update({R33 + i: byte for i, byte in enumerate(expected_bytes)})

    return MMIXTest(
        name,
        prefix,
        pc=len(prefix) - 4,
        regs=regs,
        stdin_data=input_data,
    )


def fread_stdin_bad_buffer_test(buffer_address, size, input_data):
    test = fread_failure_test(
        "semihosting-fread-stdin-bad-buffer",
        MMIX_SEMIHOSTING_STDIN,
        buffer_address,
        size,
    )

    return dataclasses.replace(test, stdin_data=input_data)


def fread_standard_handle_failure_test(name, handle, size):
    return fread_failure_test(
        name,
        handle,
        0x140,
        size,
    )


def fread_bad_buffer_test(pathname, buffer_address, size):
    arg_address = 0x180
    pathname_address = 0x200
    pathname_bytes = str(pathname).encode("utf-8") + b"\0"

    instructions = [
        *_set_args3(arg_address, pathname_address,
                    MMIX_SEMIHOSTING_TEXT_READ),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R32, R255, 0),
        *_set_args3(arg_address, buffer_address, size),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R33, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R34, R255, 0),
        *set_octa(R255, 0),
        halt(),
    ]

    prefix = b"".join(instructions)
    program = _pad_to_address(prefix, pathname_address) + pathname_bytes

    return MMIXTest(
        "semihosting-fread-bad-buffer",
        program,
        pc=len(prefix) - 4,
        regs={R32: 0, R33: MASK64 - size, R34: 0},
    )


def fwrite_file_test(pathname, data):
    arg_address = 0x180
    buffer_address = 0x1c0
    pathname_address = 0x200
    pathname_bytes = str(pathname).encode("utf-8") + b"\0"

    instructions = [
        *_set_args3(arg_address, pathname_address,
                    MMIX_SEMIHOSTING_TEXT_WRITE),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R32, R255, 0),
        *_set_args3(arg_address, buffer_address, len(data)),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R33, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R34, R255, 0),
        *set_octa(R255, 0),
        halt(),
    ]

    prefix = b"".join(instructions)
    program = _pad_to_address(prefix, buffer_address) + data
    program = _pad_to_address(program, pathname_address) + pathname_bytes

    return MMIXTest(
        "semihosting-fwrite-file",
        program,
        pc=len(prefix) - 4,
        regs={R32: 0, R33: 0, R34: 0},
    )


def fwrite_readonly_file_test(pathname, data):
    arg_address = 0x180
    buffer_address = 0x1c0
    pathname_address = 0x200
    pathname_bytes = str(pathname).encode("utf-8") + b"\0"

    instructions = [
        *_set_args3(arg_address, pathname_address,
                    MMIX_SEMIHOSTING_TEXT_READ),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R32, R255, 0),
        *_set_args3(arg_address, buffer_address, len(data)),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R33, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R34, R255, 0),
        *set_octa(R255, 0),
        halt(),
    ]

    prefix = b"".join(instructions)
    program = _pad_to_address(prefix, buffer_address) + data
    program = _pad_to_address(program, pathname_address) + pathname_bytes

    return MMIXTest(
        "semihosting-fwrite-readonly-file",
        program,
        pc=len(prefix) - 4,
        regs={R32: 0, R33: MASK64 - len(data), R34: 0},
    )


def fwrite_console_test(name, handle, data):
    arg_address = 0x100
    buffer_address = 0x140
    instructions = [
        *_set_args3(arg_address, buffer_address, len(data)),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE, handle),
        *set_octa(R255, 0),
        halt(),
    ]
    prefix = b"".join(instructions)
    program = _pad_to_address(prefix, buffer_address) + data

    return MMIXSerialTest(
        name,
        program,
        pc=len(prefix) - 4,
        output=data,
    )


def fwrite_failure_test(name, handle, buffer_address, size):
    arg_address = 0x100
    prefix = b"".join(
        [
            *_set_args3(arg_address, buffer_address, size),
            insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE, handle),
            insn(ADDI, R32, R255, 0),
            *set_octa(R255, 0),
            halt(),
        ]
    )

    return MMIXTest(
        name,
        prefix,
        pc=len(prefix) - 4,
        regs={R32: (MASK64 - size)},
    )


def fseek_ftell_test(pathname):
    arg_address = 0x180
    buffer_address = 0x1c0
    pathname_address = 0x200
    pathname_bytes = str(pathname).encode("utf-8") + b"\0"

    instructions = [
        *_set_args3(arg_address, pathname_address,
                    MMIX_SEMIHOSTING_TEXT_READ),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FOPEN,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R32, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R33, R255, 0),
        *set_octa(R255, 3),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FSEEK,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R34, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R35, R255, 0),
        *_set_args3(arg_address, buffer_address, 1),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R36, R255, 0),
        *set_octa(R4, buffer_address),
        insn(LDBUI, R37, R4, 0),
        *set_octa(R255, MASK64),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FSEEK,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R38, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R39, R255, 0),
        *set_octa(R255, MASK64 - 1),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FSEEK,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R40, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R41, R255, 0),
        insn(TRAP, 0, MMIX_SEMIHOSTING_FCLOSE,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        insn(ADDI, R42, R255, 0),
        *set_octa(R255, 0),
        halt(),
    ]

    prefix = b"".join(instructions)
    program = _pad_to_address(prefix, pathname_address) + pathname_bytes

    return MMIXTest(
        "semihosting-fseek-ftell",
        program,
        pc=len(prefix) - 4,
        regs={
            R32: 0,
            R33: 0,
            R34: 0,
            R35: 3,
            R36: 0,
            R37: ord("d"),
            R38: 0,
            R39: 6,
            R40: 0,
            R41: 5,
            R42: 0,
        },
    )


def fseek_failure_test(name, handle, offset):
    prefix = b"".join(
        [
            *set_octa(R255, offset),
            insn(TRAP, 0, MMIX_SEMIHOSTING_FSEEK, handle),
            insn(ADDI, R32, R255, 0),
            *set_octa(R255, 0),
            halt(),
        ]
    )

    return MMIXTest(
        name,
        prefix,
        pc=len(prefix) - 4,
        regs={R32: MASK64},
    )


def ftell_failure_test(name, handle):
    prefix = b"".join(
        [
            insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL, handle),
            insn(ADDI, R32, R255, 0),
            *set_octa(R255, 0),
            halt(),
        ]
    )

    return MMIXTest(
        name,
        prefix,
        pc=len(prefix) - 4,
        regs={R32: MASK64},
    )


SEMIHOSTING_TESTS = [
    MMIXTest(
        "semihosting-halt",
        insn(TRAP, 0, MMIX_SEMIHOSTING_HALT, 0),
        pc=0x00,
        regs={},
    ),
    MMIXTest(
        "semihosting-halt-exit-status",
        b"".join(
            [
                wyde(SETL, R255, 42),
                insn(TRAP, 0, MMIX_SEMIHOSTING_HALT, 0),
            ]
        ),
        pc=0x04,
        regs={R255: 42},
        exit_status=42,
    ),
    MMIXTest(
        "semihosting-argv-one-argument",
        argv_layout_program(
            [(R3, 0), (R4, 1)],
            [(R10, R3, 0), (R11, R3, 4)],
        ),
        pc=0x34,
        regs={
            R32: 1,
            R33: MMIX_POOL_SEGMENT_BASE + 8,
            R34: 2,
            R2: MMIX_POOL_SEGMENT_BASE + 0x20,
            R3: MMIX_POOL_SEGMENT_BASE + 0x18,
            R4: 0,
            R10: ord("p"),
            R11: 0,
        },
        qemu_args=("-semihosting-config", "enable=on,arg=prog"),
    ),
    MMIXTest(
        "semihosting-argv-multiple-arguments",
        argv_layout_program(
            [(R3, 0), (R4, 1), (R5, 2), (R6, 3)],
            [
                (R10, R3, 0), (R11, R3, 4),
                (R12, R4, 0), (R13, R4, 3),
                (R14, R5, 0), (R15, R5, 3),
            ],
        ),
        pc=0x4c,
        regs={
            R32: 3,
            R33: MMIX_POOL_SEGMENT_BASE + 8,
            R34: 2,
            R2: MMIX_POOL_SEGMENT_BASE + 0x40,
            R3: MMIX_POOL_SEGMENT_BASE + 0x28,
            R4: MMIX_POOL_SEGMENT_BASE + 0x30,
            R5: MMIX_POOL_SEGMENT_BASE + 0x38,
            R6: 0,
            R10: ord("p"),
            R11: 0,
            R12: ord("o"),
            R13: 0,
            R14: ord("t"),
            R15: 0,
        },
        qemu_args=("-semihosting-config",
                   "enable=on,arg=prog,arg=one,arg=two"),
    ),
    MMIXTest(
        "semihosting-argv-long-argument",
        argv_layout_program(
            [(R3, 0), (R4, 1), (R5, 2)],
            [(R10, R4, 0), (R11, R4, 16), (R12, R4, 17)],
        ),
        pc=0x3c,
        regs={
            R32: 2,
            R33: MMIX_POOL_SEGMENT_BASE + 8,
            R34: 2,
            R2: MMIX_POOL_SEGMENT_BASE + 0x40,
            R3: MMIX_POOL_SEGMENT_BASE + 0x20,
            R4: MMIX_POOL_SEGMENT_BASE + 0x28,
            R5: 0,
            R10: ord("a"),
            R11: ord("q"),
            R12: 0,
        },
        qemu_args=("-semihosting-config",
                   "enable=on,arg=prog,arg=abcdefghijklmnopq"),
    ),
]
