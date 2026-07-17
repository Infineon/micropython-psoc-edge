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

#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modmachine.h"
#include "machine_pin.h"
#include "genhdr/pins_af.h"

// PSoC Edge SAR ADC: 1 block, fixed 12-bit resolution
#define PSOC_EDGE_ADC_BLOCK_ID      (0)
#define PSOC_EDGE_ADC_BLOCK_BITS    (12)
#define PSOC_EDGE_ADC_NUM_CHANNELS  (8)

// ADCBlock object: unit and bits fields
typedef struct _machine_adc_block_obj_t {
    mp_obj_base_t base;
    uint8_t unit; // SAR block index — always 0 on PSoC Edge
    uint8_t bits; // current resolution — fixed at 12 for PSoC Edge SAR
} machine_adc_block_obj_t;

// ADCBlock(0) singleton instance
static machine_adc_block_obj_t machine_adc_block_obj = {
    .base = {&machine_adc_block_type},
    .unit = PSOC_EDGE_ADC_BLOCK_ID,
    .bits = PSOC_EDGE_ADC_BLOCK_BITS,
};

// ADCBlock.__repr__ -> <ADCBlock 0 bits=12>
static void mp_machine_adc_block_print(const mp_print_t *print, machine_adc_block_obj_t *self) {
    mp_printf(print, "<ADCBlock %u bits=%u>", self->unit, self->bits);
}

// ADCBlock(id) -> machine_adc_block_obj_t or NULL; only id=0 valid
static machine_adc_block_obj_t *mp_machine_adc_block_get(mp_int_t unit) {
    if (unit == PSOC_EDGE_ADC_BLOCK_ID) {
        return &machine_adc_block_obj;
    }
    return NULL;
}

// ADCBlock.init(bits=N) -> validate bits==12, ignore -1
static void mp_machine_adc_block_bits_set(machine_adc_block_obj_t *self, mp_int_t bits) {
    if (bits == -1) {
        return;              // No-op
    }
    if (bits != PSOC_EDGE_ADC_BLOCK_BITS) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid bits"));
    }
    (void)self;
}

// Map pin object to ADC block/channel.
static bool machine_adc_block_get_block_channel_from_pin(
    const machine_pin_obj_t *pin, uint8_t *sar_block, uint8_t *gpio_channel) {
    #define MICROPY_HW_ADC_PIN_MAP_ENTRY(b, c, p, pn) \
    if (pin->port == p && pin->pin == pn) { \
        *sar_block = (uint8_t)(b); \
        *gpio_channel = (uint8_t)(c); \
        return true; \
    }
    MICROPY_HW_ADC_PIN_MAP(MICROPY_HW_ADC_PIN_MAP_ENTRY)
#undef MICROPY_HW_ADC_PIN_MAP_ENTRY
    return false;
}

// Map ADC block/channel to pin object.
static const machine_pin_obj_t *machine_adc_block_get_pin_from_channel(uint8_t block, uint8_t channel) {
    uint8_t adc_port;
    uint8_t adc_pin;
    bool found = false;

    #define MICROPY_HW_ADC_PIN_MAP_ENTRY(b, c, p, pn) \
    if ((uint8_t)(b) == block && (uint8_t)(c) == channel) { \
        adc_port = (uint8_t)(p); \
        adc_pin = (uint8_t)(pn); \
        found = true; \
    }
    MICROPY_HW_ADC_PIN_MAP(MICROPY_HW_ADC_PIN_MAP_ENTRY)
#undef MICROPY_HW_ADC_PIN_MAP_ENTRY

    if (!found) {
        return NULL;
    }

    const mp_map_t *cpu_map = &machine_pin_cpu_pins_locals_dict.map;
    for (size_t i = 0; i < cpu_map->alloc; i++) {
        const mp_map_elem_t *elem = &cpu_map->table[i];
        if (elem->value == MP_OBJ_NULL) {
            continue;
        }
        const machine_pin_obj_t *pin = MP_OBJ_TO_PTR(elem->value);
        if (pin->port == adc_port && pin->pin == adc_pin) {
            return pin;
        }
    }

    return NULL;
}

// ADCBlock.connect(channel, pin) -> machine_adc_obj_t or NULL
static machine_adc_obj_t *mp_machine_adc_block_connect(machine_adc_block_obj_t *self,
    mp_int_t channel_id, mp_hal_pin_obj_t pin, mp_map_t *kw_args) {
    if (kw_args != NULL && kw_args->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("keyword args not supported"));
    }

    if (self->unit != PSOC_EDGE_ADC_BLOCK_ID) {
        return NULL;
    }

    const machine_pin_obj_t *resolved_pin = NULL;
    uint8_t pin_block = 0;
    uint8_t pin_channel = 0;

    if (pin != MP_HAL_PIN_OBJ_NULL) {
        resolved_pin = pin;
        if (!machine_adc_block_get_block_channel_from_pin(resolved_pin, &pin_block, &pin_channel)) {
            return NULL;
        }
        if (pin_block != self->unit) {
            return NULL;
        }
    }

    if (channel_id >= 0) {
        if ((uint8_t)channel_id >= PSOC_EDGE_ADC_NUM_CHANNELS) {
            return NULL;
        }
        if (resolved_pin == NULL) {
            resolved_pin = machine_adc_block_get_pin_from_channel((uint8_t)self->unit, (uint8_t)channel_id);
            if (resolved_pin == NULL) {
                return NULL;
            }
        } else if (pin_channel != (uint8_t)channel_id) {
            return NULL;
        }
    }

    if (resolved_pin == NULL) {
        return NULL;
    }

    mp_obj_t adc_pin_obj = MP_OBJ_FROM_PTR(resolved_pin);
    mp_obj_t adc_obj = MP_OBJ_TYPE_GET_SLOT(&machine_adc_type, make_new)(&machine_adc_type, 1, 0, &adc_pin_obj);
    return MP_OBJ_TO_PTR(adc_obj);
}
