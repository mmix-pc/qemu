# MMIX virt firmware maintainer build
#
# SPDX-License-Identifier: GPL-2.0-or-later

BUILD_DIR ?= build
CLANG ?= clang
LD_LLD ?= ld.lld
LLVM_OBJCOPY ?= llvm-objcopy
HOST_CC ?= cc

TARGET = $(BUILD_DIR)/mmix-virt.bin
ELF = $(BUILD_DIR)/mmix-virt.elf
OBJECTS = $(BUILD_DIR)/start.o $(BUILD_DIR)/main.o \
	$(BUILD_DIR)/fw-cfg.o $(BUILD_DIR)/fdt.o $(BUILD_DIR)/runtime.o
CHECKED_IN_IMAGE = ../mmix-virt.bin
DISCOVERY_TEST = $(BUILD_DIR)/test-discovery

COMMON_FLAGS = --target=mmix-unknown-elf -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic
CFLAGS = $(COMMON_FLAGS) -O2 -Wall -Wextra -Werror
ASFLAGS = $(COMMON_FLAGS)
HOST_CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -DMMIX_FIRMWARE_TEST

.PHONY: all check clean update

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/start.o: start.S | $(BUILD_DIR)
	$(CLANG) $(ASFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.c firmware.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c -o $@ $<

$(ELF): $(OBJECTS) mmix-virt.ld
	$(LD_LLD) -nostdlib -static -T mmix-virt.ld -o $@ $(OBJECTS)

$(TARGET): $(ELF)
	$(LLVM_OBJCOPY) -O binary $< $@

$(DISCOVERY_TEST): test-discovery.c fw-cfg.c fdt.c firmware.h | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ test-discovery.c fw-cfg.c fdt.c

check: $(TARGET) $(DISCOVERY_TEST)
	cmp $(TARGET) $(CHECKED_IN_IMAGE)
	$(DISCOVERY_TEST)

update: $(TARGET)
	install -m 0644 $(TARGET) $(CHECKED_IN_IMAGE)

clean:
	rm -rf $(BUILD_DIR)
