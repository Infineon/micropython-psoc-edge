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

// Board and hardware specific configuration
#define MICROPY_HW_MCU_NAME                     "PSOCE84"
#define MICROPY_HW_BOARD_NAME                   "KIT_PSE84_AI"

#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT     "KIT_PSE84_AI"

// External QSPI Flash Configuration
// These sizes are determined by the physical flash chip specifications
// and memory map layout for the KIT_PSE84_AI board
#define MICROPY_PY_EXT_FLASH (1)

// Flash memory map: Total 64MB (0x04000000) QSPI flash (S25HS512T hybrid flash)
// S25HS512T Memory Map:
//   Region 0: 0x000000-0x01FFFF (32 x 4KB sectors)
//   Region 1: 0x020000-0x03FFFF (1 x 128KB sector)
//   Region 2: 0x040000-0x3FFFFFF (255 x 256KB sectors) <- We use this region
// CRITICAL: Base address MUST be aligned to 256KB (0x40000) sector boundary!
#define EXT_FLASH_BASE              (0x00900000)  // Aligned to sector 36

// Usable filesystem space: 64MB - 9MB = 55MB (0x03700000 bytes)
#define EXT_FLASH_SIZE              (0x04000000 - EXT_FLASH_BASE)

// erase sector size : 256KB, fixed by flash chip hardware in Region 2.
#define EXT_FLASH_SECTOR_SIZE        (0x40000)       /** 256KB*/

// Block device block size: Must match erase sector size for proper filesystem operation
#define EXT_FLASH_BLOCK_SIZE_BYTES  (EXT_FLASH_SECTOR_SIZE)

// Program page size: Fixed by flash chip hardware (minimum writable unit). Matches LittleFS write_size.
#define EXT_FLASH_PAGE_SIZE         (0x200) /** 512 Bytes */

// I2C Configuration - Unified
// I2C0: Primary I2C (SCB5, P17.0/P17.1)
#define MICROPY_HW_I2C0_SCB                     (SCB5)
#define MICROPY_HW_I2C0_SCL                     (P17_0_NUM)
#define MICROPY_HW_I2C0_SDA                     (P17_1_NUM)
#define MICROPY_HW_I2C0_PCLK                    PCLK_SCB5_CLOCK_SCB_EN
#define MICROPY_HW_I2C0_IRQn                    scb_5_interrupt_IRQn
// I2C0 GPIO Configuration
#define MICROPY_HW_I2C0_GPIO_PORT               GPIO_PRT17
#define MICROPY_HW_I2C0_SCL_PIN_NUM             0U
#define MICROPY_HW_I2C0_SDA_PIN_NUM             1U
#define MICROPY_HW_I2C0_SCL_HSIOM               P17_0_SCB5_I2C_SCL
#define MICROPY_HW_I2C0_SDA_HSIOM               P17_1_SCB5_I2C_SDA

// I2C1: Secondary I2C (SCB0, P8.0/P8.1)
#define MICROPY_HW_I2C1_SCB                     (SCB0)
#define MICROPY_HW_I2C1_SCL                     (P8_0_NUM)
#define MICROPY_HW_I2C1_SDA                     (P8_1_NUM)
#define MICROPY_HW_I2C1_PCLK                    PCLK_SCB0_CLOCK_SCB_EN
#define MICROPY_HW_I2C1_IRQn                    scb_0_interrupt_IRQn
// I2C1 GPIO Configuration
#define MICROPY_HW_I2C1_GPIO_PORT               GPIO_PRT8
#define MICROPY_HW_I2C1_SCL_PIN_NUM             0U
#define MICROPY_HW_I2C1_SDA_PIN_NUM             1U
#define MICROPY_HW_I2C1_SCL_HSIOM               P8_0_SCB0_I2C_SCL
#define MICROPY_HW_I2C1_SDA_HSIOM               P8_1_SCB0_I2C_SDA

// Common I2C Configuration
#define MICROPY_HW_I2C_GPIO_DRIVE_MODE          CY_GPIO_DM_OD_DRIVESLOW
#define MICROPY_HW_I2C_INTR_PRIORITY            (7UL)

// Calculate the maximum number of I2C instances
#if defined(MICROPY_HW_I2C1_SCL)
#define MICROPY_HW_MAX_I2C (2)
#elif defined(MICROPY_HW_I2C0_SCL)
#define MICROPY_HW_MAX_I2C (1)
#else
#define MICROPY_HW_MAX_I2C (0)
#endif

// I2C Target (Slave) Configuration - Uses I2C0 hardware only
// I2C Target uses I2C0 instance (SCB5, P17.0/P17.1) exclusively
#define MICROPY_HW_I2C_TARGET_SCB               MICROPY_HW_I2C0_SCB
#define MICROPY_HW_I2C_TARGET_SCL               MICROPY_HW_I2C0_SCL
#define MICROPY_HW_I2C_TARGET_SDA               MICROPY_HW_I2C0_SDA
#define MICROPY_HW_I2C_TARGET_PCLK              MICROPY_HW_I2C0_PCLK
#define MICROPY_HW_I2C_TARGET_IRQn              MICROPY_HW_I2C0_IRQn
// I2C Target GPIO Configuration (same as I2C0)
#define MICROPY_HW_I2C_TARGET_GPIO_PORT         MICROPY_HW_I2C0_GPIO_PORT
#define MICROPY_HW_I2C_TARGET_SCL_PIN_NUM       MICROPY_HW_I2C0_SCL_PIN_NUM
#define MICROPY_HW_I2C_TARGET_SDA_PIN_NUM       MICROPY_HW_I2C0_SDA_PIN_NUM
#define MICROPY_HW_I2C_TARGET_SCL_HSIOM         MICROPY_HW_I2C0_SCL_HSIOM
#define MICROPY_HW_I2C_TARGET_SDA_HSIOM         MICROPY_HW_I2C0_SDA_HSIOM
