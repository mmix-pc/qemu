MMIX ``virt`` firmware
======================

This directory contains the source for ``mmix-virt.bin``, the minimal boot
firmware for QEMU's MMIX ``virt`` machine.  Firmware version 0.1.0 implements
reset entry, validated ``fw_cfg`` and FDT discovery, MMIX Linux ELF loading,
FDT, initrd, and command-line placement, SMP kernel handoff, early UART
diagnostics, and a deterministic panic stop.

The source is distributed under GPL-2.0-or-later.  The complete license text
is in the QEMU repository's top-level ``COPYING`` file.

The checked-in image is built with the LLVM MMIX target.  A normal QEMU build
installs that image and does not require the MMIX compiler.  Maintainers with
an MMIX-capable LLVM installation can reproduce it with::

  make CLANG=/path/to/clang \
       LD_LLD=/path/to/ld.lld \
       LLVM_OBJCOPY=/path/to/llvm-objcopy
  make check CLANG=/path/to/clang \
       LD_LLD=/path/to/ld.lld \
       LLVM_OBJCOPY=/path/to/llvm-objcopy

``make update`` deliberately replaces the checked-in image after a reviewed
source or toolchain change.  The image filename and embedded version and
compatible-machine string are part of its release provenance; they are not a
stable guest ABI.
