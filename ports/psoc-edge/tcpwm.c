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
#include "tcpwm.h"
#include "clk.h"

/**
 * The file build-<board>/genhdr/pins_af.h contains the macro
 *
 *  MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(DO)
 *
 *  which uses the X-macro (as argument) pattern to pass a worker
 *  macro DO(idx, grp, cnt) for the list of all the TCPWM0 counters
 *  of the device, where:
 *  - idx: index of the counter (0 to MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS - 1).
 *  - grp: TCPWM0 group of the counter.
 *  - cnt: TCPWM0 counter number.
 *
 * The counters are those TCPWM0 LINE alternate functions defined
 * in the boards/pse8x_af.csv file.
 * See tools/boardgen.py and psoc-edge/boards/make-pins.py
 * for more information.
 */

static mp_obj_t machine_tcpwm0_counter[MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS] = { MP_OBJ_NULL };

// Shared reference count for the TCPWM0 slave TCPWM_0_SLAVE_NR.
static uint32_t machine_tcpwm0_slave_ref_count = 0;

// Map a TCPWM0 counter number to its index in the counter ownership array.
// Returns -1 if the counter is not available in the device.
#define TCPWM_COUNTER_INDEX_CASE(idx, grp, cnt) case cnt: \
        return idx;
static int machine_tcpwm_counter_to_index(uint32_t counter_num) {
    // All the device counters (from the PDL) must be collected by make-pins.py.
    MP_STATIC_ASSERT(MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS == TCPWM_GRP_NR0_GRP_GRP_CNT_NR + TCPWM_GRP_NR1_GRP_GRP_CNT_NR);

    switch (counter_num) {
        MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TCPWM_COUNTER_INDEX_CASE)
        default:
            return -1;
    }
}
#undef TCPWM_COUNTER_INDEX_CASE

static MP_NORETURN void machine_tcpwm_counter_raise_not_supported(uint32_t counter_num) {
    mp_raise_msg_varg(&mp_type_ValueError,
        MP_ERROR_TEXT("TCPWM0 counter %lu is not supported"), counter_num);
}

// Convert a Python object representing a TCPWM0 counter ID to its integer ID.
mp_int_t machine_tcpwm_id_from_obj(mp_obj_t id_in, qstr name) {
    if (!mp_obj_is_int(id_in)) {
        mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("%q id must be an integer"), name);
    }
    mp_int_t id = mp_obj_get_int(id_in);
    if (id < 0 || id >= MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%q id must be in range 0-%d"),
            name, MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS - 1);
    }
    return id;
}

// Map a TCPWM0 counter index to its corresponding counter number.
#define TCPWM_COUNTER_NUM_ENTRY(idx, grp, cnt) [idx] = cnt,
static const uint32_t machine_tcpwm0_counter_num[MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS] = {
    MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TCPWM_COUNTER_NUM_ENTRY)
};
#undef TCPWM_COUNTER_NUM_ENTRY

// Get the counter number for a given TCPWM0 counter index.
uint32_t machine_tcpwm_counter_num(size_t idx) {
    if (idx >= MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("TCPWM0 counter index %u is not supported"), (unsigned)idx);
    }
    return machine_tcpwm0_counter_num[idx];
}

bool machine_tcpwm_counter_try_alloc(uint32_t counter_num, mp_obj_t owner) {
    int idx = machine_tcpwm_counter_to_index(counter_num);
    if (idx < 0) {
        return false;
    }
    if (machine_tcpwm0_counter[idx] == owner) {
        return true;
    }
    if (machine_tcpwm0_counter[idx] != MP_OBJ_NULL) {
        return false;
    }
    machine_tcpwm0_counter[idx] = owner;
    return true;
}

void machine_tcpwm_counter_alloc(uint32_t counter_num, mp_obj_t owner) {
    if (machine_tcpwm_counter_to_index(counter_num) < 0) {
        machine_tcpwm_counter_raise_not_supported(counter_num);
    }
    if (!machine_tcpwm_counter_try_alloc(counter_num, owner)) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("TCPWM0 counter %lu is already in use by another Timer, Counter or PWM instance"), counter_num);
    }
}

