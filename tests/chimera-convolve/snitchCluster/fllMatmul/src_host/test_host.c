// SPDX-FileCopyrightText: 2024 ETH Zurich and University of Bologna
// SPDX-License-Identifier: Apache-2.0

// Include Standard Libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include Application Headers
#include "test_cluster.h"
#include "test_host.h"

// Include Target Specific Headers
#include "soc.h"

// Include Driver Headers
#include "driver.h"
#include "sw/device/lib/dif/dif_gpio.h"

// Include Runtime Headers
#include "log.h"
#include "util.h"

// Import HAL Headers
#include "interface_api.h"

#define CLUSTER 4
#define STACK_ADDRESS (_chimera_clusterBase[CLUSTER] + 0x20000 - 1)

// Timeout for cluster execution (in RTC ticks)
#define CLUSTER_TIMEOUT_MS 5000

extern uintptr_t volatile tohost, fromhost;

static const dif_gpio_t gpio = {
    .base_addr = (volatile void *)&__base_gpio,
};

#if defined(TARGET_PLATFORM_CHIMERA_CONVOLVE) && defined(HARDWARE_BACKEND_ASIC)
void setGPIO0_UART_TX() {
    // Connect UART port to GPIO 0 Pad
    chimera_padframe_aon_gpio_0_mux_set(CHIMERA_PADFRAME_AON_GPIO_0_group_UART0_port_TX);

    // Set GPIO 0 regs to transmit
    chimera_padframe_aon_gpio_0_cfg_rxe_set(0);  // Disable Pad's Receiver
    chimera_padframe_aon_gpio_0_cfg_trie_set(0); // Disable the tri-state transmitter
}
void setGPIO1_UART_RX() {
    // Connect UART port to GPIO 1 Pad
    chimera_padframe_aon_gpio_1_mux_set(CHIMERA_PADFRAME_AON_GPIO_1_group_UART0_port_RX);
}
void setGPIO2_GPIO() {
    // Connect GPIO 2 Pad to GPIO 2
    chimera_padframe_aon_gpio_2_mux_set(CHIMERA_PADFRAME_AON_GPIO_2_group_GPIOA_port_GPIO2);

    // Set GPIO 2 regs to transmit
    chimera_padframe_aon_gpio_2_cfg_rxe_set(0);  // Disable Pad's Receiver
    chimera_padframe_aon_gpio_2_cfg_trie_set(0); // Disable the tri-state transmitter
}

/**
 * @brief Calculate FLL parameters for a target frequency.
 *
 * @param target_freq Target frequency in Hz
 * @param rtc_freq RTC frequency in Hz
 * @param mult Output: FLL multiplier
 * @param div Output: FLL divider (power of 2)
 * @return 0 on success, -1 on error
 */
int calculate_fll_params(uint32_t target_freq, uint32_t rtc_freq, uint32_t *mult, uint32_t *div) {
    if (!mult || !div || target_freq == 0 || rtc_freq == 0) return -1;

    const uint32_t FMAX = 3300000000u; // 3.3 GHz (fits in 32-bit)
    uint32_t denom = 2u * target_freq; // safe while target_freq <= ~2.147e9
    if (denom == 0u) return -1;        // overflow guard

    // Strict bound: Dout < limit
    const uint32_t limit = FMAX / denom;
    // Smallest Dout is 1 (when div=1). Need 1 < limit to have any valid Dout.
    if (limit <= 1u) {
        printf_log("Error: Target frequency too high for FLL\n");
        return -1;
    }

    const uint32_t below = limit - 1u;
    const uint32_t e = 31u - __builtin_clz(below); // e in [0..30] for our ranges
    const uint32_t dout = 1u << e;

    // Round multiplier for a closer Fout: mult ≈ target * dout / rtc
    const uint32_t num = target_freq * dout;
    uint32_t m = (num + rtc_freq / 2u) / rtc_freq;
    if (m == 0u) m = 1u;

    *div = e + 1u; // because Dout = 2^(div-1)
    *mult = m;

    // // Optional quick check:
    // uint32_t fout = (rtc_freq * (*mult)) / (1u << (*div - 1));
    // // printf_log("target=%u, div=%u, dout=%u, mult=%u, fout=%u\n", target_freq, *div, dout,
    // *mult, fout); printf_log("Calculated FLL params: mult=%u, div=%u => fout=%u Hz (error: %+d
    // ppm)\n",
    //            *mult, *div, fout,
    //            (int32_t)(((int64_t)fout - (int64_t)target_freq) * 1000000 / target_freq));

    return 0;
}

