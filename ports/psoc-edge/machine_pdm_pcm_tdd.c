#include "modmachine.h"

#if MICROPY_PY_MACHINE_PDM_PCM_TDD

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include <stdlib.h>
// #include "app_pdm_pcm.h"
#include "retarget_io_init.h"


#include "extmod/vfs.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/ringbuf.h"

/*******************************************************************************
* Macros
*******************************************************************************/
/* Number of channels  */
#define NUM_CHANNELS                            (2u)

/* Channel Index */
#define LEFT_CH_INDEX                           (2u)
#define RIGHT_CH_INDEX                          (3u)
#define LEFT_CH_CONFIG                          channel_2_config
#define RIGHT_CH_CONFIG                         channel_3_config

/* PDM PCM interrupt priority */
#define PDM_PCM_ISR_PRIORITY                    (2u)

/* Frame size over which volume is calculated */
#define FRAME_SIZE                              (32768u)

/* PDM PCM hardware FIFO size */
#define HW_FIFO_SIZE                            (64u)
/* Rx FIFO trigger level/threshold configured by user */
#define RX_FIFO_TRIG_LEVEL                      (HW_FIFO_SIZE / 2)
/* Total number of interrupts to get the FRAME_SIZE number of samples*/
#define NUMBER_INTERRUPTS_FOR_FRAME             (FRAME_SIZE / RX_FIFO_TRIG_LEVEL)

/* Noise threshold hysteresis */
#define THRESHOLD_HYSTERESIS                    (3u)
/* Volume ratio for noise and print purposes */
// #define VOLUME_RATIO                            (10*FRAME_SIZE)

/* Gain range for EVK kit PDM mic */
#define PDM_PCM_MIN_GAIN                        (-103.0)
#define PDM_PCM_MAX_GAIN                        (83.0)
#define PDM_MIC_GAIN_VALUE                      (20)

/* Gain to Scale mapping */

#define PDM_PCM_SEL_GAIN_83DB                   (83.0)
#define PDM_PCM_SEL_GAIN_77DB                   (77.0)
#define PDM_PCM_SEL_GAIN_71DB                   (71.0)
#define PDM_PCM_SEL_GAIN_65DB                   (65.0)
#define PDM_PCM_SEL_GAIN_59DB                   (59.0)
#define PDM_PCM_SEL_GAIN_53DB                   (53.0)
#define PDM_PCM_SEL_GAIN_47DB                   (47.0)
#define PDM_PCM_SEL_GAIN_41DB                   (41.0)
#define PDM_PCM_SEL_GAIN_35DB                   (35.0)
#define PDM_PCM_SEL_GAIN_29DB                   (29.0)
#define PDM_PCM_SEL_GAIN_23DB                   (23.0)
#define PDM_PCM_SEL_GAIN_17DB                   (17.0)
#define PDM_PCM_SEL_GAIN_11DB                   (11.0)
#define PDM_PCM_SEL_GAIN_5DB                    (5.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_1DB           (-1.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_7DB           (-7.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_13DB          (-13.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_19DB          (-19.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_25DB          (-25.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_31DB          (-31.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_37DB          (-37.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_43DB          (-43.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_49DB          (-49.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_55DB          (-55.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_61DB          (-61.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_67DB          (-67.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_73DB          (-73.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_79DB          (-79.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_85DB          (-85.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_91DB          (-91.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_97DB          (-97.0)
#define PDM_PCM_SEL_GAIN_NEGATIVE_103DB         (-103.0)

void app_pdm_pcm_init(void);
void pdm_interrupt_handler(void);
void app_pdm_pcm_activate(void);
void app_pdm_pcm_deactivate(void);
cy_en_pdm_pcm_gain_sel_t convert_db_to_pdm_scale(double db);
void set_pdm_pcm_gain(cy_en_pdm_pcm_gain_sel_t gain);


typedef struct _ring_buf_t {
    uint8_t *buffer;
    size_t head;
    size_t tail;
    size_t size;
} ring_buf_t;

typedef struct _non_blocking_descriptor_t {
    mp_buffer_info_t appbuf;
    uint32_t index;
    bool copy_in_progress;
} non_blocking_descriptor_t;

