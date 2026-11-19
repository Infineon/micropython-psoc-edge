# ---------------------------------------------------------------------------
# Cough detection from the live mic, over IPC (CM33 -> CM55), driven through the
# generic CM55 TFLM engine (cm55_firmware/engine/tflm):
#   MODEL_LOAD -> MODEL_INFO -> MODEL_RUN -> per window: (stage input) RUN_INFERENCE.
#
# Model: cough-rm-psoc-edge-m55-v2.0.tflite
#   input  : float32 [1, 30, 40]  (30 frames x 40 log-mel bins = 4800 bytes)
#   output : float32 [1, 2]
#
# Pipeline:
#   PDM mic (CM33) -> 16 kHz mono int16 -> log-mel front-end (CM33; 512-sample
#     frames, 10 ms hop) -> 30x40 float32 window, advancing 15 frames (150 ms)
#     -> [H2T ring] -> CM55 TFLM invoke -> raw output tensor -> [T2H ring]
#     -> CM33: threshold.
# The engine's default preprocess hook is a byte pass-through, so all feature
# extraction is done here. Hard-reset the board before each run (CM55 keeps
# running across a CM33 soft reset and rejects a second MODEL_LOAD).
#
# The model must already be in external flash (run dc_rm_cough_flash.py once).
# The device heap is ~38 KB and every file open needs a 16.5 KB contiguous block, so
# this is a single precompiled module (front-end included), imported once:
#   mpy-cross -march=armv7emsp dc_rm_cough.py
#   mpremote cp dc_rm_cough.mpy :
#   mpremote exec "import dc_rm_cough"
# ---------------------------------------------------------------------------
import gc
import math
import micropython
import struct
import time
from array import array
from machine import IPC, PDM_PCM, Pin

# ---------------------------------------------------------------------------
# Log-mel front-end (DEEPCRAFT cough preprocessor), runs on CM33.
# Per frame: 512-sample symmetric Hamming -> real DFT (257 bins) -> magnitude ->
# 40 HTK mel filters, 25..8000 Hz, peak-1 triangles -> clip at CLIP_MIN -> natural
# log. Frames advance by HOP_SAMPLES (10 ms). The FFT is fixed-point (viper), scaled
# by 1/512 with a per-frame auto-gain that is undone in the float domain.
# ---------------------------------------------------------------------------
SAMPLE_RATE = 16000
FRAME_SAMPLES = 512  # fixed: the viper code below is hard-wired to 512 / 257 / 40
HOP_SAMPLES = 160
N_BINS = FRAME_SAMPLES // 2 + 1
N_MELS = 40
FMIN = 25.0
FMAX = 8000.0
CLIP_MIN = 0.000316227766016
PCM_SCALE = (
    1.0 / 32768.0
)  # assumed: PCM normalised to [-1, 1); use 1.0 if the model saw raw int16 values

_MAG_FRAC = 4  # magnitude is kept with 2 fractional bits (sqrt of x << 4)
_W_BITS = 12  # mel weights in Q12
# mel_float = mel_int * FRAME_SAMPLES * PCM_SCALE / (2^_W_BITS * 2^2) / 2^shift
_MEL_SCALE = FRAME_SAMPLES * PCM_SCALE / ((1 << _W_BITS) * 4.0)


# --- Model / input geometry -------------------------------------------------
MODEL_NAME = "cough-rm-psoc-edge-m55-v2.0.tflite"
MODEL_OFFSET = 0x00000000  # must match dc_rm_cough_flash.py

N_FRAMES = 30
STRIDE_FRAMES = (
    5  # minimum new frames between inferences (the CM55 round trip is usually the limit)
)
WINDOW_BYTES = N_FRAMES * N_MELS * 4


# --- Run control ---------------------------------------------------------------
RUN_SECONDS = 30  # listen at least this long
PRINT_ALL = False  # also print windows with no cough

# --- Microphone (same pins as the audio readout demos) ------------------------
SCK_PIN = "P8_5"
DATA_PIN = "P8_6"
BITS = 16
MIC_GAIN_DB = (
    5  # the DEEPCRAFT cough reference uses 5 dB; the driver default (23 dB) clips cough peaks
)
HOP_BYTES = HOP_SAMPLES * 2
HOP_US = HOP_SAMPLES * 1000000 // SAMPLE_RATE
MIC_WARMUP_BYTES = SAMPLE_RATE * 2  # ~1 s; PDM output has a DC/noise transient after init

