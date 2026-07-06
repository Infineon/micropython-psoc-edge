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
// extmod/machine_adc_block.c via MICROPY_PY_MACHINE_ADC_BLOCK_INCLUDEFILE.

#include "py/mphal.h"
#include "machine_pin.h"

// PSE84 SAR ADC: 1 block, fixed 12-bit resolution
#define PSE84_ADC_BLOCK_ID    (0)
#define PSE84_ADC_BLOCK_BITS  (12)

// ADCBlock object: unit and bits fields
typedef struct _machine_adc_block_obj_t {
    mp_obj_base_t base;
    uint8_t unit; // SAR block index — always 0 on PSE84
    uint8_t bits; // current resolution — fixed at 12 for PSE84 SAR
} machine_adc_block_obj_t;

// ADCBlock(0) singleton instance
static machine_adc_block_obj_t machine_adc_block_obj = {
    .base = {&machine_adc_block_type},
    .unit = PSE84_ADC_BLOCK_ID,
    .bits = PSE84_ADC_BLOCK_BITS,
};

// ADCBlock.__repr__ -> <ADCBlock 0 bits=12>
static void mp_machine_adc_block_print(const mp_print_t *print, machine_adc_block_obj_t *self) {
    mp_printf(print, "<ADCBlock %u bits=%u>", self->unit, self->bits);
}

// ADCBlock(id) -> machine_adc_block_obj_t or NULL; only id=0 valid
static machine_adc_block_obj_t *mp_machine_adc_block_get(mp_int_t unit) {
    if (unit == PSE84_ADC_BLOCK_ID) {
        return &machine_adc_block_obj;
    }
    return NULL;
}

// ADCBlock.init(bits=N) -> validate bits==12, ignore -1
static void mp_machine_adc_block_bits_set(machine_adc_block_obj_t *self, mp_int_t bits) {
    if (bits == -1) {
        return;              // No-op
    }
    if (bits != PSE84_ADC_BLOCK_BITS) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid bits"));
    }
    (void)self;
}

// ADCBlock.connect(channel, pin) -> machine_adc_obj_t or NULL
static machine_adc_obj_t *mp_machine_adc_block_connect(machine_adc_block_obj_t *self,
    mp_int_t channel_id, mp_hal_pin_obj_t pin, mp_map_t *kw_args) {
    (void)self;
    (void)channel_id;
    (void)pin;
    (void)kw_args;
    return NULL;
}
