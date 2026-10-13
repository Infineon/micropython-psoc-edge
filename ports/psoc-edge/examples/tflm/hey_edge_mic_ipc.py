import gc
import os
import struct
import time
from machine import PDM_PCM, IPC, ExternalFlash, Pin

# ---------------------------------------------------------------------------
# "hey edge" keyword spotting from the live mic, over IPC (CM33 -> CM55).
#
# Pipeline:
#   PDM mic (CM33)  --  ~2 s mono int16 @16 kHz  -->  [H2T ring]  -->  CM55
#   CM55 (framework/tflm): MFE DSP -> int8 quantize -> TFLM invoke
#   CM55  --  raw output tensor bytes  -->  [T2H ring]  -->  CM33 (this script)
#   CM33 (this script): dequantize the int8 logits -> threshold -> decision.
#
# The CM55 firmware is the generic TFLM engine (cm55_firmware/engine/tflm): it
# applies the model's input preprocessing and returns the RAW output tensor --
# all output decoding (dequantize + threshold) is application logic and lives
# here on CM33. CM55 is a passive server driven entirely by the command
# sequence below:
#   MODEL_LOAD -> MODEL_INFO -> MODEL_RUN -> (stage PCM) -> RUN_INFERENCE.
#
# MODEL_INFO is queried once after load and returns the input and output tensor
# metadata (type/shape/scale/zero_point); this script uses the output tensor's
# scale + zero_point to dequantize each inference result generically.
#
# Transport (machine.IPC, see ports/psoc-edge/machine_ipc.c):
#   ipc.send(cmd, int_value, client)  -> control message (cmd + 32-bit value)
#   ipc.send(cmd, buffer, client)     -> bulk data into H2T ring + DATA_AVAIL
#   client.cmd / client.value         -> last control event from CM55
#   client.read()                     -> drain result bytes from T2H ring
#
# The model sits at a fixed address in the shared-data flash partition, which
# the CM55 firmware reads directly (address hardcoded in framework/tflm/main.c).
# No model bytes cross IPC and no offset is sent -- only the live mic PCM.
#   mpremote run hey_edge_mic_ipc.py
# ---------------------------------------------------------------------------

# --- Model / audio geometry (Edge Impulse project 1128757, "hey_edge") -----
# The model is written to external flash here (same pattern as sine_model_ipc.py)
# and is NOT compiled into the firmware; CM55 loads it from its hardcoded flash
# address at runtime.
MODEL_FILE = "keyword_spotting_demo.tflite"
MODEL_OFFSET = 0x00000000  # start of the shared-data flash partition
CHUNK_SIZE = 1024  # stream the model to flash in small chunks

RAW_SAMPLE_COUNT = 31984  # mono int16 samples the model expects (~2 s)
SAMPLE_RATE = 16000
BITS = 16
PCM_BYTES = RAW_SAMPLE_COUNT * 2  # 63968 bytes mono total, streamed to CM55

# --- Microphone (same pins as the audio readout demos) ---------------------
SCK_PIN = "P8_5"
DATA_PIN = "P8_6"
READ_BYTES = 2048  # mono 16-bit => 1024 frames per readinto
MIC_WARMUP_BYTES = SAMPLE_RATE * 2  # ~1 s; PDM output has a DC/noise transient after init

# --- Detection indicator: USER_LED_B (active-high) -------------------------
LED_PIN = "P20_5"
LED_BLINKS = 3
LED_BLINK_MS = 80

# --- TFLM engine wire protocol (cm55_firmware/engine/tflm/tflm_engine.h) ----
TFLM_CMD_MODEL_LOAD = 0x90  # CM55 hardcodes the model address; value unused
TFLM_CMD_MODEL_RUN = 0x92
TFLM_CMD_RUN_INFERENCE = 0x94
TFLM_CMD_MODEL_INFO = 0x95  # reply: input then output tensor info, 64 bytes

TFLM_EVT_READY = 0xB0
TFLM_EVT_MODEL_LOADED = 0xB1
TFLM_EVT_MODEL_RUNNING = 0xB3
TFLM_EVT_ERROR = 0xE0

# tflm_adapter_tensor_info_t (cm55_firmware/adapters/tflm/adapter.h), 32 bytes:
#   uint8 type; uint8 rank; uint8 reserved[2]; int32 dims[4];
#   uint32 bytes; float scale; int32 zero_point;
TENSOR_INFO_FMT = "<BBxx4iIfi"
TENSOR_INFO_SIZE = struct.calcsize(TENSOR_INFO_FMT)  # 32
TFLITE_TYPE_INT8 = 9

# Application-side KWS decision: dequantized p(hey_edge) threshold. The output
# tensor is int8 softmax over {hey_edge, noise}; element 0 is "hey_edge".
KWS_THRESHOLD = 0.6

# --- IPC client wiring (matches sine_model_ipc.py / ipc-audio) -------------
CM33_CLIENT_ID = 3  # our endpoint (CM55 doorbells replies here)
CM55_CLIENT_ID = 5  # target client for commands + staged PCM
CM33_EP_ID = 1
CM33_EP_ADDR = 1

