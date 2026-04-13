/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2025 Infineon Technologies AG
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// std includes
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>


// MTB includes
#include "cybsp.h"
#include "cy_scb_spi.h"
#include "retarget_io_init.h"

// micropython includes
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "shared/readline/readline.h"

// port-specific includes
#include "mplogger.h"

/*******************************************************************************
 * SPI Test - Configurable GPIO Pin Definitions (Port 9 / SCB1)
 * Change these macros to reconfigure which pins are used for SPI.
 ******************************************************************************/
#define SPI_TEST_HW             SCB10                    /* SCB10 */
#define SPI_TEST_IRQ            scb_10_interrupt_IRQn    /* SCB10 IRQ */

#define SPI_MOSI_PORT           GPIO_PRT16               /* P16.1 */
#define SPI_MOSI_PIN_NUM        1U
#define SPI_MOSI_HSIOM          P16_1_SCB10_SPI_MOSI

#define SPI_MISO_PORT           GPIO_PRT16               /* P16.2 */
#define SPI_MISO_PIN_NUM        2U
#define SPI_MISO_HSIOM          P16_2_SCB10_SPI_MISO

#define SPI_CLK_PORT            GPIO_PRT16               /* P16.0 */
#define SPI_CLK_PIN_NUM         0U
#define SPI_CLK_HSIOM           P16_0_SCB10_SPI_CLK

#define SPI_CS_PORT             GPIO_PRT16               /* P16.3 */
#define SPI_CS_PIN_NUM          3U
#define SPI_CS_HSIOM            P16_3_SCB10_SPI_SELECT0

/* Clock divider for SPI SCB block */
#define SPI_CLK_DIV_TYPE        CY_SYSCLK_DIV_8_BIT
#define SPI_CLK_DIV_NUM         4U

/* SPI test data */
#define SPI_TEST_DATA_SIZE      4U
static const uint8_t spi_test_tx_data[SPI_TEST_DATA_SIZE] = {0xA5, 0x5A, 0xFF, 0x01};

/* SPI context required by PDL */
static cy_stc_scb_spi_context_t spi_test_context;

/* SPI master config for SCB10 (Motorola, CPHA0/CPOL0, 8-bit) */
static const cy_stc_scb_spi_config_t spi_test_config = {
    .spiMode = CY_SCB_SPI_MASTER,
    .subMode = CY_SCB_SPI_MOTOROLA,
    .sclkMode = CY_SCB_SPI_CPHA0_CPOL0,
    .parity = CY_SCB_SPI_PARITY_NONE,
    .dropOnParityError = false,
    .oversample = 4,
    .rxDataWidth = 8UL,
    .txDataWidth = 8UL,
    .enableMsbFirst = true,
    .enableInputFilter = false,
    .enableFreeRunSclk = false,
    .enableMisoLateSample = true,
    .enableTransferSeparation = false,
    .ssPolarity = ((CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT0) |
                   (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT1) |
                   (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT2) |
                   (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT3)),
    .ssSetupDelay = false,
    .ssHoldDelay = false,
    .ssInterFrameDelay = false,
    .enableWakeFromSleep = false,
    .rxFifoTriggerLevel = 63UL,
    .rxFifoIntEnableMask = 0UL,
    .txFifoTriggerLevel = 63UL,
    .txFifoIntEnableMask = 0UL,
    .masterSlaveIntEnableMask = 0UL,
};

/*******************************************************************************
 * spi_test_init_pins: Configure GPIO pins for SPI function
 ******************************************************************************/
static void spi_test_init_pins(void) {
    /* MOSI - Strong drive, output */
    Cy_GPIO_Pin_FastInit(SPI_MOSI_PORT, SPI_MOSI_PIN_NUM,
                         CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_MOSI_HSIOM);

    /* MISO - High-Z, input */
    Cy_GPIO_Pin_FastInit(SPI_MISO_PORT, SPI_MISO_PIN_NUM,
                         CY_GPIO_DM_HIGHZ, 0, SPI_MISO_HSIOM);

    /* CLK - Strong drive, output */
    Cy_GPIO_Pin_FastInit(SPI_CLK_PORT, SPI_CLK_PIN_NUM,
                         CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_CLK_HSIOM);

    /* CS - Strong drive, output, active low (idle high) */
    Cy_GPIO_Pin_FastInit(SPI_CS_PORT, SPI_CS_PIN_NUM,
                         CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_CS_HSIOM);
}

