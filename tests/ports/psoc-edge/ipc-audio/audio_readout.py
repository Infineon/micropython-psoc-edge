# Step 1 - PDM_PCM audio readout on the CM33 core.
#
# Goal of this step: capture raw PCM audio from the on-board microphones and
# PRINT what we read, so we can visually confirm the data before we ever send
# it anywhere. There is deliberately NO IPC / shared memory here yet.
#
# Run on the target with:
#     mpremote run audio_readout.py
#
# Audio format used here (matches the record-target.py default):
#     sample_rate = 16000 Hz
#     bits        = 16
#     format      = STEREO  -> each frame is [L int16][R int16] = 4 bytes

from machine import PDM_PCM
import array
import time

# ----------------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------------
SCK_PIN = "P8_5"
DATA_PIN = "P8_6"
SAMPLE_RATE = 16000
BITS = 16
FMT = PDM_PCM.STEREO

# How much we read per iteration and how many iterations we print.
# STEREO 16-bit => 4 bytes per frame. 128 bytes = 32 frames (L+R pairs).
READ_BYTES = 128
NUM_READS = 10  # number of buffers to read and print
HEX_PREVIEW_BYTES = 16  # how many raw bytes to show as hex per buffer


# ----------------------------------------------------------------------------
# Pretty-printing helpers
# ----------------------------------------------------------------------------
def hex_preview(buf, n):
    # Show the first n raw bytes as space-separated hex, e.g. "a1 03 5f ff ..."
    n = min(n, len(buf))
    return " ".join("{:02x}".format(b) for b in buf[:n])


def decode_stereo_int16(buf, num_read):
    # Interpret the raw bytes as signed 16-bit samples.
    # Layout in memory: L0 R0 L1 R1 ...  (little-endian int16 each)
    # Note: MicroPython arrays don't support stepped slices, so split manually.
    samples = array.array("h", buf[:num_read])  # 'h' = signed 16-bit
    left = array.array("h")
    right = array.array("h")
    for idx in range(len(samples)):
        if idx % 2 == 0:
            left.append(samples[idx])
        else:
            right.append(samples[idx])
    return left, right


def summarize(name, chan):
    # Min / max / rough loudness so silence vs. sound is obvious in the log.
    if len(chan) == 0:
        print("      {:<6} (empty)".format(name))
        return
    lo = min(chan)
    hi = max(chan)
    peak = max(abs(lo), abs(hi))
    print("      {:<6} min={:>6}  max={:>6}  peak={:>6}".format(name, lo, hi, peak))


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
print("########## Step 1: PDM_PCM audio readout ##########")
print(
    "sck={}  data={}  rate={}Hz  bits={}  format=STEREO".format(
        SCK_PIN, DATA_PIN, SAMPLE_RATE, BITS
    )
)
print("read_bytes={}  num_reads={}".format(READ_BYTES, NUM_READS))
print("###################################################")

pdm_pcm = PDM_PCM(
    sck=SCK_PIN,
    data=DATA_PIN,
    sample_rate=SAMPLE_RATE,
    bits=BITS,
    format=FMT,
)

# Give the mic a moment to settle after start-up.
time.sleep_ms(100)

rx_buf = bytearray(READ_BYTES)

try:
    for i in range(NUM_READS):
        # Blocking read: fills rx_buf, returns how many bytes were written.
        num_read = pdm_pcm.readinto(rx_buf)

        frames = num_read // 4  # STEREO 16-bit -> 4 bytes/frame
        print("\n[read {:>2}] bytes={}  frames={}".format(i, num_read, frames))
        print("      raw : {} ...".format(hex_preview(rx_buf, HEX_PREVIEW_BYTES)))

        left, right = decode_stereo_int16(rx_buf, num_read)
        summarize("L", left)
        summarize("R", right)
finally:
    pdm_pcm.deinit()
    print("\n-- Done. PDM_PCM deinitialized.")
