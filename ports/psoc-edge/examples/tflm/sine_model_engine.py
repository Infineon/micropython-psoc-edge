import gc
import os
import struct
import time
from machine import ExternalFlash
from machine import IPC

# ---------------------------------------------------------------------------
# TFLM sine model, driven over the tflm_engine IPC protocol.
#
#   flash model -> boot CM55 -> LOAD -> RUN -> loop { x -> RUN_INFERENCE -> y }
#
# The CM55 firmware (framework/tflm) is a generic inference server: it knows
# nothing about the sine model. Input and output are raw tensor bytes, so after
# LOAD this script asks the CM55 for the tensor layout (MODEL_INFO) and
# quantizes/dequantizes accordingly: float32 and int8 models are supported.
# ---------------------------------------------------------------------------

MODEL_PATH = "hello_world_int8.tflite"
# Relative to the shared-data flash region; the CM55 maps it at 0x62000000.
MODEL_OFFSET = 0x00000000
CHUNK_SIZE = 4096

# Wire protocol: cm55_firmware/engine/tflm/tflm_engine.h
CMD_MODEL_LOAD = 0x90  # value = model offset
CMD_MODEL_UNLOAD = 0x91
CMD_MODEL_RUN = 0x92
CMD_MODEL_PAUSE = 0x93
CMD_RUN_INFERENCE = 0x94  # input bytes were already sent as bulk data
CMD_MODEL_INFO = 0x95  # reply: input then output tensor info, 32 bytes each
EVT_READY = 0xB0
EVT_MODEL_LOADED = 0xB1
EVT_MODEL_UNLOADED = 0xB2
EVT_MODEL_RUNNING = 0xB3
EVT_MODEL_PAUSED = 0xB4
EVT_ERROR = 0xE0  # value = error code
ERRORS = {1: "BAD_STATE", 2: "MODEL_LOAD_FAILED", 3: "INVOKE_FAILED"}

TYPE_FLOAT32 = 1  # TfLiteType values in the MODEL_INFO reply
TYPE_INT8 = 9
# Per tensor: type, rank, dims[4], bytes, scale, zero_point -> a 9-tuple
# (type [0], bytes [6], scale [7], zero_point [8]).
INFO_FMT = "<BBxx4iIfi"

CM33_CLIENT_ID = 3  # CM55 sends its events and result bytes to this client
CM55_CLIENT_ID = 5  # commands and input bytes are addressed to this client
CM33_EP_ID = 1
CM33_EP_ADDR = 1

STEP = 0.2
TWO_PI = 6.28318548
TIMEOUT_MS = 2000

