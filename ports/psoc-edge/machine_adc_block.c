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
#include "extmod/modmachine.h"
#include "machine_pin.h"

// PSE84 SAR ADC: 1 block, fixed 12-bit resolution
#define PSE84_ADC_BLOCK_ID    (0)
#define PSE84_ADC_BLOCK_BITS  (12)
#define PSE84_ADC_NUM_CHANNELS (8)

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

// Map P15_x pin to channel index (0xFF if not ADC pin).
static uint8_t machine_adc_get_channel_from_pin(const machine_pin_obj_t *pin) {
    if (pin->port == 15 && pin->pin < PSE84_ADC_NUM_CHANNELS) {
        return (uint8_t)pin->pin;
    }
    return 0xFF;
}

// Find pin object for channel index.
static const machine_pin_obj_t *machine_adc_get_pin_from_channel(uint8_t channel) {
    for (size_t i = 0; i < machine_pin_cpu_pins_locals_dict.map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&machine_pin_cpu_pins_locals_dict.map, i)) {
            continue;
        }
        const machine_pin_obj_t *pin = MP_OBJ_TO_PTR(machine_pin_cpu_pins_locals_dict.map.table[i].value);
        if (pin->port == 15 && pin->pin == channel) {
            return pin;
        }
    }
    return NULL;
}

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
    if (self->unit != PSE84_ADC_BLOCK_ID) {
        return NULL;
    }

    enum {
        ARG_channel,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_channel, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(0, NULL, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_channel].u_int >= 0) {
        channel_id = args[ARG_channel].u_int;
    }

    if (channel_id >= PSE84_ADC_NUM_CHANNELS) {
        return NULL;
    }

    const machine_pin_obj_t *resolved_pin = pin;
    uint8_t resolved_channel = 0xFF;

    if ((mp_uint_t)resolved_pin != (mp_uint_t)-1) {
        resolved_channel = machine_adc_get_channel_from_pin(resolved_pin);
        if (resolved_channel == 0xFF) {
            return NULL;
        }
    }

    if (channel_id >= 0) {
        if (resolved_channel != 0xFF && resolved_channel != channel_id) {
            return NULL;
        }
        resolved_channel = (uint8_t)channel_id;
    }

    if (resolved_channel == 0xFF) {
        return NULL;
    }

    if (resolved_pin == NULL || (mp_uint_t)resolved_pin == (mp_uint_t)-1) {
        resolved_pin = machine_adc_get_pin_from_channel(resolved_channel);
        if (resolved_pin == NULL) {
            return NULL;
        }
    }

    // Reuse ADC constructor so init and validation stay in one place.
    mp_obj_t adc_args[1] = { MP_OBJ_FROM_PTR(resolved_pin) };
    mp_obj_t adc_obj = MP_OBJ_TYPE_GET_SLOT(&machine_adc_type, make_new)(&machine_adc_type, 1, 0, adc_args);
    return MP_OBJ_TO_PTR(adc_obj);
}
