LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# Use the recovery-provided zlib; saves 40KB and isn't much slower
system_zlib := y

LOCAL_CFLAGS += -DRECOVERY_BUILD
LOCAL_CFLAGS += -DWRITE_BOOTIMG
LOCAL_LDFLAGS += -Wl,-dynamic-linker,/sbin/linker

LOCAL_MODULE := update-binary
LOCAL_SRC_FILES := src/main.c src/bootimg.c src/cpio.c src/override.c \
	src/splash.c src/system.c src/zimage.c sfpng/src/sfpng.c \
	sfpng/src/transform.c zlib/contrib/minizip/unzip.c \
	zlib/contrib/minizip/ioapi.c

ifneq ($(system_zlib),y)
	LOCAL_SRC_FILES += zlib/adler32_vec.c zlib/crc32.c zlib/deflate.c \
		zlib/inffast.c zlib/inflate.c zlib/inftrees.c zlib/trees.c \
		zlib/zutil.c
endif

LOCAL_CFLAGS += -O3 -s -fpie -flto -fno-fat-lto-objects -flto-partition=none \
	-fdata-sections -ffunction-sections -I. -std=gnu11 -Wall \
	-Wno-parentheses -pedantic -DIOAPI_NO_64
LOCAL_CFLAGS += -march=armv7-a -mtune=cortex-a15

ifneq ($(system_zlib),y)
	LOCAL_CFLAGS += -DHAVE_HIDDEN -DZLIB_CONST -DDYNAMIC_CRC_TABLE \
		-DUNALIGNED_OK
endif

LOCAL_LDFLAGS += $(LOCAL_CFLAGS) -pie -Wl,-gc-sections
ifeq ($(system_zlib),y)
	LOCAL_LDLIBS := -lz
endif

LOCAL_ARM_MODE := thumb
LOCAL_ARM_NEON := true

TARGET_ARCH := arm
TARGET_ARCH_ABI := armeabi-v7a
TARGET_PLATFORM := android-19

include $(BUILD_EXECUTABLE)
