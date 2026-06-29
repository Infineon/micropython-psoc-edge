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

#ifndef MICROPY_INCLUDED_PSOC_EDGE_MACHINE_ADCBLOCK_H
#define MICROPY_INCLUDED_PSOC_EDGE_MACHINE_ADCBLOCK_H

#include "py/obj.h"

#define ADC_HW_NATIVE_BITS      (12)
#define DEFAULT_ADC_BITS        ADC_HW_NATIVE_BITS
#define ADC_MIN_BITS            (8)
#define ADC_MAX_BITS            ADC_HW_NATIVE_BITS
#define ADC_BLOCK_CHANNEL_MAX   (16)
#define DEFAULT_ADC_ACQ_NS      (1000)

#define ADCBLOCK0               (0)

#ifndef MICROPY_HW_ADC_MAX_BLOCKS
#define MICROPY_HW_ADC_MAX_BLOCKS (1)
#endif

#define MAX_BLOCKS              MICROPY_HW_ADC_MAX_BLOCKS

typedef struct _machine_adc_obj_t machine_adc_obj_t;

typedef struct _machine_adcblock_obj_t {
    mp_obj_base_t base;
    uint8_t id;
    uint8_t bits;
    uint8_t active;
    uint8_t auto_deinit;
    machine_adc_obj_t *channel[ADC_BLOCK_CHANNEL_MAX];
} machine_adcblock_obj_t;

typedef struct {
    uint16_t block_id;
    uint16_t channel;
    uint32_t pin;
} adc_block_channel_pin_map_t;

typedef struct {
    uint16_t block_id;
    uint16_t channel_count;
} adc_block_capability_t;

machine_adcblock_obj_t *adc_block_obj_find(mp_obj_t pin);
machine_adcblock_obj_t *adc_block_obj_init(mp_obj_t pin);
machine_adc_obj_t *adc_block_channel_find(machine_adcblock_obj_t *adc_block, mp_obj_t pin);
machine_adc_obj_t *adc_block_channel_alloc(machine_adcblock_obj_t *adc_block, mp_obj_t pin);
void adc_block_channel_free(machine_adcblock_obj_t *adc_block, machine_adc_obj_t *adc);
void adc_block_maybe_release(machine_adcblock_obj_t *adc_block);
void adc_block_apply_runtime_config(machine_adcblock_obj_t *adc_block, uint32_t sample_ns);
int16_t adc_get_channel_number_for_pin(uint32_t pin);

uint32_t adc_pin_addr_by_obj(mp_obj_t pin_obj);
mp_obj_t adc_pin_obj_by_addr(uint32_t pin_addr);

void machine_adcblock_deinit_all(void);

extern const mp_obj_type_t machine_adcblock_type;

#endif // MICROPY_INCLUDED_PSOC_EDGE_MACHINE_ADCBLOCK_H
