/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Infineon Technologies AG
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


// std includes
#include <stdio.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "cybsp.h"
#include "cy_pdl.h"
#include "cy_ipc_pipe.h"

// port-specific includes
#include "machine_ipc.h"
#include "ipc_ring.h"

/*******************************************************************************
* Macros
*******************************************************************************/
#define IPC_MAX_CLIENTS_PER_EP      (8UL)
#define IPC_MAX_ENDPOINTS           (5UL)

/* SRAM drain window: the persistent host buffer that target->host bytes are
 * copied into. Deliberately decoupled from (and smaller than) IPC_RING_T2H_CAPACITY
 * so a large SOCMEM result ring does not pin a large block of the MicroPython SRAM heap. */
#define IPC_T2H_CHUNK               (8192UL)

/* Back-pressure while streaming a host->target payload: when the ring is full
 * wait (bounded) for CM55 to drain before writing the remainder, so payloads
 * larger than the ring transfer losslessly instead of being rejected. */
#define IPC_TX_BACKPRESSURE_MAX_SPINS  (1000UL)
#define IPC_TX_BACKPRESSURE_DELAY_US   (100UL)

#define IPC_SEND_MAX_RETRIES        (100UL)
#define IPC_SEND_RETRY_DELAY_US     (1000UL)
#define CM55_APP_BOOT_ADDR          (CYMEM_CM33_0_m55_nvm_START + CYBSP_MCUBOOT_HEADER_SIZE)
#define CM55_BOOT_WAIT_TIME_USEC    (10U)
#define CM55_APP_DELAY_MS           (50U)

#define CM33_APP_DELAY_MS           (50U)

/* Core ID constants */
#define CM33               (0U)
#define CM55               (1U)

/*******************************************************************************
* Global Variables
*******************************************************************************/
static bool cm55_enabled = false;
CY_SECTION_SHAREDMEM static ipc_msg_t ipc_msg_buffer;
machine_ipc_obj_t *machine_ipc_obj[IPC_MAX_CLIENTS_PER_EP] = {NULL};

/*******************************************************************************
* machine_ipc_client_obj_t IRQ methods and type
*******************************************************************************/

static mp_uint_t machine_ipc_client_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    if (new_trigger == 0) {
        machine_ipc_client_obj_t *client = MP_OBJ_TO_PTR(self_in);
        client->base.handler = mp_const_none;
        if (client->client_id < IPC_MAX_CLIENTS_PER_EP) {
            MP_STATE_PORT(machine_ipc_client_handlers)[client->client_id] = mp_const_none;
        }
    }
    return 0;
}

static mp_uint_t machine_ipc_client_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    (void)self_in;
    (void)info_type;
    return 0;
}

static const mp_irq_methods_t machine_ipc_client_irq_methods = {
    .trigger = machine_ipc_client_irq_trigger,
    .info = machine_ipc_client_irq_info,
};

static void machine_ipc_client_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    machine_ipc_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_cmd) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->last_cmd);
    } else if (attr == MP_QSTR_value) {
        dest[0] = mp_obj_new_int_from_uint(self->last_value);
    } else if (attr == MP_QSTR_id) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->client_id);
    } else {
        // Not a property: fall through to locals_dict so methods (read) resolve.
        dest[1] = MP_OBJ_SENTINEL;
    }
}

/* client.read() — drain up to IPC_T2H_CHUNK bytes of the announced result from
 * the target->host ring into the persistent SRAM window and return them as a
 * memoryview (reused every call, so copy out to retain). Returns empty when
 * nothing is available yet; loop until `value` bytes have been read. */
static mp_obj_t machine_ipc_client_read(mp_obj_t self_in) {
    (void)self_in;
    uint8_t *buf = MP_STATE_PORT(machine_ipc_rx_buf);
    if (buf == NULL) {
        return mp_const_empty_bytes;
    }
    size_t n = ipc_ring_read(IPC_RING_TARGET_TO_HOST, buf, IPC_T2H_CHUNK);
    return (n > 0u) ? mp_obj_new_memoryview('B', n, buf) : mp_const_empty_bytes;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_ipc_client_read_obj, machine_ipc_client_read);

