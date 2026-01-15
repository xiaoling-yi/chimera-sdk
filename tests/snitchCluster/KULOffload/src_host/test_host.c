// SPDX-FileCopyrightText: 2024 ETH Zurich and University of Bologna
// SPDX-License-Identifier: Apache-2.0

// Include Standard Libraries

// Include Application Headers
#include "test_cluster.h"
#include "test_host.h"

// Include Snitch Cluster Headers
#include "kultest/snax-kul-cluster-gemmx-test.h"
#include "kultest/snax-kul-cluster-xdma-test.h"

// Include Target Specific Headers
#include "soc.h"

// Include Driver Headers
#include "driver.h"

// Include Runtime Headers
#include "log.h"

// Import HAL Headers

#define kul_clusterId 3
#define CLUSTER kul_clusterId
#define STACK_ADDRESS (_chimera_clusterBase[CLUSTER] + 0x20000 - 1)

static offloadArgs_t offloadArgs = {.value = 0xdeadbeef};

#if defined(TARGET_PLATFORM_CHIMERA_CONVOLVE) && defined(HARDWARE_BACKEND_ASIC)
void setGPIO0_UART() {
    // Connect UART port to GPIO 0 Pad
    chimera_padframe_aon_gpio_0_mux_set(CHIMERA_PADFRAME_AON_GPIO_0_group_UART0_port_TX);

    // Set GPIO 0 regs to transmit
    chimera_padframe_aon_gpio_0_cfg_rxe_set(0);  // Disable Pad's Receiver
    chimera_padframe_aon_gpio_0_cfg_trie_set(0); // Disable the tri-state transmitter
}
#endif

static inline void delay_cycles(uint64_t cycle) {
    uint64_t target_cycle, current_cycle;
    __asm__ volatile("csrr %0, mcycle;" : "=r"(current_cycle));
    target_cycle = current_cycle + cycle;
    while (current_cycle < target_cycle) {
        __asm__ volatile("csrr %0, mcycle;" : "=r"(current_cycle));
    }
}

int main(void) {
#if defined(TARGET_PLATFORM_CHIMERA_CONVOLVE) && defined(HARDWARE_BACKEND_ASIC)
    // Connect UART to GPIO 0
    setGPIO0_UART();
#endif

    // while(1){

    // };

    // printf("Starting time test...\r\n");
    // delay_cycles(400000000);
    // printf("Time test done.\r\n");

    void *stack_cluster_ptr[NUM_CLUSTER_CORES];
    generate_snitchCluster_SPs_uniform(CLUSTER, (void *)STACK_ADDRESS, 0x2000, stack_cluster_ptr);

    setup_snitchCluster_interruptHandler(clusterInterruptHandler);

    set_snitchCluster_clockGating(CLUSTER, 0);

    set_snitchCluster_reset(CLUSTER, 1);
    for (volatile int i = 0; i < 10; i++);
    set_snitchCluster_reset(CLUSTER, 0);

    printf("Starting GEMMX tests on cluster %d...\r\n", CLUSTER);
    // printf("Test for SNAX KUL Power Measurement started...\r\n");
    // printf("Run GEMMX tests on cluster %d recursively... \r\n", CLUSTER);
    printf("Waiting for cluster to finish... \r\n");

    offload_snitchCluster(kul_cluster_gemmx_test, &offloadArgs, stack_cluster_ptr, CLUSTER);
    uint32_t retVal_gemmx = wait_snitchCluster_return(CLUSTER);
    printf("GEMMX tests on cluster %d completed.\r\n", CLUSTER);

    offload_snitchCluster(kul_cluster_xdma_test, &offloadArgs, stack_cluster_ptr, CLUSTER);
    uint32_t retVal_xdma = wait_snitchCluster_return(CLUSTER);
    printf("XDMA tests on cluster %d completed.\r\n", CLUSTER);

    set_snitchCluster_clockGating(CLUSTER, 1);

    printf("Returned value from GeMMx, Error = : %d \r\n", retVal_gemmx - 1);
    printf("Returned value from XDMA, Error = : %d \r\n", retVal_xdma - 1);

    uint32_t retVal = (retVal_gemmx << 16) | retVal_xdma;
    return retVal;
}
