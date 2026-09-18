# MMIX virt firmware maintainer build
#
# SPDX-License-Identifier: GPL-2.0-or-later

BUILD_DIR ?= build
CLANG ?= clang
LD_LLD ?= ld.lld
LLVM_OBJCOPY ?= llvm-objcopy

TARGET = $(BUILD_DIR)/mmix-virt.bin
ELF = $(BUILD_DIR)/mmix-virt.elf
OBJECTS = $(BUILD_DIR)/start.o $(BUILD_DIR)/main.o
CHECKED_IN_IMAGE = ../mmix-virt.bin

COMMON_FLAGS = --target=mmix-unknown-elf -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic
CFLAGS = $(COMMON_FLAGS) -O2 -Wall -Wextra -Werror
ASFLAGS = $(COMMON_FLAGS)

.PHONY: all check clean update

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/start.o: start.S | $(BUILD_DIR)
	$(CLANG) $(ASFLAGS) -c -o $@ $<

$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c -o $@ $<

$(ELF): $(OBJECTS) mmix-virt.ld
	$(LD_LLD) -nostdlib -static -T mmix-virt.ld -o $@ $(OBJECTS)

$(TARGET): $(ELF)
	$(LLVM_OBJCOPY) -O binary $< $@

check: $(TARGET)
	cmp $(TARGET) $(CHECKED_IN_IMAGE)

update: $(TARGET)
	install -m 0644 $(TARGET) $(CHECKED_IN_IMAGE)

clean:
	rm -rf $(BUILD_DIR)
