# MMIX TCG Tests

This directory contains pytest-based softmmu tests for the MMIX target.

The tests run `qemu-system-mmix` with raw instruction images, ELF images, or
hosted `.mmo` objects and check CPU state, loader failures, serial output, and
semihosting console output. Some fixtures are generated from MMIXAL sources
when `mmixal` is available.

Semihosted runtime tests wrap synthetic programs as hosted MMO objects, use
separate pytest entry points, and pass `-semihosting` explicitly. Other
raw-image, serial, and loader tests do not enable semihosting by default.

Tests that need deterministic semihosting `StdIn` should use the stdio chardev
helper in `lib.qemu` and pass input bytes through the test case. This keeps
stdin setup explicit and avoids manual terminal interaction.

## Dependencies

Required Python packages:

- `pytest`
- `bitstruct`
- `construct`

Required binary:

- `qemu-system-mmix`

Optional MMIXWare binaries:

- `mmixal`
- `mmix`

Tests that need `mmixal` are skipped when it is not found in `PATH`.
MMIXWare output-comparison tests are skipped when `mmix` is not found.

## Running

From the QEMU source tree, the normal TCG entry point is:

```sh
make -C build/tests/tcg/mmix-softmmu run
```

To run the pytest wrapper directly:

```sh
tests/tcg/mmix/run-mmix-tests.py \
    --qemu build/qemu-system-mmix \
    --workdir build/tests/tcg/mmix-softmmu/mmix-tests
```

The `--workdir` directory is used for generated images, logs, serial output,
and semihosting console output.
