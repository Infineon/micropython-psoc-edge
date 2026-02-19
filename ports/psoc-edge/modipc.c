/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2026 Infineon Technologies AG
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
#include <string.h>

// micropython includes
#include "py/runtime.h"
#include "py/mphal.h"

// mtb includes
#include "cybsp.h"
#include "cy_pdl.h"
#include "ipc_communication.h"

// port-specific includes
#include "modipc.h"
#include "mplogger.h"

/*******************************************************************************
* Macros
*******************************************************************************/
/* CM55 boot address - matches official BSP configuration */
#define CM55_APP_BOOT_ADDR          (CYMEM_CM33_0_m55_nvm_START + CYBSP_MCUBOOT_HEADER_SIZE)

/*******************************************************************************
* Global Variables
*******************************************************************************/
static bool cm55_enabled = false;
static bool cm55_ipc_initialized = false;
CY_SECTION_SHAREDMEM static ipc_msg_t cm33_msg_data;
static volatile uint32_t cm55_last_response = 0;
static volatile bool cm55_response_received = false;
static volatile bool cm55_command_detected = false;
static char cm55_last_command[256] = {0};
static volatile bool cm55_wakeword_detected = false;

/*******************************************************************************
* Function Name: ipc_enable
********************************************************************************
* Summary:
*  Initialize IPC with CM55 (CM55 is already booted by main.c)
*
* Return:
*  mp_obj_t: None
*
*******************************************************************************/
static mp_obj_t ipc_enable(void) {
    if (cm55_enabled && cm55_ipc_initialized) {
        mplogger_print("CM55 already enabled and IPC initialized\n");
        return mp_const_none;
    }

    mplogger_print("Initializing IPC with CM55 (CM55 already booted by main.c)...\n");

    // ToDo: Enable IPC from here instead from main.c
    /* CM55 is already booted by main.c, IPC and callback are already set up */
    if (!cm55_ipc_initialized) {
        mplogger_print("IPC already set up by main.c\n");
        cm55_ipc_initialized = true;
    }

    cm55_enabled = true;

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_enable_obj, ipc_enable);

/*******************************************************************************
* Function Name: ipc_handle_cm55_message
********************************************************************************
* Summary:
*  External function to handle messages received from CM55
*  This is called from CM33 IPC callback
*
* Parameters:
*  cmd: Command code received from CM55
*  value: Value/data associated with the command
*
* Return:
*  void
*
*******************************************************************************/
void ipc_handle_cm55_message(uint8_t cmd, uint32_t value) {
    if (cmd == IPC_CMD_VA_COMMAND_DETECTED) {
        /* Command detected - extract command string from value */
        /* The value contains pointer to command string */
        const char *cmd_str = (const char *)value;
        if (cmd_str != NULL && cmd_str[0] != '\0') {
            strncpy(cm55_last_command, cmd_str, sizeof(cm55_last_command) - 1);
            cm55_last_command[sizeof(cm55_last_command) - 1] = '\0';
            cm55_command_detected = true;
            mplogger_print("\r\n[CM33] Command detected: %s\r\n", cm55_last_command);
        }
    } else if (cmd == IPC_CMD_VA_WAKEWORD_DETECTED) {
        cm55_wakeword_detected = true;
        mplogger_print("\r\n[CM33] Wake-word detected!\r\n");
    }
}


/*******************************************************************************
* Function Name: ipc_status
********************************************************************************
* Summary:
*  Returns whether CM55 is enabled
*
* Return:
*  mp_obj_t: True if CM55 is enabled, False otherwise
*
*******************************************************************************/
static mp_obj_t ipc_status(void) {
    return mp_obj_new_bool(cm55_enabled);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_status_obj, ipc_status);