# --- Output decoding (application logic, lives on CM33) ----------------------
COUGH_INDEX = 1  # output element treated as "cough"; the other is "no cough"
COUGH_THRESHOLD = 0.5

# --- Detection indicator: USER_LED_B (active-high) ---------------------------
LED_PIN = "P20_5"
LED_ON_MS = 150  # turned off from the capture loop, so the mic keeps being read

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

# --- IPC client wiring (matches sine_model_ipc.py) ---------------------------
CM33_CLIENT_ID = 3
CM55_CLIENT_ID = 5
CM33_EP_ID = 1
CM33_EP_ADDR = 1

EVT_TIMEOUT_MS = 3000
RESULT_TIMEOUT_MS = 3000

# Allocate the big persistent blocks first: each needs contiguous heap and the ~38 KB heap fragments fast.
# The collect frees the import's temporary 16.5 KB file buffer, which otherwise stays allocated.
gc.collect()
# Create every later module global now: the globals dict grows to 127 slots (1016 B) at the 97th name, and the fragmented heap has no such block later.
pdm = features = _build_tables = _win = _tw = _sc = _wq = _buf = _mel_acc = _prof = _logt = _i = (
    None
)
_load = _fft = _magnitude = _mel = _log = _peak = frame_features = state = on_ipc = wait_event = (
    None
)
wait_data = parse_tensor_info = decode_output = ipc = led = ring = ring_mv = hop = hop_mv = None
features_mv = ring_head = info = in_info = out_info = name = t = in_peak = clip_hops = read_hop = (
    None
)
warm = fe_frames = fe_overruns = slot = feed_frame = detections = inf_count = inf_total_ms = None
max_prob = pending_t = led_off_at = poll_result = staged = last_sent = t_start = first = None
elapsed_ms = i = None
gc.collect()
features = array(
    "f", range(N_FRAMES * N_MELS)
)  # circular: one 40-float slot per frame; range() init avoids a temporary list on the tiny heap
pdm = PDM_PCM(
    sck=SCK_PIN,
    data=DATA_PIN,
    sample_rate=SAMPLE_RATE,
    bits=BITS,
    format=PDM_PCM.MONO,
    gain=MIC_GAIN_DB,
)
# Remaining persistent tables, biggest first and before anything else fragments the heap; _build_tables() fills them in place.
_buf = array(
    "h", range(2 * FRAME_SAMPLES)
)  # int16: [0:FRAME_SAMPLES] real, [FRAME_SAMPLES:] imag; magnitudes (uint16) overwrite [0:N_BINS]
_wq = array(
    "H", range(572)
)  # mel weights; 572 is fixed by the filterbank geometry (checked in _build_tables)
_tw = array("h", range(FRAME_SAMPLES))  # [0:half] = cos, [half:] = sin of 2*pi*k/n
_win = array("H", range(FRAME_SAMPLES // 2))
_sc = array("H", range(2 * N_MELS))  # (first bin, bin count) per filter
_mel_acc = array("i", range(N_MELS))
gc.collect()


def _build_tables():
    n = FRAME_SAMPLES
    two_pi_n = 2.0 * math.pi / n
    win = _win
    tw = _tw

    # Hamming (symmetric) in Q15: only the first half is stored, w[i] == w[511 - i].
    for i in range(n // 2):
        win[i] = int(32767 * (0.54 - 0.46 * math.cos(2.0 * math.pi * i / (n - 1))))

    half = n // 2
    for k in range(half):
        tw[k] = int(round(32767 * math.cos(two_pi_n * k)))
        tw[half + k] = int(round(32767 * math.sin(two_pi_n * k)))

    def hz_to_mel(f):
        return 2595.0 * math.log10(1.0 + f / 700.0)

    def mel_to_hz(m):
        return 700.0 * (math.pow(10.0, m / 2595.0) - 1.0)

    mel_lo = hz_to_mel(FMIN)
    mel_step = (hz_to_mel(FMAX) - mel_lo) / (N_MELS + 1)
    gc.collect()  # reclaim temporary floats from the window/twiddle-table loops
    df = SAMPLE_RATE / n

    # Keep only compact integer filter geometry. A Python list of mel-point
    # floats plus a list of 40 edge tuples exceeds the small MicroPython heap.
    sc = _sc
    total = 0
    for m in range(N_MELS):
        f0 = mel_to_hz(mel_lo + m * mel_step)
        f1 = mel_to_hz(mel_lo + (m + 1) * mel_step)
        f2 = mel_to_hz(mel_lo + (m + 2) * mel_step)
        k_lo = int(f0 / df)
        k_hi = min(N_BINS - 1, int(f2 / df) + 1)
        sc[2 * m] = k_lo
        sc[2 * m + 1] = k_hi - k_lo + 1
        total += k_hi - k_lo + 1

    if total != len(_wq):
        raise ValueError("mel table size")
    wq = _wq
    o = 0
    for m in range(N_MELS):
        f0 = mel_to_hz(mel_lo + m * mel_step)
        f1 = mel_to_hz(mel_lo + (m + 1) * mel_step)
        f2 = mel_to_hz(mel_lo + (m + 2) * mel_step)
        k_lo = sc[2 * m]
        k_hi = k_lo + sc[2 * m + 1] - 1
        for k in range(k_lo, k_hi + 1):
            f = k * df
            w = (f - f0) / (f1 - f0) if f <= f1 else (f2 - f) / (f2 - f1)
            wq[o] = int(round(w * (1 << _W_BITS))) if w > 0.0 else 0
            o += 1


_build_tables()
gc.collect()
_prof = array("i", range(5))  # accumulated microseconds: load, fft, magnitude, mel, log
# [0:33] log2(1 + i/32) in Q16, [33] ln(CLIP_MIN) in Q16, [34] ln(per-frame mel scale) in Q16 (set per frame)
_logt = array("i", range(35))
for _i in range(33):
    _logt[_i] = int(round(math.log(1.0 + _i / 32.0) / math.log(2.0) * 65536))
_logt[33] = int(math.log(CLIP_MIN) * 65536)


@micropython.viper
def _load(ring: ptr16, start: int, win: ptr16, buf: ptr16) -> int:
    # ring: 512 int16 circular buffer, oldest sample at index `start`.
    peak = 0
    i = 0
    while i < 512:
        s = (int(ring[(start + i) & 511]) << 16) >> 16
        if s < 0:
            s = -s
        if s > peak:
            peak = s
        i += 1
    # Auto-gain so quiet frames keep precision through the 1/512-scaled FFT.
    shift = 0
    while peak > 0 and shift < 14 and (peak << (shift + 1)) <= 16384:
        shift += 1
    i = 0
    j = 0  # bit-reversed index of i
    while i < 512:
        s = (int(ring[(start + i) & 511]) << 16) >> 16
        w = i
        if w > 255:
            w = 511 - i
        buf[j] = ((s << shift) * int(win[w])) >> 15
        buf[512 + j] = 0
        bit = 256
        while bit != 0 and (j & bit) != 0:
            j ^= bit
            bit >>= 1
        j |= bit
        i += 1
    return shift


@micropython.viper
def _fft(buf: ptr16, tw: ptr16):
    size = 2
    step = 256
    while size <= 512:
        half = size >> 1
        base = 0
        while base < 512:
            j = 0
            k = 0
            while j < half:
                wr = (int(tw[k]) << 16) >> 16
                wi = -((int(tw[256 + k]) << 16) >> 16)
                a = base + j
                b = a + half
                br = (int(buf[b]) << 16) >> 16
                bi = (int(buf[512 + b]) << 16) >> 16
                tr = (wr * br - wi * bi) >> 15
                ti = (wr * bi + wi * br) >> 15
                ar = (int(buf[a]) << 16) >> 16
                ai = (int(buf[512 + a]) << 16) >> 16
                buf[b] = (ar - tr) >> 1
                buf[512 + b] = (ai - ti) >> 1
                buf[a] = (ar + tr) >> 1
                buf[512 + a] = (ai + ti) >> 1
                j += 1
                k += step
            base += size
        size <<= 1
        step >>= 1


@micropython.viper
def _magnitude(buf: ptr16):
    # buf[k] = sqrt(re^2 + im^2) * 4 (uint16, <= ~35400), in place. |bin| <= ~8850 after auto-gain, so x << 4 stays below 2^31.
    k = 0
    while k < 257:
        r = (int(buf[k]) << 16) >> 16
        m = (int(buf[512 + k]) << 16) >> 16
        n = (r * r + m * m) << 4
        res = 0
        bit = 1 << 28  # 1 << 30 does not fit a small int, so viper types it as an object
        bit <<= 2
        while bit > n:
            bit >>= 2
        while bit != 0:
            if n >= res + bit:
                n -= res + bit
                res = (res >> 1) + bit
            else:
                res >>= 1
            bit >>= 2
        buf[k] = res
        k += 1


@micropython.viper
def _mel(mag: ptr16, sc: ptr16, wts: ptr16, out: ptr32):
    m = 0
    o = 0
    while m < 40:
        k = int(sc[2 * m])
        c = int(sc[2 * m + 1])
        acc = 0
        j = 0
        while j < c:
            acc += int(wts[o + j]) * int(mag[k + j])
            j += 1
        out[m] = acc
        o += c
        m += 1


@micropython.viper
def _log(acc: ptr32, out: ptr32, off: int, tab: ptr32):
    # out[off + m] = float32(max(ln(acc[m] * scale), ln(CLIP_MIN))) without allocating, via Q16 fixed point.
    clip = int(tab[33])
    lk = int(tab[34])
    sgn = 1 << 28  # the sign bit does not fit a compile-time small int
    sgn <<= 3
    m = 0
    while m < 40:
        a = int(acc[m])
        y = clip
        if a > 0:
            p = 30
            while (a >> p) == 0:
                p -= 1
            if p >= 16:
                mant = (a >> (p - 16)) & 65535
            else:
                mant = (a << (16 - p)) & 65535
            idx = mant >> 11
            t0 = int(tab[idx])
            g = t0 + (((int(tab[idx + 1]) - t0) * (mant & 2047)) >> 11)
            v = p * 45426 + ((g * 22713) >> 15) + lk  # ln(a) + ln(scale); 45426 = ln2 * 2^16
            if v > clip:
                y = v
        bits = 0
        if y != 0:
            neg = 0
            if y < 0:
                y = -y
                neg = 1
            p = 0
            while (y >> (p + 1)) != 0:
                p += 1
            bits = ((p + 111) << 23) | ((y << (23 - p)) & 8388607)
            if neg != 0:
                bits |= sgn
        out[off + m] = bits
        m += 1


@micropython.viper
def _peak(buf: ptr16, n: int) -> int:
    p = 0
    i = 0
    while i < n:
        s = (int(buf[i]) << 16) >> 16
        if s < 0:
            s = -s
        if s > p:
            p = s
        i += 1
    return p


def frame_features(ring, start, out, off):  # noqa: F811
    """ring: bytearray of FRAME_SAMPLES int16 samples (circular, oldest at index `start`).
    Writes N_MELS floats to out[off:]. Returns the elapsed microseconds."""
    us = time.ticks_us
    diff = time.ticks_diff
    t0 = us()
    shift = _load(ring, start, _win, _buf)
    t1 = us()
    _fft(_buf, _tw)
    t2 = us()
    _magnitude(_buf)
    t3 = us()
    _mel(_buf, _sc, _wq, _mel_acc)
    t4 = us()
    k = _MEL_SCALE / (1 << shift)
    _logt[34] = int(math.log(k) * 65536)
    _log(_mel_acc, out, off, _logt)
    t5 = us()
    _prof[0] += diff(t1, t0)
    _prof[1] += diff(t2, t1)
    _prof[2] += diff(t3, t2)
    _prof[3] += diff(t4, t3)
    _prof[4] += diff(t5, t4)
    return diff(t5, t0)


state = {"evt": None, "evt_val": 0, "data": None}


def on_ipc(client):  # noqa: F811
    cmd = client.cmd
    if cmd == IPC.CMD_DATA_AVAIL:
        mv = client.read()  # reused memoryview -> copy out immediately
        if len(mv):
            state["data"] = bytes(mv)
    else:
        state["evt"] = cmd
        state["evt_val"] = client.value


def wait_event(expected, timeout_ms):  # noqa: F811
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


def wait_data(timeout_ms):  # noqa: F811
    waited = 0
    while waited < timeout_ms:
        data = state["data"]
        if data is not None:
            return data
        if state["evt"] == TFLM_EVT_ERROR:
            state["evt"] = None
            print("  CM55 reported ERROR (code=%d)" % state["evt_val"])
            return None
        time.sleep_ms(2)
        waited += 2
    print("  timeout waiting for CM55 data")
    return None


def parse_tensor_info(buf):  # noqa: F811
    ttype, rank, d0, d1, d2, d3, nbytes, scale, zero_point = struct.unpack(TENSOR_INFO_FMT, buf)
    return {
        "type": ttype,
        "rank": rank,
        "dims": (d0, d1, d2, d3)[:rank],
        "bytes": nbytes,
        "scale": scale,
        "zero_point": zero_point,
    }


def decode_output(buf, info):  # noqa: F811
    if info["type"] == TFLITE_TYPE_FLOAT32:
        return list(struct.unpack("<%df" % (len(buf) // 4), buf))
    # int8 output: p = (q - zero_point) * scale
    return [((b - 256 if b >= 128 else b) - info["zero_point"]) * info["scale"] for b in buf]


print("########## Cough detection over IPC ##########")
print("Model name :", MODEL_NAME)
print("Mic gain   : %d dB" % MIC_GAIN_DB)

gc.collect()
print("Transport : ", end="")
ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()  # pipe + H2T ring, BEFORE booting CM55

# Claim the remaining persistent objects before CM55 bring-up fragments the heap.
led = Pin(LED_PIN, Pin.OUT, value=0)
ring = bytearray(FRAME_SAMPLES * 2)  # circular, last 512 samples
ring_mv = memoryview(ring)
hop = bytearray(HOP_BYTES)
hop_mv = memoryview(hop)
features_mv = memoryview(features)
ring_head = 0  # index of the oldest sample
time.sleep_ms(100)  # let the mic settle
gc.collect()

ipc.register_client(CM33_CLIENT_ID, on_ipc, CM33_EP_ID, CM33_EP_ADDR)
ipc.enable_core(IPC.CM55)

# READY is best-effort; MODEL_LOAD below confirms the engine is alive.
wait_event(TFLM_EVT_READY, EVT_TIMEOUT_MS)

ipc.send(TFLM_CMD_MODEL_LOAD, MODEL_OFFSET, CM55_CLIENT_ID)
if not wait_event(TFLM_EVT_MODEL_LOADED, EVT_TIMEOUT_MS):
    raise SystemExit("MODEL_LOAD failed (check op support, arena size, or hard-reset the board)")

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
if in_info["type"] != TFLITE_TYPE_FLOAT32 or in_info["bytes"] != WINDOW_BYTES:
    raise SystemExit("unexpected input tensor: expected float32 %d bytes" % WINDOW_BYTES)

ipc.send(TFLM_CMD_MODEL_RUN, 0, CM55_CLIENT_ID)
if not wait_event(TFLM_EVT_MODEL_RUNNING, EVT_TIMEOUT_MS):
    raise SystemExit("MODEL_RUN failed")

in_peak = 0  # largest |sample| seen since the warm-up
clip_hops = 0  # hops that came close to int16 full scale


def read_hop():  # noqa: F811
    # Read one hop of PCM from the mic and append it to the ring.
    global ring_head, in_peak, clip_hops
    got = 0
    while got < HOP_BYTES:
        n = pdm.readinto(hop_mv[got:])
        if n > 0:
            got += n
    p = _peak(hop, HOP_SAMPLES)
    if p > in_peak:
        in_peak = p
    if p >= 32000:
        clip_hops += 1
    pos = ring_head * 2
    first = min(HOP_BYTES, len(ring) - pos)
    ring_mv[pos : pos + first] = hop_mv[:first]
    if first < HOP_BYTES:
        ring_mv[: HOP_BYTES - first] = hop_mv[first:]
    ring_head = (ring_head + HOP_SAMPLES) % FRAME_SAMPLES


warm = 0
while warm < MIC_WARMUP_BYTES:
    read_hop()
    warm += HOP_BYTES
in_peak = 0
clip_hops = 0
print("Model running. Cough near the mic.\n")


fe_frames = 0
fe_overruns = 0  # frames whose front-end took longer than the hop (mic audio gets dropped)
slot = 0  # circular slot of the oldest frame in `features` (the next one to be overwritten)


def feed_frame():  # noqa: F811
    # Read one hop and compute its 40 log-mel values into the oldest slot.
    global slot, fe_frames, fe_overruns
    read_hop()
    dt = frame_features(ring, ring_head, features, slot * N_MELS)
    slot += 1
    if slot == N_FRAMES:
        slot = 0
    fe_frames += 1
    if dt > HOP_US:
        fe_overruns += 1


detections = 0
inf_count = 0
inf_total_ms = 0
max_prob = 0.0
pending_t = None  # ticks_ms of the in-flight RUN_INFERENCE, None when idle
led_off_at = None


def poll_result():  # noqa: F811
    # Non-blocking: consume the CM55 result if it arrived, and switch the LED off when due.
    global pending_t, led_off_at, detections, inf_count, inf_total_ms, max_prob
    now = time.ticks_ms()
    if led_off_at is not None and time.ticks_diff(now, led_off_at) >= 0:
        led.value(0)
        led_off_at = None
    if pending_t is None:
        return
    out = state["data"]
    if out is None:
        if state["evt"] == TFLM_EVT_ERROR:
            state["evt"] = None
            print("  CM55 reported ERROR (code=%d)" % state["evt_val"])
            pending_t = None
        elif time.ticks_diff(now, pending_t) > RESULT_TIMEOUT_MS:
            print("  timeout waiting for CM55 data")
            pending_t = None
        return
    state["data"] = None
    inf_ms = time.ticks_diff(now, pending_t)
    pending_t = None
    inf_total_ms += inf_ms
    inf_count += 1
    probs = decode_output(out, out_info)
    p = probs[COUGH_INDEX]
    if p > max_prob:
        max_prob = p
    detail = " ".join("out[%d]=%.4f" % (i, v) for i, v in enumerate(probs))
    if p >= COUGH_THRESHOLD:
        print("  >>> COUGH DETECTED  %s (%d ms)" % (detail, inf_ms))
        detections += 1
        led.value(1)
        led_off_at = time.ticks_add(now, LED_ON_MS)
    elif PRINT_ALL:
        print("  no cough            %s (%d ms)" % (detail, inf_ms))


staged = False  # window copied to CM55; RUN_INFERENCE is sent after the next frame so CM55 has drained it
last_sent = 0
t_start = time.ticks_ms()
try:
    while time.ticks_diff(time.ticks_ms(), t_start) < RUN_SECONDS * 1000:
        feed_frame()
        poll_result()
        if staged:
            staged = False
            state["data"] = None
            state["evt"] = None
            pending_t = time.ticks_ms()
            ipc.send(TFLM_CMD_RUN_INFERENCE, 0, CM55_CLIENT_ID)
        elif (
            pending_t is None and fe_frames >= N_FRAMES and fe_frames - last_sent >= STRIDE_FRAMES
        ):
            first = slot * N_MELS  # oldest frame first, as the model expects
            ipc.send(IPC.CMD_DATA_AVAIL, features_mv[first:], CM55_CLIENT_ID)
            if first:
                ipc.send(IPC.CMD_DATA_AVAIL, features_mv[:first], CM55_CLIENT_ID)
            last_sent = fe_frames
            staged = True
    elapsed_ms = time.ticks_diff(time.ticks_ms(), t_start)
    while pending_t is not None:
        poll_result()
        time.sleep_ms(2)
finally:
    led.value(0)
    pdm.deinit()

print("\n========== Runtime Summary ==========")
print("Model             : %s" % MODEL_NAME)
print("Windows           : %d" % inf_count)
print("Cough detections  : %d" % detections)
print("Max cough prob    : %.4f" % max_prob)
print("Input peak        : %d / 32767 (%d hops near full scale)" % (in_peak, clip_hops))
print("Avg round trip    : %s" % ("%d ms" % (inf_total_ms // inf_count) if inf_count else "n/a"))
print(
    "Audio coverage    : %d %% (%d frames, %d ms)"
    % (fe_frames * HOP_US // 10 // elapsed_ms, fe_frames, elapsed_ms)
)
if fe_frames:
    print(
        "Front-end         : %d us/frame (budget %d us), %d of %d frames over budget"
        % (sum(_prof) // fe_frames, HOP_US, fe_overruns, fe_frames)
    )
    print(
        "  load %d, fft %d, magnitude %d, mel %d, log %d (us/frame)"
        % tuple(_prof[i] // fe_frames for i in range(5))
    )
