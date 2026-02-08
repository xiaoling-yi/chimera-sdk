# Copyright 2023 KU Leuven.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Xiaoling Yi <xiaoling.yi@esat.kuleuven.be>

# Usage of absolute paths is required to externally include this Makefile
MK_DIR   := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
DATA_DIR := $(realpath $(MK_DIR))

SW_CFG ?= $(DATA_DIR)/params.hjson
HW_CFG ?= $(DATA_DIR)/snax_KUL_cluster.hjson

DATA_H = $(DATA_DIR)/data.h

$(DATA_H): $(DATA_DIR)/datagen.py $(SW_CFG) $(HW_CFG) $(DATA_DIR)/data_utils.py $(DATA_DIR)/snax_utils.py
	$< --swcfg $(SW_CFG) --hwcfg $(HW_CFG) > $@

.PHONY: clean-data clean

clean-data:
	rm -f $(DATA_H)

clean: clean-data
