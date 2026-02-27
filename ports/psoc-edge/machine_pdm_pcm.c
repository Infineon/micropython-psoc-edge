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

ringbuf_t ring_buffer;

typedef struct _machine_pdm_pcm_obj_t {
    mp_obj_base_t base;
    uint8_t id;     // Private variable in this port. ID not associated to any port pin pdm-pcm group.
    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t data;
    io_mode_t io_mode;
    format_t format;
    uint8_t bits;
    uint32_t sample_rate;
    uint8_t decimation_rate;
    int16_t left_gain;
    int16_t right_gain;
    int32_t ibuf;   // Private variable
} machine_pdm_pcm_obj_t;

static void mp_machine_pdm_pcm_init_helper(machine_pdm_pcm_obj_t *self, mp_arg_val_t *args) {
    /* TODO: Implement  */
}


/*******************************************************************************
* Header Files
*******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include <stdlib.h>
#include "retarget_io_init.h"


#include "extmod/vfs.h"
#include "py/stream.h"


#define PDM_PCM_RX_FRAME_SIZE_IN_BYTES     (8)

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
cy_en_pdm_pcm_gain_sel_t convert_db_to_pdm_scale(float db);
void set_pdm_pcm_gain(cy_en_pdm_pcm_gain_sel_t gain);


typedef struct _non_blocking_descriptor_t {
    mp_buffer_info_t appbuf;
    uint32_t index;
    bool copy_in_progress;
} non_blocking_descriptor_t;


/* Array containing the recorded data */
uint32_t rxbuf_len = 20000;
ringbuf_t ring_buffer;

/* PDM PCM interrupt configuration parameters */
const cy_stc_sysint_t PDM_IRQ_cfg =
{
    .intrSrc = (IRQn_Type)CYBSP_PDM_CHANNEL_3_IRQ,
    .intrPriority = PDM_PCM_ISR_PRIORITY
};

/* PDM PCM flag indicating PCM data is available to process */
volatile bool pdm_pcm_flag = false;

#include "mphalport.h"

extern const machine_pin_obj_t pin_P10_7_obj;
static void pdm_pcm_rx_fifo_buffer_init(void);

void app_pdm_pcm_init(void) {

    // ringbuf_init(&ring_buffer, rxbuf_len);
    ringbuf_alloc(&ring_buffer, rxbuf_len);

    cy_en_pdm_pcm_gain_sel_t gain_scale = CY_PDM_PCM_SEL_GAIN_NEGATIVE_37DB;

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

    mp_hal_pin_output(&pin_P10_7_obj);
    mp_hal_pin_write(&pin_P10_7_obj, 0); // Set pin high to indicate ready state

    pdm_pcm_rx_fifo_buffer_init();
}

void app_pdm_pcm_activate(void) {
    /* Activate recording from channel after init */
    Cy_PDM_PCM_Activate_Channel(PDM0, LEFT_CH_INDEX);
    Cy_PDM_PCM_Activate_Channel(PDM0, RIGHT_CH_INDEX);
}

static bool audio_file_saved = false;
void save_audio_to_file() {

    while (ringbuf_free(&ring_buffer) > 8) {
    }
    mp_hal_pin_write(&pin_P10_7_obj, 1);

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
    uint8_t *data_ptr = ring_buffer.buf;

    int errcode = 0;
    for (size_t i = 0; i < total_bytes && errcode == 0; i += chunk_size) {
        size_t bytes_to_write = (i + chunk_size > total_bytes) ? (total_bytes - i) : chunk_size;
        stream->write(file, data_ptr + i, bytes_to_write, &errcode);
    }

    // Close file
    mp_stream_close(file);

    // return mp_const_none;
    audio_file_saved = true;
}

void app_pdm_pcm_deactivate(void) {
    Cy_PDM_PCM_DeActivate_Channel(PDM0, LEFT_CH_INDEX);
    Cy_PDM_PCM_DeActivate_Channel(PDM0, RIGHT_CH_INDEX);
}

