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

#include "py/nlr.h"
#include "py/runtime.h"
#include "py/mphal.h"

#include "modmachine.h"
#include "machine_adcblock.h"
#include "machine_adc.h"
#include "machine_pin.h"

#include "cybsp.h"
#include "cy_autanalog.h"
#include "cy_autanalog_sar.h"
#include "cycfg_peripherals.h"

extern void adc_obj_init(machine_adc_obj_t *adc, machine_adcblock_obj_t *adc_block, mp_obj_t pin_name, uint32_t sampling_time);
extern void adc_obj_deinit(machine_adc_obj_t *adc);

machine_adcblock_obj_t *adc_block[MAX_BLOCKS] = {NULL};
static bool adc_autanalog_initialized = false;
static uint16_t adc_hw_sample_setting = UINT16_MAX;
static bool _adc_block_channel_is_valid(const machine_adcblock_obj_t *adc_block_ptr, uint16_t channel);
static void _adc_block_obj_deinit(machine_adcblock_obj_t *adc_block_ptr);

#define ADC_PIN(block, ch, port, pin)  {(block), (ch), ((uint32_t)(port) << 8) | (pin)},
#define ADC_CAP(block, ch_count)       {(block), (ch_count)},
#define ADC_GPIO_CHANNEL_COUNT         (ADC_BLOCK_CHANNEL_MAX)
#define ADC_GPIO_CHANNEL_MASK          ((uint8_t)0xffu)
#define ADC_HS_CLOCK_HZ                (80000000u)
#define ADC_HS_SAMPLE_TIME_MIN_CYCLES  (1u)
#define ADC_HS_SAMPLE_TIME_MAX_CYCLES  (1024u)
#define ADC_HS_SAMPLE_TIMER_INDEX      (0u)

static cy_stc_autanalog_sar_hs_chan_t adc_gpio_ch_cfg[ADC_GPIO_CHANNEL_COUNT] = {
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO0, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO1, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO2, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO3, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO4, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO5, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO6, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
    { .posPin = CY_AUTANALOG_SAR_PIN_GPIO7, .hsDiffEn = false, .sign = false, .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .negPin = CY_AUTANALOG_SAR_PIN_GPIO0, .accShift = false, .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED, .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED, .fifoSel = CY_AUTANALOG_FIFO_DISABLED },
};

#ifndef MICROPY_HW_ADC_BLOCK_CAPS
#define MICROPY_HW_ADC_BLOCK_CAPS(ENTRY) \
    ENTRY(ADCBLOCK0, ADC_BLOCK_CHANNEL_MAX)
#endif

#ifndef MICROPY_HW_ADC_PIN_LOOKUP_POLICY_BOARD_THEN_CPU
#define MICROPY_HW_ADC_PIN_LOOKUP_POLICY_BOARD_THEN_CPU (1)
#endif

#ifndef MICROPY_HW_ADC_PIN_MAP
#define MICROPY_HW_ADC_PIN_MAP(ENTRY) \
    ENTRY(ADCBLOCK0, 0, 15u, 0u) \
    ENTRY(ADCBLOCK0, 1, 15u, 1u) \
    ENTRY(ADCBLOCK0, 2, 15u, 2u) \
    ENTRY(ADCBLOCK0, 3, 15u, 3u) \
    ENTRY(ADCBLOCK0, 4, 15u, 4u) \
    ENTRY(ADCBLOCK0, 5, 15u, 5u) \
    ENTRY(ADCBLOCK0, 6, 15u, 6u) \
    ENTRY(ADCBLOCK0, 7, 15u, 7u)
#endif

static const adc_block_capability_t adc_block_caps[] = {
    MICROPY_HW_ADC_BLOCK_CAPS(ADC_CAP)
};

const adc_block_channel_pin_map_t adc_block_pin_map[] = {
    MICROPY_HW_ADC_PIN_MAP(ADC_PIN)
};

