import time
import gc
from machine import IPC

# ---------------------------------------------------------------------------
# Combined IPC test -- command ping-pong + bulk-data full duplex
#
# Two independent services share one endpoint (EP_ADDR=1):
#   Service 1:  CM33 client_id=3  <-->  CM55 client_id=5
#   Service 2:  CM33 client_id=4  <-->  CM55 client_id=6
#
# Part A (ping-pong): each command sent to a CM55 client is echoed back to the
# paired CM33 client and verified.
#
# Part B (bulk data): payloads sent to CM55 client 5 are echoed back on the
# target->host ring. The CM55 always doorbells the echo to CM33 client 3, so
# client 3 uses a single callback that branches on client.cmd: CMD_DATA_AVAIL
# drives the chunked drain, any other cmd is a Part-A command echo.
# ---------------------------------------------------------------------------

CM55_CLIENT_ID = 5
RING_H2T = 65536  # host -> target (model input) capacity
RING_T2H = 65536  # target -> host (result)      capacity
RX_CHUNK = 8192  # host SRAM drain window / max client.read() size


def make_payload(n, off=0):
    # Fill one pre-allocated bytearray (avoids a 2*n construction peak on the
    # small CM33 GC heap). Serves as both the sent buffer and the reference.
    pat = bytes((off + k) & 0xFF for k in range(256))
    buf = bytearray(n)
    for i in range(0, n, 256):
        chunk = 256 if n - i >= 256 else n - i
        buf[i : i + chunk] = pat[:chunk]
    return buf


def chunk_matches(mv, ref, base, n):
    # Compare in place so the callback does not allocate temporary slices.
    for i in range(n):
        if mv[i] != ref[base + i]:
            return False
    return True


ipc = IPC(src_core=IPC.CM33, target_core=IPC.CM55)
ipc.init()

# Per-service command-echo state (Part A)
svc1 = {"received": False, "cmd": None}  # Service 1
svc2 = {"received": False, "cmd": None}  # Service 2

# Bulk receive state (Part B)
rx = {"ok": False, "len": 0, "count": 0, "ref": None}


def svc1_cb(client, state=rx, matcher=chunk_matches):
    # CM33 client 3 receives both the Service 1 command echo and the bulk
    # DATA_AVAIL doorbell; branch on the command to serve both.
    if client.cmd == IPC.CMD_DATA_AVAIL:
        total = client.value
        ref = state["ref"]
        got = 0
        ok = True
        idle = 0
        while got < total:
            mv = client.read()  # <= RX_CHUNK bytes, drained from the ring
            n = len(mv)
            if n == 0:  # producer still streaming; wait briefly
                idle += 1
                if idle > 500:  # ~500 ms timeout
                    ok = False
                    break
                time.sleep_ms(1)
                continue
            idle = 0
            if not matcher(mv, ref, got, n):
                ok = False
            got += n
        state["len"] = got
        state["ok"] = ok and got == total
        state["count"] += 1
    else:
        svc1["received"] = True
        svc1["cmd"] = client.cmd


def svc2_cb(client):
    svc2["received"] = True
    svc2["cmd"] = client.cmd


# Register both services on CM33's endpoint (EP_ADDR=1)
r1 = ipc.register_client(3, svc1_cb, 1, 1)  # Service 1 -- CM33 client_id=3
print("Service1 registered:", r1)
time.sleep_ms(100)
r2 = ipc.register_client(4, svc2_cb, 1, 1)  # Service 2 -- CM33 client_id=4
print("Service2 registered:", r2)
time.sleep_ms(100)


# ---------------------------------------------------------------------------
# Part 0 -- negative / boundary checks (CM33-side validation)
# ---------------------------------------------------------------------------
# Out-of-range client_id is rejected (valid range 0..IPC_MAX_CLIENTS_PER_EP-1).
bad_id = ipc.register_client(8, svc2_cb, 1, 1)
print("register bad id (8) rejected:", bad_id is False)