static const mp_rom_map_elem_t machine_ipc_client_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&machine_ipc_client_read_obj) },
};
static MP_DEFINE_CONST_DICT(machine_ipc_client_locals_dict, machine_ipc_client_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_ipc_client_type,
    MP_QSTR_IPCClient,
    MP_TYPE_FLAG_NONE,
    attr, machine_ipc_client_attr,
    locals_dict, &machine_ipc_client_locals_dict);

/*******************************************************************************
* IPC Helper functions
*******************************************************************************/

/**
 * Get endpoint configuration based on core ID
 * Returns endpoint address, channel, interrupt, and other config params
 */
static void get_endpoint_config_for_core(uint8_t core_id,
    uint32_t *ep_addr,
    uint32_t *ep_channel,
    uint32_t *ep_intr,
    uint32_t *ep_intr_mux,
    uint32_t *ep_intr_priority) {
    switch (core_id) {
        case CM33:
            *ep_addr = CM33_IPC_PIPE_EP_ADDR;
            *ep_channel = CY_IPC_CHAN_CYPIPE_EP1;
            *ep_intr = CY_IPC_INTR_CYPIPE_EP1;
            *ep_intr_mux = CY_IPC_INTR_CYPIPE_MUX_EP1;
            *ep_intr_priority = CY_IPC_INTR_CYPIPE_PRIOR_EP1;
            break;

        case CM55:
            *ep_addr = CM55_IPC_PIPE_EP_ADDR;
            *ep_channel = CY_IPC_CHAN_CYPIPE_EP2;
            *ep_intr = CY_IPC_INTR_CYPIPE_EP2;
            *ep_intr_mux = CY_IPC_INTR_CYPIPE_MUX_EP2;
            *ep_intr_priority = CY_IPC_INTR_CYPIPE_PRIOR_EP2;
            break;

        default:
            // Invalid core - set to zero
            *ep_addr = 0;
            *ep_channel = 0;
            *ep_intr = 0;
            *ep_intr_mux = 0;
            *ep_intr_priority = 0;
            break;
    }
}

/**
 * Get endpoint address for a given core ID
 */
static uint32_t get_ep_addr_for_core(uint8_t core_id) {
    switch (core_id) {
        case CM33:
            return CM33_IPC_PIPE_EP_ADDR;
        case CM55:
            return CM55_IPC_PIPE_EP_ADDR;
        default:
            return 0;
    }
}

/**
 * Get channel for a given core ID
 */
static uint32_t get_channel_for_core(uint8_t core_id) {
    switch (core_id) {
        case CM33:
            return CY_IPC_CHAN_CYPIPE_EP1;
        case CM55:
            return CY_IPC_CHAN_CYPIPE_EP2;
        default:
            return 0;
    }
}

/**
 * Build IPC pipe configuration structure
 */
