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

#include "modmachine.h"
#include "py/runtime.h"

#if MICROPY_PY_MACHINE_PDM_PCM

// --------------------------------------------------------------------------------
// ----------------- extmod/machine_pdm_pcm.c ------------------------------------
// This file could be promoted in future to extmod/machine_pdm_pcm.c
// as it replicates the extmod/machine_i2s.c structure for separation of
// concerns between port specific code and higher level MPY functions.
// Eventually both could be refactored to share common code for
// serial audio interfaces.

#include "py/ringbuf.h"
#include "mphalport.h"

typedef enum
{
    SAMPLE_RATE_8000 = 8000,
    SAMPLE_RATE_16000 = 16000,
    SAMPLE_RATE_22050 = 22050,
    SAMPLE_RATE_44100 = 44100,
    SAMPLE_RATE_48000 = 48000,
} sample_rate_t;

/**
 * Note: Actually from 8 to 32 is in 2 increments
 * is supported. TODO: Evaluate if implementation is
 * required
 */
typedef enum {
    BITS_16 = 16,
    BITS_32 = 32
} pdm_pcm_word_length_t;

/**
 * TODO: Do we need to differentiate
 * between LEFT and RIGHT?
 */
typedef enum {
    MONO_LEFT,
    MONO_RIGHT,
    STEREO
} format_t;

typedef enum {
    BLOCKING,
    NON_BLOCKING,
} io_mode_t;

// Arguments for PDM_PCM() constructor and PDM_PCM.init().
enum {
    ARG_sck,
    ARG_data,
    ARG_sample_rate,
    ARG_decimation_rate,
    ARG_bits,
    ARG_format,
    ARG_left_gain,
    ARG_right_gain,
    ARG_ibuf
};

#if MICROPY_PY_MACHINE_PDM_PCM_RING_BUF

static void fill_appbuf_from_ringbuf_non_blocking(machine_pdm_pcm_obj_t *self);

#endif // MICROPY_PY_MACHINE_PDM_PCM_RING_BUF

// The port must provide implementations of these low-level PDM PCM functions.
static void mp_machine_pdm_pcm_init_helper(machine_pdm_pcm_obj_t *self, mp_arg_val_t *args);
static machine_pdm_pcm_obj_t *mp_machine_pdm_pcm_make_new_instance(mp_int_t pdm_pcm_id);
static void mp_machine_pdm_pcm_deinit(machine_pdm_pcm_obj_t *self);
static void mp_machine_pdm_pcm_irq_update(machine_pdm_pcm_obj_t *self);

// The port provides implementations of the above in this file.
#include MICROPY_PY_MACHINE_PDM_PCM_INCLUDEFILE

#if MICROPY_PY_MACHINE_PDM_PCM_RING_BUF

static uint32_t fill_appbuf_from_ringbuf(machine_pdm_pcm_obj_t *self, mp_buffer_info_t *appbuf) {
    uint32_t num_bytes_copied_to_appbuf = 0;
    uint8_t *app_p = (uint8_t *)appbuf->buf;
    uint32_t num_bytes_needed_from_ringbuf = appbuf->len;
    int data;

    while (num_bytes_needed_from_ringbuf-- > 0) {
        while ((data = ringbuf_get(&ring_buffer)) == -1) {
            ;
        }
        app_p[num_bytes_copied_to_appbuf++] = (uint8_t)data;
    }
    return num_bytes_copied_to_appbuf;
}

// function is used in IRQ context
static void fill_appbuf_from_ringbuf_non_blocking(machine_pdm_pcm_obj_t *self) {
    /* TODO: Implement  */
}

#endif // MICROPY_PY_MACHINE_PDM_PCM_RING_BUF

MP_NOINLINE static void machine_pdm_pcm_init_helper(machine_pdm_pcm_obj_t *self, size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sck,             MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_data,            MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sample_rate,     MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = 16000} },
        { MP_QSTR_decimation_rate, MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = 96} },
        { MP_QSTR_bits,            MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = 16} },
        { MP_QSTR_format,          MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = MONO_LEFT} },
        { MP_QSTR_left_gain,       MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = 0} },
        { MP_QSTR_right_gain,      MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = 0} },
        { MP_QSTR_ibuf,            MP_ARG_KW_ONLY | MP_ARG_INT,   {.u_int = -1} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_machine_pdm_pcm_init_helper(self, args);
}

static void machine_pdm_pcm_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_pdm_pcm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "PDM_PCM(id=%u,\n"
        "sck="MP_HAL_PIN_FMT ",\n"
        "data="MP_HAL_PIN_FMT ",\n"
        "sample_rate=%ld,\n"
        "decimation_rate=%d,\n"
        "bits=%u,\n"
        "format=%u,\n"
        "left_gain=%d,\n"
        "right_gain=%d)",
        self->id,
        mp_hal_pin_name(self->sck),
        mp_hal_pin_name(self->data),
        self->sample_rate,
        self->decimation_rate,
        self->bits,
        self->format,
        self->left_gain,
        self->right_gain
        );
}

// PDM_PCM(...) Constructor
static mp_obj_t machine_pdm_pcm_make_new(const mp_obj_type_t *type, size_t n_pos_args, size_t n_kw_args, const mp_obj_t *args) {
    /* TODO: Add implementation*/
    machine_pdm_pcm_obj_t *self = NULL;

    self = mp_obj_malloc(machine_pdm_pcm_obj_t, &machine_pdm_pcm_type);
    pdm_pcm_obj = self;

    return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t machine_pdm_pcm_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_init),            MP_ROM_PTR(&machine_pdm_pcm_init_obj) },

    { MP_ROM_QSTR(MP_QSTR_isready),         MP_ROM_PTR(&machine_pdm_pcm_is_ready_obj) },

    { MP_ROM_QSTR(MP_QSTR_readinto),        MP_ROM_PTR(&mp_stream_readinto_obj) },
};

static mp_uint_t machine_pdm_pcm_stream_read(mp_obj_t self_in, void *buf_in, mp_uint_t size, int *errcode) {
    /* TODO: Add implementation*/
    machine_pdm_pcm_obj_t *self = MP_OBJ_TO_PTR(self_in);

    uint8_t appbuf_sample_size_in_bytes = (bits / 8) * (format == STEREO ? 2: 1);

    if (size % appbuf_sample_size_in_bytes != 0) { // size should be multiple of sample size
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }

    if (size == 0) {
        return 0;
    }

    if (io_mode == BLOCKING) {
        mp_buffer_info_t appbuf;
        appbuf.buf = (void *)buf_in;
        appbuf.len = size;
        #if MICROPY_PY_MACHINE_PDM_PCM_RING_BUF
        uint32_t num_bytes_read = fill_appbuf_from_ringbuf(self, &appbuf);
        #else
        uint32_t num_bytes_read = fill_appbuf_from_dma(self, &appbuf);
        #endif
        return num_bytes_read;
    }
    /* TODO: NON-BLOCKING implementation */

    return 0;
}

static const mp_stream_p_t pdm_pcm_stream_p = {
    .read = machine_pdm_pcm_stream_read,
    .is_text = false,
};


MP_DEFINE_CONST_DICT(machine_pdm_pcm_locals_dict, machine_pdm_pcm_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pdm_pcm_type,
    MP_QSTR_PDM_PCM,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, machine_pdm_pcm_make_new,
    print, machine_pdm_pcm_print,
    protocol, &pdm_pcm_stream_p,
    locals_dict, &machine_pdm_pcm_locals_dict
    );

#endif // MICROPY_PY_MACHINE_PDM_PCM_TDD