void ringbuf_init(ring_buf_t *rbuf, size_t size);
bool ringbuf_push(ring_buf_t *rbuf, uint8_t data);
bool ringbuf_pop(ring_buf_t *rbuf, uint8_t *data);
size_t ringbuf_available_data(ring_buf_t *rbuf);
size_t ringbuf_available_space(ring_buf_t *rbuf);

void ringbuf_init(ring_buf_t *rbuf, size_t size) {
    rbuf->buffer = m_new(uint8_t, size);
    memset(rbuf->buffer, 0, size);
    rbuf->size = size;
    rbuf->head = 0;
    rbuf->tail = 0;
}

bool ringbuf_push(ring_buf_t *rbuf, uint8_t data) {
    size_t next_tail = (rbuf->tail + 1) % rbuf->size;
    if (next_tail != rbuf->head) {
        rbuf->buffer[rbuf->tail] = data;
        rbuf->tail = next_tail;
        return true;
    }
    // full
    return false;
}

bool ringbuf_pop(ring_buf_t *rbuf, uint8_t *data) {
    if (rbuf->head == rbuf->tail) {
        // empty
        return false;
    }

    *data = rbuf->buffer[rbuf->head];
    rbuf->head = (rbuf->head + 1) % rbuf->size;
    return true;
}

size_t ringbuf_available_data(ring_buf_t *rbuf) {
    return (rbuf->tail - rbuf->head + rbuf->size) % rbuf->size;
}

size_t ringbuf_available_space(ring_buf_t *rbuf) {
    return rbuf->size - ringbuf_available_data(rbuf) - 1;
}


/* Array containing the recorded data */
uint8_t audio_buffer0[NUM_CHANNELS * FRAME_SIZE * 2];
// ringbuf_t ring_buffer;
// int16_t buf_array[NUM_CHANNELS * FRAME_SIZE * 2]; // 2 bytes per sample for 16-bit audio
uint32_t rxbuf_len = NUM_CHANNELS * FRAME_SIZE * 2;
ring_buf_t ring_buffer;
// ringbuf_t ring_buffer = {(uint8_t *)audio_buffer0, NUM_CHANNELS * FRAME_SIZE * 2, 0, 0}; // Using audio_buffer0 as the storage for the ring buffer
ring_buf_t ring_buffer = {(uint8_t *)audio_buffer0, NUM_CHANNELS *FRAME_SIZE * 2, 0, 0};  // Initialize with NULL buffer and zero size, will be set in app_pdm_pcm_init

/* PDM PCM interrupt configuration parameters */
const cy_stc_sysint_t PDM_IRQ_cfg =
{
    .intrSrc = (IRQn_Type)CYBSP_PDM_CHANNEL_3_IRQ,
    .intrPriority = PDM_PCM_ISR_PRIORITY
};

/* PDM PCM flag indicating PCM data is available to process */
volatile bool pdm_pcm_flag = false;

/* Volume variables */
// uint32_t volume = 0;
// uint32_t noise_threshold = THRESHOLD_HYSTERESIS;

/* Counts number of half FIFO frames captured */
uint32_t static frame_counter = 0;