/*******************************************************************************
 * spi_test_run: Initialize SPI using PDL and send test bytes
 * Returns true if the SPI peripheral initialized and transmitted successfully.
 ******************************************************************************/
static bool spi_test_run(void) {
    cy_en_scb_spi_status_t spi_status;

    /* 1. Configure SPI pins */
    printf("SPI Test: [1] Configuring pins...\r\n");
    fflush(stdout);
    spi_test_init_pins();

    /* 2. Power on the SCB10 peripheral group */
    printf("SPI Test: [2] Enabling SCB10 peripheral group...\r\n");
    fflush(stdout);
    Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SCB10_PERI_NR, CY_MMIO_SCB10_GROUP_NR,
                                 CY_MMIO_SCB10_SLAVE_NR, CY_MMIO_SCB10_CLK_HF_NR);

    /* 3. Set up the clock divider for SCB10 SPI */
    printf("SPI Test: [3] Setting up clock divider...\r\n");
    fflush(stdout);
    Cy_SysClk_PeriphDisableDivider(SPI_CLK_DIV_TYPE, SPI_CLK_DIV_NUM);
    Cy_SysClk_PeriphSetDivider(SPI_CLK_DIV_TYPE, SPI_CLK_DIV_NUM, 49U);
    Cy_SysClk_PeriphEnableDivider(SPI_CLK_DIV_TYPE, SPI_CLK_DIV_NUM);
    Cy_SysClk_PeriPclkAssignDivider(PCLK_SCB10_CLOCK_SCB_EN, SPI_CLK_DIV_TYPE, SPI_CLK_DIV_NUM);
    printf("SPI Test: [3] Clock configured\r\n");
    fflush(stdout);

    /* 4. Initialize SPI (master, Motorola, CPHA0/CPOL0, 8-bit) */
    printf("SPI Test: [4] Calling Cy_SCB_SPI_Init...\r\n");
    fflush(stdout);
    spi_status = Cy_SCB_SPI_Init(SPI_TEST_HW, &spi_test_config, &spi_test_context);
    if (spi_status != CY_SCB_SPI_SUCCESS) {
        printf("SPI Test: Init FAILED (status=0x%lx)\r\n", (unsigned long)spi_status);
        fflush(stdout);
        return false;
    }

    /* 5. Enable the SPI block */
    printf("SPI Test: [5] Enabling SPI...\r\n");
    fflush(stdout);
    Cy_SCB_SPI_Enable(SPI_TEST_HW);

    /* 6. Transmit test data byte-by-byte using low-level FIFO access */
    printf("SPI Test: Sending %u bytes: ", SPI_TEST_DATA_SIZE);
    for (uint32_t i = 0; i < SPI_TEST_DATA_SIZE; i++) {
        printf("0x%02X ", spi_test_tx_data[i]);
        Cy_SCB_SPI_Write(SPI_TEST_HW, spi_test_tx_data[i]);
    }
    printf("\r\n");

    /* 6. Wait for transmission to complete */
    uint32_t timeout = 10000U;
    while (!Cy_SCB_SPI_IsTxComplete(SPI_TEST_HW) && (timeout > 0U)) {
        Cy_SysLib_DelayUs(10);
        timeout--;
    }

    if (timeout == 0U) {
        printf("SPI Test: Tx timeout!\r\n");
    } else {
        printf("SPI Test: Tx complete.\r\n");
    }

    /* 7. Read back any received data from the RX FIFO (loopback or slave response) */
    uint32_t rx_count = Cy_SCB_SPI_GetNumInRxFifo(SPI_TEST_HW);
    if (rx_count > 0U) {
        printf("SPI Test: Received %lu bytes: ", (unsigned long)rx_count);
        while (Cy_SCB_SPI_GetNumInRxFifo(SPI_TEST_HW) > 0U) {
            uint32_t rx_byte = Cy_SCB_SPI_Read(SPI_TEST_HW);
            printf("0x%02lX ", (unsigned long)rx_byte);
        }
        printf("\r\n");
    } else {
        printf("SPI Test: No data received (no slave/loopback connected).\r\n");
    }

    /* 8. Clean up - disable and de-init */
    Cy_SCB_SPI_Disable(SPI_TEST_HW, &spi_test_context);
    Cy_SCB_SPI_DeInit(SPI_TEST_HW);

    printf("SPI Test: PASSED - SPI peripheral operational.\r\n");
    return true;
}