cy_en_pdm_pcm_gain_sel_t convert_db_to_pdm_scale(float db) {
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

void set_pdm_pcm_gain(cy_en_pdm_pcm_gain_sel_t gain) {
    Cy_PDM_PCM_SetGain(PDM0, RIGHT_CH_INDEX, gain);
    Cy_PDM_PCM_SetGain(PDM0, LEFT_CH_INDEX, gain);
}

static const int8_t pdm_pcm_frame_map[4][PDM_PCM_RX_FRAME_SIZE_IN_BYTES] = {
    { 0,  1, -1, -1, -1, -1, -1, -1 },  // Mono, 16-bits
    { 0,  1,  2,  3, -1, -1, -1, -1 },  // Mono, 32-bits
    { 0,  1, -1, -1,  2,  3, -1, -1 },  // Stereo, 16-bits
    { 0,  1,  2,  3,  4,  5,  6,  7 },  // Stereo, 32-bits
};

int8_t get_frame_mapping_index(int8_t bits, format_t format) {
    if ((format == MONO_LEFT) | (format == MONO_RIGHT)) {
        if (bits == 16) {
            return 0;
        }
    } else { // STEREO
        if (bits == 16) {
            return 2;
        }
    }
    return -1;
}


/**
 * Number of FIFO frames that will be stored
 * in each half of the ping-pong buffer.
 */
#define NUM_OF_RX_HW_FIFO_FRAMES_IN_RX_BUFFER         16
#define NUM_OF_RX_HW_FIFO_FRAMES_IN_HALF_RX_BUFFER    (NUM_OF_RX_HW_FIFO_FRAMES_IN_RX_BUFFER / 2)

/**
 * The PDM PCM peripheral has hardware FIFO capable
 * of up to 64 samples.
 * The IRQ trigger level is set to half of the FIFO size.
 * That way we can process the samples when the FIFO is
 * half full, ensuring that we read the samples before the
 * FIFO is full and we lose data.
 * Each half size FIFO read will compose a frame that will
 * be added to the ping-pong buffer.
 * As each sample is considered to be 8 bytes,
 * a frame match the actual size of the hardware FIFO for
 * a mono channel, or 2 halves FIFO for stereo.
 * In case of
 */
#define MAX_SAMPLES_RX_HW_FIFO                 64
#define NUM_OF_SAMPLES_IN_RX_HW_FIFO_FRAME     32
#define NUM_OF_WORDS_IN_RX_HW_FIFO_FRAME       (NUM_OF_SAMPLES_IN_RX_HW_FIFO_FRAME * 2)
#define RX_HW_FIFO_IRQ_TRIGGER_LEVEL           (NUM_OF_SAMPLES_IN_RX_HW_FIFO_FRAME)

/**
 * A ping-pong buffer implementation will store the samples read
 * from the PDM PCM hardware FIFO.
 *
 * Each sample will contain up to 8 bytes of data, being able
 * to store up to 32-bit stereo samples.
 * As the buffer is a uint32_t array, each sample will take
 * 2 positions in the buffer.
 * In case of mono audio, the odd positions will be empty.
 *
 * The buffer is be split into two halves.
 * The active half will be the one used in the IRQ handler to
 * store the incoming samples from the FIFO.
 * The processing half will be used to copy the samples
 * into the ring buffer.
 *
 * The ring buffer will be used to transfer the data stream
 * into the application provided buffer.
 */
#define SIZEOF_RX_BUFFER_IN_SAMPLES            (NUM_OF_RX_HW_FIFO_FRAMES_IN_RX_BUFFER * NUM_OF_SAMPLES_IN_RX_HW_FIFO_FRAME)
#define SIZEOF_RX_BUFFER_IN_WORDS              (SIZEOF_RX_BUFFER_IN_SAMPLES * 2)
#define SIZEOF_HALF_RX_BUFFER_IN_SAMPLES       (SIZEOF_RX_BUFFER_IN_SAMPLES / 2)
#define SIZEOF_HALF_RX_BUFFER_IN_WORDS         (SIZEOF_HALF_RX_BUFFER_IN_SAMPLES * 2)

#define SIZEOF_PDM_PCM_SAMPLE_IN_BYTES          8

static uint32_t rx_fifo_buffer[SIZEOF_RX_BUFFER_IN_WORDS];
uint32_t *active_half_rx_fifo_buf_ptr;
uint32_t *processing_half_rx_fifo_buf_ptr;

uint8_t rx_hw_fifo_frame_count;

pdm_pcm_word_length_t bits = BITS_16;
format_t format = STEREO;
io_mode_t io_mode = BLOCKING;

static void pdm_pcm_rx_fifo_buffer_init(void) {
    memset(rx_fifo_buffer, 0, sizeof(rx_fifo_buffer));
    active_half_rx_fifo_buf_ptr = rx_fifo_buffer;
    processing_half_rx_fifo_buf_ptr = &rx_fifo_buffer[SIZEOF_HALF_RX_BUFFER_IN_WORDS];
    rx_hw_fifo_frame_count = 0;
}

static void _pdm_pcm_read_half_rx_fifo(uint32_t *half_rx_fifo_buf_ptr) {
    for (uint32_t index = 0; index < RX_HW_FIFO_IRQ_TRIGGER_LEVEL; index++)
    {
        uint32_t data = (uint32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, LEFT_CH_INDEX);
        /**
         * The mono/left channel uses the even indexes.
         */
        half_rx_fifo_buf_ptr[index * 2] = data;
        if (format == STEREO) {
            data = Cy_PDM_PCM_Channel_ReadFifo(PDM0, RIGHT_CH_INDEX);
            /**
             * The stereo/right channel uses the odd indexes.
             */
            half_rx_fifo_buf_ptr[index * 2 + 1] = data;
        }
    }
}


static inline bool pdm_pcm_is_half_rx_buffer_full(void) {
    return rx_hw_fifo_frame_count == 0;
}

static void _pdm_pcm_update_rx_hw_fifo_frame_counter(void) {
    rx_hw_fifo_frame_count++;
    if (rx_hw_fifo_frame_count == NUM_OF_RX_HW_FIFO_FRAMES_IN_HALF_RX_BUFFER) {
        rx_hw_fifo_frame_count = 0;
    }
}

static void _pdm_pcm_swap_half_rx_buffer(void) {
    if (pdm_pcm_is_half_rx_buffer_full()) {
        uint32_t *temp = active_half_rx_fifo_buf_ptr;
        active_half_rx_fifo_buf_ptr = processing_half_rx_fifo_buf_ptr;
        processing_half_rx_fifo_buf_ptr = temp;
    }
}

static inline uint32_t *_pdm_pcm_get_next_rx_buffer_segment_to_fill(void) {
    return &active_half_rx_fifo_buf_ptr[NUM_OF_WORDS_IN_RX_HW_FIFO_FRAME * rx_hw_fifo_frame_count];
}

static void pdm_pcm_read_rx_buffer(void) {
    uint32_t *rx_fifo_buffer_segment_ptr = _pdm_pcm_get_next_rx_buffer_segment_to_fill();
    _pdm_pcm_read_half_rx_fifo(rx_fifo_buffer_segment_ptr);
    _pdm_pcm_update_rx_hw_fifo_frame_counter();
    _pdm_pcm_swap_half_rx_buffer();
}

static void pdm_pcm_copy_half_rx_buffer_to_ringbuf() {
    uint8_t sample_size_in_bytes = (bits == BITS_16 ? 2 : 4) * (format == STEREO ? 2: 1);
    uint8_t *rx_buf_sample_ptr = (uint8_t *)processing_half_rx_fifo_buf_ptr;
    uint32_t num_bytes_needed_from_ringbuf = NUM_OF_SAMPLES_IN_RX_HW_FIFO_FRAME * sample_size_in_bytes;

    // when space exists, copy samples into ring buffer
    if (ringbuf_free(&ring_buffer) >= num_bytes_needed_from_ringbuf) {
        uint8_t f_index = get_frame_mapping_index(bits, format);
        uint32_t i = 0;
        while (i < (SIZEOF_HALF_RX_BUFFER_IN_SAMPLES * SIZEOF_PDM_PCM_SAMPLE_IN_BYTES)) {
            for (uint8_t j = 0; j < SIZEOF_PDM_PCM_SAMPLE_IN_BYTES; j++) {
                int8_t r_to_a_mapping = pdm_pcm_frame_map[f_index][j];
                if (r_to_a_mapping != -1) {
                    ringbuf_put(&ring_buffer, rx_buf_sample_ptr[i]);

                }
                i++;
                // For now we are not going to add the -1 value to the ring buffer,
                // we add this later
                /*
                } else { // r_a_mapping == -1
                    ringbuf_put(&ring_buffer, -1);
                }
                */
            }
        }
    }
}

void pdm_disable() {
    // Toggle the LED0 to indicate that it is full
    mp_hal_pin_write(&pin_P10_7_obj, 1);

    pdm_pcm_flag = true;

    app_pdm_pcm_deactivate();
    NVIC_ClearPendingIRQ(PDM_IRQ_cfg.intrSrc);
    NVIC_DisableIRQ(PDM_IRQ_cfg.intrSrc);
}

void ready_to_save() {
    mp_hal_pin_write(&pin_P10_7_obj, 1);
    pdm_pcm_flag = true;
}

void pdm_interrupt_handler(void) {
    volatile uint32_t intr_status = Cy_PDM_PCM_Channel_GetInterruptStatusMasked(PDM0, RIGHT_CH_INDEX);
    if (CY_PDM_PCM_INTR_RX_TRIGGER & intr_status) {
        pdm_pcm_read_rx_buffer();
        Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, RIGHT_CH_INDEX, CY_PDM_PCM_INTR_RX_TRIGGER);
        if (pdm_pcm_is_half_rx_buffer_full()) {
            pdm_pcm_copy_half_rx_buffer_to_ringbuf();
        }
    }
    if ((CY_PDM_PCM_INTR_RX_FIR_OVERFLOW | CY_PDM_PCM_INTR_RX_OVERFLOW | CY_PDM_PCM_INTR_RX_IF_OVERFLOW |
         CY_PDM_PCM_INTR_RX_UNDERFLOW) & intr_status) {
        Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, RIGHT_CH_INDEX, CY_PDM_PCM_INTR_MASK);
    }
}

// ------------------------------------------------------------------------------------------------

#include "py/stream.h"
#include "py/runtime.h"

machine_pdm_pcm_obj_t *pdm_pcm_obj;

static mp_obj_t machine_pdm_pcm_init(mp_obj_t self_in) {
    /* TODO: Add implementation*/

    app_pdm_pcm_init();
    app_pdm_pcm_activate();
    // save_audio_to_file();

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pdm_pcm_init_obj, machine_pdm_pcm_init);

static mp_obj_t machine_pdm_pcm_is_ready(mp_obj_t self_in) {
    /* TODO: Add implementation*/

    if (audio_file_saved) {
        return mp_const_true;
    } else {
        return mp_const_false;
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pdm_pcm_is_ready_obj, machine_pdm_pcm_is_ready);
