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

#include <stdbool.h>

#include "py/runtime.h"
#include "py/mphal.h"

#include "modmachine.h"
#include "machine_adcblock.h"
#include "machine_adc.h"
#include "machine_pin.h"

#include "cybsp.h"
#include "cy_autanalog.h"
#include "cycfg_peripherals.h"

extern void adc_obj_init(machine_adc_obj_t *adc, machine_adcblock_obj_t *adc_block, mp_obj_t pin_name, uint32_t sampling_time);
extern void adc_obj_deinit(machine_adc_obj_t *adc);

machine_adcblock_obj_t *adc_block[MAX_BLOCKS] = {NULL};
static bool adc_autanalog_initialized = false;

const adc_block_channel_pin_map_t adc_block_pin_map[] = {
    {ADCBLOCK0, 0, (15u << 8) | 1u},
};

static bool adc_block_map_has_pin(uint32_t pin_addr) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (adc_block_pin_map[i].pin == pin_addr) {
            return true;
        }
    }
    return false;
}

uint32_t adc_pin_addr_by_obj(mp_obj_t pin_obj) {
    const machine_pin_obj_t *pin = mp_hal_get_pin_obj(pin_obj);
    uint32_t pin_addr = ((uint32_t)pin->port << 8) | pin->pin;
    if (!adc_block_map_has_pin(pin_addr)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Pin doesn't have ADC capabilities"));
    }
    return pin_addr;
}

mp_obj_t adc_pin_obj_by_addr(uint32_t pin_addr) {
    const mp_map_t *named_map = &machine_pin_board_pins_locals_dict.map;
    for (size_t i = 0; i < named_map->alloc; i++) {
        if (mp_map_slot_is_filled(named_map, i)) {
            mp_obj_t pin_obj = named_map->table[i].value;
            const machine_pin_obj_t *pin = MP_OBJ_TO_PTR(pin_obj);
            if (pin_addr == (((uint32_t)pin->port << 8) | pin->pin)) {
                return pin_obj;
            }
        }
    }

    named_map = &machine_pin_cpu_pins_locals_dict.map;
    for (size_t i = 0; i < named_map->alloc; i++) {
        if (mp_map_slot_is_filled(named_map, i)) {
            mp_obj_t pin_obj = named_map->table[i].value;
            const machine_pin_obj_t *pin = MP_OBJ_TO_PTR(pin_obj);
            if (pin_addr == (((uint32_t)pin->port << 8) | pin->pin)) {
                return pin_obj;
            }
        }
    }

    return MP_OBJ_NULL;
}

int16_t adc_get_channel_number_for_pin(uint32_t pin) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (pin == adc_block_pin_map[i].pin) {
            return adc_block_pin_map[i].channel;
        }
    }
    return -1;
}

static int32_t _get_adc_pin_number_for_channel(uint16_t channel) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (channel == adc_block_pin_map[i].channel) {
            return (int32_t)adc_block_pin_map[i].pin;
        }
    }
    return -1;
}

static int16_t _get_block_id_for_pin(uint32_t pin) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (pin == adc_block_pin_map[i].pin) {
            return adc_block_pin_map[i].block_id;
        }
    }
    return -1;
}

static inline bool _is_valid_id(uint8_t adc_id) {
    return adc_id < MAX_BLOCKS;
}

static machine_adcblock_obj_t *_adc_block_obj_find(uint8_t adc_block_id) {
    if (!_is_valid_id(adc_block_id)) {
        mp_raise_ValueError(MP_ERROR_TEXT("specified ADC id not supported"));
    }

    for (uint8_t i = 0; i < MAX_BLOCKS; i++) {
        if (adc_block[i] != NULL && adc_block[i]->id == adc_block_id) {
            return adc_block[i];
        }
    }

    return NULL;
}

static machine_adcblock_obj_t *_adc_block_obj_alloc(void) {
    for (uint8_t i = 0; i < MAX_BLOCKS; i++) {
        if (adc_block[i] == NULL) {
            adc_block[i] = mp_obj_malloc(machine_adcblock_obj_t, &machine_adcblock_type);
            return adc_block[i];
        }
    }

    return NULL;
}

