
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := psxbridge

LOCAL_SRC_FILES := psxbridge.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)

LOCAL_CFLAGS := -std=c11 -Wall

LOCAL_LDLIBS := -landroid -llog -ldl

include $(BUILD_SHARED_LIBRARY)
