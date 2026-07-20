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

#include "py/mphal.h"
#include "py/mperrno.h"
#include "machine_pin.h"
#include "cycfg_peripherals.h"
#include "cycfg_system.h"
#include "cy_autanalog.h"
#include "cy_autanalog_sar.h"
#include "genhdr/pins_af.h"

// PSoC Edge SAR ADC: 1 block, 8 GPIO channels (P15_0 to P15_7)
#define ADC_NUM_CHANNELS (8)
#define ADC_READ_TIMEOUT_US (1000)

#define ADC_VDDA_MV (CY_CFG_PWR_VDDA_MV)

#define MICROPY_PY_MACHINE_ADC_CLASS_CONSTANTS

// Global state: track if autonomous analog has been initialized
static bool adc_autanalog_initialized = false;
// Bitmask of ADC channels enabled by user-created ADC objects.
static uint8_t adc_enabled_channels_mask = 0;
// Per-channel user reference count to support shared ADC objects per channel.
static uint8_t adc_channel_refcount[ADC_NUM_CHANNELS] = {0};

// ADC channel object: stores pin mapping
typedef struct _machine_adc_obj_t {
    mp_obj_base_t base;
    const machine_pin_obj_t *pin;      // GPIO pin used as the ADC input
    uint8_t sar_block;                 // SAR block index (always 0 on PSE84)
    uint8_t gpio_channel;              // GPIO channel index (0-7 for P15_0-P15_7)
    bool active;                       // Tracks whether this object still owns a channel reference.
} machine_adc_obj_t;

// ADC.__repr__ -> <ADC pin='P15_0' ch=0>
static void mp_machine_adc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<ADC pin='%q' ch=%u>", self->pin->name, self->gpio_channel);
}

// Initialize BSP-generated CYBSP_SAR_ADC_* structures for one requested GPIO channel.
static void machine_adc_init_gpio_channel(uint8_t channel) {
    if (channel >= ADC_NUM_CHANNELS) {
        return;
    }

    // All channels share the same base configuration and only differ by GPIO pin index.
    const cy_stc_autanalog_sar_hs_chan_t default_ch_cfg = {
        .hsDiffEn = false,
        .sign = false,
        .posCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED,
        .negPin = CY_AUTANALOG_SAR_PIN_MUX_VSSA,
        .accShift = false,
        .negCoeff = CY_AUTANALOG_SAR_CH_COEFF_DISABLED,
        .hsLimit = CY_AUTANALOG_SAR_LIMIT_STATUS_DISABLED,
        .fifoSel = CY_AUTANALOG_FIFO_DISABLED,
    };

    CYBSP_SAR_ADC_gpio_ch_cfg[channel] = default_ch_cfg;
    CYBSP_SAR_ADC_gpio_ch_cfg[channel].posPin = CY_AUTANALOG_SAR_PIN_GPIO0 + channel;
}

// Initialize high-speed static ADC configuration
static void machine_adc_init_sta_hs_cfg(void) {
    CYBSP_SAR_ADC_sta_hs_cfg.hsVref = CY_AUTANALOG_SAR_VREF_VDDA;

    // All channels use same sample time
    for (size_t i = 0; i < 4; i++) {
        CYBSP_SAR_ADC_sta_hs_cfg.hsSampleTime[i] = 31U;
    }

    // Start with no enabled GPIO channels. Channels are enabled on ADC(pin) creation.
    for (size_t i = 0; i < ADC_NUM_CHANNELS; i++) {
        CYBSP_SAR_ADC_sta_hs_cfg.hsGpioChan[i] = NULL;
    }

    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioResultMask = 0x00U;
}