static void _adc_block_obj_free(machine_adcblock_obj_t *adc_block_ptr) {
    for (uint8_t i = 0; i < MAX_BLOCKS; i++) {
        if (adc_block[i] == adc_block_ptr) {
            adc_block[i] = NULL;
            return;
        }
    }
}

machine_adc_obj_t *adc_block_channel_alloc(machine_adcblock_obj_t *adc_block_ptr, mp_obj_t pin) {
    int16_t adc_channel_no = adc_get_channel_number_for_pin(adc_pin_addr_by_obj(pin));
    if (adc_channel_no < 0 || adc_channel_no >= ADC_BLOCK_CHANNEL_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC channel"));
    }

    adc_block_ptr->channel[adc_channel_no] = mp_obj_malloc(machine_adc_obj_t, &machine_adc_type);
    return adc_block_ptr->channel[adc_channel_no];
}

void adc_block_channel_free(machine_adcblock_obj_t *adc_block_ptr, machine_adc_obj_t *adc) {
    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        if (adc_block_ptr->channel[i] == adc) {
            adc_block_ptr->channel[i] = NULL;
            return;
        }
    }
}

static uint32_t _adc_block_get_any_pin(uint16_t adc_block_id) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (adc_block_pin_map[i].block_id == adc_block_id) {
            return adc_block_pin_map[i].pin;
        }
    }

    return 0;
}

static void _adc_block_obj_init(machine_adcblock_obj_t *adc_block_ptr, uint16_t adc_block_id, uint8_t bits) {
    if (!adc_autanalog_initialized) {
        uint32_t status = Cy_AutAnalog_Init(&autonomous_analog_init);
        if (status != CY_AUTANALOG_SUCCESS) {
            mp_raise_TypeError(MP_ERROR_TEXT("ADC initialization failed"));
        }

        Cy_AutAnalog_SetInterruptMask(CY_AUTANALOG_INT_SAR0_RESULT);
        Cy_AutAnalog_StartAutonomousControl();
        adc_autanalog_initialized = true;
    }

    (void)_adc_block_get_any_pin(adc_block_id);

    adc_block_ptr->id = adc_block_id;
    adc_block_ptr->bits = bits;
    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        adc_block_ptr->channel[i] = NULL;
    }
}

static void _adc_block_obj_deinit(machine_adcblock_obj_t *adc_block_ptr) {
    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        if (adc_block_ptr->channel[i] != NULL) {
            adc_obj_deinit(adc_block_ptr->channel[i]);
            adc_block_channel_free(adc_block_ptr, adc_block_ptr->channel[i]);
        }
    }
}

static machine_adcblock_obj_t *machine_adcblock_make_init(uint8_t adc_id, uint8_t bits) {
    machine_adcblock_obj_t *adc_block_ptr = _adc_block_obj_find(adc_id);

    if (adc_block_ptr == NULL) {
        adc_block_ptr = _adc_block_obj_alloc();
        if (adc_block_ptr == NULL) {
            mp_raise_TypeError(MP_ERROR_TEXT("ADC blocks are fully allocated"));
        }
        _adc_block_obj_init(adc_block_ptr, adc_id, bits);
    }

    return adc_block_ptr;
}

machine_adcblock_obj_t *adc_block_obj_find(mp_obj_t pin) {
    int adc_block_id = _get_block_id_for_pin(adc_pin_addr_by_obj(pin));
    if (adc_block_id < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("no ADC block associated with pin"));
    }

    return _adc_block_obj_find(adc_block_id);
}

machine_adcblock_obj_t *adc_block_obj_init(mp_obj_t pin) {
    int adc_block_id = _get_block_id_for_pin(adc_pin_addr_by_obj(pin));
    if (adc_block_id < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("no ADC block associated with pin"));
    }

    return machine_adcblock_make_init(adc_block_id, DEFAULT_ADC_BITS);
}