_Static_assert(MP_ARRAY_SIZE(adc_block_caps) <= MAX_BLOCKS,
    "ADC block capability descriptors exceed MAX_BLOCKS");
_Static_assert(MP_ARRAY_SIZE(adc_block_pin_map) <= ADC_BLOCK_CHANNEL_MAX,
    "ADC pin map exceeds ADC_BLOCK_CHANNEL_MAX");
_Static_assert(ADC_GPIO_CHANNEL_COUNT <= 8,
    "ADC GPIO channel count exceeds hardware support");

static void adc_hw_configure_supported_gpio_channels(void) {
    for (size_t i = 0; i < ADC_GPIO_CHANNEL_COUNT; i++) {
        CYBSP_SAR_ADC_sta_hs_cfg.hsGpioChan[i] = &adc_gpio_ch_cfg[i];
    }
    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioResultMask = ADC_GPIO_CHANNEL_MASK;
    CYBSP_SAR_ADC_sta_cfg.muxResultMask = CY_AUTANALOG_SAR_CHAN_MASK_MUX_DISABLED;
    CYBSP_SAR_ADC_seq_hs_cfg[0].chanEn = ADC_GPIO_CHANNEL_MASK;
    CYBSP_SAR_ADC_seq_hs_cfg[0].muxMode = CY_AUTANALOG_SAR_CHAN_CFG_MUX_DISABLED;
}

static bool adc_block_map_has_pin(uint32_t pin_addr) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (adc_block_pin_map[i].pin == pin_addr) {
            return true;
        }
    }
    return false;
}

static const adc_block_capability_t *adc_block_capability_find(uint16_t adc_block_id) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_caps); i++) {
        if (adc_block_caps[i].block_id == adc_block_id) {
            return &adc_block_caps[i];
        }
    }
    return NULL;
}

static mp_obj_t adc_pin_obj_find_in_dict(const mp_map_t *named_map, uint32_t pin_addr) {
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

uint32_t adc_pin_addr_by_obj(mp_obj_t pin_obj) {
    const machine_pin_obj_t *pin = mp_hal_get_pin_obj(pin_obj);
    uint32_t pin_addr = ((uint32_t)pin->port << 8) | pin->pin;
    if (!adc_block_map_has_pin(pin_addr)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Pin doesn't have ADC capabilities"));
    }
    return pin_addr;
}

mp_obj_t adc_pin_obj_by_addr(uint32_t pin_addr) {
    #if MICROPY_HW_ADC_PIN_LOOKUP_POLICY_BOARD_THEN_CPU
    mp_obj_t pin_obj = adc_pin_obj_find_in_dict(&machine_pin_board_pins_locals_dict.map, pin_addr);
    if (pin_obj != MP_OBJ_NULL) {
        return pin_obj;
    }
    return adc_pin_obj_find_in_dict(&machine_pin_cpu_pins_locals_dict.map, pin_addr);
    #else
    return adc_pin_obj_find_in_dict(&machine_pin_cpu_pins_locals_dict.map, pin_addr);
    #endif
}

int16_t adc_get_channel_number_for_pin(uint32_t pin) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (pin == adc_block_pin_map[i].pin) {
            return adc_block_pin_map[i].channel;
        }
    }
    return -1;
}

static int32_t _get_adc_pin_number_for_channel(uint16_t block_id, uint16_t channel) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (block_id == adc_block_pin_map[i].block_id && channel == adc_block_pin_map[i].channel) {
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
    return adc_block_capability_find(adc_id) != NULL;
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

static bool _adc_block_has_active_channels(const machine_adcblock_obj_t *adc_block_ptr) {
    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        if (adc_block_ptr->channel[i] != NULL) {
            return true;
        }
    }

    return false;
}