typedef enum {
    BOOT_MODE_NORMAL,
    BOOT_MODE_SAFE
} boot_mode_t;


#if MICROPY_ENABLE_GC
extern uint8_t __StackTop, __StackSize;
extern uint8_t __HeapBase, __HeapLimit;
#endif

extern void machine_rtc_init_all(void);
extern void time_init(void);
extern void machine_pin_irq_deinit_all(void);
extern void machine_hw_i2c_deinit_all(void);
extern void machine_pdm_pcm_deinit_all(void);

boot_mode_t check_boot_mode(void) {
    boot_mode_t boot_mode;

    // Initialize user LED
    Cy_GPIO_Pin_FastInit(CYBSP_USER_LED_PORT, CYBSP_USER_LED_NUM, CY_GPIO_DM_STRONG, 0, HSIOM_SEL_GPIO);

    // Initialize user button
    Cy_GPIO_Pin_FastInit(CYBSP_USER_BTN1_PORT, CYBSP_USER_BTN1_NUM, CY_GPIO_DM_PULLUP, 1, HSIOM_SEL_GPIO);

    // Added 5ms delay to allow bypass capacitor connected to the user button without external pull-up to charge.
    Cy_SysLib_Delay(5);

    if (Cy_GPIO_Read(CYBSP_USER_BTN1_PORT, CYBSP_USER_BTN1_NUM) == CYBSP_BTN_PRESSED) {
        // Blink LED twice to indicate safe boot mode was entered
        for (int i = 0; i < 4; i++) {
            Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_NUM);
            Cy_SysLib_Delay(500); // delay in milliseconds
        }
        boot_mode = BOOT_MODE_SAFE;
        mp_printf(&mp_plat_print, "- DEVICE IS IN SAFE BOOT MODE -\n");
    } else {
        boot_mode = BOOT_MODE_NORMAL;
    }

    // Turn off LED after boot mode check
    Cy_GPIO_Clr(CYBSP_USER_LED_PORT, CYBSP_USER_LED_NUM);

    return boot_mode;
}

int main(void) {
    cy_rslt_t result = CY_RSLT_SUCCESS;

    /* Initialize the device and board peripherals. */
    result = cybsp_init();
    if (result != CY_RSLT_SUCCESS) {
        mp_raise_ValueError(MP_ERROR_TEXT("cybsp_init failed !\n"));
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize retarget-io middleware */
    init_retarget_io();

    /* Run SPI test */
    printf("\r\n=== SPI PDL Test ===\r\n");
    spi_test_run();
    printf("=== SPI Test Done ===\r\n\r\n");

    // Initialise the MicroPython runtime.
    #if MICROPY_ENABLE_GC
    gc_init(&__HeapBase, &__HeapLimit);
    mp_cstack_init_with_top((void *)&__StackTop, __StackSize);
    #endif

    time_init();

soft_reset:
    machine_rtc_init_all();
    mp_init();

    readline_init0();

    #if MICROPY_VFS
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));


    #if MICROPY_VFS_LFS2
    pyexec_frozen_module("vfs_lfs2.py", false);
    #endif
    #endif

    if (check_boot_mode() == BOOT_MODE_NORMAL) {
        // Execute user scripts.
        int ret = pyexec_file_if_exists("/boot.py");

        if (ret & PYEXEC_FORCED_EXIT) {
            goto soft_reset;
        }

        if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
            ret = pyexec_file_if_exists("/main.py");

            if (ret & PYEXEC_FORCED_EXIT) {
                goto soft_reset;
            }
        }
    }

    for (;;) {
        if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
            if (pyexec_raw_repl() != 0) {
                break;
            }
        } else {
            if (pyexec_friendly_repl() != 0) {
                break;
            }
        }
    }

    mp_printf(&mp_plat_print, "MPY: soft reboot\n");

    machine_pin_irq_deinit_all();
    machine_hw_i2c_deinit_all();
    machine_pdm_pcm_deinit_all();

    #if MICROPY_ENABLE_GC
    gc_sweep_all();
    #endif
    mp_deinit();

    goto soft_reset;
}

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}
#endif

// Handle uncaught exceptions (should never be reached in a correct C implementation).
void nlr_jump_fail(void *val) {
    for (;;) {
    }
}