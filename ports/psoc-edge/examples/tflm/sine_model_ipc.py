import os
import gc
import time
import struct
from machine import ExternalFlash
from machine import IPC

# ---------------------------------------------------------------------------
# TFLM sine demo, streamed over IPC (POC)
#
# Instead of the CM55 sweeping x on its own and CM33 polling a shared-memory
# result struct, this version streams the data across the two cores:
#
#   CM33 (this script) --  x (float32)  --> [H2T ring] --> CM55
#   CM55 runs the TFLM sine model on x, then
#   CM55 --  y = model(x) (float32)  --> [T2H ring] --> CM33 (prints x, y)
#
# Transport: machine.IPC bulk rings in shared SOCMEM + a doorbell message
#   ipc.send(cmd, buf, client_id)  -> streams bytes into the CM33->CM55 ring,
#                                     doorbells CM55
#   client.read()                  -> drains bytes from the CM55->CM33 ring
#                                     inside the result-client callback
# The CM55 firmware (framework/tflm) drains x on the doorbell, runs one
# inference per record, and coalesces the y results back into as few reply
# doorbells as possible (CM33 client id 3).
#
# Mode switch (BATCH_SIZE below):
#   BATCH_SIZE = 1  -> scalar mode: one x per doorbell, one y reply.
#   BATCH_SIZE = N  -> batched mode: N x packed into one doorbell, N y collected.
# The same code path serves both; only the records-per-doorbell count changes.
# ---------------------------------------------------------------------------

SOURCE_PATH = "hello_world_int8.tflite"
CHUNK_SIZE = 4096
MODEL_OFFSET = 0x00000000

# 1 = scalar mode; N > 1 = batched mode (N x-records streamed per doorbell).
BATCH_SIZE = 1

CM33_CLIENT_ID = 3  # our endpoint's client id (CM55 doorbells this on reply)
CM55_CLIENT_ID = 5  # target client the x doorbell is addressed to
CM33_EP_ID = 1
CM33_EP_ADDR = 1

# --- Write model to external flash (CM55 tflm_init reads it from XIP) --------
flash = ExternalFlash()
flash.init()
sector_size = flash.sector_size()
file_size = os.stat(SOURCE_PATH)[6]
reserved_size = ((file_size + sector_size - 1) // sector_size) * sector_size
flash.erase(MODEL_OFFSET, reserved_size)

# Read the (small) model once and reuse it for write + verify -- the device
# heap is tiny, so a second open()/read() can fail with MemoryError.
with open(SOURCE_PATH, "rb") as source:
    model = source.read()

written = 0
while written < file_size:
    chunk = model[written : written + CHUNK_SIZE]
    flash.write(MODEL_OFFSET + written, chunk)
    written += len(chunk)
    gc.collect()

verify = flash.read(MODEL_OFFSET, min(256, file_size))
assert verify == model[: len(verify)], "Model verification failed"
del model
gc.collect()
print("Model written and verified,", file_size, "bytes")

# --- Bring up IPC (pipe + H2T ring producer) BEFORE booting CM55 ------------
ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()

# y results streamed back from CM55 accumulate here across reply doorbells.
# CM55 may coalesce or split replies, so we append every drained byte and let
# the main loop wait for the full batch -- this serves scalar and batched alike.
# Held in a dict so the main loop can swap in a fresh buffer per batch (MicroPython
# bytearray has no in-place clear).
state = {"rx": bytearray()}


def on_result(client):
    if client.cmd != IPC.CMD_DATA_AVAIL:
        return
    mv = client.read()  # reused memoryview -> copy out immediately
    if len(mv):
        state["rx"].extend(mv)  # may carry several y records at once


ipc.register_client(CM33_CLIENT_ID, on_result, CM33_EP_ID, CM33_EP_ADDR)
print("CM33 result client registered")

# --- Boot CM55 (runs TFLM + IPC server) -------------------------------------
ipc.enable_core(IPC.CM55)
print("CM55 enabled; streaming x -> y over IPC\n")
time.sleep(1)  # let CM55 tflm_init + IPC bring-up finish

# --- Stream x, receive y, BATCH_SIZE records per doorbell --------------------
STEP = 0.2
TWO_PI = 6.28318548
REPLY_TIMEOUT_MS = 2000
REC = 4  # bytes per float32 record

print("mode:", "scalar" if BATCH_SIZE == 1 else "batched (%d)" % BATCH_SIZE, "\n")

x = 0.0
while True:
    # Build the next batch of x values and pack them into one payload.
    xs = []
    payload = bytearray()
    for _ in range(BATCH_SIZE):
        xs.append(x)
        payload.extend(struct.pack("<f", x))
        x += STEP
        if x >= TWO_PI:
            x = 0.0

    rx = bytearray()  # fresh accumulator for this batch
    state["rx"] = rx
    ipc.send(IPC.CMD_DATA_AVAIL, payload, CM55_CLIENT_ID)

    expected = BATCH_SIZE * REC
    waited = 0
    while len(rx) < expected and waited < REPLY_TIMEOUT_MS:
        time.sleep_ms(2)
        waited += 2

    if len(rx) < expected:
        print("batch of %d -> timeout (%d/%d bytes)" % (BATCH_SIZE, len(rx), expected))
    else:
        ys = struct.unpack("<%df" % BATCH_SIZE, bytes(rx[:expected]))
        for xi, yi in zip(xs, ys):
            print("x=%.4f  y=%.4f" % (xi, yi))

    time.sleep_ms(50)
