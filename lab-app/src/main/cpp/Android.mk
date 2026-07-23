LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := labprobe
LOCAL_SRC_FILES := labprobe.c
LOCAL_CFLAGS := -Wall -Wextra -Werror -fno-omit-frame-pointer
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