// Initialize static ADC configuration
static void machine_adc_init_sta_cfg(void) {
    CYBSP_SAR_ADC_sta_cfg.lpStaCfg = NULL;
    CYBSP_SAR_ADC_sta_cfg.hsStaCfg = &CYBSP_SAR_ADC_sta_hs_cfg;
    CYBSP_SAR_ADC_sta_cfg.posBufPwr = CY_AUTANALOG_SAR_BUF_PWR_OFF;
    CYBSP_SAR_ADC_sta_cfg.negBufPwr = CY_AUTANALOG_SAR_BUF_PWR_OFF;
    CYBSP_SAR_ADC_sta_cfg.accMode = CY_AUTANALOG_SAR_ACC_DISABLED;
    CYBSP_SAR_ADC_sta_cfg.startupCal = CY_AUTANALOG_SAR_CAL_DISABLED;
    CYBSP_SAR_ADC_sta_cfg.chanID = false;
    CYBSP_SAR_ADC_sta_cfg.shiftMode = false;

    // Initialize all null pointers
    for (size_t i = 0; i < 16; i++) {
        CYBSP_SAR_ADC_sta_cfg.intMuxChan[i] = NULL;
    }
    for (size_t i = 0; i < 4; i++) {
        CYBSP_SAR_ADC_sta_cfg.limitCond[i] = NULL;
    }

    CYBSP_SAR_ADC_sta_cfg.muxResultMask = CY_AUTANALOG_SAR_CHAN_MASK_MUX_DISABLED;
    CYBSP_SAR_ADC_sta_cfg.firResultMask = CY_AUTANALOG_SAR_MASK_FIR_DISABLED;
}

// Initialize sequencer configuration
static void machine_adc_init_seq_cfg(void) {
    const cy_stc_autanalog_sar_seq_tab_hs_t default_seq_cfg = {
        .chanEn = 0x00U,
        .muxMode = CY_AUTANALOG_SAR_CHAN_CFG_MUX_DISABLED,
        .mux0Sel = CY_AUTANALOG_SAR_CHAN_CFG_MUX0,
        .mux1Sel = CY_AUTANALOG_SAR_CHAN_CFG_MUX0,
        .sampleTimeEn = true,
        .sampleTime = CY_AUTANALOG_SAR_SAMPLE_TIME0,
        .accEn = false,
        .accCount = CY_AUTANALOG_SAR_ACC_CNT2,
        .calReq = CY_AUTANALOG_SAR_CAL_DISABLED,
        .nextAction = CY_AUTANALOG_SAR_NEXT_ACTION_GO_TO_ENTRY_ADDR,
    };

    // Initialize all sequencer entries with same config
    for (size_t i = 0; i < 2; i++) {
        CYBSP_SAR_ADC_seq_hs_cfg[i] = default_seq_cfg;
    }
}

// Initialize main SAR ADC configuration
static void machine_adc_init_sar_cfg(void) {
    CYBSP_SAR_ADC_cfg.sarStaCfg = &CYBSP_SAR_ADC_sta_cfg;
    CYBSP_SAR_ADC_cfg.hsSeqTabNum = 2;
    CYBSP_SAR_ADC_cfg.hsSeqTabArr = &CYBSP_SAR_ADC_seq_hs_cfg[0U];
    CYBSP_SAR_ADC_cfg.lpSeqTabNum = 0U;
    CYBSP_SAR_ADC_cfg.lpSeqTabArr = NULL;
    CYBSP_SAR_ADC_cfg.firNum = 0U;
    CYBSP_SAR_ADC_cfg.firCfg = NULL;
    CYBSP_SAR_ADC_cfg.fifoCfg = NULL;
}

// Initialize startup state configuration
static void machine_adc_init_stt_cfg(void) {
    const cy_stc_autanalog_stt_sar_t default_stt_cfg = {
        .unlock = true,
        .enable = true,
        .trigger = false,
        .entryState = 0U,
    };

    // Initialize all 3 startup states
    for (size_t i = 0; i < 3; i++) {
        CYBSP_SAR_ADC_stt[i] = default_stt_cfg;
    }

    // State 1 triggers scanning
    CYBSP_SAR_ADC_stt[1].trigger = true;
}

