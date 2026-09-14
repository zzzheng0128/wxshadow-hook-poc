KP_DIR ?= ../references/KernelPatch
ROOT_DIR := ..

# Target device selection. Each target maps to a pinned kernel source tree that
# matches the device's running kernel. The raw compat layer compiles against
# these target headers (struct mm_struct / pte_t ABI is kernel-version bound).
#
#   oriole -> Pixel 6 (Android 14 GKI, kernel 6.1.99)
#   redfin -> Pixel 5 (Android 14 legacy msm, kernel 4.19.278)
#   panther -> Pixel 7 (Android 14 GKI, kernel 5.10.x) [original target]
TARGET ?= oriole

# Kernel source trees (vendored in-repo under references/kernel-src, gitignored).
# Only the header trees are needed (include/ + arch/arm64/include, with their
# generated/ subdirs) for the raw compat ABI; the full kernel source is not
# required. Override KERNEL_SRC_ROOT to point at a full checkout if desired.
KERNEL_SRC_ROOT ?= $(ROOT_DIR)/references/kernel-src

ifeq ($(TARGET),oriole)
  KERNEL_SRC := $(KERNEL_SRC_ROOT)/kernel-common-6.1
  KERNEL_OUT := $(KERNEL_SRC)
  MODULE_NAME := r0lab-m1-oriole
  MM_LOCK_FIELD := mmap_lock
else ifeq ($(TARGET),redfin)
  KERNEL_SRC := $(KERNEL_SRC_ROOT)/kernel-msm-4.19
  KERNEL_OUT := $(KERNEL_SRC)
  MODULE_NAME := r0lab-m1-redfin
  MM_LOCK_FIELD := mmap_sem
else ifeq ($(TARGET),panther)
  KERNEL_SRC := $(ROOT_DIR)/build/pixel7-kernel-gs
  KERNEL_OUT := $(ROOT_DIR)/build/pixel7-kernel-out
  MODULE_NAME := r0lab-m1
  MM_LOCK_FIELD := mmap_lock
else
  $(error unknown TARGET: $(TARGET) (want oriole|redfin|panther))
endif

# Toolchain. Two supported profiles:
#   - Darwin NDK (developer Mac): NDK set to the Android NDK root.
#   - Linux cross GCC: CC_PREFIX=aarch64-linux-gnu-
#
# Pick automatically when possible, but allow explicit override.
NDK ?= /Users/freeman/Library/Android/sdk/ndk/29.0.14206865
CC_PREFIX ?=

ifeq ($(CC_PREFIX),)
  ifeq ($(shell uname -s),Darwin)
    CC := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android34-clang
    OBJCOPY := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objcopy
  else
    CC := aarch64-linux-gnu-gcc
    OBJCOPY := aarch64-linux-gnu-objcopy
  endif
else
  CC := $(CC_PREFIX)gcc
  OBJCOPY := $(CC_PREFIX)objcopy
endif

INCLUDE_DIRS := include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))

# -ffixed-x18 is mandatory: the target kernels enable CONFIG_SHADOW_CALL_STACK,
# so x18 is the kernel shadow call stack pointer. Any module function that
# spills x18 clobbers the shadow stack and panics the device (SP/PC alignment).
CFLAGS := -O2 -Wall -Wextra -Werror -Wno-unused-parameter -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -ffixed-x18

RAW_INCLUDE_FLAGS := \
	-I. \
	-Iraw-compat/include \
	-I$(KERNEL_SRC)/arch/arm64/include \
	-I$(KERNEL_OUT)/arch/arm64/include/generated \
	-I$(KERNEL_SRC)/include \
	-I$(KERNEL_OUT)/include \
	-I$(KERNEL_OUT)/include/generated \
	-I$(KERNEL_SRC)/arch/arm64/include/uapi \
	-I$(KERNEL_OUT)/arch/arm64/include/generated/uapi \
	-I$(KERNEL_SRC)/include/uapi \
	-I$(KERNEL_OUT)/include/generated/uapi \
	-include $(KERNEL_SRC)/include/linux/kconfig.h

RAW_CFLAGS := -O2 -D__KERNEL__ -DMODULE -DR0LAB_RAW_MM_LOCK_FIELD=$(MM_LOCK_FIELD) -Wall -Wextra -Werror -Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare -Wno-unused-function -Wno-type-limits -Wno-array-bounds -Wno-strict-aliasing -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -ffixed-x18

BUILD_DIR := build
MODULE := $(BUILD_DIR)/$(MODULE_NAME).kpm

all: $(MODULE)

$(MODULE): $(BUILD_DIR)/r0lab.o $(BUILD_DIR)/r0lab_raw_compat.o
	@mkdir -p $(BUILD_DIR)
	$(CC) -r -nostdlib -o $@ $^
	$(OBJCOPY) \
		--remove-section=.altinstructions \
		--remove-section=.rela.altinstructions \
		--remove-section=.altinstr_replacement \
		--remove-section=__jump_table \
		--remove-section=.rela__jump_table \
		--remove-section=__bug_table \
		--remove-section=.rela__bug_table \
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