static bool build_ipc_pipe_config(cy_stc_ipc_pipe_config_t *config_out,
    uint8_t src_core,
    uint8_t target_core,
    cy_ipc_pipe_callback_ptr_t *callbacks_array,
    void (*isr_handler)(void)) {

    // Validate cores
    if (src_core >= CM55 + 1 || target_core >= CM55 + 1) {
        printf("Error: Invalid core IDs (src=%d, target=%d)\r\n", src_core, target_core);
        return false;
    }

    if (src_core == target_core) {
        printf("Error: Source and target cores cannot be the same\r\n");
        return false;
    }

    // Get source endpoint configuration
    uint32_t src_addr, src_channel, src_intr, src_intr_mux, src_intr_priority;
    get_endpoint_config_for_core(src_core, &src_addr, &src_channel, &src_intr,
        &src_intr_mux, &src_intr_priority);

    // Get target endpoint configuration
    uint32_t tgt_addr, tgt_channel, tgt_intr, tgt_intr_mux, tgt_intr_priority;
    get_endpoint_config_for_core(target_core, &tgt_addr, &tgt_channel, &tgt_intr,
        &tgt_intr_mux, &tgt_intr_priority);

    // Build configuration structure to match static config roles
    // Endpoint 0: Source core (CM33 - receiver for messages from CM55)
    config_out->ep0ConfigData.ipcNotifierNumber = src_intr;
    config_out->ep0ConfigData.ipcNotifierPriority = src_intr_priority;
    config_out->ep0ConfigData.ipcNotifierMuxNumber = src_intr_mux;
    config_out->ep0ConfigData.epAddress = src_addr;
    config_out->ep0ConfigData.epConfig.epChannel = src_channel;
    config_out->ep0ConfigData.epConfig.epIntr = src_intr;
    config_out->ep0ConfigData.epConfig.epIntrmask = CY_IPC_CYPIPE_INTR_MASK;

    // Endpoint 1: Target core (CM55 - sender of messages to CM33)
    config_out->ep1ConfigData.ipcNotifierNumber = tgt_intr;
    config_out->ep1ConfigData.ipcNotifierPriority = tgt_intr_priority;
    config_out->ep1ConfigData.ipcNotifierMuxNumber = tgt_intr_mux;
    config_out->ep1ConfigData.epAddress = tgt_addr;
    config_out->ep1ConfigData.epConfig.epChannel = tgt_channel;
    config_out->ep1ConfigData.epConfig.epIntr = tgt_intr;
    config_out->ep1ConfigData.epConfig.epIntrmask = CY_IPC_CYPIPE_INTR_MASK;

    // Common configuration
    config_out->endpointClientsCount = IPC_MAX_CLIENTS_PER_EP;
    config_out->endpointsCallbacksArray = callbacks_array;
    config_out->userPipeIsrHandler = isr_handler;

    return true;
}

/**
 * Send an IPC message to target core
 *
 * @param src_endpoint_addr Source endpoint address (e.g., CM33_IPC_PIPE_EP_ADDR)
 * @param target_endpoint_addr Target endpoint address (e.g., CM55_IPC_PIPE_EP_ADDR)
 * @param target_client_id Target client ID (0-7)
 * @param cmd Command byte
 * @param value Data value
 * @return true if send successful, false otherwise
 */
static bool ipc_send_message(uint32_t src_endpoint_addr,
    uint32_t target_endpoint_addr,
    uint8_t target_client_id,
    uint8_t cmd,
    uint32_t value) {
    // Validate client_id
    if (target_client_id >= IPC_MAX_CLIENTS_PER_EP) {
        mp_printf(&mp_plat_print, "[CM33] Error: Invalid target_client_id %d\r\n", target_client_id);
        return false;
    }

    // Prepare message in shared memory buffer
    ipc_msg_buffer.client_id = target_client_id;
    ipc_msg_buffer.intr_mask = CY_IPC_CYPIPE_INTR_MASK;
    ipc_msg_buffer.cmd = cmd;
    ipc_msg_buffer.value = value;

    // Wait for channel to be free, then send
    cy_en_ipc_pipe_status_t status;
    uint32_t retry_count = 0;

    do {
        status = Cy_IPC_Pipe_SendMessage(
            target_endpoint_addr,    // toAddr   = destination (CM55)
            src_endpoint_addr,       // fromAddr = source      (CM33)
            (void *)&ipc_msg_buffer,
            NULL
            );

        if (status == CY_IPC_PIPE_SUCCESS) {
            break;
        }

        // If channel is busy, wait a bit and retry
        if (status == CY_IPC_PIPE_ERROR_SEND_BUSY) {
            Cy_SysLib_DelayUs(IPC_SEND_RETRY_DELAY_US);
            retry_count++;
        } else {
            // Other error - fail immediately
            mp_printf(&mp_plat_print, "[CM33] Error: Cy_IPC_Pipe_SendMessage failed with status %d\r\n", status);
            return false;
        }
    } while (retry_count < IPC_SEND_MAX_RETRIES);

    if (status != CY_IPC_PIPE_SUCCESS) {
        mp_printf(&mp_plat_print, "[CM33] Error: Channel busy after %lu retries, status %d\r\n", retry_count, status);
        return false;
    }

    return true;
}

