LOCAL_PATH := $(call my-dir)
ifneq ($(filter imx,$(TARGET_BOARD_PLATFORM)),)
include $(call all-named-subdir-makefiles,libbt-vendor)
endif