/*******************************************************************************
* Function Name: app_pdm_pcm_init
********************************************************************************
* Summary: This function initializes the PDM PCM block
*
* Parameters:
*  none
*
* Return :
*  none
*
*******************************************************************************/
void app_pdm_pcm_init(void) {
    cy_en_pdm_pcm_gain_sel_t gain_scale = CY_PDM_PCM_SEL_GAIN_NEGATIVE_37DB;

    // ringbuf_alloc(&ring_buffer, rxbuf_len);
    // ringbuf_init(&ring_buffer, rxbuf_len);

    /* Initialize PDM PCM block */
    if (CY_PDM_PCM_SUCCESS != Cy_PDM_PCM_Init(PDM0, &CYBSP_PDM_config)) {
        CY_ASSERT(0);
    }

    /* Initialize PDM PCM channel 2 -Left and 3 -Right */
    Cy_PDM_PCM_Channel_Enable(PDM0, LEFT_CH_INDEX);
    Cy_PDM_PCM_Channel_Enable(PDM0, RIGHT_CH_INDEX);

    Cy_PDM_PCM_Channel_Init(PDM0, &LEFT_CH_CONFIG, (uint8_t)LEFT_CH_INDEX);
    Cy_PDM_PCM_Channel_Init(PDM0, &RIGHT_CH_CONFIG, (uint8_t)RIGHT_CH_INDEX);

    /* Set the gain for both left and right channels. */

    gain_scale = convert_db_to_pdm_scale((double)PDM_MIC_GAIN_VALUE);
    set_pdm_pcm_gain(gain_scale);

    /* As interrupt is registered for right channel, clear and set masks for it. */
    Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, RIGHT_CH_INDEX, CY_PDM_PCM_INTR_MASK);
    Cy_PDM_PCM_Channel_SetInterruptMask(PDM0, RIGHT_CH_INDEX, CY_PDM_PCM_INTR_MASK);

    /* Register the IRQ handler */
    if (CY_SYSINT_SUCCESS != Cy_SysInt_Init(&PDM_IRQ_cfg, &pdm_interrupt_handler)) {
        printf("PDM PCM Initialization has failed! \r\n");
        CY_ASSERT(0);
    }
    NVIC_ClearPendingIRQ(PDM_IRQ_cfg.intrSrc);
    NVIC_EnableIRQ(PDM_IRQ_cfg.intrSrc);


}

/*******************************************************************************
* Function Name: app_pdm_pcm_activate
********************************************************************************
* Summary: This function activates the left and right channel.
*
* Parameters:
*  none
*
* Return :
*  none
*
*******************************************************************************/
void app_pdm_pcm_activate(void) {
    /* Activate recording from channel after init */
    Cy_PDM_PCM_Activate_Channel(PDM0, LEFT_CH_INDEX);
    Cy_PDM_PCM_Activate_Channel(PDM0, RIGHT_CH_INDEX);
}

void save_audio_to_file() {
    while (!pdm_pcm_flag) {
        // Wait until the PDM PCM data is ready
    }

    // for(uint32_t i=0; i < rxbuf_len/2; i++)
    // {
    //     uint8_t msb = audio_buffer0[i] >> 8; // Get the most significant byte
    //     uint8_t lsb = audio_buffer0[i] & 0xFF; //Get the least significant byte
    //     ringbuf_put(&ring_buffer, msb);
    //     ringbuf_put(&ring_buffer, lsb);
    // }

    // Hardcoded file creation and save
    mp_obj_t file_args[2];
    file_args[0] = mp_obj_new_str("audio.raw", 14);  // filename
    file_args[1] = MP_OBJ_NEW_QSTR(MP_QSTR_wb);      // mode 'wb'

    mp_obj_t file = mp_vfs_open(2, file_args, (mp_map_t *)&mp_const_empty_map);

    const mp_stream_p_t *stream = mp_get_stream(file);

    // Write in chunks of 1024 bytes
    size_t total_bytes = FRAME_SIZE * NUM_CHANNELS;
    size_t chunk_size = 1024;
    // uint8_t *data_ptr = (uint8_t *)audio_buffer0;
    uint8_t *data_ptr = ring_buffer.buffer;

    int errcode = 0;
    for (size_t i = 0; i < total_bytes && errcode == 0; i += chunk_size) {
        size_t bytes_to_write = (i + chunk_size > total_bytes) ? (total_bytes - i) : chunk_size;
        stream->write(file, data_ptr + i, bytes_to_write, &errcode);
    }

    // int errcode = 0;
    // for (size_t i = 0; i < total_bytes && errcode == 0; i ++) {
    //     // if (ringbuf_avail(&ring_buffer) == 0) {
    //     //     // No more data in ring buffer, break out of loop
    //     //     break;
    //     // }
    //     uint8_t data = ringbuf_get(&ring_buffer);
    //     stream->write(file, &data, 1, &errcode);
    // }

    // Close file
    mp_stream_close(file);

    // return mp_const_none;
}