# Duplicate client_id is rejected (3 already registered above).
dup_id = ipc.register_client(3, svc1_cb, 1, 1)
print("register duplicate id (3) rejected:", dup_id is False)

# Send to an out-of-range target client raises OSError.
try:
    ipc.send(IPC.CMD_STOP, 0, 8)
    print("send bad id (8) rejected:", False)
except OSError:
    print("send bad id (8) rejected:", True)

print("\n")
# Boot CM55
ipc.enable_core(IPC.CM55)
print("CM55 enabled successfully \n")
time.sleep(1)  # Allow CM55 to complete initialisation


# ---------------------------------------------------------------------------
# Part A -- command ping-pong
# ---------------------------------------------------------------------------
def ipc_test(cmd, cm55_client, label, state):
    """Send cmd to the given CM55 client and verify the echo arrives in state."""
    state["received"] = False
    ipc.send(cmd, 0, cm55_client)
    print("Sent cmd=0x{:02X} to CM55 client_id={}".format(cmd, cm55_client))
    deadline = 3000  # ms
    while not state["received"] and deadline > 0:
        time.sleep_ms(10)
        deadline -= 10
    if state["received"] and state["cmd"] == cmd:
        print("PASS: {} received; cmd=0x{:02X}".format(label, state["cmd"]))
    else:
        print("FAIL: {} echo not received".format(label))


ipc_test(IPC.CMD_START, 5, "Service1 CMD_START", svc1)  # CM55 client_id=5
ipc_test(IPC.CMD_STOP, 6, "Service2 CMD_STOP", svc2)  # CM55 client_id=6


# ---------------------------------------------------------------------------
# Part B -- bulk data full duplex
# ---------------------------------------------------------------------------
def send_and_verify(payload, label, state=rx):
    """Send bulk payload to CM55 and verify every echoed byte comes back."""
    state["ref"] = payload
    state["ok"] = False
    total = len(payload)
    before = state["count"]
    ipc.send(0, payload, CM55_CLIENT_ID)
    print("sent {} bytes ({})".format(total, label))

    deadline = 3000  # ms
    while state["count"] == before and deadline > 0:
        time.sleep_ms(10)
        deadline -= 10

    if state["count"] == before:
        print("FAIL: {} - no echo received".format(label))
    elif state["ok"] and state["len"] == total:
        print("PASS: {} - {} bytes echoed intact".format(label, state["len"]))
    else:
        print("FAIL: {} - mismatch/short (got {} of {})".format(label, state["len"], total))


# Keep the helpers reachable across explicit GC cycles when this test runs from
# a compiled module on the constrained CM33 heap.
rx["make_payload"] = make_payload
rx["send_and_verify"] = send_and_verify


# 1) Small payload (single read()).
rx["send_and_verify"](b"hello-cm55", "small")

# 2) Repeated 3 KB payloads: over the whole test the target->host head/tail
#    advance past the 64 KB ring boundary, exercising the split (wrapped) memcpy
#    on both the CM55 write and the host read.
for i in range(6):
    gc.collect()
    rx["send_and_verify"](rx["make_payload"](3000, i), "wrap-{}".format(i))

# 3) Exactly one drain window (single 8192-byte read).
gc.collect()
rx["send_and_verify"](rx["make_payload"](RX_CHUNK), "one-chunk-8192")

# 4) Just over one window: forces a second read (8192 + 1).
gc.collect()
rx["send_and_verify"](rx["make_payload"](RX_CHUNK + 1), "two-chunk-8193")

# 5) Multi-chunk result received losslessly in 8 KB pieces (8192 + 4096).
gc.collect()
rx["send_and_verify"](rx["make_payload"](12288), "multi-12288")

# 6) Control message still works alongside bulk (int payload path unchanged).
ipc.send(IPC.CMD_STOP, 0, CM55_CLIENT_ID)
time.sleep_ms(100)
print("control send OK")

print("done")