// Initialize all ADC configuration structures
static void machine_adc_init_configs(void) {
    machine_adc_init_sta_hs_cfg();
    machine_adc_init_sta_cfg();
    machine_adc_init_seq_cfg();
    machine_adc_init_sar_cfg();
    machine_adc_init_stt_cfg();
}

// Reload SAR config after channel mask updates.
static void machine_adc_reload_config(uint8_t sar_block) {
    uint32_t status = Cy_AutAnalog_SAR_LoadConfig(sar_block, &CYBSP_SAR_ADC_cfg);
    if (status != CY_AUTANALOG_SUCCESS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ADC config load failed"));
    }
    Cy_AutAnalog_StartAutonomousControl();
}

// Increase channel refcount and enable HW on first user.
static bool machine_adc_enable_channel(uint8_t channel) {
    if (channel >= ADC_NUM_CHANNELS) {
        return false;
    }

    if (adc_channel_refcount[channel] < UINT8_MAX) {
        adc_channel_refcount[channel]++;
    }

    uint8_t channel_mask = (uint8_t)(1u << channel);
    // Channel already configured; nothing else to do.
    if ((adc_enabled_channels_mask & channel_mask) != 0u) {
        return false;
    }

    // Enable only the requested channel in GPIO config, result mask, and sequencer mask.
    machine_adc_init_gpio_channel(channel);
    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioChan[channel] = &CYBSP_SAR_ADC_gpio_ch_cfg[channel];
    adc_enabled_channels_mask |= channel_mask;
    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioResultMask = adc_enabled_channels_mask;
    CYBSP_SAR_ADC_seq_hs_cfg[0].chanEn = adc_enabled_channels_mask;
    CYBSP_SAR_ADC_seq_hs_cfg[1].chanEn = adc_enabled_channels_mask;
    return true;
}

// Decrease channel refcount and disable HW when the last user releases it.
static bool machine_adc_disable_channel(uint8_t channel) {
    if (channel >= ADC_NUM_CHANNELS) {
        return false;
    }

    if (adc_channel_refcount[channel] == 0u) {
        return false;
    }

    adc_channel_refcount[channel]--;
    if (adc_channel_refcount[channel] != 0u) {
        return false;
    }

    uint8_t channel_mask = (uint8_t)(1u << channel);
    if ((adc_enabled_channels_mask & channel_mask) == 0u) {
        return false;
    }

    adc_enabled_channels_mask &= (uint8_t) ~channel_mask;
    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioChan[channel] = NULL;
    CYBSP_SAR_ADC_sta_hs_cfg.hsGpioResultMask = adc_enabled_channels_mask;
    CYBSP_SAR_ADC_seq_hs_cfg[0].chanEn = adc_enabled_channels_mask;
    CYBSP_SAR_ADC_seq_hs_cfg[1].chanEn = adc_enabled_channels_mask;
    return true;
}

// Map pin to (SAR block, channel) using MICROPY_HW_ADC_PIN_MAP from pins_af.h.
static bool machine_adc_get_block_channel_from_pin(
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

// Initialize autonomous analog controller and SAR ADC (called once on first ADC creation)
static void machine_adc_init_autanalog(void) {
    if (adc_autanalog_initialized) {
        return;
    }

    // Initialize ADC configuration structures dynamically
    machine_adc_init_configs();

    // Initialize autonomous analog controller
    uint32_t status = Cy_AutAnalog_Init((cy_stc_autanalog_t *)&autonomous_analog_init);
    if (status != CY_AUTANALOG_SUCCESS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ADC autanalog init failed"));
    }

    // Start autonomous controller
    Cy_AutAnalog_StartAutonomousControl();
    adc_autanalog_initialized = true;
}

void machine_adc_deinit_all(void) {
    if (adc_autanalog_initialized) {
        Cy_AutAnalog_Disable();
    }

    adc_autanalog_initialized = false;
    adc_enabled_channels_mask = 0;
    for (size_t i = 0; i < ADC_NUM_CHANNELS; i++) {
        adc_channel_refcount[i] = 0;
    }

    // Rebuild the cached config structures so the next ADC creation starts from
    // a clean software state after the hardware MMIO/STT contents are dropped.
    machine_adc_init_configs();
}

// ADC(pin) -> machine_adc_obj_t *; validates pin
static mp_obj_t mp_machine_adc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    const machine_pin_obj_t *pin = mp_hal_get_pin_obj(args[0]);
    uint8_t sar_block = 0;
    uint8_t channel = 0;
    if (!machine_adc_get_block_channel_from_pin(pin, &sar_block, &channel)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Pin doesn't have ADC capabilities"));
    }

    machine_adc_obj_t *o = mp_obj_malloc(machine_adc_obj_t, &machine_adc_type);
    o->pin = pin;
    o->sar_block = sar_block;
    o->gpio_channel = channel;
    o->active = true;

    // Initialize autonomous analog once and enable only the requested channel.
    machine_adc_init_autanalog();
    // Per-user pin enable: this avoids pre-enabling all ADC-capable channels.
    bool channel_enabled = machine_adc_enable_channel(channel);

    // Apply updated channel selection once when configuration changes.
    if (channel_enabled) {
        machine_adc_reload_config(o->sar_block);
    }
    return MP_OBJ_FROM_PTR(o);
}