/*******************************************************************************
* Function Name: app_pdm_pcm_deactivate
********************************************************************************
* Summary: This function activates the left and right channel.
*
* Parameters:
*  none
*
* Return :
*  none
*
*******************************************************************************/
void app_pdm_pcm_deactivate(void) {
    Cy_PDM_PCM_DeActivate_Channel(PDM0, LEFT_CH_INDEX);
    Cy_PDM_PCM_DeActivate_Channel(PDM0, RIGHT_CH_INDEX);
}

/*******************************************************************************
 * Function Name: convert_db_to_pdm_scale
 ********************************************************************************
 * Summary:
 * Converts dB to PDM scale (fixed scale from 0 to 31)
 * Refer
 *
 * Parameters:
 *  gain  : gain in dB
 * Return:
 *  Scale value
 *
 *******************************************************************************/

cy_en_pdm_pcm_gain_sel_t convert_db_to_pdm_scale(double db) {
    if (db <= PDM_PCM_MIN_GAIN) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_103DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_103DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_97DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_97DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_97DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_91DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_91DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_91DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_85DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_85DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_85DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_79DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_79DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_79DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_73DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_73DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_73DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_67DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_67DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_67DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_61DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_61DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_61DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_55DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_55DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_55DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_49DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_49DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_49DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_43DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_43DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_43DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_37DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_37DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_37DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_31DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_31DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_31DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_25DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_25DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_25DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_19DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_19DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_19DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_13DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_13DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_13DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_7DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_7DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_7DB && db <= PDM_PCM_SEL_GAIN_NEGATIVE_1DB) {
        return CY_PDM_PCM_SEL_GAIN_NEGATIVE_1DB;
    } else if (db > PDM_PCM_SEL_GAIN_NEGATIVE_1DB && db <= PDM_PCM_SEL_GAIN_5DB) {
        return CY_PDM_PCM_SEL_GAIN_5DB;
    } else if (db > PDM_PCM_SEL_GAIN_5DB && db <= PDM_PCM_SEL_GAIN_11DB) {
        return CY_PDM_PCM_SEL_GAIN_11DB;
    } else if (db > PDM_PCM_SEL_GAIN_11DB && db <= PDM_PCM_SEL_GAIN_17DB) {
        return CY_PDM_PCM_SEL_GAIN_17DB;
    } else if (db > PDM_PCM_SEL_GAIN_17DB && db <= PDM_PCM_SEL_GAIN_23DB) {
        return CY_PDM_PCM_SEL_GAIN_23DB;
    } else if (db > PDM_PCM_SEL_GAIN_23DB && db <= PDM_PCM_SEL_GAIN_29DB) {
        return CY_PDM_PCM_SEL_GAIN_29DB;
    } else if (db > PDM_PCM_SEL_GAIN_29DB && db <= PDM_PCM_SEL_GAIN_35DB) {
        return CY_PDM_PCM_SEL_GAIN_35DB;
    } else if (db > PDM_PCM_SEL_GAIN_35DB && db <= PDM_PCM_SEL_GAIN_41DB) {
        return CY_PDM_PCM_SEL_GAIN_41DB;
    } else if (db > PDM_PCM_SEL_GAIN_41DB && db <= PDM_PCM_SEL_GAIN_47DB) {
        return CY_PDM_PCM_SEL_GAIN_47DB;
    } else if (db > PDM_PCM_SEL_GAIN_47DB && db <= PDM_PCM_SEL_GAIN_53DB) {
        return CY_PDM_PCM_SEL_GAIN_53DB;
    } else if (db > PDM_PCM_SEL_GAIN_53DB && db <= PDM_PCM_SEL_GAIN_59DB) {
        return CY_PDM_PCM_SEL_GAIN_59DB;
    } else if (db > PDM_PCM_SEL_GAIN_59DB && db <= PDM_PCM_SEL_GAIN_65DB) {
        return CY_PDM_PCM_SEL_GAIN_65DB;
    } else if (db > PDM_PCM_SEL_GAIN_65DB && db <= PDM_PCM_SEL_GAIN_71DB) {
        return CY_PDM_PCM_SEL_GAIN_71DB;
    } else if (db > PDM_PCM_SEL_GAIN_71DB && db <= PDM_PCM_SEL_GAIN_77DB) {
        return CY_PDM_PCM_SEL_GAIN_77DB;
    } else if (db > PDM_PCM_SEL_GAIN_77DB && db <= PDM_PCM_SEL_GAIN_83DB) {
        return CY_PDM_PCM_SEL_GAIN_83DB;
    } else if (db > PDM_PCM_MAX_GAIN) {
        return CY_PDM_PCM_SEL_GAIN_83DB;
    } else {
        return (cy_en_pdm_pcm_gain_sel_t)PDM_MIC_GAIN_VALUE;
    }

}
/*******************************************************************************
 * Function Name: set_pdm_pcm_gain
 ********************************************************************************
 *
 * Set PDM scale value for gain.
 *
 *******************************************************************************/