/**
 * @brief Configure FLLs to target frequency and reconfigure UART.
 *
 * @param target_freq Target frequency in Hz
 * @param rtc_freq RTC frequency in Hz
 * @return Actual configured frequency in Hz, or 0 on error
 */
uint32_t configure_fll(uint32_t target_freq, uint32_t rtc_freq) {
    uint32_t mult, div;
    dif_result_t result;

    if (calculate_fll_params(target_freq, rtc_freq, &mult, &div) != 0) {
        printf_log("Error: Cannot calculate FLL parameters for %u Hz\n", target_freq);
        return 0;
    }

    result = dif_gpio_write(&gpio, 2, 1);
    if (result != kDifOk) {
        printf_log("Error: Cannot enable FLL bypass\n");
        return 0;
    }

    // Configure both FLLs (SOC and Cluster)
    volatile uint32_t *fllPtr = getFllPtr(0);
    initFll(fllPtr);
    setFllFreq(fllPtr, mult, div);

    fllPtr = getFllPtr(1);
    initFll(fllPtr);
    setFllFreq(fllPtr, mult, div);

    // Delay for FLL lock
    for (volatile int i = 0; i < 1000; i++);

    // Disable FLL bypass
    result = dif_gpio_write(&gpio, 2, 0);
    if (result != kDifOk) {
        printf_log("Error: Cannot disable FLL bypass\n");
        return 0;
    }

    // Relative Error = 1 / 128 = 0.78%
    // uint32_t ref_time_inv = rtc_freq / 128; // 32768 // 128 = 256
    // uint32_t core_freq_fll = clint_get_core_freq(rtc_freq, ref_time_inv);

    // // Relative Error = 1 / 32768 = 30.5 ppm
    uint32_t ref_time_inv = 1;
    uint32_t core_freq_fll = clint_get_core_freq(rtc_freq, ref_time_inv);

    // Reconfigure UART with new frequency
    uart_config_t uart_cfg = default_uart_cfg;
    uart_cfg.clk_freq_hz = core_freq_fll;
    default_uart_inst.cfg = &uart_cfg;

    if (iface_open(&default_uart_inst) != 0) {
        return 0;
    }

    return core_freq_fll;
}

/**
 * @brief Restore default frequency and UART configuration.
 *
 * @param rtc_freq RTC frequency in Hz
 * @return Default core frequency in Hz
 */
uint32_t restore_default_freq(uint32_t rtc_freq) {
    dif_result_t result;

    // Enable FLL bypass
    result = dif_gpio_write(&gpio, 2, 1);
    if (result != kDifOk) {
        printf_log("Error: Cannot enable FLL bypass\n");
        return 0;
    }

    // Calculate default core frequency
    // Relative Error = 1 / 128 = 0.78%
    // Absolute Error = 78.125 kHz
    uint32_t ref_time_inv = rtc_freq / 128; // 32768 // 128 = 256
    uint32_t core_freq = clint_get_core_freq(rtc_freq, ref_time_inv);

    // Reconfigure UART with default frequency
    uart_config_t uart_cfg = default_uart_cfg;
    uart_cfg.clk_freq_hz = core_freq;
    default_uart_inst.cfg = &uart_cfg;

    if (iface_open(&default_uart_inst) != 0) {
        return 0;
    }

    return core_freq;
}
#endif

