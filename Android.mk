LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := arc_dark
LOCAL_SRC_FILES := ArcDarkModule.cpp \
    third_party/lsplt/lsplt.cc \
    third_party/lsplt/elf_util.cc

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/third_party \
    $(LOCAL_PATH)/third_party/lsplt/include \
    $(LOCAL_PATH)/third_party/json/single_include

# nlohmann/json builds against -fno-exceptions; error paths abort instead
LOCAL_CPPFLAGS += -DJSON_NOEXCEPTION
LOCAL_STATIC_LIBRARIES := libcxx
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)

include $(LOCAL_PATH)/third_party/libcxx/Android.mk
