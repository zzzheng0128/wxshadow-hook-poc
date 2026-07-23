KP_DIR ?= ../references/KernelPatch
ROOT_DIR := ..
PIXEL_SRC ?= $(ROOT_DIR)/build/pixel7-kernel-gs
PIXEL_OUT ?= $(ROOT_DIR)/build/pixel7-kernel-out
NDK ?= /Users/ivory/Library/Android/sdk/ndk/29.0.14206865
CC := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android34-clang
OBJCOPY := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objcopy

INCLUDE_DIRS := include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))
CFLAGS := -O2 -Wall -Wextra -Werror -Wno-unused-parameter -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
RAW_INCLUDE_FLAGS := \
	-I. \
	-Iraw-compat/include \
	-I$(PIXEL_SRC)/arch/arm64/include \
	-I$(PIXEL_OUT)/arch/arm64/include/generated \
	-I$(PIXEL_SRC)/include \
	-I$(PIXEL_OUT)/include \
	-I$(PIXEL_OUT)/include/generated \
	-I$(PIXEL_SRC)/arch/arm64/include/uapi \
	-I$(PIXEL_OUT)/arch/arm64/include/generated/uapi \
	-I$(PIXEL_SRC)/include/uapi \
	-I$(PIXEL_OUT)/include/generated/uapi \
	-include $(PIXEL_SRC)/include/linux/kconfig.h
RAW_CFLAGS := -O2 -D__KERNEL__ -DMODULE -Wall -Wextra -Werror -Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare -Wno-unused-function -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables

BUILD_DIR := build
MODULE := $(BUILD_DIR)/r0lab-m1.kpm

all: $(MODULE)

$(MODULE): $(BUILD_DIR)/r0lab.o $(BUILD_DIR)/r0lab_raw_compat.o
	@mkdir -p $(BUILD_DIR)
	$(CC) -r -nostdlib -o $@ $^
	$(OBJCOPY) \
		--remove-section=.altinstructions \
		--remove-section=.rela.altinstructions \
		--remove-section=__jump_table \
		--remove-section=.rela__jump_table \
		$@

$(BUILD_DIR)/r0lab.o: r0lab.c r0lab.lds r0lab_raw.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) -I. -c -o $@ $<

$(BUILD_DIR)/r0lab_raw_compat.o: r0lab_raw_compat.c r0lab_raw.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(RAW_CFLAGS) $(RAW_INCLUDE_FLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
