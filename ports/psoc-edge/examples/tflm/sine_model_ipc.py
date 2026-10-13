import os
import gc
import time
import struct
from machine import ExternalFlash
from machine import IPC

# ---------------------------------------------------------------------------
# TFLM sine demo over IPC, driven through the generic CM55 TFLM engine
# (cm55_firmware/engine/tflm):
#   MODEL_LOAD -> MODEL_INFO -> MODEL_RUN -> per x: (stage input) RUN_INFERENCE.
#
# The engine passes raw tensor bytes both ways, so this script quantizes x into
# the model's input tensor and dequantizes the raw output tensor into y, using
# the scale/zero_point returned by MODEL_INFO (works for int8 and float models).
# ---------------------------------------------------------------------------

SOURCE_PATH = "hello_world_int8.tflite"
CHUNK_SIZE = 4096
MODEL_OFFSET = 0x00000000

CM33_CLIENT_ID = 3  # our endpoint's client id (CM55 doorbells this on reply)
CM55_CLIENT_ID = 5  # target client for commands + staged input
CM33_EP_ID = 1
CM33_EP_ADDR = 1

# --- TFLM engine wire protocol (cm55_firmware/engine/tflm/tflm_engine.h) ----
TFLM_CMD_MODEL_LOAD = 0x90
TFLM_CMD_MODEL_RUN = 0x92
TFLM_CMD_RUN_INFERENCE = 0x94
TFLM_CMD_MODEL_INFO = 0x95

TFLM_EVT_READY = 0xB0
TFLM_EVT_MODEL_LOADED = 0xB1
TFLM_EVT_MODEL_RUNNING = 0xB3
TFLM_EVT_ERROR = 0xE0

# tflm_adapter_tensor_info_t (cm55_firmware/adapters/tflm/adapter.h), 32 bytes.
TENSOR_INFO_FMT = "<BBxx4iIfi"
TENSOR_INFO_SIZE = struct.calcsize(TENSOR_INFO_FMT)
TFLITE_TYPE_FLOAT32 = 1
TFLITE_TYPE_INT8 = 9

EVT_TIMEOUT_MS = 3000
REPLY_TIMEOUT_MS = 2000

