# MMIX Virt Firmware Test Fixture

The fixture is a standalone validation program for the MMIX `virt` firmware
contract. It is not production firmware and its RAM destinations are not
platform ABI.

`build-fixtures.py` is the reviewable source for both checked-in binaries. It
uses only the Python standard library and is licensed under
GPL-2.0-or-later. Regenerate the exact binaries with:

```sh
python3 tests/tcg/mmix/data/firmware/build-fixtures.py
```

Verify the checked-in binaries without changing them with:

```sh
python3 tests/tcg/mmix/data/firmware/build-fixtures.py --check
```

The deterministic test gate consumes the checked-in binaries and therefore
does not require Python, MMIXAL, or a cross compiler at test execution time.
The generated firmware enumerates fw_cfg, copies `etc/fdt` and
`opt/mmix/kernel` into ordinary RAM, releases secondary CPUs through a shared
RAM barrier, and enters the next stage using the documented register handoff.