/*******************************************************************************
* Setup Functions
*******************************************************************************/

void cm33_ipc_pipe_isr(void) {
    Cy_IPC_Pipe_ExecuteCallback(CM33_IPC_PIPE_EP_ADDR);
}

/**
 * Initialize IPC pipe
 *
 * @param src_core Source core ID
 * @param target_core Target core ID
 * @return true if initialization successful, false otherwise
 */
static bool ipc_pipe_init(uint8_t src_core, uint8_t target_core) {
    static cy_stc_ipc_pipe_ep_t cm33_ipc_pipe_ep_array[IPC_MAX_ENDPOINTS];
    static cy_ipc_pipe_callback_ptr_t ep1_cb_array[IPC_MAX_CLIENTS_PER_EP];
    CY_SECTION_SHAREDMEM static uint32_t ipc_sema_array[CY_IPC_SEMA_COUNT / CY_IPC_SEMA_PER_WORD];

    // Allocate configuration structure
    cy_stc_ipc_pipe_config_t pipe_config;

    // Build configuration dynamically
    bool config_set = build_ipc_pipe_config(&pipe_config,
        src_core,
        target_core,
        ep1_cb_array,
        &cm33_ipc_pipe_isr);

    if (!config_set) {
        printf("Error: Failed to build IPC pipe configuration\r\n");
        return false;
    }

    // Initialize semaphores (always needed)
    Cy_IPC_Sema_Init(IPC0_SEMA_CH_NUM, CY_IPC_SEMA_COUNT, ipc_sema_array);

    // Configure endpoint array
    Cy_IPC_Pipe_Config(cm33_ipc_pipe_ep_array);

    // Initialize pipe with dynamic configuration
    Cy_IPC_Pipe_Init(&pipe_config);

    return true;
}

/********************************************************************************************************
* MicroPython Constructor:
*  IPC(src_core, target_core) - Create an IPC object to function between 2 cores.
*  Max of 5 IPC objects can be created (based on hardware capabilities) and each object can represent
*  an endpoint/channel to communicate with the target core. The src_core and target_core
*  parameters can be used to set up the endpoint based on the cores involved in communication. Currently,
*  only CM33 to CM55 communication is supported and the endpoint configuration is fixed based on this use case but in
*  the future, this can be extended to support dynamic endpoint configuration based on the cores selected during
*  object creation.
********************************************************************************************************/
static mp_obj_t machine_ipc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_src_core, ARG_target_core };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_src_core, MP_ARG_INT, {.u_int = CM33} },
        { MP_QSTR_target_core, MP_ARG_INT, {.u_int = CM55} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    machine_ipc_obj_t *self = NULL;
    for (uint8_t i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (MP_STATE_PORT(machine_ipc_obj[i]) == NULL) {
            self = mp_obj_malloc(machine_ipc_obj_t, &machine_ipc_type);
            MP_STATE_PORT(machine_ipc_obj[i]) = self;
            break;
        }
    }
    if (self == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("all available IPC instances are allocated"));
    }
    self->base.type = &machine_ipc_type;

    // Initialise all client slots to unregistered sentinel so duplicate-check works
    for (uint8_t i = 0; i < IPC_MAX_CLIENTS_PER_EP; i++) {
        mp_irq_init(&sender_clients_arr[i].base, &machine_ipc_client_irq_methods,
            MP_OBJ_FROM_PTR(&sender_clients_arr[i]));
        sender_clients_arr[i].base.base.type = &machine_ipc_client_type;
        sender_clients_arr[i].client_id = IPC_CLIENT_ID_UNREGISTERED;
        sender_clients_arr[i].last_cmd = 0;
        sender_clients_arr[i].last_value = 0;
        sender_clients_arr[i].last_data_len = 0;
        MP_STATE_PORT(machine_ipc_client_handlers)[i] = mp_const_none;
    }

    // Validate cores selected
    if (args[ARG_src_core].u_int != CM33 || args[ARG_target_core].u_int != CM55) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid core values: only CM33<->CM55 communication is supported"));
    }

    // Store core information in the object for future reference (e.g., during endpoint registration)
    self->src_core = args[ARG_src_core].u_int;
    self->target_core = args[ARG_target_core].u_int;

    // Initialize sender endpoint structure based on source and target cores. Each endpoint can have max of IPC_MAX_CLIENTS_PER_EP clients,
    self->sender_endpoint = &sender_clients_arr[0];
    self->sender_endpoint->ep_id = args[ARG_src_core].u_int;
    self->sender_endpoint->ep_addr = CM33_IPC_PIPE_EP_ADDR;

    return MP_OBJ_FROM_PTR(self);
}