machine_adc_obj_t *adc_block_channel_alloc(machine_adcblock_obj_t *adc_block_ptr, mp_obj_t pin) {
    int16_t adc_channel_no = adc_get_channel_number_for_pin(adc_pin_addr_by_obj(pin));
    if (adc_channel_no < 0 || !_adc_block_channel_is_valid(adc_block_ptr, (uint16_t)adc_channel_no)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC channel"));
    }

    if (adc_block_ptr->channel[adc_channel_no] != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("ADC channel already in use"));
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

void adc_block_maybe_release(machine_adcblock_obj_t *adc_block_ptr) {
    if (adc_block_ptr == NULL || !adc_block_ptr->active || !adc_block_ptr->auto_deinit) {
        return;
    }

    if (_adc_block_has_active_channels(adc_block_ptr)) {
        return;
    }

    _adc_block_obj_deinit(adc_block_ptr);
    _adc_block_obj_free(adc_block_ptr);
}

static bool _adc_block_has_mapped_pins(uint16_t adc_block_id) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(adc_block_pin_map); i++) {
        if (adc_block_pin_map[i].block_id == adc_block_id) {
            return true;
        }
    }
    return false;
}

static bool _adc_block_channel_is_valid(const machine_adcblock_obj_t *adc_block_ptr, uint16_t channel) {
    const adc_block_capability_t *cap = adc_block_capability_find(adc_block_ptr->id);
    return cap != NULL && channel < cap->channel_count && channel < ADC_BLOCK_CHANNEL_MAX;
}

static bool _adc_bits_supported(uint8_t bits) {
    return (bits >= ADC_MIN_BITS) && (bits <= ADC_MAX_BITS);
}

static uint16_t adc_sample_setting_from_ns(uint32_t sample_ns) {
    uint64_t cycles = ((uint64_t)sample_ns * ADC_HS_CLOCK_HZ + 999999999u) / 1000000000u;
    if (cycles < ADC_HS_SAMPLE_TIME_MIN_CYCLES || cycles > ADC_HS_SAMPLE_TIME_MAX_CYCLES) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_ns out of range"));
    }
    return (uint16_t)(cycles - 1u);
}

static void adc_block_ensure_active(machine_adcblock_obj_t *adc_block_ptr) {
    if (adc_block_ptr == NULL || !adc_block_ptr->active) {
        mp_raise_ValueError(MP_ERROR_TEXT("ADC block is deinitialized"));
    }
}

uint16_t adc_block_validate_sample_ns(machine_adcblock_obj_t *adc_block_ptr, machine_adc_obj_t *current_adc, uint32_t sample_ns) {
    adc_block_ensure_active(adc_block_ptr);

    uint16_t sample_setting = adc_sample_setting_from_ns(sample_ns);

    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        machine_adc_obj_t *other_adc = adc_block_ptr->channel[i];
        if (other_adc == NULL || other_adc == current_adc || !other_adc->active) {
            continue;
        }
        if (other_adc->sample_ns != sample_ns) {
            mp_raise_ValueError(MP_ERROR_TEXT("sample_ns must match active ADC channels"));
        }
    }

    return sample_setting;
}

void adc_block_apply_runtime_config(machine_adcblock_obj_t *adc_block_ptr, uint32_t sample_ns) {
    adc_block_ensure_active(adc_block_ptr);

    if (adc_block_ptr->id != ADCBLOCK0) {
        return;
    }

    uint16_t sample_setting = adc_block_validate_sample_ns(adc_block_ptr, NULL, sample_ns);
    if (adc_hw_sample_setting == sample_setting) {
        return;
    }

    CYBSP_SAR_ADC_sta_hs_cfg.hsSampleTime[ADC_HS_SAMPLE_TIMER_INDEX] = sample_setting;
    if (Cy_AutAnalog_SAR_LoadConfig(adc_block_ptr->id, &CYBSP_SAR_ADC_cfg) != CY_AUTANALOG_SUCCESS) {
        mp_raise_TypeError(MP_ERROR_TEXT("ADC configuration failed"));
    }

    Cy_AutAnalog_StartAutonomousControl();
    adc_hw_sample_setting = sample_setting;
}