/*******************************************************************************
* Function Name: ipc_start_cm55_va_model
********************************************************************************
* Summary:
*  Sends command to CM55 to start voice assistant model
*
* Return:
*  mp_obj_t: None
*
*******************************************************************************/
static mp_obj_t ipc_start_cm55_va_model(void) {
    /* Auto-initialize if not enabled */
    if (!cm55_enabled) {
        ipc_enable();
    }

    mplogger_print("[CM33] Sending VA_START command to CM55...\n");

    cm33_msg_data.client_id = CM55_IPC_PIPE_CLIENT_ID;
    cm33_msg_data.intr_mask = CY_IPC_CYPIPE_INTR_MASK;
    cm33_msg_data.cmd = IPC_CMD_VA_START;
    cm33_msg_data.value = 0;

    /* Check if IPC channel is unlocked */
    bool is_locked = Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(CY_IPC_CHAN_CYPIPE_EP2));
    if (is_locked) {
        mplogger_print("[CM33] IPC channel locked, cannot send\n");
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("IPC channel locked"));
    }

    /* Send message to CM55 */
    cy_en_ipcdrv_status_t drv_status = Cy_IPC_Drv_SendMsgPtr(
        Cy_IPC_Drv_GetIpcBaseAddress(CY_IPC_CHAN_CYPIPE_EP2),
        CY_IPC_CYPIPE_INTR_MASK_EP2,
        (void *)&cm33_msg_data
        );

    if (drv_status != CY_IPC_DRV_SUCCESS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("IPC send failed"));
    }

    mplogger_print("[CM33] Voice Assistant START command sent\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_start_cm55_va_model_obj, ipc_start_cm55_va_model);

/*******************************************************************************
* Function Name: ipc_stop_cm55_va_model
********************************************************************************
* Summary:
*  Sends command to CM55 to stop voice assistant model
*
* Return:
*  mp_obj_t: None
*
*******************************************************************************/
static mp_obj_t ipc_stop_cm55_va_model(void) {
    mplogger_print("[CM33] Sending VA_STOP command to CM55...\n");

    cm33_msg_data.client_id = CM55_IPC_PIPE_CLIENT_ID;
    cm33_msg_data.cmd = IPC_CMD_VA_STOP;
    cm33_msg_data.intr_mask = 0;

    /* Check if channel is locked */
    bool is_locked = Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(CY_IPC_CHAN_CYPIPE_EP2));
    if (is_locked) {
        mplogger_print("[CM33] IPC channel locked\n");
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("IPC channel locked"));
    }

    /* Send message */
    cy_en_ipcdrv_status_t drv_status = Cy_IPC_Drv_SendMsgPtr(
        Cy_IPC_Drv_GetIpcBaseAddress(CY_IPC_CHAN_CYPIPE_EP2),
        CY_IPC_CYPIPE_INTR_MASK_EP2,
        &cm33_msg_data
        );

    if (drv_status != CY_IPC_DRV_SUCCESS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("IPC send failed"));
    }

    mplogger_print("[CM33] Voice Assistant STOP command sent\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_stop_cm55_va_model_obj, ipc_stop_cm55_va_model);

/*******************************************************************************
* Function Name: ipc_get_last_command
********************************************************************************
* Summary:
*  Returns the last command detected by CM55 voice assistant
*
* Return:
*  mp_obj_t: String containing the last command, or None if no command
*
*******************************************************************************/
static mp_obj_t ipc_get_last_command(void) {
    if (cm55_command_detected && cm55_last_command[0] != '\0') {
        return mp_obj_new_str(cm55_last_command, strlen(cm55_last_command));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_get_last_command_obj, ipc_get_last_command);

/*******************************************************************************
* Function Name: ipc_has_command
********************************************************************************
* Summary:
*  Returns whether a new command has been detected
*
* Return:
*  mp_obj_t: True if command detected, False otherwise
*
*******************************************************************************/
static mp_obj_t ipc_has_command(void) {
    return mp_obj_new_bool(cm55_command_detected);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_has_command_obj, ipc_has_command);

/*******************************************************************************
* Function Name: ipc_clear_command
********************************************************************************
* Summary:
*  Clears the command detected flag
*
* Return:
*  mp_obj_t: None
*
*******************************************************************************/
static mp_obj_t ipc_clear_command(void) {
    cm55_command_detected = false;
    cm55_last_command[0] = '\0';
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_clear_command_obj, ipc_clear_command);

/*******************************************************************************
* Function Name: ipc_has_wakeword
********************************************************************************
* Summary:
*  Returns whether wake word has been detected
*
* Return:
*  mp_obj_t: True if wake word detected, False otherwise
*
*******************************************************************************/
static mp_obj_t ipc_has_wakeword(void) {
    return mp_obj_new_bool(cm55_wakeword_detected);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_has_wakeword_obj, ipc_has_wakeword);

/*******************************************************************************
* Function Name: ipc_clear_wakeword
********************************************************************************
* Summary:
*  Clears the wake word detected flag
*
* Return:
*  mp_obj_t: None
*
*******************************************************************************/
static mp_obj_t ipc_clear_wakeword(void) {
    cm55_wakeword_detected = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ipc_clear_wakeword_obj, ipc_clear_wakeword);

/*******************************************************************************
 * Module globals
 *******************************************************************************/

// Module globals table
static const mp_rom_map_elem_t ipc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ipc) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&ipc_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&ipc_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_cm55_va_model), MP_ROM_PTR(&ipc_start_cm55_va_model_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_cm55_va_model), MP_ROM_PTR(&ipc_stop_cm55_va_model_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_last_command), MP_ROM_PTR(&ipc_get_last_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_command), MP_ROM_PTR(&ipc_has_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_command), MP_ROM_PTR(&ipc_clear_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_wakeword), MP_ROM_PTR(&ipc_has_wakeword_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_wakeword), MP_ROM_PTR(&ipc_clear_wakeword_obj) },
};
static MP_DEFINE_CONST_DICT(ipc_module_globals, ipc_module_globals_table);

// Module definition
const mp_obj_module_t mp_module_ipc = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ipc_module_globals,
};

// Register the module
MP_REGISTER_MODULE(MP_QSTR_ipc, mp_module_ipc);
