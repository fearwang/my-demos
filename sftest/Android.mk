bootanimation_CommonCFlags = -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES
bootanimation_CommonCFlags += -Wall -Werror -Wunused -Wunreachable-code

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CFLAGS += ${bootanimation_CommonCFlags}

LOCAL_CFLAGS += -Wunreachable-code -Wno-unused-parameter -Wno-unused-variable

LOCAL_CFLAGS += -Wno-non-virtual-dtor -Wno-maybe-uninitialized -Wno-parentheses

LOCAL_CPPFLAGS += -std=c++17 -Wno-conversion-null

LOCAL_C_INCLUDES += \
    external/tinyalsa/include \
    frameworks/wilhelm/include \
    frameworks/base/libs/hwui \
    frameworks/base/libs/hwui/renderthread \
    frameworks/base/libs/hwui/hwui \
    frameworks/base/libs/hwui/renderstate \
    frameworks/base/libs/hwui/utils \
    frameworks/base/libs/hwui/thread \
    frameworks/base/libs/hwui/service \
    frameworks/base/libs/hwui/font \
    frameworks/base/libs/hwui/debug \
    frameworks/base/libs/hwui/pipeline \
    frameworks/base/libs/hwui/private \
    frameworks/base/libs/hwui/protos \
    external/skia/include/private \
    external/skia/include/core \
	external/skia/src/codec \
	external/skia/src/core \
	external/skia/src/effects \
	external/skia/src/image \
	external/skia/src/images \
	frameworks/base/media/jni \
	frameworks/minikin/include \
	external/harfbuzz_ng/src \
	bionic/libc/private \
	libcore/include \
    system/media/camera/include \
    system/media/private/camera/include
	


LOCAL_SHARED_LIBRARIES := \
    libOpenSLES \
    libbase \
    libutils \
    libcutils \
    liblog \
    libandroidfw \
    libbinder \
    libui \
    libhwui \
    libEGL \
    libGLESv1_CM \
    libgui \
    libtinyalsa \
	libandroid

LOCAL_SRC_FILES += \
    sftest_main.cpp \
    sftest.cpp

LOCAL_MODULE:= sftest 

ifdef TARGET_32_BIT_SURFACEFLINGER
LOCAL_32_BIT_ONLY := true
endif

include $(BUILD_EXECUTABLE)

