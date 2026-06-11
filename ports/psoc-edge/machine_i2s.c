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

#define MICROPY_HW_MAX_I2S (1)

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