int main(void) {
    dif_result_t result;

    // Read the RTC frequency from a hardware register
    uint32_t rtc_freq = *reg32(&__base_regs, CHESHIRE_RTC_FREQ_REG_OFFSET);

#if defined(TARGET_PLATFORM_CHIMERA_CONVOLVE) && defined(HARDWARE_BACKEND_ASIC)
    // Set GPIO 2 to output and enable FLL bypass
    setGPIO2_GPIO();
    // Connect UART to GPIO 0
    setGPIO0_UART_TX();
    // Connect UART RX to GPIO 1
    setGPIO1_UART_RX();

    result = dif_gpio_output_set_enabled(&gpio, 2, kDifToggleEnabled);
    if (result != kDifOk) {
        printf_log("Error: Cannot set GPIO 2 as output\n");
        return -1;
    }

    restore_default_freq(rtc_freq);
#endif

    // Calculate the initial core frequency from the RTC frequency
    // Relative Error = 1 / 128 = 0.78%
    // Absolute Error = 78.125 kHz
    uint32_t ref_time_inv = rtc_freq / 128; // 32768 // 128 = 256
    uint32_t core_freq = clint_get_core_freq(rtc_freq, ref_time_inv);

    printf("\n\n");
    printf_log("========================================\n");
    printf_log("Chimera MatMul Test with FLL Configuration\n");
    printf_log("========================================\n");
    printf_log("Initial frequency: %u.%03u MHz\n", (core_freq / 1000000), (core_freq % 1000000));

    // Buffer to hold run again
    char input_buffer[32];
    do {
#if defined(TARGET_PLATFORM_CHIMERA_CONVOLVE) && defined(HARDWARE_BACKEND_ASIC)
        // Ask user for target frequency
        printf_log("Enter target frequency in MHz (10-1000, or -1 to skip FLL configuration): ");
        fflush(stdout);

        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            printf("\n");
            printf_log("Error reading input\n");
            return -1;
        }

        int target_freq_mhz = atoi(input_buffer);
        printf("%d\n", target_freq_mhz);

        printf_log("Enter number of repetitions (default 1): ");
        fflush(stdout);

        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            printf("\n");
            printf_log("Error reading input\n");
            return -1;
        }

        int repetitions = 1;
        if (strlen(input_buffer) > 1) {
            repetitions = atoi(input_buffer);
        }
        printf("%d\n", repetitions);

        uint32_t actual_freq = core_freq;

        if (target_freq_mhz == -1) {
            printf_log("Skipping FLL configuration, using default frequency\n");
        } else if (target_freq_mhz < 10 || target_freq_mhz > 1000) {
            printf_log("Error: Frequency out of range (10-1000 MHz)\n");
            return -1;
        } else {
            printf_log("Configuring system to %d MHz...\n", target_freq_mhz);

            uint32_t target_freq = target_freq_mhz * 1000000;
            actual_freq = configure_fll(target_freq, rtc_freq);
            if (actual_freq == 0) {
                printf_log("Error: Failed to configure FLL\n");
                return -1;
            }

            printf_log("FLL configured successfully!\n");
            printf_log("Actual frequency: %u.%03u MHz\n", (actual_freq / 1000000),
                       (actual_freq % 1000000));
        }

#else
        uint32_t actual_freq = core_freq;
        printf_log("Note: FLL configuration only available on chimera-convolve ASIC target\n");
