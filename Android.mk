LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := arc_dark
LOCAL_SRC_FILES := ArcDarkModule.cpp lsplt/lsplt.cc lsplt/elf_util.cc
LOCAL_STATIC_LIBRARIES := libcxx
# LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)

include libcxx/Android.mk