static void adc_block_set_bits(machine_adcblock_obj_t *adc_block_ptr, int bits) {
    adc_block_ensure_active(adc_block_ptr);

    if (bits < 0) {
        return;
    }

    if (!_adc_bits_supported((uint8_t)bits)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be in range 8..12"));
    }

    adc_block_ptr->bits = (uint8_t)bits;
}

static void _adc_block_obj_init(machine_adcblock_obj_t *adc_block_ptr, uint16_t adc_block_id, uint8_t bits, bool auto_deinit) {
    if (!adc_autanalog_initialized) {
        adc_hw_configure_supported_gpio_channels();
        uint32_t status = Cy_AutAnalog_Init(&autonomous_analog_init);
        if (status != CY_AUTANALOG_SUCCESS) {
            mp_raise_TypeError(MP_ERROR_TEXT("ADC initialization failed"));
        }

        Cy_AutAnalog_SetInterruptMask(CY_AUTANALOG_INT_SAR0_RESULT);
        Cy_AutAnalog_StartAutonomousControl();
        adc_autanalog_initialized = true;
    }

    if (!_adc_block_has_mapped_pins(adc_block_id)) {
        mp_raise_ValueError(MP_ERROR_TEXT("no pins mapped for ADC block"));
    }

    adc_block_ptr->id = adc_block_id;
    adc_block_ptr->bits = bits;
    adc_block_ptr->active = true;
    adc_block_ptr->auto_deinit = auto_deinit;
    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        adc_block_ptr->channel[i] = NULL;
    }
    adc_hw_sample_setting = UINT16_MAX;
}

static void _adc_block_obj_deinit(machine_adcblock_obj_t *adc_block_ptr) {
    if (adc_block_ptr == NULL || !adc_block_ptr->active) {
        return;
    }

    adc_block_ptr->active = false;

    for (uint8_t i = 0; i < ADC_BLOCK_CHANNEL_MAX; i++) {
        if (adc_block_ptr->channel[i] != NULL) {
            adc_obj_deinit(adc_block_ptr->channel[i]);
            adc_block_channel_free(adc_block_ptr, adc_block_ptr->channel[i]);
        }
    }

    // Stop and power-down hardware only when the last block is torn down.
    // Check all slots: if no other block remains initialized, shut down AutoAnalog.
    bool any_remaining = false;
    for (uint8_t i = 0; i < MAX_BLOCKS; i++) {
        if (adc_block[i] != NULL && adc_block[i] != adc_block_ptr) {
            any_remaining = true;
            break;
        }
    }

    if (!any_remaining && adc_autanalog_initialized) {
        Cy_AutAnalog_Disable();
        Cy_AutAnalog_Clear();
        adc_autanalog_initialized = false;
        adc_hw_sample_setting = UINT16_MAX;
    }
}