// Read one ADC conversion as raw 12-bit value (0-4095).
static uint16_t machine_adc_read_raw_12b(machine_adc_obj_t *self) {
    if (!self->active) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ADC is deinitialized"));
    }

    uint8_t channel_mask = (uint8_t)(1u << self->gpio_channel);
    uint32_t timeout = ADC_READ_TIMEOUT_US;

    // Clear result status and wait for new result
    Cy_AutAnalog_SAR_ClearHSchanResultStatus(self->sar_block, channel_mask);
    while (((Cy_AutAnalog_SAR_GetHSchanResultStatus(self->sar_block) & channel_mask) == 0u) && timeout != 0u) {
        mp_hal_delay_us(1);
        timeout--;
    }

    if (timeout == 0u) {
        mp_raise_OSError(MP_ETIMEDOUT);
    }

    // Read 12-bit result
    int32_t raw = Cy_AutAnalog_SAR_ReadResult(self->sar_block, CY_AUTANALOG_SAR_INPUT_GPIO, self->gpio_channel);
    if (raw < 0) {
        raw = 0;
    }
    return (uint16_t)((uint32_t)raw & 0x0FFFu);
}

// ADC.read_u16() -> int (0-65535, 0=0V, 65535=VDDA)
static mp_int_t mp_machine_adc_read_u16(machine_adc_obj_t *self) {
    uint16_t raw_12b = machine_adc_read_raw_12b(self);
    // Scale 12-bit (0-4095) to 16-bit (0-65535)
    return (mp_int_t)(((uint32_t)raw_12b * 65535u) / 4095u);
}

// ADC.read_uv() -> int (0-VDDA microvolts)
static mp_int_t mp_machine_adc_read_uv(machine_adc_obj_t *self) {
    uint16_t raw_12b = machine_adc_read_raw_12b(self);
    // Use raw SAR code for better precision than converting from quantized read_u16().
    return (mp_int_t)(((uint64_t)raw_12b * ((uint64_t)ADC_VDDA_MV * 1000u)) / 4095u);
}

// ADC.deinit() - release this ADC user's channel reference.
static void mp_machine_adc_deinit(machine_adc_obj_t *self) {
    if (!self->active) {
        return;
    }

    self->active = false;
    if (machine_adc_disable_channel(self->gpio_channel)) {
        machine_adc_reload_config(self->sar_block);
    }
}

// ADC.block() -> ADCBlock(0)
static mp_obj_t mp_machine_adc_block(machine_adc_obj_t *self) {
    (void)self;
    mp_obj_t block_id = MP_OBJ_NEW_SMALL_INT(0);
    return MP_OBJ_TYPE_GET_SLOT(&machine_adc_block_type, make_new)(&machine_adc_block_type, 1, 0, &block_id);
}