machine_adc_obj_t *adc_block_channel_find(machine_adcblock_obj_t *adc_block_ptr, mp_obj_t pin) {
    int16_t adc_channel_no = adc_get_channel_number_for_pin(adc_pin_addr_by_obj(pin));
    if (adc_channel_no < 0 || adc_channel_no >= ADC_BLOCK_CHANNEL_MAX) {
        return NULL;
    }
    return adc_block_ptr->channel[adc_channel_no];
}

static void machine_adcblock_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "ADCBlock(%u, bits=%u)", self->id, self->bits);
}

static mp_obj_t machine_adcblock_make_new(const mp_obj_type_t *type, size_t n_pos_args, size_t n_kw_args, const mp_obj_t *all_args) {
    mp_arg_check_num(n_pos_args, n_kw_args, 1, MP_OBJ_FUN_ARGS_MAX, true);

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw_args, all_args + n_pos_args);

    uint8_t adc_id = mp_obj_get_int(all_args[0]);

    enum { ARG_bits };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bits, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = DEFAULT_ADC_BITS} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args - 1, all_args + 1, &kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint8_t bits = args[ARG_bits].u_int;
    if (bits != DEFAULT_ADC_BITS) {
        mp_raise_ValueError(MP_ERROR_TEXT("only 12-bit ADC is currently supported"));
    }

    return MP_OBJ_FROM_PTR(machine_adcblock_make_init(adc_id, bits));
}

static mp_obj_t machine_adcblock_deinit(mp_obj_t self_in) {
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    _adc_block_obj_deinit(self);
    _adc_block_obj_free(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_adcblock_deinit_obj, machine_adcblock_deinit);

static mp_obj_t machine_adcblock_connect(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    (void)kw_args;
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    uint8_t channel = 0xff;
    mp_obj_t pin_name = MP_OBJ_NULL;

    if (n_pos_args == 2) {
        if (mp_obj_is_int(pos_args[1])) {
            channel = mp_obj_get_int(pos_args[1]);
            int32_t pin_addr = _get_adc_pin_number_for_channel(channel);
            if (pin_addr < 0) {
                mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC channel"));
            }
            pin_name = adc_pin_obj_by_addr((uint32_t)pin_addr);
            if (pin_name == MP_OBJ_NULL) {
                mp_raise_ValueError(MP_ERROR_TEXT("channel pin is not exposed as a Python Pin"));
            }
        } else {
            pin_name = pos_args[1];
        }
    } else if (n_pos_args == 3) {
        channel = mp_obj_get_int(pos_args[1]);
        int32_t exp_pin = _get_adc_pin_number_for_channel(channel);
        uint32_t actual_pin = adc_pin_addr_by_obj(pos_args[2]);
        if (exp_pin < 0 || (uint32_t)exp_pin != actual_pin) {
            mp_raise_ValueError(MP_ERROR_TEXT("wrong pin for requested ADC channel"));
        }
        pin_name = pos_args[2];
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("too many positional args"));
    }

    machine_adc_obj_t *adc = adc_block_channel_find(self, pin_name);
    if (adc == NULL) {
        adc = adc_block_channel_alloc(self, pin_name);
        adc_obj_init(adc, self, pin_name, DEFAULT_ADC_ACQ_NS);
    }

    return MP_OBJ_FROM_PTR(adc);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_adcblock_connect_obj, 2, machine_adcblock_connect);

void machine_adcblock_deinit_all(void) {
    for (uint8_t i = 0; i < MAX_BLOCKS; i++) {
        if (adc_block[i] != NULL) {
            _adc_block_obj_deinit(adc_block[i]);
            _adc_block_obj_free(adc_block[i]);
        }
    }
}

static const mp_rom_map_elem_t machine_adcblock_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_adcblock_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&machine_adcblock_connect_obj) },
};
static MP_DEFINE_CONST_DICT(machine_adcblock_locals_dict, machine_adcblock_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_adcblock_type,
    MP_QSTR_ADCBlock,
    MP_TYPE_FLAG_NONE,
    make_new, machine_adcblock_make_new,
    print, machine_adcblock_print,
    locals_dict, &machine_adcblock_locals_dict
    );
