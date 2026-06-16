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

// This file is never compiled standalone; it is included directly from
// extmod/machine_i2s.c via MICROPY_PY_MACHINE_I2S_INCLUDEFILE.

#include "cy_sysclk.h"
#include "cy_tdm.h"
#include "cycfg_peripheral_clocks.h"
#include "genhdr/pins_af.h"
#include "machine_pin_af.h"
#include "mphalport.h"

// The TDM peripheral on PSoC Edge uses HF1 (400 MHz) as its clock source,
// fed through a 16.5-bit fractional PCLK divider, then the TDM clkDiv register.
//
// clk_if_srss = HF1 / (pclk_int + 1 + pclk_frac/32)
// SCK         = clk_if_srss / (clkDiv + 1)
//
// We always set clkDiv=0 and compute pclk_int/pclk_frac to hit the target SCK.
//
// Target SCK frequency for I2S (stereo) = sample_rate * bits_per_channel * 2
// The 16.5-bit divider has 5 fractional bits (0..31), giving 1/32 step resolution.
//
// Returns true on success; sets *div_int and *div_frac (0..31).
static bool i2s_calc_clock_divider(uint32_t sample_rate, uint8_t bits,
    uint32_t *div_int, uint32_t *div_frac) {

    // f_sck = sample_rate * bits_per_channel * 2 channels (stereo I2S frame)
    uint32_t f_sck = sample_rate * (uint32_t)bits * 2U;
    if (f_sck == 0) {
        return false;
    }

    // Read the actual HF1 frequency at runtime so we're not hardcoded to 400 MHz
    uint32_t f_hf = Cy_SysClk_ClkHfGetFrequency(CY_MMIO_TDM0_CLK_HF_NR);
    if (f_hf == 0) {
        return false;
    }

    // Compute the required total divider with 5 fractional bits:
    //   total_x32 = (f_hf * 32) / f_sck   (integer arithmetic, rounded)
    // This represents (divInt + 1 + divFrac/32) * 32
    uint64_t total_x32 = ((uint64_t)f_hf * 32U + f_sck / 2U) / f_sck;

    if (total_x32 < 32U) {
        return false;  // Requested frequency is too high
    }

    // total_x32 = (divInt + 1) * 32 + divFrac
    //           = div_int_plus1 * 32 + frac
    uint32_t div_int_plus1 = (uint32_t)(total_x32 / 32U);
    uint32_t frac = (uint32_t)(total_x32 % 32U);

    // The PDL API takes dividerIntValue = (div_int_plus1 - 1)
    *div_int = div_int_plus1 - 1U;
    *div_frac = frac;
    return true;
}

// Configure and enable the PCLK fractional divider for TDM0.
// Called by the constructor with the computed div_int/div_frac values.
static void i2s_clock_configure(uint32_t div_int, uint32_t div_frac) {
    en_clk_dst_t grp = (en_clk_dst_t)CYBSP_TDM_CONTROLLER_0_CLK_DIV_GRP_NUM;

    Cy_SysClk_PeriPclkDisableDivider(grp, CY_SYSCLK_DIV_16_5_BIT, 0U);
    Cy_SysClk_PeriPclkSetFracDivider(grp, CY_SYSCLK_DIV_16_5_BIT, 0U,
        div_int, div_frac);
    Cy_SysClk_PeriPclkEnableDivider(grp, CY_SYSCLK_DIV_16_5_BIT, 0U);
}

// Size of the chunk (in bytes) copied per non-blocking IRQ invocation.
#define SIZEOF_NON_BLOCKING_COPY_IN_BYTES (256)

typedef enum {
    RX,
    TX
} i2s_mode_t;

typedef struct _machine_i2s_obj_t {
    mp_obj_base_t base;
    uint8_t i2s_id;
    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t ws;
    mp_hal_pin_obj_t sd;
    i2s_mode_t mode;
    int8_t bits;
    format_t format;
    int32_t rate;
    int32_t ibuf;
    mp_obj_t callback_for_non_blocking;
    io_mode_t io_mode;
    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_storage;
    non_blocking_descriptor_t non_blocking_descriptor;
    TDM_STRUCT_Type *tdm_base;  // NULL when not initialised; acts as init guard
} machine_i2s_obj_t;

// Frame map: transforms 8-byte I2S RX frames to the user-requested sample format.
// Row order: Mono-16, Mono-32, Stereo-16, Stereo-32
static const int8_t i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES] = {
    {-1, -1,  0,  1, -1, -1, -1, -1 },  // Mono, 16-bits
    { 0,  1,  2,  3, -1, -1, -1, -1 },  // Mono, 32-bits
    {-1, -1,  0,  1, -1, -1,  2,  3 },  // Stereo, 16-bits
    { 0,  1,  2,  3,  4,  5,  6,  7 },  // Stereo, 32-bits
};