EVT_TIMEOUT_MS = 3000
RESULT_TIMEOUT_MS = 3000

RUN_SECONDS = 20  # listening duration before the summary
EXPECTED_DETECTIONS = 5  # times "hey edge" is spoken during the run

SUMMARY_FMT = """========== Runtime Summary ==========

Model                    : %s
Model Size               : %.1f KB
Load Time                : %d ms
Inference Latency        : %s
Transport                : IPC
Execution Target         : CM55
Duration                 : %d seconds
Detection Count          : %d
Unsuccessful Detection   : %d
Successful Detection     : %d %%
"""

# ---------------------------------------------------------------------------
# IPC receive callback: CM55 sends both control events (cmd + value) and bulk
# replies (DATA_AVAIL doorbell) -- MODEL_INFO's 64-byte tensor info and each
# inference's raw output tensor. The read buffer is reused, so copy it out
# immediately; the caller sequences which reply it is waiting for.
# ---------------------------------------------------------------------------
state = {"evt": None, "evt_val": 0, "data": None}


def on_ipc(client):
    cmd = client.cmd
    if cmd == IPC.CMD_DATA_AVAIL:
        mv = client.read()
        if len(mv):
            state["data"] = bytes(mv)  # raw reply: tensor info or output tensor
    else:
        state["evt"] = cmd
        state["evt_val"] = client.value


def wait_data(timeout_ms):
    waited = 0
    while waited < timeout_ms:
        data = state["data"]
        if data is not None:
            return data
        if state["evt"] == TFLM_EVT_ERROR:
            state["evt"] = None
            print("  CM55 reported ERROR (code=%d)" % state["evt_val"])
            return None
        time.sleep_ms(5)
        waited += 5
    print("  timeout waiting for CM55 data")
    return None


def parse_tensor_info(buf):
    ttype, rank, d0, d1, d2, d3, nbytes, scale, zero_point = struct.unpack(TENSOR_INFO_FMT, buf)
    return {
        "type": ttype,
        "rank": rank,
        "dims": (d0, d1, d2, d3)[:rank],
        "bytes": nbytes,
        "scale": scale,
        "zero_point": zero_point,
    }


def dequantize_output(buf, info):
    # Generic int8 dequantize: p = (q - zero_point) * scale.
    scale = info["scale"]
    zp = info["zero_point"]
    probs = []
    for b in buf:
        q = b - 256 if b >= 128 else b  # reinterpret byte as int8
        probs.append((q - zp) * scale)
    return probs


def wait_event(expected, timeout_ms):
    waited = 0
    while waited < timeout_ms:
        evt = state["evt"]
        if evt is not None:
            state["evt"] = None
            if evt == expected:
                return True
            if evt == TFLM_EVT_ERROR:
                print("  CM55 reported ERROR (code=%d)" % state["evt_val"])
                return False
            # Unrelated event (e.g. a late READY); keep waiting.
        time.sleep_ms(5)
        waited += 5
    return False


def blink_led(led):
    for _ in range(LED_BLINKS):
        led.value(1)
        time.sleep_ms(LED_BLINK_MS)
        led.value(0)
        time.sleep_ms(LED_BLINK_MS)


def capture_and_stream(pdm, ipc, chunk):
    # Stream exactly PCM_BYTES of live mono int16 to CM55 in small chunks. The
    # engine accumulates every bulk send into its input buffer until
    # RUN_INFERENCE, so CM33 never has to hold the whole ~2 s window (the
    # device heap is too small for a 63072-byte buffer).
    mv = memoryview(chunk)
    sent = 0
    while sent < PCM_BYTES:
        n = pdm.readinto(chunk)
        if n <= 0:
            continue
        if sent + n > PCM_BYTES:
            n = PCM_BYTES - sent
        ipc.send(IPC.CMD_DATA_AVAIL, mv[:n], CM55_CLIENT_ID)
        sent += n
    return sent


# ---------------------------------------------------------------------------
# Provision the model into external shared flash, then run the live-mic loop.
# ---------------------------------------------------------------------------
print("########## hey_edge KWS over IPC ##########")

gc.collect()
model_size = os.stat(MODEL_FILE)[6]
print("Model name :", MODEL_FILE)
print("Model size :", model_size, "bytes")

# Open the model first: open() needs a ~16.5 KB contiguous heap block, which
# is no longer available once the IPC and flash drivers are up.
source = open(MODEL_FILE, "rb")

# machine.IPC.init() prints "IPC initialized successfully" itself.
print("Transport : ", end="")
ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()

