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

// SPI Master - Port 16 / SCB10
#define SPI_M_HW                SCB10
#define SPI_M_MOSI_PORT         GPIO_PRT16
#define SPI_M_MOSI_PIN          1U
#define SPI_M_MOSI_HSIOM        P16_1_SCB10_SPI_MOSI
#define SPI_M_MISO_PORT         GPIO_PRT16
#define SPI_M_MISO_PIN          2U
#define SPI_M_MISO_HSIOM        P16_2_SCB10_SPI_MISO
#define SPI_M_CLK_PORT          GPIO_PRT16
#define SPI_M_CLK_PIN           0U
#define SPI_M_CLK_HSIOM         P16_0_SCB10_SPI_CLK
#define SPI_M_CS_PORT           GPIO_PRT16
#define SPI_M_CS_PIN            3U
#define SPI_M_CS_HSIOM          P16_3_SCB10_SPI_SELECT0
#define SPI_M_CLK_DIV_NUM       4U

// SPI Slave - Port 9 / SCB1
#define SPI_S_HW                SCB1
#define SPI_S_MOSI_PORT         GPIO_PRT9
#define SPI_S_MOSI_PIN          2U
#define SPI_S_MOSI_HSIOM        P9_2_SCB1_SPI_MOSI
#define SPI_S_MISO_PORT         GPIO_PRT9
#define SPI_S_MISO_PIN          1U
#define SPI_S_MISO_HSIOM        P9_1_SCB1_SPI_MISO
#define SPI_S_CLK_PORT          GPIO_PRT9
#define SPI_S_CLK_PIN           3U
#define SPI_S_CLK_HSIOM         P9_3_SCB1_SPI_CLK
#define SPI_S_CS_PORT           GPIO_PRT9
#define SPI_S_CS_PIN            0U
#define SPI_S_CS_HSIOM          P9_0_SCB1_SPI_SELECT0
#define SPI_S_CLK_DIV_TYPE      CY_SYSCLK_DIV_16_BIT
#define SPI_S_CLK_DIV_NUM       0U

static cy_stc_scb_spi_context_t spi_master_ctx;
static cy_stc_scb_spi_context_t spi_slave_ctx;