static int8_t get_frame_mapping_index(int8_t bits, format_t format) {
    if (format == MONO) {
        return (bits == 16) ? 0 : 1;
    } else {
        return (bits == 16) ? 2 : 3;
    }
}

static machine_i2s_obj_t *mp_machine_i2s_make_new_instance(mp_int_t i2s_id) {
    if ((uint32_t)i2s_id >= MICROPY_PY_MACHINE_I2S_NUM_ENTRIES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("I2S(%d) does not exist"), i2s_id);
    }
    // Reuse existing instance (deinit first to release hardware).
    machine_i2s_obj_t *self = MP_STATE_PORT(machine_i2s_obj[i2s_id]);
    if (self != NULL) {
        mp_machine_i2s_deinit(self);
    }
    self = mp_obj_malloc(machine_i2s_obj_t, &machine_i2s_type);
    self->i2s_id = (uint8_t)i2s_id;
    self->tdm_base = NULL;
    MP_STATE_PORT(machine_i2s_obj[i2s_id]) = self;
    return self;
}

static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    // --- Validate pins ---
    mp_hal_pin_obj_t sck = mp_hal_get_pin_obj(args[ARG_sck].u_obj);
    mp_hal_pin_obj_t ws = mp_hal_get_pin_obj(args[ARG_ws].u_obj);
    mp_hal_pin_obj_t sd = mp_hal_get_pin_obj(args[ARG_sd].u_obj);

    // --- Validate mode ---
    i2s_mode_t mode = (i2s_mode_t)args[ARG_mode].u_int;
    if (mode != RX && mode != TX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    // --- Validate bits ---
    int8_t bits = (int8_t)args[ARG_bits].u_int;
    if (bits != 16 && bits != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid bits (must be 16 or 32)"));
    }

    // --- Validate format ---
    format_t format = (format_t)args[ARG_format].u_int;
    if (format != MONO && format != STEREO) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid format"));
    }

    // --- Validate rate ---
    int32_t rate = args[ARG_rate].u_int;
    if (rate <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid rate"));
    }

    // --- Validate and allocate ring buffer ---
    int32_t ring_buffer_len = args[ARG_ibuf].u_int;
    if (ring_buffer_len <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ibuf"));
    }
    uint8_t *storage = m_new(uint8_t, ring_buffer_len);
    ringbuf_init(&self->ring_buffer, storage, ring_buffer_len);
    self->ring_buffer_storage = storage;

    // --- Store fields ---
    self->sck = sck;
    self->ws = ws;
    self->sd = sd;
    self->mode = mode;
    self->bits = bits;
    self->format = format;
    self->rate = rate;
    self->ibuf = ring_buffer_len;
    self->callback_for_non_blocking = MP_OBJ_NULL;
    self->non_blocking_descriptor.copy_in_progress = false;
    self->io_mode = BLOCKING;

    // --- Clock ---
    // SCK = sample_rate * bits_per_channel * 2
    // Frequency is set by the 16.5-bit PCLK fractional divider (clkDiv=0 in TDM
    // hardware so the TDM block passes clk_if_srss[0] directly to SCK).
    uint32_t div_int, div_frac;
    if (!i2s_calc_clock_divider((uint32_t)rate, (uint8_t)bits, &div_int, &div_frac)) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("cannot achieve I2S clock for requested rate/bits"));
    }
    i2s_clock_configure(div_int, div_frac);

    // --- Pin alternate functions ---
    // PSoC Edge is always I2S master: SCK and WS are outputs regardless of mode.
    // In TX mode all three pins belong to the TDM TX block.
    // In RX mode SCK/WS are driven by the TDM RX master, SD is an input.
    if (mode == TX) {
        const mp_hal_pin_af_config_t pin_cfg[] = {
            MP_HAL_PIN_AF_CONF(sck, CY_GPIO_DM_STRONG_IN_OFF, 1,
                MACHINE_PIN_AF_SIGNAL_TDM_TX_SCK),
            MP_HAL_PIN_AF_CONF(ws,  CY_GPIO_DM_STRONG_IN_OFF, 1,
                MACHINE_PIN_AF_SIGNAL_TDM_TX_FSYNC),
            MP_HAL_PIN_AF_CONF(sd,  CY_GPIO_DM_STRONG_IN_OFF, 0,
                MACHINE_PIN_AF_SIGNAL_TDM_TX_SD),
        };
        mp_hal_periph_pins_af_config(pin_cfg, 3);
    } else {
        const mp_hal_pin_af_config_t pin_cfg[] = {
            MP_HAL_PIN_AF_CONF(sck, CY_GPIO_DM_STRONG_IN_OFF, 1,
                MACHINE_PIN_AF_SIGNAL_TDM_RX_SCK),
            MP_HAL_PIN_AF_CONF(ws,  CY_GPIO_DM_STRONG_IN_OFF, 1,
                MACHINE_PIN_AF_SIGNAL_TDM_RX_FSYNC),
            MP_HAL_PIN_AF_CONF(sd,  CY_GPIO_DM_HIGHZ, 0,
                MACHINE_PIN_AF_SIGNAL_TDM_RX_SD),
        };
        mp_hal_periph_pins_af_config(pin_cfg, 3);
    }

    // --- TDM/I2S hardware init ---
    // Use TDM STRUCT0.  clkDiv=0: SCK comes directly from clk_if_srss[0]
    // (the PCLK fractional divider has already been set above).
    // I2S framing: 2 channels, 1-bit FSYNC pulse (CY_TDM_BIT_PERIOD),
    // data one SCK cycle after FSYNC edge (CY_TDM_LEFT_DELAYED = standard I2S).
    TDM_STRUCT_Type *tdm_base = TDM0_TDM_STRUCT0;
    cy_en_tdm_ws_t word_size = (bits == 16) ? CY_TDM_SIZE_16 : CY_TDM_SIZE_32;
    uint8_t ch_size = (uint8_t)bits;

    cy_stc_tdm_config_tx_t tx_cfg = {
        .enable = (mode == TX),
        .masterMode = CY_TDM_DEVICE_MASTER,
        .wordSize = word_size,
        .format = CY_TDM_LEFT_DELAYED,
        .clkDiv = 0U,
        .clkSel = CY_TDM_SEL_SRSS_CLK0,
        .sckPolarity = CY_TDM_CLK,
        .fsyncPolarity = CY_TDM_SIGN,
        .fsyncFormat = CY_TDM_BIT_PERIOD,
        .channelNum = 2U,
        .channelSize = ch_size,
        .fifoTriggerLevel = 4U,
        .chEn = 0x3U,
        .signalInput = 0U,
        .i2sMode = true,
    };
    cy_stc_tdm_config_rx_t rx_cfg = {
        .enable = (mode == RX),
        .masterMode = CY_TDM_DEVICE_MASTER,
        .wordSize = word_size,
        .signExtend = CY_ZERO_EXTEND,
        .format = CY_TDM_LEFT_DELAYED,
        .clkDiv = 0U,
        .clkSel = CY_TDM_SEL_SRSS_CLK0,
        .sckPolarity = CY_TDM_CLK,
        .fsyncPolarity = CY_TDM_SIGN,
        .lateSample = false,
        .fsyncFormat = CY_TDM_BIT_PERIOD,
        .channelNum = 2U,
        .channelSize = ch_size,
        .chEn = 0x3U,
        .fifoTriggerLevel = 4U,
        .signalInput = 0U,
        .i2sMode = true,
    };
    cy_stc_tdm_config_t tdm_cfg = {
        .tx_config = &tx_cfg,
        .rx_config = &rx_cfg,
    };

    cy_en_tdm_status_t status = Cy_AudioTDM_Init(tdm_base, &tdm_cfg);
    if (status != CY_TDM_SUCCESS) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("TDM init failed: 0x%lx"), (uint32_t)status);
    }

    if (mode == TX) {
        Cy_AudioTDM_EnableTx(TDM0_TDM_STRUCT0_TDM_TX_STRUCT);
    } else {
        Cy_AudioTDM_EnableRx(TDM0_TDM_STRUCT0_TDM_RX_STRUCT);
    }

    self->tdm_base = tdm_base;
}

static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    if (self->tdm_base == NULL) {
        return;  // already de-initialised
    }
    if (self->mode == TX) {
        Cy_AudioTDM_DisableTx(TDM0_TDM_STRUCT0_TDM_TX_STRUCT);
    } else {
        Cy_AudioTDM_DisableRx(TDM0_TDM_STRUCT0_TDM_RX_STRUCT);
    }
    Cy_AudioTDM_DeInit(self->tdm_base);
    m_free(self->ring_buffer_storage);
    self->ring_buffer_storage = NULL;
    self->tdm_base = NULL;
    MP_STATE_PORT(machine_i2s_obj[self->i2s_id]) = NULL;
}

static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    (void)self;
}

MP_REGISTER_ROOT_POINTER(struct _machine_i2s_obj_t *machine_i2s_obj[MICROPY_PY_MACHINE_I2S_NUM_ENTRIES]);