void set_pdm_pcm_gain(cy_en_pdm_pcm_gain_sel_t gain) {

    Cy_PDM_PCM_SetGain(PDM0, RIGHT_CH_INDEX, gain);
    Cy_PDM_PCM_SetGain(PDM0, LEFT_CH_INDEX, gain);

}

void read_from_channel(uint8_t channel) {
    int32_t data = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, channel);
    ringbuf_push(&ring_buffer, (uint8_t)(data & 0xFF));
    ringbuf_push(&ring_buffer, (uint8_t)(data >> 8));
}

/*******************************************************************************
* Function Name: pdm_interrupt_handler
********************************************************************************
* Summary:
*  PDM Overflow interrupt handler. Sets up the descriptor for junk buffer to consume
*  data from the PDM fifo
*
*******************************************************************************/
void pdm_interrupt_handler(void) {
    frame_counter++;
    volatile uint32_t intr_status = Cy_PDM_PCM_Channel_GetInterruptStatusMasked(PDM0, RIGHT_CH_INDEX);
    if (CY_PDM_PCM_INTR_RX_TRIGGER & intr_status) {
        for (uint32_t index = 0; index < RX_FIFO_TRIG_LEVEL;)
        {
            read_from_channel(LEFT_CH_INDEX);
            read_from_channel(RIGHT_CH_INDEX);
            index += 4; // 4 bytes read (2 bytes per channel)

            // int32_t left_data = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, LEFT_CH_INDEX);
            // int32_t right_data = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, RIGHT_CH_INDEX);

            // audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(left_data & 0xFF);
            // ringbuf_push(&ring_buffer, (uint8_t)(left_data & 0xFF));
            // index++;
            // audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(left_data >> 8);
            // audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(right_data & 0xFF);
            // audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(right_data >> 8);
            // ringbuf_push(&ring_buffer, (uint8_t)(left_data >> 8));
            // index++;
            // ringbuf_put(&ring_buffer, (uint8_t)(left_data >> 8));
            // ringbuf_push(&ring_buffer, (uint8_t)(right_data & 0xFF));
            // index++;
            // ringbuf_push(&ring_buffer, (uint8_t)(right_data >> 8));
            // index++;
            // ringbuf_put(&ring_buffer, (uint8_t)(right_data & 0xFF));
            // ringbuf_put(&ring_buffer, (uint8_t)(right_data >> 8));
        }

        // for(uint32_t index=0; index < RX_FIFO_TRIG_LEVEL;)
        // {
        //     int32_t data = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, LEFT_CH_INDEX);
        //     audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(data & 0xFF);
        //     audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(data >> 8);
        //     // ringbuf_put(&ring_buffer, (uint8_t)(data & 0xFF));
        //     // ringbuf_put(&ring_buffer, (uint8_t)(data >> 8));

        //     data = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, RIGHT_CH_INDEX);
        //     audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(data & 0xFF);
        //     audio_buffer0[frame_counter * RX_FIFO_TRIG_LEVEL + index++] = (uint8_t)(data >> 8);
        //     // ringbuf_put(&ring_buffer, (uint8_t)(data & 0xFF));
        //     // ringbuf_put(&ring_buffer, (uint8_t)(data >> 8));
        // }

        Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, RIGHT_CH_INDEX, CY_PDM_PCM_INTR_RX_TRIGGER);
    }
    if ((NUMBER_INTERRUPTS_FOR_FRAME - 1) == frame_counter) {
        pdm_pcm_flag = true;
        frame_counter = 0;

        app_pdm_pcm_deactivate();
        NVIC_ClearPendingIRQ(PDM_IRQ_cfg.intrSrc);
        NVIC_DisableIRQ(PDM_IRQ_cfg.intrSrc);

        // if(ringbuf_avail(&ring_buffer) < rxbuf_len)
        // {
        // for(uint32_t i=0; i < rxbuf_len; i++)

        // {
        //     ringbuf_put(&ring_buffer, audio_buffer0[i]);
        // }
        // }
    }
    if ((CY_PDM_PCM_INTR_RX_FIR_OVERFLOW | CY_PDM_PCM_INTR_RX_OVERFLOW | CY_PDM_PCM_INTR_RX_IF_OVERFLOW |
         CY_PDM_PCM_INTR_RX_UNDERFLOW) & intr_status) {
        Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, RIGHT_CH_INDEX, CY_PDM_PCM_INTR_MASK);
    }
}