# --- Write the model to shared flash ----------------------------------------
flash = ExternalFlash()
flash.init()
sector_size = flash.sector_size()
file_size = os.stat(MODEL_PATH)[6]
flash.erase(MODEL_OFFSET, ((file_size + sector_size - 1) // sector_size) * sector_size)

with open(MODEL_PATH, "rb") as f:
    model = f.read()  # the device heap is small: read once, reuse for verify
view = memoryview(model)  # slicing a memoryview does not copy the data
written = 0
while written < file_size:
    chunk = view[written : written + CHUNK_SIZE]
    flash.write(MODEL_OFFSET + written, chunk)
    written += len(chunk)
    gc.collect()
head = flash.read(MODEL_OFFSET, min(256, file_size))
assert head == model[: len(head)], "model verification failed"
del view, model
gc.collect()
print("model written and verified,", file_size, "bytes")

# --- Event and result plumbing ----------------------------------------------
# on_event runs in the pipe ISR for command events, with the GC locked, so it
# only stores small ints into preallocated slots (no allocation).
EV_SLOTS = 8
ev_cmd = [0] * EV_SLOTS
ev_val = [0] * EV_SLOTS
ev_idx = [0, 0]  # head (written by the callback), tail (read by the script)

# Result bytes from the CM55 accumulate here. The script swaps in a fresh
# bytearray per request (bytearray has no in-place clear).
state = {"rx": bytearray()}


def on_event(client):
    cmd = client.cmd
    if cmd == IPC.CMD_DATA_AVAIL:
        # Scheduled in VM context (allocation allowed). The doorbell announces
        # the length; the bytes may land in the ring just after it.
        need = client.value
        got = 0
        for _ in range(200):
            mv = client.read()  # reused memoryview: copy out immediately
            if len(mv):
                state["rx"].extend(mv)
                got += len(mv)
            if got >= need:
                break
            time.sleep_ms(1)
        return
    head = ev_idx[0]
    ev_cmd[head % EV_SLOTS] = cmd
    ev_val[head % EV_SLOTS] = client.value
    ev_idx[0] = head + 1


def next_event(timeout_ms):
    waited = 0
    while ev_idx[1] == ev_idx[0]:
        if waited >= timeout_ms:
            return None
        time.sleep_ms(1)
        waited += 1
    tail = ev_idx[1]
    ev_idx[1] = tail + 1
    return ev_cmd[tail % EV_SLOTS], ev_val[tail % EV_SLOTS]


def check(ev, expected):
    if ev is None:
        raise OSError("timeout waiting for event 0x%02X" % expected)
    cmd, val = ev
    if cmd == EVT_ERROR:
        raise OSError("engine error: " + ERRORS.get(val, str(val)))
    if cmd != expected:
        raise OSError("unexpected event 0x%02X" % cmd)


def command(cmd, value, expected_event):
    ipc.send(cmd, value, CM55_CLIENT_ID)
    check(next_event(TIMEOUT_MS), expected_event)


def wait_rx(buf, n):
    waited = 0
    while len(buf) < n:
        if ev_idx[1] != ev_idx[0]:  # an event instead of bytes is an error
            check(next_event(0), 0)
        if waited >= TIMEOUT_MS:
            raise OSError("timeout waiting for result")
        time.sleep_ms(1)
        waited += 1


def model_info():
    buf = bytearray()
    state["rx"] = buf
    ipc.send(CMD_MODEL_INFO, 0, CM55_CLIENT_ID)
    wait_rx(buf, 64)
    return struct.unpack(INFO_FMT, buf[:32]), struct.unpack(INFO_FMT, buf[32:64])


def infer(x):
    out = bytearray()
    state["rx"] = out
    if IN[0] == TYPE_FLOAT32:
        payload = struct.pack("<f", x)
    else:
        q = int(round(x / IN[7])) + IN[8]
        payload = struct.pack("b", max(-128, min(127, q)))
    # Input bytes first (bulk ring + doorbell), then the command that runs them.
    ipc.send(IPC.CMD_DATA_AVAIL, payload, CM55_CLIENT_ID)
    ipc.send(CMD_RUN_INFERENCE, 0, CM55_CLIENT_ID)
    wait_rx(out, OUT[6])
    if OUT[0] == TYPE_FLOAT32:
        return struct.unpack("<f", out)[0]
    return (struct.unpack("b", out)[0] - OUT[8]) * OUT[7]


# --- Bring up IPC, then boot the CM55 ---------------------------------------
ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()
ipc.register_client(CM33_CLIENT_ID, on_event, CM33_EP_ID, CM33_EP_ADDR)
ipc.enable_core(IPC.CM55)

# READY is sent once when the CM55 task starts. If the CM55 was already running
# (CM33 soft reset), it never arrives: unload so LOAD starts from a clean state.
ev = next_event(TIMEOUT_MS)
if ev is None:
    print("no READY (CM55 already running), resetting engine state")
    ipc.send(CMD_MODEL_UNLOAD, 0, CM55_CLIENT_ID)
    next_event(TIMEOUT_MS)  # UNLOADED, or ERROR if it was already unloaded
else:
    check(ev, EVT_READY)

command(CMD_MODEL_LOAD, MODEL_OFFSET, EVT_MODEL_LOADED)
IN, OUT = model_info()
print("input: ", IN)
print("output:", OUT)
if IN[0] not in (TYPE_FLOAT32, TYPE_INT8) or OUT[0] not in (TYPE_FLOAT32, TYPE_INT8):
    raise OSError("unsupported tensor type")
command(CMD_MODEL_RUN, 0, EVT_MODEL_RUNNING)
print("model running\n")

# --- Sweep x over one period and print y ------------------------------------
x = 0.0
try:
    while True:
        y = infer(x)
        print("x=%.4f  y=%.4f" % (x, y))
        x += STEP
        if x >= TWO_PI:
            x = 0.0
        time.sleep_ms(50)
except KeyboardInterrupt:
    pass
finally:
    # Leave the engine unloaded so the next run starts clean.
    try:
        command(CMD_MODEL_PAUSE, 0, EVT_MODEL_PAUSED)
        command(CMD_MODEL_UNLOAD, 0, EVT_MODEL_UNLOADED)
    except OSError as e:
        print("cleanup:", e)
