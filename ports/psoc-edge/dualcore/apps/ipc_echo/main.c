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

// ---------------------------------------------------------------------------
// CM55 IPC echo firmware for the `ipc.py` board test.
//
// This is the CM55 counterpart flashed at the m55_nvm region (0x60580000).
// CM33 (running MicroPython) boots this image via ipc.enable_core(IPC.CM55).
//
// Two independent services share the single CM55 endpoint (EP_ADDR=2):
//   Service 1: CM55 client_id=5  -> echoes back to CM33 client_id=3
//   Service 2: CM55 client_id=6  -> echoes back to CM33 client_id=4
//
// Part A (command ping-pong): each callback records the received command/value
// and the main loop sends the same command straight back to the matching CM33
// client.
//
// Part B (bulk data): a CMD_DATA_AVAIL doorbell to client 5 announces a payload
// streamed into the host->target shared ring. The main loop pumps those bytes
// straight back into the target->host ring and doorbells CM33 client 3, so the
// payload is echoed losslessly.
//
// All sends and the bulk pump run from the main loop (not the ISR) to keep the
// IPC work out of interrupt context.
// ---------------------------------------------------------------------------

#include "ipc_communication.h"
#include "ipc_ring.h"

#define CM55_APP_DELAY_MS   (50U)

// Doorbell command: bulk bytes are available in the shared ring buffer.
// Must match IPC_CMD_DATA_AVAIL in ports/psoc-edge/machine_ipc.h.
#define IPC_CMD_DATA_AVAIL      (0x84U)

// Bounded back-pressure while pumping the bulk echo between the shared rings.
#define CM55_BULK_MAX_SPINS     (1000U)
#define CM55_BULK_SPIN_DELAY_US (100U)
#define CM55_BULK_SCRATCH_SIZE  (8192U)

// IPC message layout shared with CM33 (see ports/psoc-edge/machine_ipc.h).
typedef struct {
    uint8_t client_id;
    uint16_t intr_mask;
    uint8_t cmd;
    uint32_t value;
} ipc_msg_t;

// Outgoing echo buffers must live in shared memory so CM33 can read them.
CY_SECTION_SHAREDMEM static ipc_msg_t cm55_svc1_tx;
CY_SECTION_SHAREDMEM static ipc_msg_t cm55_svc2_tx;
CY_SECTION_SHAREDMEM static ipc_msg_t cm55_bulk_tx;

// Staging buffer for copying the bulk payload between the two shared rings.
static uint8_t bulk_scratch[CM55_BULK_SCRATCH_SIZE];

// Pending-echo state written in the ISR callbacks, consumed by the main loop.
static volatile bool svc1_pending = false;
static volatile uint8_t svc1_cmd = 0U;
static volatile uint32_t svc1_value = 0UL;

static volatile bool svc2_pending = false;
static volatile uint8_t svc2_cmd = 0U;
static volatile uint32_t svc2_value = 0UL;

// Pending bulk-echo state: total length announced by the CMD_DATA_AVAIL doorbell.
static volatile bool bulk_pending = false;
static volatile uint32_t bulk_total = 0UL;

// Service 1 receive callback (CM55 client_id=5).
static void cm55_svc1_callback(uint32_t *msg_data) {
    if (msg_data == NULL) {
        return;
    }
    ipc_msg_t *rx = (ipc_msg_t *)msg_data;
    if (rx->cmd == IPC_CMD_DATA_AVAIL) {
        // Bulk doorbell: value carries the total payload length.
        bulk_total = rx->value;
        bulk_pending = true;
        return;
    }
    svc1_cmd = rx->cmd;
    svc1_value = rx->value;
    svc1_pending = true;
}

// Service 2 receive callback (CM55 client_id=6).
static void cm55_svc2_callback(uint32_t *msg_data) {
    if (msg_data == NULL) {
        return;
    }
    ipc_msg_t *rx = (ipc_msg_t *)msg_data;
    svc2_cmd = rx->cmd;
    svc2_value = rx->value;
    svc2_pending = true;
}