#endif

        // Setup cluster

        printf_log("----------------------------------------\n");
        printf_log("Setting up cluster %d...\n", CLUSTER);
        printf_log("----------------------------------------\n");

        argCluster_t arg_struct = {0};
        arg_struct.repitions = repetitions;
        argCluster_t *arg = &arg_struct;

        void *stack_cluster_ptr[NUM_CLUSTER_CORES];
        generate_snitchCluster_SPs_uniform(CLUSTER, (void *)STACK_ADDRESS, 0x2000,
                                           stack_cluster_ptr);

        setup_snitchCluster_interruptHandler(clusterInterruptHandler);

        set_snitchCluster_clockGating(CLUSTER, 0);

        set_snitchCluster_reset(CLUSTER, 1);
        for (volatile int i = 0; i < 10; i++);
        set_snitchCluster_reset(CLUSTER, 0);

        // Record start time
        clint_mtime_t start_time = clint_get_mtime();
        uint32_t timeout_ticks = rtc_freq * CLUSTER_TIMEOUT_MS / 1000;
        clint_mtime_t deadline = start_time;
        deadline.low += timeout_ticks;
        if (deadline.low < start_time.low) {
            deadline.high += 1; // Handle overflow
        }

        offload_snitchCluster(testReturn, (void *)arg, stack_cluster_ptr, CLUSTER);

        // Handle tohost/fromhost communication with timeout
        int timed_out = 0;
        while (snitchCluster_busy(CLUSTER)) {
            // Check for timeout
            clint_mtime_t current_time = clint_get_mtime();
            if (clint_mtime_less_than(deadline, current_time)) {
                printf("Error: Cluster execution timed out after %u ms\n", CLUSTER_TIMEOUT_MS);
                timed_out = 1;
                break;
            }

            // Wait for tohost to be set by the device
            if (tohost != 0) {
                volatile uint32_t syscall_addr = tohost;

                // Acknowledge tohost
                tohost = 0;

                // Cluster does tohost = (uintptr_t)buf->hdr.syscall_mem;
                uint32_t *syscall_mem = (uint32_t *)syscall_addr;

                if (syscall_mem[0] == 64) { // sys_write
                    fwrite((const void *)syscall_mem[2], 1, syscall_mem[3], (FILE *)syscall_mem[1]);
                    fflush((FILE *)syscall_mem[1]);
                } else {
                    printf_log("Unknown syscall: %u\n", syscall_mem[0]);
                }

                // Notify cluster that syscall is done
                fromhost = syscall_addr;
            }
        }

        uint32_t retVal = 0;
        if (!timed_out) {
            retVal = wait_snitchCluster_return(CLUSTER);
            retVal = retVal >> 1;
        }

        set_snitchCluster_clockGating(CLUSTER, 1);

        // Calculate and display metrics
        printf_log("----------------------------------------\n");
        printf_log("Execution Results\n");
        printf_log("----------------------------------------\n");
        printf_log("Return value: 0x%08x (%d errors)\n", retVal, retVal);

        if (!timed_out) {
            // Calculate operations per second
            // Ops in Op/cycle * 1e6
            // Frequency in Hz
            // Ops per cycle in  Op / cycle * 1e6 * 1e-3 * Hz * 1e-3 = Op/s
            uint32_t ops_per_sec = (arg->ops_per_cycle / 1000) * (actual_freq / 1000);

            // printf(" arg->ops_per_cycle = %u\n", arg->ops_per_cycle);
            // printf(" actual_freq = %u\n", actual_freq);
            // printf(" ops_per_sec = %u\n", ops_per_sec);

            printf("Op/Cycle: %u.%06u\n", arg->ops_per_cycle / 1000000,
                   arg->ops_per_cycle % 1000000);
            printf("Op/s: %u.%03u M\n", ops_per_sec / 1000000, ops_per_sec % 1000000);
        }

#if defined(TARGET_PLATFORM_CHIMERA_CONVOLVE) && defined(HARDWARE_BACKEND_ASIC)
        // Restore default frequency if FLL was used
        if (target_freq_mhz != -1 && target_freq_mhz >= 10 && target_freq_mhz <= 1000) {
            printf_log("Restoring default frequency...\n");
            uint32_t restored_freq = restore_default_freq(rtc_freq);
            printf_log("Restored to %u.%03u MHz\n", (restored_freq / 1000000),
                       (restored_freq % 1000000));
        }
#endif

        printf_log("========================================\n");

        // Ask to run again
        printf_log("Run again? (y/n): ");
        fflush(stdout);
        if (fgets(input_buffer, 2, stdin) == NULL ||
            (input_buffer[0] != 'y' && input_buffer[0] != 'Y')) {
            break;
        }
        printf("%s\n", input_buffer);

        if (timed_out) {
            return -1;
        }
    } while (1);

    return 0;
}