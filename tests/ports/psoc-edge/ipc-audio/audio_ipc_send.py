# Step 3 - PDM_PCM audio -> IPC streaming test (CM33 sends, CM55 computes back).
#
# Builds on Step 1 (audio_readout.py). Same capture, but each captured buffer is
# pushed into the CM33 -> CM55 shared-memory ring via IPC bulk transfer, and
# CM55 returns a per-buffer result which we tally here.
#
# What "bulk send" does under the hood (see ports/psoc-edge/machine_ipc.c and
# ports/psoc-edge/dualcore/include/ipc_ring.h):
#   ipc.send(cmd, <buffer>, client_id)
#     -> copies the bytes into the 64 KB lock-free ring in shared SOCMEM
#        (host->target ring at 0x262FC000)
#     -> rings the CM55 "doorbell" message (IPC_CMD_DATA_AVAIL = 0x84)
#   The CM55 core drains the ring on the 0x84 doorbell (Step 3) and returns a
#   per-buffer result. This test streams several times the 64 KB ring size, so
#   the ring wraps repeatedly and only keeps up because CM55 drains continuously.
#
# Run on the target with:
#     mpremote run audio_ipc_send.py

from machine import PDM_PCM, IPC
import array
import gc
import time

# ----------------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------------
SCK_PIN = "P8_5"
DATA_PIN = "P8_6"
SAMPLE_RATE = 16000
BITS = 16
FMT = PDM_PCM.STEREO

READ_BYTES = 4096  # STEREO 16-bit => 4 bytes/frame => 1024 frames (64 ms)

