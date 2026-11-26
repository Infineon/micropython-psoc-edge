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
#include "genhdr/pins_af.h"
#include "tcpwm_alt.h"
#include "clk.h"


/* Forward declaration */
static void tcpwm_irq_handler(uint16_t tcpwm);

#define DEFINE_TCPWM_IRQ_HANDLER(tcpwm) \
    void TCPWM##tcpwm##_IRQ_Handler(void) { \
        tcpwm_irq_handler(tcpwm); \
    }

/**
 * The file build-<board>/genhdr/pins_af.h contains the macro
 *
 *  MICROPY_PY_FOR_ALL_TCPWMXX(DO)
 *
 *  which uses the X-macro (as argument) pattern to pass a worker
 *  macro DO(port) for the list of all user available ports.
 *
 * The available (not hidden) user TCPWMXXs are those alternate
 * functions defined in the boards/pse8x_af.csv file, for which
 * the corresponding pin are available for the user.
 * The available pins are those defined in the
 * boards/<board>/pins.csv file, which are not prefixed
 * with a hyphen(-).
 * See tools/boardgen.py and psoc-edge/boards/make-pins.py
 * for more information.
 */

MICROPY_PY_MACHINE_TCPWM0_32BITS_FOR_ALL_COUNTERS(DEFINE_TCPWM_IRQ_HANDLER)
MICROPY_PY_MACHINE_TCPWM0_16BITS_FOR_ALL_COUNTERS(DEFINE_TCPWM_IRQ_HANDLER)

#define MAP_TCPWM_CONFIG(tcpwm, offset) \
    [tcpwm - offset] = { \
        tcpwm, \
        TCPWM0, \
        NULL, \
        { \
            tcpwm_0_interrupts_##tcpwm##_IRQn, \
            SYS_INT_IRQ_LOWEST_PRIORITY, \
            TCPWM##tcpwm##_IRQ_Handler \
        }, \
        PCLK_TCPWM0_CLOCK_COUNTER_EN##tcpwm, \
        NULL, \
        NULL, \
    },


#define MAP_TCPWM_CONFIG32(tcpwm) MAP_TCPWM_CONFIG(tcpwm, 0)
#define MAP_TCPWM_CONFIG16(tcpwm) MAP_TCPWM_CONFIG(tcpwm, MICROPY_PY_MACHINE_TCPWM0_16BITS_MIN_ID)

static tcpwm_obj_t tcpwm16_obj[MICROPY_PY_MACHINE_TCPWM0_16BITS_NUM_COUNTERS] = {
    MICROPY_PY_MACHINE_TCPWM0_16BITS_FOR_ALL_COUNTERS(MAP_TCPWM_CONFIG16)
};

static tcpwm_obj_t tcpwm32_obj[MICROPY_PY_MACHINE_TCPWM0_32BITS_NUM_COUNTERS] = {
    MICROPY_PY_MACHINE_TCPWM0_32BITS_FOR_ALL_COUNTERS(MAP_TCPWM_CONFIG32)
};

static uint32_t tcpwm_pclk_div_slave = 0;

static tcpwm_obj_t *tcpwm_get_obj(uint16_t tcpwm) {
    if (tcpwm >= MICROPY_PY_MACHINE_TCPWM0_16BITS_MIN_ID) {
        return &tcpwm16_obj[tcpwm - MICROPY_PY_MACHINE_TCPWM0_16BITS_MIN_ID];
    } else {
        return &tcpwm32_obj[tcpwm];
    }
}

static void tcpwm_irq_handler(uint16_t tcpwm) {
    tcpwm_obj_t *obj = tcpwm_get_obj(tcpwm);
    if (obj->owner != NULL && obj->owner_handler != NULL) {
        obj->owner_handler(obj->owner);
    }
}

void tcpwm_id_assert(uint16_t id) {
    if (id >= MICROPY_PY_MACHINE_TCPWM0_16BITS_MIN_ID) {
        if (id - MICROPY_PY_MACHINE_TCPWM0_16BITS_MIN_ID >= MICROPY_PY_MACHINE_TCPWM0_16BITS_NUM_COUNTERS) {
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("invalid TCPWM id %u"), id);
        }
    } else {
        if (id >= MICROPY_PY_MACHINE_TCPWM0_32BITS_NUM_COUNTERS) {
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("invalid TCPWM id %u"), id);
        }
    }
}

uint32_t tcpwm_get_max_period(uint16_t id) {
    uint8_t width;
    if (id >= MICROPY_PY_MACHINE_TCPWM0_16BITS_MIN_ID) {
        width = TCPWM_GRP_NR1_CNT_GRP_CNT_WIDTH;
    } else {
        width = TCPWM_GRP_NR0_CNT_GRP_CNT_WIDTH;
    }

    return (uint32_t)((1ULL << width) - 1ULL);
}

tcpwm_obj_t *tcpwm_obj_alloc(uint16_t id, mp_obj_t owner, tcpwm_owner_irq_handler_t handler) {
    tcpwm_obj_t *obj = tcpwm_get_obj(id);
    if (obj->owner != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("TCPWM %u is already in use"), id);
    }

    pclk_div_slave_init(obj->clk, CY_MMIO_TCPWM0_PERI_NR, CY_MMIO_TCPWM0_GROUP_NR, CY_MMIO_TCPWM0_SLAVE_NR);
    tcpwm_pclk_div_slave++;
    obj->owner = owner;
    obj->owner_handler = handler;

    return obj;
}

void tcpwm_obj_free(tcpwm_obj_t *obj) {
    obj->owner = NULL;
    obj->owner_handler = NULL;

    if (tcpwm_pclk_div_slave > 0 && --tcpwm_pclk_div_slave == 0) {
        pclk_div_slave_deinit(CY_MMIO_TCPWM0_PERI_NR, CY_MMIO_TCPWM0_GROUP_NR, CY_MMIO_TCPWM0_SLAVE_NR);
    }
}

void tcpwm_clock_init(tcpwm_obj_t *obj, uint32_t target_hz) {
    uint32_t clk_freq = pclk_div_get_input_freq(obj->clk);
    if (clk_freq == 0U) {
        mp_raise_ValueError(MP_ERROR_TEXT("failed to get TCPWM input clock"));
    }
    // Fractional divider: f_out = f_in / ((divider + 1) + frac / 32).
    // Pick the smallest divisor that keeps f_out at or below target_hz.
    uint64_t scaled_div = ((uint64_t)clk_freq * 32ULL + target_hz - 1ULL) / target_hz;
    if (scaled_div < 32ULL) {
        scaled_div = 32ULL;
    }
    uint32_t divider = (uint32_t)(scaled_div / 32ULL) - 1U;
    uint8_t divider_frac = (uint8_t)(scaled_div % 32ULL);

    pclk_div_obj_t *div = pclk_div_init(obj->clk, divider, divider_frac);
    if (div == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("TCPWM clock dividers exhausted"));
    }
    obj->pclk_div = div;
}

void tcpwm_clock_deinit(tcpwm_obj_t *obj) {
    if (obj->pclk_div == NULL) {
        return;
    }
    pclk_div_obj_t *div = obj->pclk_div;
    obj->pclk_div = NULL;
    pclk_div_deinit(div);
}
