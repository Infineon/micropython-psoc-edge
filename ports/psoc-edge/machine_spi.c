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

#include <stdio.h>
#include "cybsp.h"
#include "cy_scb_spi.h"
#include "machine_spi.h"

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

void spi_test_run(void) {
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