/* Scheduled in the MicroPython VM (scheduler) context from the pipe ISR for an
 * IPC_CMD_DATA_AVAIL doorbell. The doorbell only announces the total length
 * (client.value); the handler pulls the bytes with client.read() in <=IPC_T2H_CHUNK
 * chunks, so the ring is drained in VM context (never the ISR) and the result
 * ring can be far larger than the host SRAM window. */
static mp_obj_t ipc_data_dispatch(mp_obj_t client_in) {
    machine_ipc_client_obj_t *client = MP_OBJ_TO_PTR(client_in);
    if (client->base.handler != mp_const_none) {
        mp_call_function_1_protected(client->base.handler, client_in);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ipc_data_dispatch_obj, ipc_data_dispatch);

/* Callback to handle messages received from CM55 in CM33-NS. Registered with
*  the IPC driver for the CM33 source endpoint; called from the IPC ISR
*  (cm33_ipc_pipe_isr) when a message arrives from CM55.
*  Extracts cmd/value from the message and stores them in the per-client object
*  identified by client_id, then defers the Python callback to the MicroPython
*  VM via mp_irq_handler / mp_sched_schedule instead of calling it directly.
*/
void cm33_msg_callback(uint32_t *msg_data) {
    if (msg_data == NULL) {
        return;
    }
    ipc_msg_t *ipc_recv_msg = (ipc_msg_t *)msg_data;

    if (ipc_recv_msg->cmd == IPC_CMD_DATA_AVAIL) {
        /* Bulk data doorbell: value carries the total payload length (which may
         * exceed the ring; the host pulls it out in chunks). GC allocation is
         * not allowed in the pipe ISR, so defer to scheduler (VM) context. */
        uint32_t len = ipc_recv_msg->value;

        uint8_t data_client_id = ipc_recv_msg->client_id;
        if (data_client_id < IPC_MAX_CLIENTS_PER_EP) {
            machine_ipc_client_obj_t *data_client = &sender_clients_arr[data_client_id];
            if (data_client->base.handler != mp_const_none) {
                data_client->last_cmd = IPC_CMD_DATA_AVAIL;
                data_client->last_value = len;
                data_client->last_data_len = len;
                mp_sched_schedule(MP_OBJ_FROM_PTR(&ipc_data_dispatch_obj), data_client->base.parent);
            }
        }
        return;
    }

    uint8_t client_id = ipc_recv_msg->client_id;

    if (client_id >= IPC_MAX_CLIENTS_PER_EP) {
        mp_printf(&mp_plat_print, "[CM33] Invalid client_id %d in received message\r\n", client_id);
        return;
    }

    machine_ipc_client_obj_t *client = &sender_clients_arr[client_id];
    if (client->base.handler != mp_const_none) {
        client->last_cmd = ipc_recv_msg->cmd;
        client->last_value = ipc_recv_msg->value;
        mp_irq_handler(&client->base);
    }
}
/********************************************************************************************************
// IPC.register_client(client_id, callback, endpoint_id, endpoint_addr) - Register a client to receive messages from the other core. The client_id is an identifier for the client (0-7) and should be unique for each client. The callback is the Python function that will be called when a message is received for this client. The endpoint_id and endpoint_addr are used to register the callback with the IPC driver for the correct endpoint.
* @param args[0] self - IPC object
* @param args[1] client_id - Unique client identifier (0-7)
* @param args[2] callback - Python function to call when message received for this client
* @param args[3] endpoint_id - Endpoint ID to register with IPC driver (e.g., CM33_IPC_PIPE_EP_ADDR)
* @param args[4] endpoint_addr - Endpoint address to register with IPC driver (e.g., CM33_IPC_PIPE_EP_ADDR)
********************************************************************************************************/

static mp_obj_t machine_ipc_register_client(size_t n_args, const mp_obj_t *args) {
    uint8_t client_id = mp_obj_get_int(args[1]);
    mp_obj_t callback_handler = args[2];
    uint8_t endpoint_id = mp_obj_get_int(args[3]);
    uint32_t endpoint_addr = mp_obj_get_int(args[4]);


    // Validate client_id : Should be unique i.e no two clients can have same id and should be less than IPC_MAX_CLIENTS_PER_EP
    if (client_id >= IPC_MAX_CLIENTS_PER_EP) {
        mp_printf(&mp_plat_print, "Error: Invalid client_id %d (max %ld)\r\n",
            client_id, IPC_MAX_CLIENTS_PER_EP - 1);
        return mp_const_false;
    }

    // Validate callback
    if (callback_handler != mp_const_none && !mp_obj_is_callable(callback_handler)) {
        mp_printf(&mp_plat_print, "Error: Callback is not callable\r\n");
        return mp_const_false;
    }

    // Check if client_id is already registered (unregistered slots have IPC_CLIENT_ID_UNREGISTERED)
    for (uint8_t i = 0; i < IPC_MAX_CLIENTS_PER_EP; i++) {
        if (sender_clients_arr[i].client_id != IPC_CLIENT_ID_UNREGISTERED &&
            sender_clients_arr[i].client_id == client_id) {
            mp_printf(&mp_plat_print, "Error: client_id %d is already registered\r\n", client_id);
            return mp_const_false;
        }
    }

    // Set the client information in the global sender_clients_arr based on client_id
    sender_clients_arr[client_id].client_id = client_id;
    sender_clients_arr[client_id].base.handler = callback_handler;
    MP_STATE_PORT(machine_ipc_client_handlers)[client_id] = callback_handler;
    sender_clients_arr[client_id].ep_addr = endpoint_addr;
    sender_clients_arr[client_id].ep_id = endpoint_id;

    // Register the C callback with IPC pipe framework. The client_id parameter is the index in the callback array where the function pointer is saved
    cy_en_ipc_pipe_status_t status = Cy_IPC_Pipe_RegisterCallback(
        endpoint_addr,           // Endpoint address (CM33_IPC_PIPE_EP_ADDR)
        &cm33_msg_callback,       // Callback function pointer
        (uint32_t)client_id      // Client ID - index in callback array
        );
    if (status != CY_IPC_PIPE_SUCCESS) {
        mp_printf(&mp_plat_print, "Error: Cy_IPC_Pipe_RegisterCallback failed with status %d\r\n", status);
        return mp_const_false;
    }

    mp_printf(&mp_plat_print, "[CM33] -> Registered client with ID: %d successfully \r\n", client_id);

    return mp_const_true;

}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_ipc_register_client_obj, 5, 5, machine_ipc_register_client);


