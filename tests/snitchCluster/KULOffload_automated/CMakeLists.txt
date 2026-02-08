# SPDX-FileCopyrightText: 2024 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

set(TEST_NAME test_snitchCluster_KULOffload)

######## HOST Code #############################################################
file(GLOB_RECURSE TEST_HOST_SRCS
  "src_host/*.c"
)

add_library(${TEST_NAME}_host OBJECT ${TEST_HOST_SRCS})
target_include_directories(${TEST_NAME}_host PUBLIC include)

# WIESEP: Set the correct ISA and ABI for the host
target_compile_options(${TEST_NAME}_host
  PRIVATE
  -O2
)
target_link_libraries(${TEST_NAME}_host PUBLIC runtime_host picolibc_host trampoline_rv32im)

######## CLUSTER Code ##########################################################
file(GLOB_RECURSE TEST_SNITCH_SRCS
  "src_cluster/*.c"
)

# Automated data.h generation
set(DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/kultest/gemmx/data)
set(DATAGEN_SCRIPT ${DATA_DIR}/datagen.py)
set(SW_CFG ${DATA_DIR}/params.hjson)
set(HW_CFG ${DATA_DIR}/snax_KUL_cluster.hjson)
set(DATA_H ${DATA_DIR}/data.h)

add_custom_command(
  OUTPUT ${DATA_H}
  COMMAND ${DATAGEN_SCRIPT} --swcfg ${SW_CFG} --hwcfg ${HW_CFG} > ${DATA_H} # Assuming executable permissions or implicit python shebang
  DEPENDS ${DATAGEN_SCRIPT} ${SW_CFG} ${HW_CFG} ${DATA_DIR}/data_utils.py ${DATA_DIR}/snax_utils.py
  COMMENT "Generating data.h for snax-gemmx"
)

# Add generated header to sources to trigger generation
list(APPEND TEST_SNITCH_SRCS ${DATA_H})

add_library(${TEST_NAME}_cluster OBJECT ${TEST_SNITCH_SRCS})
target_include_directories(${TEST_NAME}_cluster PUBLIC include)

# WIESEP: Set the correct ISA and ABI for the cluster
target_compile_options(${TEST_NAME}_cluster
  PRIVATE
  -O2
)
target_link_libraries(${TEST_NAME}_cluster PUBLIC runtime_cluster_snitch picolibc_cluster_snitch)

######## TEST Executable #######################################################
add_chimera_test(
  ${TEST_NAME}
)

# WIESEP: Link the host and cluster code to the test executable
target_link_libraries(${TEST_NAME} PUBLIC ${TEST_NAME}_host)
target_link_libraries(${TEST_NAME} PUBLIC ${TEST_NAME}_cluster)

