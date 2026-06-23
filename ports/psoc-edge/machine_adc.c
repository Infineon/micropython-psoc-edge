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
#include "machine_adc.h"
#include "machine_adcblock.h"

#include "cybsp.h"
#include "cy_autanalog.h"
#include "cy_autanalog_sar.h"

#define PSOC_EDGE_SAR_ADC_INDEX      (0u)
#define PSOC_EDGE_SAR_ADC_SEQUENCER  (0u)
#define PSOC_EDGE_SAR_ADC_VREF_MV    (1800u)

void adc_obj_init(machine_adc_obj_t *adc, machine_adcblock_obj_t *adc_block, mp_obj_t pin_name, uint32_t sampling_time) {
    uint32_t pin_addr = adc_pin_addr_by_obj(pin_name);
    int16_t adc_channel_no = adc_get_channel_number_for_pin(pin_addr);
    if (adc_channel_no < 0 || adc_channel_no >= ADC_BLOCK_CHANNEL_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC channel"));
    }

    adc->pin_addr = pin_addr;
    adc->block = adc_block;
    adc->sample_ns = sampling_time;
    adc->channel_id = (uint8_t)adc_channel_no;
}

void adc_obj_deinit(machine_adc_obj_t *adc) {
    (void)adc;
}

static machine_adc_obj_t *machine_adc_make_init(uint32_t sampling_time, mp_obj_t pin_name) {
    machine_adc_obj_t *adc;

    machine_adcblock_obj_t *adc_block = adc_block_obj_find(pin_name);
    if (adc_block == NULL) {
        adc_block = adc_block_obj_init(pin_name);
    } else {
        adc = adc_block_channel_find(adc_block, pin_name);
        if (adc != NULL) {
            adc->sample_ns = sampling_time;
            return adc;
        }
    }

    adc = adc_block_channel_alloc(adc_block, pin_name);
    adc_obj_init(adc, adc_block, pin_name, sampling_time);

    return adc;
}

static uint8_t adc_get_resolution(machine_adc_obj_t *adc) {
    return adc->block->bits;
}

static void machine_adc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<ADC Pin=%u, ADCBlock_id=%u, sampling_time_ns=%lu>", self->pin_addr, self->block->id, self->sample_ns);
}

static mp_obj_t machine_adc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 6, true);

    enum { ARG_sample_ns };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_ns, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = DEFAULT_ADC_ACQ_NS} },
    };

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, all_args + 1, &kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint32_t sampling_time = args[ARG_sample_ns].u_int;
    machine_adc_obj_t *o = machine_adc_make_init(sampling_time, all_args[0]);

    return MP_OBJ_FROM_PTR(o);
}

static mp_obj_t machine_adc_deinit(mp_obj_t self_in) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    adc_obj_deinit(self);
    adc_block_channel_free(self->block, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_adc_deinit_obj, machine_adc_deinit);

static mp_obj_t machine_adc_block(mp_obj_t self_in) {
    const machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_FROM_PTR(self->block);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_adc_block_obj, machine_adc_block);

static mp_obj_t machine_adc_read_u16(mp_obj_t self_in) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int32_t raw = Cy_AutAnalog_SAR_ReadResult(PSOC_EDGE_SAR_ADC_INDEX, CY_AUTANALOG_SAR_INPUT_GPIO, self->channel_id);
    if (raw < 0) {
        raw = 0;
    }
    mp_int_t bits = (mp_int_t)adc_get_resolution(self);
    mp_uint_t u16 = ((mp_uint_t)raw) << (16 - bits);
    return MP_OBJ_NEW_SMALL_INT(u16);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_adc_read_u16_obj, machine_adc_read_u16);

static mp_obj_t machine_adc_read_uv(mp_obj_t self_in) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int32_t raw = Cy_AutAnalog_SAR_ReadResult(PSOC_EDGE_SAR_ADC_INDEX, CY_AUTANALOG_SAR_INPUT_GPIO, self->channel_id);
    if (raw < 0) {
        raw = 0;
    }
    int16_t mv = Cy_AutAnalog_SAR_CountsTo_mVolts(
        PSOC_EDGE_SAR_ADC_INDEX,
        false,
        PSOC_EDGE_SAR_ADC_SEQUENCER,
        CY_AUTANALOG_SAR_INPUT_GPIO,
        self->channel_id,
        PSOC_EDGE_SAR_ADC_VREF_MV,
        raw
        );
    uint32_t uv = (uint32_t)mv * 1000u;
    return MP_OBJ_NEW_SMALL_INT(uv);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_adc_read_uv_obj, machine_adc_read_uv);

static const mp_rom_map_elem_t machine_adc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_adc_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_u16), MP_ROM_PTR(&machine_adc_read_u16_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_uv), MP_ROM_PTR(&machine_adc_read_uv_obj) },
    { MP_ROM_QSTR(MP_QSTR_block), MP_ROM_PTR(&machine_adc_block_obj) },
};
static MP_DEFINE_CONST_DICT(machine_adc_locals_dict, machine_adc_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_adc_type,
    MP_QSTR_ADC,
    MP_TYPE_FLAG_NONE,
    make_new, machine_adc_make_new,
    print, machine_adc_print,
    locals_dict, &machine_adc_locals_dict
    );