/*******************************************************************************
* IPC.init() - Initialize IPC communication
* @param args[0] self - IPC object
*******************************************************************************/
static mp_obj_t machine_ipc_init(mp_obj_t self_in) {
    machine_ipc_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Initialize IPC pipe with dynamic configuration based on object's cores
    bool init_ok = ipc_pipe_init(self->src_core, self->target_core);

    if (!init_ok) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("IPC pipe initialization failed"));
    }

    /* CM33 owns the host -> target ring (m33_m55_shared SOCMEM region). */
    ipc_ring_init(IPC_RING_HOST_TO_TARGET, IPC_RING_H2T_CAPACITY);

    /* Receive: one persistent fixed SRAM window (IPC_T2H_CHUNK), allocated once
     * and rooted, reused for every client.read() drain. */
    if (MP_STATE_PORT(machine_ipc_rx_buf) == NULL) {
        MP_STATE_PORT(machine_ipc_rx_buf) = m_new(uint8_t, IPC_T2H_CHUNK);
    }

    Cy_SysLib_Delay(CM33_APP_DELAY_MS);

    mp_printf(&mp_plat_print, "IPC initialized successfully\r\n");

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_ipc_init_obj, machine_ipc_init);

/*******************************************************************************
* IPC.enable_core(core_id) - Enable target core
* @param args[0] self - IPC object
* @param args[1] core_id - Core to enable (default: CM55)
*******************************************************************************/
// Not required as API to user? Call this from constructor or init?
static mp_obj_t machine_ipc_enable_core(size_t n_args, const mp_obj_t *args) {
    // Default to CM55 (CM55) if no argument provided
    uint8_t core_id = (n_args > 1) ? mp_obj_get_int(args[1]) : CM55;

    // Currently only CM55 (CM55) is supported
    if (core_id == CM55) {
        if (cm55_enabled) {
            mp_printf(&mp_plat_print, "CM55 already enabled\r\n");
            return mp_const_true;
        }
        Cy_SysEnableCM55(MXCM55, CM55_APP_BOOT_ADDR, CM55_BOOT_WAIT_TIME_USEC);
        cm55_enabled = true;
        Cy_SysLib_Delay(CM33_APP_DELAY_MS);

        mp_printf(&mp_plat_print, "Enabling CM55 core at boot address: 0x%08X\r\n", CM55_APP_BOOT_ADDR);

        return mp_const_true;
    } else if (core_id == CM33) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid operation: CM33 core is already active and cannot be re-enabled"));
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("unsupported core, only CM55 is supported")); // ToDo: This should be generalised
    }

    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_ipc_enable_core_obj, 1, 2, machine_ipc_enable_core);