// ------------------------------------------------------------------------------------------------

#include "py/stream.h"
#include "py/runtime.h"

typedef struct _machine_pdm_pcm_obj_t {
    mp_obj_base_t base;
} machine_pdm_pcm_obj_t;

machine_pdm_pcm_obj_t *pdm_pcm_obj;

static void machine_pdm_pcm_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    /* TODO: Add implementation*/
}

// PDM_PCM(...) Constructor
static mp_obj_t machine_pdm_pcm_make_new(const mp_obj_type_t *type, size_t n_pos_args, size_t n_kw_args, const mp_obj_t *args) {
    /* TODO: Add implementation*/
    machine_pdm_pcm_obj_t *self = NULL;

    self = mp_obj_malloc(machine_pdm_pcm_obj_t, &machine_pdm_pcm_type);
    pdm_pcm_obj = self;

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_pdm_pcm_init(mp_obj_t self_in) {
    /* TODO: Add implementation*/

    app_pdm_pcm_init();
    app_pdm_pcm_activate();
    save_audio_to_file();

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pdm_pcm_init_obj, machine_pdm_pcm_init);

static mp_obj_t machine_pdm_pcm_is_ready(mp_obj_t self_in) {
    /* TODO: Add implementation*/

    if (pdm_pcm_flag) {
        return mp_const_true;
    } else {
        return mp_const_false;
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pdm_pcm_is_ready_obj, machine_pdm_pcm_is_ready);

static mp_uint_t machine_pdm_pcm_stream_read(mp_obj_t self_in, void *buf_in, mp_uint_t size, int *errcode) {
    /* TODO: Add implementation*/

    mp_buffer_info_t appbuf;
    appbuf.buf = (void *)buf_in;

    // for (size_t i = 0; i < size; i++) {
    //     ((uint8_t *)appbuf.buf)[i] = (uint8_t)(i & 0xFF); // Fill the buffer with dummy data for testing
    // }

    static bool first_read = true;
    if (first_read) {
        while (!pdm_pcm_flag) {
            // Wait until the PDM PCM data is ready
        }
        mp_printf(&mp_plat_print, ".");
        first_read = false;
    }

    static uint32_t bytes_left_to_copy = FRAME_SIZE * NUM_CHANNELS;
    static uint32_t copy_index = 0;
    if (size > bytes_left_to_copy) {
        size = bytes_left_to_copy; // Adjust size to copy only the remaining data
    }
    appbuf.len = size;

    // if (pdm_pcm_flag) {
    for (size_t i = 0; i < size; i++) {
        ((uint8_t *)appbuf.buf)[i] = audio_buffer0[copy_index++]; // Fill the buffer with recorded data
    }
    // }
    bytes_left_to_copy -= size;

    return size;
}

static const mp_stream_p_t pdm_pcm_stream_p = {
    .read = machine_pdm_pcm_stream_read,
    .is_text = false,
};

static const mp_rom_map_elem_t machine_pdm_pcm_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_init),            MP_ROM_PTR(&machine_pdm_pcm_init_obj) },

    { MP_ROM_QSTR(MP_QSTR_isready),         MP_ROM_PTR(&machine_pdm_pcm_is_ready_obj) },

    { MP_ROM_QSTR(MP_QSTR_readinto),        MP_ROM_PTR(&mp_stream_readinto_obj) },
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
