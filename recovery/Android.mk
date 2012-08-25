LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(TARGET_RECOVERY_HAS_EFUSE),)
TARGET_RECOVERY_HAS_EFUSE := true
endif

ifeq ($(TARGET_RECOVERY_HAS_SDCARD_ONLY),)
TARGET_RECOVERY_HAS_SDCARD_ONLY := true
endif
commands_recovery_local_path := $(LOCAL_PATH)

LOCAL_SRC_FILES := \
    recovery.c \
    bootloader.c \
    install.c \
    roots.c \
    ui.c \
    verifier.c \
    ubi/ubiutils-common.c \
    ubi/libubi.c

LOCAL_C_INCLUDES += external/fw_env/ \
    external/mtd-utils/include/ \
    external/mtd-utils/ubi-utils/include


LOCAL_MODULE := recovery

LOCAL_FORCE_STATIC_EXECUTABLE := true

RECOVERY_API_VERSION := 3
LOCAL_CFLAGS += -DRECOVERY_API_VERSION=$(RECOVERY_API_VERSION)

LOCAL_CFLAGS += -DUDEV_SETTLE_HACK

LOCAL_STATIC_LIBRARIES :=

ifeq ($(TARGET_USERIMAGES_USE_EXT4), true)
LOCAL_CFLAGS += -DUSE_EXT4
LOCAL_C_INCLUDES += system/extras/ext4_utils
LOCAL_STATIC_LIBRARIES += libext4_utils libz
endif

# This binary is in the recovery ramdisk, which is otherwise a copy of root.
# It gets copied there in config/Makefile.  LOCAL_MODULE_TAGS suppresses
# a (redundant) copy of the binary in /system/bin for user builds.
# TODO: Build the ramdisk image in a more principled way.

LOCAL_MODULE_TAGS := eng

ifeq ($(TARGET_RECOVERY_UI_LIB),)
  LOCAL_SRC_FILES += default_recovery_ui.c
else
  LOCAL_STATIC_LIBRARIES += $(TARGET_RECOVERY_UI_LIB)
endif
LOCAL_STATIC_LIBRARIES += libext4_utils libz
LOCAL_STATIC_LIBRARIES += libminzip libunz libmtdutils libmincrypt
LOCAL_STATIC_LIBRARIES += libminui libpixelflinger_static libpng libcutils
LOCAL_STATIC_LIBRARIES += libstdc++ libc libfw_env

LOCAL_LDFLAGS += $(TARGET_RECOVERY_LDFLAGS)

ifeq ($(TARGET_RECOVERY_VOLUMEDOWN_SELECT),true)
LOCAL_CFLAGS += -DRECOVERY_VOLUMEDOWN_SELECT
endif

ifeq ($(TARGET_RECOVERY_TOGGLE_DISPLAY),true)
LOCAL_CFLAGS += -DRECOVERY_TOGGLE_DISPLAY
endif

ifeq ($(TARGET_RECOVERY_MENU_LOOP_SELECT),true)
LOCAL_CFLAGS += -DRECOVERY_MENU_LOOP_SELECT
endif

ifeq ($(TARGET_RECOVERY_HAS_MEDIA),true)
LOCAL_CFLAGS += -DRECOVERY_HAS_MEDIA
endif # TARGET_RECOVERY_HAS_MEDIA == true

ifeq ($(TARGET_RECOVERY_MEDIA_LABEL),)
LOCAL_CFLAGS += -DRECOVERY_MEDIA_LABEL="\"android\""
else
LOCAL_CFLAGS += -DRECOVERY_MEDIA_LABEL=$(TARGET_RECOVERY_MEDIA_LABEL)
endif

#ifeq ($(TARGET_RECOVERY_HAS_EFUSE),true)
LOCAL_CFLAGS += -DRECOVERY_HAS_EFUSE

LOCAL_SRC_FILES += \
	efuse.c
#endif # TARGET_RECOVERY_HAS_EFUSE == true

ifeq ($(TARGET_RECOVERY_HAS_FACTORY_TEST),true)
LOCAL_CFLAGS += -DRECOVERY_HAS_FACTORY_TEST
endif
ifeq ($(TARGET_RECOVERY_HAS_SDCARD_ONLY),true)
LOCAL_CFLAGS += -DRECOVERY_HAS_SDCARD_ONLY
endif # TARGET_RECOVERY_HAS_SDCARD_ONLY

LOCAL_C_INCLUDES += system/extras/ext4_utils

include $(BUILD_EXECUTABLE)


include $(CLEAR_VARS)

LOCAL_SRC_FILES := verifier_test.c verifier.c

LOCAL_MODULE := verifier_test

LOCAL_FORCE_STATIC_EXECUTABLE := true

LOCAL_MODULE_TAGS := tests

LOCAL_STATIC_LIBRARIES := libmincrypt libcutils libstdc++ libc

include $(BUILD_EXECUTABLE)


include $(commands_recovery_local_path)/minui/Android.mk
include $(commands_recovery_local_path)/minelf/Android.mk
include $(commands_recovery_local_path)/minzip/Android.mk
include $(commands_recovery_local_path)/mtdutils/Android.mk
include $(commands_recovery_local_path)/tools/Android.mk
include $(commands_recovery_local_path)/edify/Android.mk
include $(commands_recovery_local_path)/updater/Android.mk
include $(commands_recovery_local_path)/applypatch/Android.mk
commands_recovery_local_path :=
