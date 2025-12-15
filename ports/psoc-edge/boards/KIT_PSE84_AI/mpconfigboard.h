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

#define MICROPY_GC_HEAP_SIZE                    (32 * 1024) // TODO: 315 was too big for non-secure RAM?

// I2C Configuration
#define MICROPY_HW_I2C0_SCB                     (SCB5)
#define MICROPY_HW_I2C0_SCL_PORT                GPIO_PRT17
#define MICROPY_HW_I2C0_SCL_PIN                 P17_0_NUM
#define MICROPY_HW_I2C0_SCL_HSIOM               P17_0_SCB5_I2C_SCL
#define MICROPY_HW_I2C0_SDA_PORT                GPIO_PRT17
#define MICROPY_HW_I2C0_SDA_PIN                 P17_1_NUM
#define MICROPY_HW_I2C0_SDA_HSIOM               P17_1_SCB5_I2C_SDA
#define MICROPY_HW_I2C0_SCL                     (P17_0_NUM)
#define MICROPY_HW_I2C0_SDA                     (P17_1_NUM)
#define MAX_I2C                                 1
#define MICROPY_HW_I2C_INTR_PRIORITY            (7UL)
#define MICROPY_HW_I2C_PCLK                     PCLK_SCB5_CLOCK_SCB_EN
#define MICROPY_HW_I2C_IRQn                     scb_5_interrupt_IRQn
