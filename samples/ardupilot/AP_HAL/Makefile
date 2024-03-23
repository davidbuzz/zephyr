#
# Copyright (c) 2017 Intel Corporation
# Copyright (c) 2022 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: Apache-2.0
#

PREFIX ?= .
OBJ_DIR ?= $(PREFIX)/obj
LIB_DIR ?= $(PREFIX)/lib

ifeq ($(findstring Windows, $(OS)),Windows)
	DEL := rmdir /S /Q
	MKDIR := mkdir
else
	DEL := rm -rf
	MKDIR := mkdir -p
endif

all:
	-$(MKDIR) "$(OBJ_DIR)"
	-$(MKDIR) "$(LIB_DIR)"
	/home/buzz/zephyr-sdk-0.16.5-1/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-g++ -c $(CXXFLAGS) -MD -Iinclude -I../AP_HAL/ -I../AP_Common -I.. -I../.. -I../../.. -I../../../..  -I -D__AP_LINE__=17 -DCONFIG_HAL_BOARD=HAL_BOARD_ESP32 -DCONFIG_HAL_BOARD_SUBTYPE=HAL_BOARD_SUBTYPE_ESP32_S3EMPTY -fsingle-precision-constant HAL.cpp -o $(OBJ_DIR)/AP_HAL.o
	/home/buzz/zephyr-sdk-0.16.5-1/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-ar -rcs $(LIB_DIR)/libAP_HAL.a $(OBJ_DIR)/AP_HAL.o

clean:
	$(DEL) "$(OBJ_DIR)" "$(LIB_DIR)"