# --- Write model to external flash (CM55 tflm_init reads it from XIP) --------
flash = ExternalFlash()
flash.init()
sector_size = flash.sector_size()
file_size = os.stat(SOURCE_PATH)[6]
reserved_size = ((file_size + sector_size - 1) // sector_size) * sector_size
flash.erase(MODEL_OFFSET, reserved_size)

# Read the (small) model once and reuse it for write + verify -- the device
# heap is tiny, so a second open()/read() can fail with MemoryError.
gc.collect()
with open(SOURCE_PATH, "rb") as source:
    model = source.read()

# memoryview slices avoid copying the model on the tiny, fragmented heap.
model_mv = memoryview(model)
written = 0
while written < file_size:
    chunk = model_mv[written : written + CHUNK_SIZE]
    flash.write(MODEL_OFFSET + written, chunk)
    written += len(chunk)
    gc.collect()

verify = flash.read(MODEL_OFFSET, min(256, file_size))
assert verify == model[: len(verify)], "Model verification failed"
del model_mv, model, chunk
gc.collect()
print("Model written and verified,", file_size, "bytes")

# --- Bring up IPC (pipe + H2T ring producer) BEFORE booting CM55 ------------
ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()

# CM55 sends control events (cmd + value) and bulk replies (DATA_AVAIL).
state = {"evt": None, "evt_val": 0, "data": None}


def on_ipc(client):
    cmd = client.cmd
    if cmd == IPC.CMD_DATA_AVAIL:
        mv = client.read()  # reused memoryview -> copy out immediately
        if len(mv):
            state["data"] = bytes(mv)
    else:
        state["evt"] = cmd
        state["evt_val"] = client.value


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
        time.sleep_ms(5)
        waited += 5
    return False


def wait_data(timeout_ms):
    waited = 0
    while waited < timeout_ms:
        if state["data"] is not None:
            return state["data"]
        if state["evt"] == TFLM_EVT_ERROR:
            state["evt"] = None
            print("  CM55 reported ERROR (code=%d)" % state["evt_val"])
            return None
        time.sleep_ms(2)
        waited += 2
    print("  timeout waiting for CM55 data")
    return None


def parse_tensor_info(buf):
    ttype, rank, d0, d1, d2, d3, nbytes, scale, zero_point = struct.unpack(TENSOR_INFO_FMT, buf)
    return {
        "type": ttype,
        "dims": (d0, d1, d2, d3)[:rank],
        "bytes": nbytes,
        "scale": scale,
        "zero_point": zero_point,
    }


def quantize(x, info):
    if info["type"] == TFLITE_TYPE_FLOAT32:
        return struct.pack("<f", x)
    q = int(round(x / info["scale"])) + info["zero_point"]
    return struct.pack("<b", max(-128, min(127, q)))


def dequantize(buf, info):
    if info["type"] == TFLITE_TYPE_FLOAT32:
        return struct.unpack("<f", buf[:4])[0]
    q = struct.unpack("<b", buf[:1])[0]
    return (q - info["zero_point"]) * info["scale"]


ipc.register_client(CM33_CLIENT_ID, on_ipc, CM33_EP_ID, CM33_EP_ADDR)
print("CM33 client registered")

# --- Boot CM55 and bring the model up ---------------------------------------
ipc.enable_core(IPC.CM55)
print("CM55 enabled; waiting for engine ready...")
if not wait_event(TFLM_EVT_READY, EVT_TIMEOUT_MS):
    print("  (no READY seen; continuing)")

ipc.send(TFLM_CMD_MODEL_LOAD, 0, CM55_CLIENT_ID)
if not wait_event(TFLM_EVT_MODEL_LOADED, EVT_TIMEOUT_MS):
    raise SystemExit("MODEL_LOAD failed")

state["data"] = None
ipc.send(TFLM_CMD_MODEL_INFO, 0, CM55_CLIENT_ID)
info = wait_data(EVT_TIMEOUT_MS)
if info is None or len(info) < 2 * TENSOR_INFO_SIZE:
    raise SystemExit("MODEL_INFO failed")
in_info = parse_tensor_info(info[:TENSOR_INFO_SIZE])
out_info = parse_tensor_info(info[TENSOR_INFO_SIZE : 2 * TENSOR_INFO_SIZE])
for name, t in (("Input ", in_info), ("Output", out_info)):
    print(
        "%s tensor: type=%d dims=%s bytes=%d scale=%.6g zp=%d"
        % (name, t["type"], t["dims"], t["bytes"], t["scale"], t["zero_point"])
    )
if in_info["type"] not in (TFLITE_TYPE_INT8, TFLITE_TYPE_FLOAT32):
    raise SystemExit("unsupported input tensor type")

ipc.send(TFLM_CMD_MODEL_RUN, 0, CM55_CLIENT_ID)
if not wait_event(TFLM_EVT_MODEL_RUNNING, EVT_TIMEOUT_MS):
    raise SystemExit("MODEL_RUN failed")
print("Model running.\n")

# --- Sweep x over [0, 2*pi): stage input, run one inference, read y ---------
STEP = 0.2
TWO_PI = 6.28318548

x = 0.0
while True:
    ipc.send(IPC.CMD_DATA_AVAIL, quantize(x, in_info), CM55_CLIENT_ID)
    time.sleep_ms(2)  # let CM55 drain the staged input before the trigger
    state["data"] = None
    state["evt"] = None
    ipc.send(TFLM_CMD_RUN_INFERENCE, 0, CM55_CLIENT_ID)

    out = wait_data(REPLY_TIMEOUT_MS)
    if out is not None:
        print("x=%.4f  y=%.4f" % (x, dequantize(out, out_info)))

    x += STEP
    if x >= TWO_PI:
        x = 0.0
    time.sleep_ms(50)
