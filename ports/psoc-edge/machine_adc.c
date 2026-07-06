/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Infineon Technologies AG
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

// This file is never compiled standalone, it is included directly from
// extmod/machine_adc.c via MICROPY_PY_MACHINE_ADC_INCLUDEFILE.

#include "py/mphal.h"
#include "py/mperrno.h"
#include "machine_pin.h"
#include "cycfg_peripherals.h"
#include "cycfg_system.h"
#include "cy_autanalog.h"
#include "cy_autanalog_sar.h"

// PSE84 SAR ADC: 1 block, 8 GPIO channels (P15_0 to P15_7), 3.3V VDDA
#define PSE84_ADC_BLOCK_ID    (0)
#define PSE84_ADC_NUM_CHANNELS (8)
#define PSE84_ADC_VDDA_MV    (3300)
#define PSE84_ADC_READ_TIMEOUT_US (1000)

#define MICROPY_PY_MACHINE_ADC_CLASS_CONSTANTS

// Global state: track if autonomous analog has been initialized
static bool adc_autanalog_initialized = false;

// ADC channel object: stores pin mapping
typedef struct _machine_adc_obj_t {
    mp_obj_base_t base;
    const machine_pin_obj_t *pin;      // GPIO pin used as the ADC input
    uint8_t sar_block;                 // SAR block index (always 0 on PSE84)
    uint8_t gpio_channel;              // GPIO channel index (0-7 for P15_0-P15_7)
} machine_adc_obj_t;

// ADC.__repr__ -> <ADC pin='P15_0' ch=0>
static void mp_machine_adc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<ADC pin='%q' ch=%u>", self->pin->name, self->gpio_channel);
}

// Map P15_x pin to channel index (0xFF if not ADC pin)
static uint8_t machine_adc_get_channel_from_pin(const machine_pin_obj_t *pin) {
    if (pin->port == 15 && pin->pin < PSE84_ADC_NUM_CHANNELS) {
        return (uint8_t)pin->pin;
    }
    return 0xFF;  // Invalid ADC pin
}

// Initialize autonomous analog controller and SAR ADC (called once on first ADC creation)
static void machine_adc_init_autanalog(void) {
    if (adc_autanalog_initialized) {
        return;
    }

    // Configure all GPIO channels
    for (size_t i = 0; i < PSE84_ADC_NUM_CHANNELS; i++) {
        CYBSP_SAR_ADC_sta_hs_cfg.hsGpioChan[i] = &CYBSP_SAR_ADC_gpio_ch_cfg[i];
    }
    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioResultMask = 0xFFU;  // All GPIO channels
    CYBSP_SAR_ADC_seq_hs_cfg[0].chanEn = 0xFFU;         // All GPIO channels

    // Initialize autonomous analog controller
    uint32_t status = Cy_AutAnalog_Init(&autonomous_analog_init);
    if (status != CY_AUTANALOG_SUCCESS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ADC autanalog init failed"));
    }

    // Start autonomous controller
    Cy_AutAnalog_StartAutonomousControl();
    adc_autanalog_initialized = true;
}

// ADC(pin) -> machine_adc_obj_t *; validates pin
static mp_obj_t mp_machine_adc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    // Initialize autonomous analog on first call
    machine_adc_init_autanalog();

    const machine_pin_obj_t *pin = mp_hal_get_pin_obj(args[0]);
    uint8_t channel = machine_adc_get_channel_from_pin(pin);
    if (channel == 0xFF) {
        mp_raise_ValueError(MP_ERROR_TEXT("Pin doesn't have ADC capabilities"));
    }
    // Check if channel is configured in BSP
    if (CYBSP_SAR_ADC_sta_hs_cfg.hsGpioChan[channel] == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("ADC channel not available"));
    }
    machine_adc_obj_t *o = mp_obj_malloc(machine_adc_obj_t, &machine_adc_type);
    o->pin = pin;
    o->sar_block = PSE84_ADC_BLOCK_ID;
    o->gpio_channel = channel;
    return MP_OBJ_FROM_PTR(o);
}

// ADC.read_u16() -> int (0-65535, 0=0V, 65535=3.3V)
static mp_int_t mp_machine_adc_read_u16(machine_adc_obj_t *self) {
    // Reload config and restart autonomous control before each read
    uint32_t status = Cy_AutAnalog_SAR_LoadConfig(PSE84_ADC_BLOCK_ID, &CYBSP_SAR_ADC_cfg);
    if (status != CY_AUTANALOG_SUCCESS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ADC config load failed"));
    }
    Cy_AutAnalog_StartAutonomousControl();

    uint8_t channel_mask = (uint8_t)(1u << self->gpio_channel);
    uint32_t timeout = PSE84_ADC_READ_TIMEOUT_US;

    // Clear result status and wait for new result
    Cy_AutAnalog_SAR_ClearHSchanResultStatus(PSE84_ADC_BLOCK_ID, channel_mask);
    while (((Cy_AutAnalog_SAR_GetHSchanResultStatus(PSE84_ADC_BLOCK_ID) & channel_mask) == 0u) && timeout != 0u) {
        mp_hal_delay_us(1);
        timeout--;
    }

    if (timeout == 0u) {
        mp_raise_OSError(MP_ETIMEDOUT);
    }

    // Read 12-bit result, scale to 16-bit (0-65535)
    int32_t raw = Cy_AutAnalog_SAR_ReadResult(PSE84_ADC_BLOCK_ID, CY_AUTANALOG_SAR_INPUT_GPIO, self->gpio_channel);
    if (raw < 0) {
        raw = 0;
    }
    // Scale 12-bit (0-4095) to 16-bit (0-65535)
    return (mp_int_t)((((uint32_t)raw & 0xFFF) * 65535) / 4095);
}

// ADC.read_uv() -> int (0-3,300,000 microvolts)
static mp_int_t mp_machine_adc_read_uv(machine_adc_obj_t *self) {
    uint16_t u16 = (uint16_t)mp_machine_adc_read_u16(self);
    return (mp_int_t)((uint32_t)u16 * 3300000 / 65535);
}

// ADC.deinit() - no-op on PSE84
static void mp_machine_adc_deinit(machine_adc_obj_t *self) {
    (void)self;
}

// ADC.block() -> ADCBlock or None
static mp_obj_t mp_machine_adc_block(machine_adc_obj_t *self) {
    (void)self;
    return mp_const_none;
}