# --- Write the model into external shared flash (same pattern as
# sine_model_ipc.py -- no separate flasher script). The model is NOT compiled
# into the firmware; CM55 loads it from flash (XIP) at runtime. Stream it from
# the file in small chunks -- the tiny device heap can't hold it all at once --
# then release the flash driver before the mic/IPC buffers are claimed. -------
print("Loading model . . .")
t0 = time.ticks_ms()
header = None
written = 0
flash = ExternalFlash()
flash.init()
sector_size = flash.sector_size()
flash.erase(MODEL_OFFSET, ((model_size + sector_size - 1) // sector_size) * sector_size)
while True:
    data = source.read(CHUNK_SIZE)
    if not data:
        break
    if header is None:
        header = bytes(data[:256])  # kept for the read-back check
    flash.write(MODEL_OFFSET + written, data)
    written += len(data)
    gc.collect()
source.close()
assert bytes(flash.read(MODEL_OFFSET, len(header))) == header, "Model verification failed"
load_ms = time.ticks_diff(time.ticks_ms(), t0)
print("Model loaded into external flash in %d ms" % load_ms)
del flash, source, data, header
gc.collect()

# Allocate the big persistent objects on the freshest heap. The device heap is
# small (~38 KB) and CM55 bring-up fragments it, so the mic buffer (~2 KB) and
# the PDM driver's internal buffer (~4 KB) must be claimed up front.
gc.collect()
rx_chunk = bytearray(READ_BYTES)
led = Pin(LED_PIN, Pin.OUT, value=0)

pdm = PDM_PCM(
    sck=SCK_PIN,
    data=DATA_PIN,
    sample_rate=SAMPLE_RATE,
    bits=BITS,
    format=PDM_PCM.MONO,
)
time.sleep_ms(100)  # let the mic settle

ipc.register_client(CM33_CLIENT_ID, on_ipc, CM33_EP_ID, CM33_EP_ADDR)

# --- Boot CM55 (model sits at its fixed address in the shared-data flash) ---
ipc.enable_core(IPC.CM55)

# READY is best-effort: the engine sends it once at boot. If we miss it (timing),
# proceed anyway -- MODEL_LOAD below will still confirm the engine is alive.
wait_event(TFLM_EVT_READY, EVT_TIMEOUT_MS)

# --- Load + run the model (CM55 reads it from its hardcoded flash address) ---
ipc.send(TFLM_CMD_MODEL_LOAD, 0, CM55_CLIENT_ID)
if not wait_event(TFLM_EVT_MODEL_LOADED, EVT_TIMEOUT_MS):
    raise SystemExit("MODEL_LOAD failed")

# --- Query tensor metadata once (input + output tflm_adapter_tensor_info_t) --
state["data"] = None
ipc.send(TFLM_CMD_MODEL_INFO, 0, CM55_CLIENT_ID)
info = wait_data(EVT_TIMEOUT_MS)
if info is None or len(info) < 2 * TENSOR_INFO_SIZE:
    raise SystemExit("MODEL_INFO failed")
out_info = parse_tensor_info(info[TENSOR_INFO_SIZE : 2 * TENSOR_INFO_SIZE])

ipc.send(TFLM_CMD_MODEL_RUN, 0, CM55_CLIENT_ID)
if not wait_event(TFLM_EVT_MODEL_RUNNING, EVT_TIMEOUT_MS):
    raise SystemExit("MODEL_RUN failed")

warm = 0
while warm < MIC_WARMUP_BYTES:
    warm += max(0, pdm.readinto(rx_chunk))
print("Model running. Say 'hey edge'.\n")

# ---------------------------------------------------------------------------
# Capture -> stage PCM -> trigger inference -> read the result, for RUN_SECONDS.
# ---------------------------------------------------------------------------
chunk = rx_chunk
detections = 0
inf_total_ms = 0
inf_count = 0
t_start = time.ticks_ms()
try:
    while time.ticks_diff(time.ticks_ms(), t_start) < RUN_SECONDS * 1000:
        print("[listening ~2 s] ...")
        capture_and_stream(pdm, ipc, chunk)  # stream live PCM to CM55

        time.sleep_ms(10)  # let CM55 drain the last chunk
        state["data"] = None
        state["evt"] = None
        t_inf = time.ticks_ms()
        ipc.send(TFLM_CMD_RUN_INFERENCE, 0, CM55_CLIENT_ID)  # run one inference

        out = wait_data(RESULT_TIMEOUT_MS)
        if out is not None:
            inf_ms = time.ticks_diff(time.ticks_ms(), t_inf)
            inf_total_ms += inf_ms
            inf_count += 1
            probs = dequantize_output(out, out_info)
            p_hey = probs[0]
            p_noise = probs[1] if len(probs) > 1 else 0.0
            detail = " [p(hey_edge)=%.2f p(noise)=%.2f]" % (p_hey, p_noise)
            if p_hey >= KWS_THRESHOLD:
                print("  >>> 'hey edge' DETECTED" + detail)
                # CM33-side round trip: IPC + MFE DSP + TFLM invoke on CM55.
                print("Inference time : %d ms" % inf_ms)
                detections += 1
                blink_led(led)
            else:
                print("  ... not detected" + detail)
        print()

        gc.collect()

    latency = "%d ms" % (inf_total_ms // inf_count) if inf_count else "n/a"
    # Inference Latency = average CM33 round trip.
    print(
        SUMMARY_FMT
        % (
            MODEL_FILE,
            model_size / 1000,
            load_ms,
            latency,
            RUN_SECONDS,
            detections,
            max(0, EXPECTED_DETECTIONS - detections),
            detections * 100 // EXPECTED_DETECTIONS,
        )
    )
finally:
    led.value(0)
    pdm.deinit()