/*******************************************************************************
* IPC.send(cmd, value, client_id) - Send message to target core
*
* @param args[0] self - IPC object
* @param args[1] cmd - Command byte
* @param args[2] value - Optional data value (default: 0)
* @param args[3] client_id - Target client ID
*******************************************************************************/
static mp_obj_t machine_ipc_send(size_t n_args, const mp_obj_t *args) {
    machine_ipc_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    // Parse arguments
    uint32_t cmd = mp_obj_get_int(args[1]);
    uint8_t client_id = mp_obj_get_int(args[3]);

    uint32_t src_ep_addr = self->sender_endpoint->ep_addr;
    uint32_t target_ep_addr = get_ep_addr_for_core(self->target_core);

    // Bulk data path: when the payload argument is a buffer (bytes/bytearray/
    // memoryview), stream it to the host->target ring losslessly. Write what
    // fits, announce the total via the DATA_AVAIL doorbell, then keep writing
    // the remainder, blocking (bounded) while the ring is full until CM55
    // drains. This allows payloads larger than the ring, which CM55 picks up
    // in chunks.
    mp_buffer_info_t bufinfo;
    if (n_args > 2 && mp_get_buffer(args[2], &bufinfo, MP_BUFFER_READ)) {
        const uint8_t *base = (const uint8_t *)bufinfo.buf;
        size_t total = bufinfo.len;
        size_t sent = ipc_ring_write(IPC_RING_HOST_TO_TARGET, base, total);
        if (!ipc_send_message(src_ep_addr, target_ep_addr, client_id,
            IPC_CMD_DATA_AVAIL, (uint32_t)total)) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("[CM33] IPC send failed"));
        }
        uint32_t spins = 0;
        while (sent < total) {
            size_t n = ipc_ring_write(IPC_RING_HOST_TO_TARGET, base + sent, total - sent);
            if (n > 0) {
                sent += n;
                spins = 0;
                continue;
            }
            if (++spins > IPC_TX_BACKPRESSURE_MAX_SPINS) {
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("[CM33] IPC ring stalled"));
            }
            Cy_SysLib_DelayUs(IPC_TX_BACKPRESSURE_DELAY_US);
        }
        return mp_const_none;
    }

    // Control message path: fixed cmd + 32-bit value.
    uint32_t value = (n_args > 2) ? mp_obj_get_int(args[2]) : 0;

    // Use helper function to send message
    bool success = ipc_send_message(
        src_ep_addr,     // Source endpoint (from self)
        target_ep_addr,  // Target endpoint (from self->target_core)
        client_id,       // Target client
        (uint8_t)cmd,    // Command
        value            // Value
        );

    if (!success) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("[CM33] IPC send failed"));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_ipc_send_obj, 2, 4, machine_ipc_send);