static const cy_stc_scb_spi_config_t spi_master_config = {
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

static const cy_stc_scb_spi_config_t spi_slave_config = {
    .spiMode = CY_SCB_SPI_SLAVE,
    .subMode = CY_SCB_SPI_MOTOROLA,
    .sclkMode = CY_SCB_SPI_CPHA0_CPOL0,
    .parity = CY_SCB_SPI_PARITY_NONE,
    .dropOnParityError = false,
    .oversample = 0,
    .rxDataWidth = 8UL,
    .txDataWidth = 8UL,
    .enableMsbFirst = true,
    .enableInputFilter = false,
    .enableFreeRunSclk = false,
    .enableMisoLateSample = false,
    .enableTransferSeparation = false,
    .ssPolarity = ((CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT0) |
        (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT1) |
        (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT2) |
        (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT3)),
    .ssSetupDelay = false,
    .ssHoldDelay = false,
    .ssInterFrameDelay = false,
    .enableWakeFromSleep = false,
    .rxFifoTriggerLevel = 0UL,
    .rxFifoIntEnableMask = 0UL,
    .txFifoTriggerLevel = 0UL,
    .txFifoIntEnableMask = 0UL,
    .masterSlaveIntEnableMask = 0UL,
};

static void spi_test_run(void) {
    static const uint8_t tx_data[] = {0xA5, 0x5A, 0xFF, 0x01};
    static const uint8_t slave_tx[] = {0x11, 0x22, 0x33, 0x44};

    // Master pins (Port 16) - SCB10
    Cy_GPIO_Pin_FastInit(SPI_M_MOSI_PORT, SPI_M_MOSI_PIN, CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_M_MOSI_HSIOM);
    Cy_GPIO_Pin_FastInit(SPI_M_MISO_PORT, SPI_M_MISO_PIN, CY_GPIO_DM_HIGHZ, 0, SPI_M_MISO_HSIOM);
    Cy_GPIO_Pin_FastInit(SPI_M_CLK_PORT, SPI_M_CLK_PIN, CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_M_CLK_HSIOM);
    Cy_GPIO_Pin_FastInit(SPI_M_CS_PORT, SPI_M_CS_PIN, CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_M_CS_HSIOM);

    // Slave pins (Port 9) - SCB1
    Cy_GPIO_Pin_FastInit(SPI_S_MOSI_PORT, SPI_S_MOSI_PIN, CY_GPIO_DM_HIGHZ, 0, SPI_S_MOSI_HSIOM);
    Cy_GPIO_Pin_FastInit(SPI_S_MISO_PORT, SPI_S_MISO_PIN, CY_GPIO_DM_STRONG_IN_OFF, 1, SPI_S_MISO_HSIOM);
    Cy_GPIO_Pin_FastInit(SPI_S_CLK_PORT, SPI_S_CLK_PIN, CY_GPIO_DM_HIGHZ, 0, SPI_S_CLK_HSIOM);
    Cy_GPIO_Pin_FastInit(SPI_S_CS_PORT, SPI_S_CS_PIN, CY_GPIO_DM_HIGHZ, 0, SPI_S_CS_HSIOM);

    // Power on SCB10 and SCB1 peripheral groups
    Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SCB10_PERI_NR, CY_MMIO_SCB10_GROUP_NR,
        CY_MMIO_SCB10_SLAVE_NR, CY_MMIO_SCB10_CLK_HF_NR);
    Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SCB1_PERI_NR, CY_MMIO_SCB1_GROUP_NR,
        CY_MMIO_SCB1_SLAVE_NR, CY_MMIO_SCB1_CLK_HF_NR);

    // Master clock (SCB10) - PERI group 1, 8-bit divider
    Cy_SysClk_PeriPclkDisableDivider(PCLK_SCB10_CLOCK_SCB_EN, CY_SYSCLK_DIV_8_BIT, SPI_M_CLK_DIV_NUM);
    Cy_SysClk_PeriPclkSetDivider(PCLK_SCB10_CLOCK_SCB_EN, CY_SYSCLK_DIV_8_BIT, SPI_M_CLK_DIV_NUM, 49U);
    Cy_SysClk_PeriPclkEnableDivider(PCLK_SCB10_CLOCK_SCB_EN, CY_SYSCLK_DIV_8_BIT, SPI_M_CLK_DIV_NUM);
    Cy_SysClk_PeriPclkAssignDivider(PCLK_SCB10_CLOCK_SCB_EN, CY_SYSCLK_DIV_8_BIT, SPI_M_CLK_DIV_NUM);

    // Slave clock (SCB1) - PERI group 8, only 16-bit dividers available
    Cy_SysClk_PeriPclkDisableDivider(PCLK_SCB1_CLOCK_SCB_EN, SPI_S_CLK_DIV_TYPE, SPI_S_CLK_DIV_NUM);
    Cy_SysClk_PeriPclkSetDivider(PCLK_SCB1_CLOCK_SCB_EN, SPI_S_CLK_DIV_TYPE, SPI_S_CLK_DIV_NUM, 0U);
    Cy_SysClk_PeriPclkEnableDivider(PCLK_SCB1_CLOCK_SCB_EN, SPI_S_CLK_DIV_TYPE, SPI_S_CLK_DIV_NUM);
    Cy_SysClk_PeriPclkAssignDivider(PCLK_SCB1_CLOCK_SCB_EN, SPI_S_CLK_DIV_TYPE, SPI_S_CLK_DIV_NUM);

    // Init slave first so it's ready before master starts clocking
    if (Cy_SCB_SPI_Init(SPI_S_HW, &spi_slave_config, &spi_slave_ctx) != CY_SCB_SPI_SUCCESS) {
        printf("SPI slave init failed\r\n");
        return;
    }
    Cy_SCB_SPI_Enable(SPI_S_HW);

    // Pre-load slave TX FIFO with response data
    for (uint32_t i = 0; i < sizeof(slave_tx); i++) {
        Cy_SCB_SPI_Write(SPI_S_HW, slave_tx[i]);
    }

    // Init master
    if (Cy_SCB_SPI_Init(SPI_M_HW, &spi_master_config, &spi_master_ctx) != CY_SCB_SPI_SUCCESS) {
        printf("SPI master init failed\r\n");
        return;
    }
    Cy_SCB_SPI_Enable(SPI_M_HW);

    // Master sends data
    for (uint32_t i = 0; i < sizeof(tx_data); i++) {
        Cy_SCB_SPI_Write(SPI_M_HW, tx_data[i]);
    }

    // Wait for master TX to complete
    uint32_t timeout = 10000U;
    while (!Cy_SCB_SPI_IsTxComplete(SPI_M_HW) && --timeout) {
        Cy_SysLib_DelayUs(10);
    }
    Cy_SysLib_DelayUs(100);

    // Print master RX (slave's response)
    printf("Master RX: ");
    while (Cy_SCB_SPI_GetNumInRxFifo(SPI_M_HW) > 0U) {
        printf("0x%02lX ", (unsigned long)Cy_SCB_SPI_Read(SPI_M_HW));
    }
    printf("\r\n");

    // Print slave RX (master's data)
    printf("Slave  RX: ");
    while (Cy_SCB_SPI_GetNumInRxFifo(SPI_S_HW) > 0U) {
        printf("0x%02lX ", (unsigned long)Cy_SCB_SPI_Read(SPI_S_HW));
    }
    printf("\r\n");

    // Cleanup
    Cy_SCB_SPI_Disable(SPI_M_HW, &spi_master_ctx);
    Cy_SCB_SPI_DeInit(SPI_M_HW);
    Cy_SCB_SPI_Disable(SPI_S_HW, &spi_slave_ctx);
    Cy_SCB_SPI_DeInit(SPI_S_HW);
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