int main(void) {
    // Enable global interrupts so the IPC notify interrupt can fire.
    __enable_irq();

    // Set up the CM55 IPC pipe endpoint (EP2) and its ISR.
    cm55_ipc_communication_setup();

    // CM55 owns the target->host ring (m33_m55_shared SOCMEM region).
    ipc_ring_init(IPC_RING_TARGET_TO_HOST, IPC_RING_T2H_CAPACITY);

    Cy_SysLib_Delay(CM55_APP_DELAY_MS);

    // Register Service 1 at CM55 client_id=5.
    (void)Cy_IPC_Pipe_RegisterCallback(CM55_IPC_PIPE_EP_ADDR,
        &cm55_svc1_callback, (uint32_t)CM55_IPC_PIPE_CLIENT_ID);

    // Register Service 2 at CM55 client_id=6.
    (void)Cy_IPC_Pipe_RegisterCallback(CM55_IPC_PIPE_EP_ADDR,
        &cm55_svc2_callback, (uint32_t)(CM55_IPC_PIPE_CLIENT_ID + 1U));

    for (;;) {
        if (svc1_pending) {
            svc1_pending = false;
            // Echo Service 1 back to CM33 client_id=3.
            cm55_svc1_tx.client_id = CM33_IPC_PIPE_CLIENT_ID;
            cm55_svc1_tx.intr_mask = CY_IPC_CYPIPE_INTR_MASK;
            cm55_svc1_tx.cmd = svc1_cmd;
            cm55_svc1_tx.value = svc1_value;
            (void)Cy_IPC_Pipe_SendMessage(CM33_IPC_PIPE_EP_ADDR,
                CM55_IPC_PIPE_EP_ADDR, (void *)&cm55_svc1_tx, NULL);
        }

        if (svc2_pending) {
            svc2_pending = false;
            // Echo Service 2 back to CM33 client_id=4.
            cm55_svc2_tx.client_id = CM33_IPC_PIPE_CLIENT_ID + 1U;
            cm55_svc2_tx.intr_mask = CY_IPC_CYPIPE_INTR_MASK;
            cm55_svc2_tx.cmd = svc2_cmd;
            cm55_svc2_tx.value = svc2_value;
            (void)Cy_IPC_Pipe_SendMessage(CM33_IPC_PIPE_EP_ADDR,
                CM55_IPC_PIPE_EP_ADDR, (void *)&cm55_svc2_tx, NULL);
        }

        if (bulk_pending) {
            bulk_pending = false;
            uint32_t total = bulk_total;

            // Announce the echo up-front so CM33 (client 3) starts draining the
            // target->host ring while we stream, matching its chunked read.
            cm55_bulk_tx.client_id = CM33_IPC_PIPE_CLIENT_ID;
            cm55_bulk_tx.intr_mask = CY_IPC_CYPIPE_INTR_MASK;
            cm55_bulk_tx.cmd = IPC_CMD_DATA_AVAIL;
            cm55_bulk_tx.value = total;
            (void)Cy_IPC_Pipe_SendMessage(CM33_IPC_PIPE_EP_ADDR,
                CM55_IPC_PIPE_EP_ADDR, (void *)&cm55_bulk_tx, NULL);

            // Pump host->target payload straight back into the target->host
            // ring in <= CM55_BULK_SCRATCH_SIZE pieces, blocking (bounded) when
            // either ring is momentarily empty (producer) or full (consumer).
            uint32_t done = 0U;
            uint32_t rx_spins = 0U;
            while (done < total) {
                uint32_t want = total - done;
                if (want > sizeof(bulk_scratch)) {
                    want = sizeof(bulk_scratch);
                }
                size_t n = ipc_ring_read(IPC_RING_HOST_TO_TARGET, bulk_scratch, want);
                if (n == 0U) {
                    if (++rx_spins > CM55_BULK_MAX_SPINS) {
                        break;              // producer stalled: bail (partial)
                    }
                    Cy_SysLib_DelayUs(CM55_BULK_SPIN_DELAY_US);
                    continue;
                }
                rx_spins = 0U;
                size_t w = 0U;
                uint32_t tx_spins = 0U;
                while (w < n) {
                    size_t k = ipc_ring_write(IPC_RING_TARGET_TO_HOST,
                        bulk_scratch + w, n - w);
                    if (k == 0U) {
                        if (++tx_spins > CM55_BULK_MAX_SPINS) {
                            break;          // host stalled: bail (partial)
                        }
                        Cy_SysLib_DelayUs(CM55_BULK_SPIN_DELAY_US);
                        continue;
                    }
                    w += k;
                    tx_spins = 0U;
                }
                done += (uint32_t)n;
            }
        }

        Cy_SysLib_Delay(CM55_APP_DELAY_MS);
    }
}