void machine_tcpwm_counter_free(uint32_t counter_num, mp_obj_t owner) {
    int idx = machine_tcpwm_counter_to_index(counter_num);
    if (idx < 0) {
        return;
    }
    if (machine_tcpwm0_counter[idx] == owner) {
        machine_tcpwm0_counter[idx] = MP_OBJ_NULL;
    }
}

#define TCPWM_PCLK_CASE(idx, grp, cnt) case cnt: \
        return PCLK_TCPWM0_CLOCK_COUNTER_EN##cnt;
en_clk_dst_t machine_tcpwm_counter_pclk(uint32_t counter_num) {
    switch (counter_num) {
        MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TCPWM_PCLK_CASE)
        default:
            machine_tcpwm_counter_raise_not_supported(counter_num);
    }
}
#undef TCPWM_PCLK_CASE

// The maximum period of a counter is given by the counter width of its group.
#define TCPWM_GRP_PERIOD_MAX(grp) ((uint32_t)((1ULL << TCPWM_GRP_NR##grp##_CNT_GRP_CNT_WIDTH) - 1ULL))
#define TCPWM_PERIOD_MAX_CASE(idx, grp, cnt) case cnt: \
        return TCPWM_GRP_PERIOD_MAX(grp);
uint32_t machine_tcpwm_counter_period_max(uint32_t counter_num) {
    switch (counter_num) {
        MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TCPWM_PERIOD_MAX_CASE)
        default:
            machine_tcpwm_counter_raise_not_supported(counter_num);
    }
}
#undef TCPWM_PERIOD_MAX_CASE
#undef TCPWM_GRP_PERIOD_MAX

// return the IRQ number for the given counter.
#define TCPWM_IRQ_CASE(idx, grp, cnt) case cnt: \
        return tcpwm_0_interrupts_##cnt##_IRQn;
IRQn_Type machine_tcpwm_counter_irq(uint32_t counter_num) {
    switch (counter_num) {
        MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TCPWM_IRQ_CASE)
        default:
            machine_tcpwm_counter_raise_not_supported(counter_num);
    }
}
#undef TCPWM_IRQ_CASE

void machine_tcpwm_slave_init(en_clk_dst_t pclk_dst) {
    pclk_div_slave_init(pclk_dst, CY_MMIO_TCPWM0_PERI_NR, CY_MMIO_TCPWM0_GROUP_NR, CY_MMIO_TCPWM0_SLAVE_NR);
    machine_tcpwm0_slave_ref_count++;
}

void machine_tcpwm_slave_deinit(void) {
    if (machine_tcpwm0_slave_ref_count == 0) {
        return;
    }
    machine_tcpwm0_slave_ref_count--;
    // Only tear the shared slave down once the last Counter/Timer/PWM using the
    // TCPWM0 block has been deinitialised.
    if (machine_tcpwm0_slave_ref_count == 0) {
        pclk_div_slave_deinit(CY_MMIO_TCPWM0_PERI_NR, CY_MMIO_TCPWM0_GROUP_NR, CY_MMIO_TCPWM0_SLAVE_NR);
    }
}

// Initialize the TCPWM clock with the given PCLK destination and target frequency.
// Returns a pointer to the PCLK divider object.
pclk_div_obj_t *machine_tcpwm_clock_init(en_clk_dst_t pclk_dst, uint32_t target_hz) {
    machine_tcpwm_slave_init(pclk_dst);

    pclk_div_obj_t *pclk_div = NULL;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        uint32_t clk_freq = pclk_div_get_input_freq(pclk_dst);
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

        pclk_div = pclk_div_init(pclk_dst, divider, divider_frac);
        if (pclk_div == NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT("TCPWM clock dividers exhausted"));
        }
        nlr_pop();
    } else {
        // Release the slave reference taken above, so a failed init leaves nothing claimed.
        machine_tcpwm_slave_deinit();
        nlr_jump(nlr.ret_val);
    }
    return pclk_div;
}

void machine_tcpwm_clock_deinit(pclk_div_obj_t **pclk_div) {
    if (*pclk_div == NULL) {
        return;
    }
    pclk_div_obj_t *div = *pclk_div;
    *pclk_div = NULL;
    pclk_div_deinit(div);
    machine_tcpwm_slave_deinit();
}