/*******************************************************************************
* IPC.is_busy() - Check if IPC is busy
* @param args[0] self - IPC object
* @param args[1] Optional core_id to check specific core's IPC status (default: target_core)
*******************************************************************************/
static mp_obj_t machine_ipc_is_busy(size_t n_args, const mp_obj_t *args) {
    machine_ipc_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint8_t core_id = (n_args > 1) ? mp_obj_get_int(args[1]) : self->target_core;

    uint32_t channel = get_channel_for_core(core_id);
    if (channel == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid core ID"));
    }
    bool locked = Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(channel));
    return mp_obj_new_bool(locked);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_ipc_is_busy_obj, 1, 2, machine_ipc_is_busy);

/*******************************************************************************
* IPC locals dictionary
*******************************************************************************/
static const mp_rom_map_elem_t machine_ipc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_ipc_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_enable_core), MP_ROM_PTR(&machine_ipc_enable_core_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&machine_ipc_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy), MP_ROM_PTR(&machine_ipc_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_register_client), MP_ROM_PTR(&machine_ipc_register_client_obj) },

    // ToDo: API encapsulation for model fns - also similar to openamp Should be a wrapper on send()?
    // { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&machine_ipc_start_obj) },
    // { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&machine_ipc_stop_obj) },

    // Core ID constants
    { MP_ROM_QSTR(MP_QSTR_CM33), MP_ROM_INT(CM33) },
    { MP_ROM_QSTR(MP_QSTR_CM55), MP_ROM_INT(CM55) },

    // Command constants
    { MP_ROM_QSTR(MP_QSTR_CMD_START), MP_ROM_INT(IPC_CMD_START) },
    { MP_ROM_QSTR(MP_QSTR_CMD_STOP), MP_ROM_INT(IPC_CMD_STOP) },
    { MP_ROM_QSTR(MP_QSTR_CMD_DATA_AVAIL), MP_ROM_INT(IPC_CMD_DATA_AVAIL) },
};
static MP_DEFINE_CONST_DICT(machine_ipc_locals_dict, machine_ipc_locals_dict_table);

MP_REGISTER_ROOT_POINTER(struct _machine_ipc_obj_t *machine_ipc_obj[IPC_MAX_CLIENTS_PER_EP]);
MP_REGISTER_ROOT_POINTER(mp_obj_t machine_ipc_client_handlers[IPC_MAX_CLIENTS_PER_EP]);
MP_REGISTER_ROOT_POINTER(uint8_t * machine_ipc_rx_buf);

void machine_ipc_deinit_all(void) {
    cm55_enabled = false;
    for (uint8_t i = 0; i < IPC_MAX_CLIENTS_PER_EP; i++) {
        sender_clients_arr[i].client_id = IPC_CLIENT_ID_UNREGISTERED;
        sender_clients_arr[i].base.handler = mp_const_none;
        MP_STATE_PORT(machine_ipc_client_handlers)[i] = mp_const_none;
    }
}

MP_DEFINE_CONST_OBJ_TYPE(
    machine_ipc_type,
    MP_QSTR_IPC,
    MP_TYPE_FLAG_NONE,
    make_new, machine_ipc_make_new,
    locals_dict, &machine_ipc_locals_dict
    );
