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
#include "cycfg_peripheral_clocks.h"

#define MICROPY_HW_MAX_I2S (1)

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
    // TODO: implement
    (void)i2s_id;
    return NULL;
}

static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    // TODO: implement
    (void)self;
    (void)args;
}

static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    // TODO: implement
    (void)self;
}

static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    // TODO: implement
    (void)self;
}

MP_REGISTER_ROOT_POINTER(struct _machine_i2s_obj_t *machine_i2s_obj[MICROPY_HW_MAX_I2S]);