# Recording length. This is a LIVE STREAM: every chunk is pushed to CM55 and the
# buffer is reused, so nothing piles up in RAM -> the duration is effectively
# unbounded. Set CAPTURE_SECONDS to whatever you need (seconds, minutes, ...).
CAPTURE_SECONDS = 10
BYTES_PER_FRAME = (BITS // 8) * 2  # stereo => 2 channels
BYTES_PER_SEC = SAMPLE_RATE * BYTES_PER_FRAME
NUM_READS = max(1, round(CAPTURE_SECONDS * BYTES_PER_SEC / READ_BYTES))

HEX_PREVIEW_BYTES = 16
PRINT_EVERY = max(1, round(BYTES_PER_SEC / READ_BYTES))  # ~one snapshot per second
DECODE_BYTES = 512  # decode only a preview on CM33; CM55 sees the full buffer

# IPC client ids (see tests/.../board_only_hw/single/ipc.py for the convention).
CM33_CLIENT_ID = 3  # our endpoint's client id on the CM33 side
CM55_CLIENT_ID = 5  # the target client the doorbell is addressed to
CM33_EP_ID = 1  # CM33 endpoint id / address (fixed for CM33<->CM55)
CM33_EP_ADDR = 1

# Bulk-data doorbell command. Some firmware builds don't expose this as the
# IPC.CMD_DATA_AVAIL class constant, and the bulk-send path ignores the cmd arg
# anyway (it always doorbells 0x84 internally), so define it locally.
CMD_DATA_AVAIL = 0x84

# CM55 -> CM33 result command (Step 3). CM55 packs its audio stats into the
# 32-bit value: high 16 bits = byte count, low 16 bits = peak |sample|.
CMD_AUDIO_RESULT = 0xB0


# ----------------------------------------------------------------------------
# Pretty-printing helpers (same as Step 1)
# ----------------------------------------------------------------------------
def hex_preview(buf, n):
    n = min(n, len(buf))
    return " ".join("{:02x}".format(b) for b in buf[:n])


def decode_stereo_int16(buf, num_read):
    # MicroPython arrays don't support stepped slices, so split manually.
    samples = array.array("h", buf[:num_read])
    left = array.array("h")
    right = array.array("h")
    for idx in range(len(samples)):
        if idx % 2 == 0:
            left.append(samples[idx])
        else:
            right.append(samples[idx])
    return left, right


def summarize(name, chan):
    if len(chan) == 0:
        print("      {:<6} (empty)".format(name))
        return
    lo = min(chan)
    hi = max(chan)
    peak = max(abs(lo), abs(hi))
    print("      {:<6} min={:>6}  max={:>6}  peak={:>6}".format(name, lo, hi, peak))


# ----------------------------------------------------------------------------
# IPC receive callback
# ----------------------------------------------------------------------------
# CM55 computes over each buffer and returns a packed result. During a long
# stream we accumulate instead of printing every line, so we can confirm at the
# end that CM55 processed every buffer we sent (no drops) and track the peak.
recv_count = 0
recv_bytes = 0
recv_peak = 0


def on_ipc_msg(client):
    global recv_count, recv_bytes, recv_peak
    if client.cmd == CMD_AUDIO_RESULT:
        packed = client.value
        nbytes = (packed >> 16) & 0xFFFF
        peak = packed & 0xFFFF
        recv_count += 1
        recv_bytes += nbytes
        if peak > recv_peak:
            recv_peak = peak
    elif client.cmd == CMD_DATA_AVAIL:
        mv = client.read()
        print("      [ipc<-] CM55 sent {} bytes back".format(len(mv)))
    else:
        print("      [ipc<-] CM55 cmd=0x{:02X} value={}".format(client.cmd, client.value))


# ----------------------------------------------------------------------------
# Setup
# ----------------------------------------------------------------------------
print("########## Step 3: PDM_PCM -> IPC streaming test ##########")
print(
    "audio: rate={}Hz bits={} STEREO  read={}B  capture={}s ({} reads)".format(
        SAMPLE_RATE, BITS, READ_BYTES, CAPTURE_SECONDS, NUM_READS
    )
)
print("ipc:   CM33 client={} -> CM55 client={}".format(CM33_CLIENT_ID, CM55_CLIENT_ID))
print("##########################################################")

# --- Audio ---
pdm_pcm = PDM_PCM(
    sck=SCK_PIN,
    data=DATA_PIN,
    sample_rate=SAMPLE_RATE,
    bits=BITS,
    format=FMT,
)
time.sleep_ms(100)  # let the mic settle

# --- IPC ---
ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()  # inits pipe + host->target ring
ipc.register_client(CM33_CLIENT_ID, on_ipc_msg, CM33_EP_ID, CM33_EP_ADDR)
ipc.enable_core(IPC.CM55)  # boot CM55 so it services the pipe
time.sleep(1)  # allow CM55 to come up

# ----------------------------------------------------------------------------
# Capture + send loop
# ----------------------------------------------------------------------------
rx_buf = bytearray(READ_BYTES)
total_sent = 0

try:
    for i in range(NUM_READS):
        # 1) Read one buffer of audio (blocking).
        num_read = pdm_pcm.readinto(rx_buf)
        frames = num_read // 4

        # 2) Occasionally print a detailed view; decoding every buffer in pure
        #    Python is too slow (and allocation-heavy) to keep a long stream
        #    tight, so we only decode a small preview slice here.
        if i % PRINT_EVERY == 0:
            print("\n[read {:>3}] bytes={}  frames={}".format(i, num_read, frames))
            print("      raw : {} ...".format(hex_preview(rx_buf, HEX_PREVIEW_BYTES)))
            left, right = decode_stereo_int16(rx_buf, min(num_read, DECODE_BYTES))
            summarize("L", left)
            summarize("R", right)
            gc.collect()

        # 3) Bulk-send exactly the bytes we read into the shared ring.
        #    memoryview avoids copying; slice keeps to the valid length.
        payload = memoryview(rx_buf)[:num_read]
        ipc.send(CMD_DATA_AVAIL, payload, CM55_CLIENT_ID)
        total_sent += num_read
        if i % PRINT_EVERY == 0:
            print("      [ipc->] sent {} bytes (total {} bytes)".format(num_read, total_sent))
finally:
    time.sleep_ms(200)  # let the last CM55 results arrive before summarizing
    pdm_pcm.deinit()
    duration_ms = (total_sent // 4) * 1000 // SAMPLE_RATE
    print(
        "\n-- Done. Streamed {} bytes across {} reads (~{} ms of audio).".format(
            total_sent, NUM_READS, duration_ms
        )
    )
    print(
        "-- CM55 processed {}/{} buffers, {} bytes, max peak={}.".format(
            recv_count, NUM_READS, recv_bytes, recv_peak
        )
    )
    print("-- PDM_PCM deinitialized.")
