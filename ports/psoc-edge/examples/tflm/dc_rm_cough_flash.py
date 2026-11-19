# ---------------------------------------------------------------------------
# Write the cough model into the shared external-flash partition (CM55 reads it
# in place via XIP). The flash content persists, so run this once per model
# change, then run dc_rm_cough.py. Kept separate because open() needs a large
# contiguous heap block that the bigger application script has already used up.
#
# mpremote cp cough-rm-psoc-edge-m55-v2.0.tflite :
# mpremote run dc_rm_cough_flash.py
# ---------------------------------------------------------------------------
import gc
import os
import time
from machine import ExternalFlash

MODEL_FILE = "cough-rm-psoc-edge-m55-v2.0.tflite"
MODEL_OFFSET = 0x00000000  # start of the shared-data flash partition; must match dc_rm_cough.py
CHUNK_SIZE = 1024  # stream to flash in small chunks; the heap is tiny

gc.collect()
model_size = os.stat(MODEL_FILE)[6]
print("Model name :", MODEL_FILE)
print("Model size :", model_size, "bytes")

source = open(MODEL_FILE, "rb")

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
print("Model written to external flash in %d ms" % time.ticks_diff(time.ticks_ms(), t0))