static machine_adcblock_obj_t *machine_adcblock_make_init(uint8_t adc_id, uint8_t bits, bool auto_deinit) {
    machine_adcblock_obj_t *adc_block_ptr = _adc_block_obj_find(adc_id);

    if (!_adc_bits_supported(bits)) {
        mp_raise_ValueError(MP_ERROR_TEXT("unsupported ADC resolution"));
    }

    if (adc_block_ptr == NULL) {
        adc_block_ptr = _adc_block_obj_alloc();
        if (adc_block_ptr == NULL) {
            mp_raise_TypeError(MP_ERROR_TEXT("ADC blocks are fully allocated"));
        }
        _adc_block_obj_init(adc_block_ptr, adc_id, bits, auto_deinit);
    } else if (adc_block_ptr->bits != bits) {
        mp_raise_ValueError(MP_ERROR_TEXT("ADC block already initialized with different resolution"));
    } else if (!auto_deinit) {
        adc_block_ptr->auto_deinit = false;
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

    return machine_adcblock_make_init(adc_block_id, DEFAULT_ADC_BITS, true);
}

machine_adc_obj_t *adc_block_channel_find(machine_adcblock_obj_t *adc_block_ptr, mp_obj_t pin) {
    if (adc_block_ptr == NULL || !adc_block_ptr->active) {
        return NULL;
    }

    int16_t adc_channel_no = adc_get_channel_number_for_pin(adc_pin_addr_by_obj(pin));
    if (adc_channel_no < 0 || !_adc_block_channel_is_valid(adc_block_ptr, (uint16_t)adc_channel_no)) {
        return NULL;
    }
    return adc_block_ptr->channel[adc_channel_no];
}

static void machine_adcblock_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->active) {
        mp_printf(print, "ADCBlock(deinitialized)");
        return;
    }

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
    if (!_adc_bits_supported(bits)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be in range 8..12"));
    }

    return MP_OBJ_FROM_PTR(machine_adcblock_make_init(adc_id, bits, false));
}

static mp_obj_t machine_adcblock_deinit(mp_obj_t self_in) {
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    _adc_block_obj_deinit(self);
    _adc_block_obj_free(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_adcblock_deinit_obj, machine_adcblock_deinit);

static mp_obj_t machine_adcblock_init(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    adc_block_ensure_active(self);

    enum { ARG_bits };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bits, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    adc_block_set_bits(self, args[ARG_bits].u_int);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_adcblock_init_obj, 1, machine_adcblock_init);

static mp_obj_t machine_adcblock_connect(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_adcblock_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    adc_block_ensure_active(self);
    uint8_t channel = 0xff;
    int channel_int = -1;
    mp_obj_t pin_name = MP_OBJ_NULL;

    if (n_pos_args == 2) {
        if (mp_obj_is_int(pos_args[1])) {
            channel_int = mp_obj_get_int(pos_args[1]);
            if (channel_int < 0 || channel_int >= ADC_BLOCK_CHANNEL_MAX) {
                mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC channel"));
            }
            channel = (uint8_t)channel_int;
            int32_t pin_addr = _get_adc_pin_number_for_channel(self->id, channel);
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
        channel_int = mp_obj_get_int(pos_args[1]);
        if (channel_int < 0 || channel_int >= ADC_BLOCK_CHANNEL_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC channel"));
        }
        channel = (uint8_t)channel_int;
        int32_t exp_pin = _get_adc_pin_number_for_channel(self->id, channel);
        uint32_t actual_pin = adc_pin_addr_by_obj(pos_args[2]);
        if (exp_pin < 0 || (uint32_t)exp_pin != actual_pin) {
            mp_raise_ValueError(MP_ERROR_TEXT("wrong pin for requested ADC channel"));
        }
        pin_name = pos_args[2];
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("too many positional args"));
    }

    machine_adc_obj_t *adc = adc_block_channel_find(self, pin_name);
    if (adc != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("ADC channel already in use"));
    }

    adc = adc_block_channel_alloc(self, pin_name);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        adc_obj_init(adc, self, pin_name, DEFAULT_ADC_ACQ_NS);

        // Match the generic ADCBlock contract by forwarding extra kwargs to ADC.init().
        adc_obj_init_helper(adc, 0, NULL, kw_args);
        nlr_pop();
    } else {
        adc_obj_deinit(adc);
        adc_block_channel_free(self, adc);
        nlr_raise(MP_OBJ_FROM_PTR(nlr.ret_val));
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
    // Guarantee hardware is off and flags are clean after a full teardown.
    if (adc_autanalog_initialized) {
        Cy_AutAnalog_Disable();
        Cy_AutAnalog_Clear();
        adc_autanalog_initialized = false;
        adc_hw_sample_setting = UINT16_MAX;
    }
}

static const mp_rom_map_elem_t machine_adcblock_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_adcblock_init_obj) },
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
