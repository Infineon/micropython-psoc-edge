# SPI test helper for psoc-edge MicroPython port.
#
# Usage from REPL:
#   import spi_test
#   spi_test.loopback_test()          # MOSI <-> MISO shorted
#   spi_test.flash_jedec_test()       # external SPI flash connected
#
# Default pin map matches SCB10 setup used in current bring-up:
#   SCK  = P16_0
#   MOSI = P16_1
#   MISO = P16_2
#   CS   = P16_3

from machine import SPI, Pin
from time import sleep_us


def _new_spi(
    baudrate=1_000_000,
    polarity=0,
    phase=0,
    bits=8,
    firstbit=None,
    sck_pin="P16_0",
    mosi_pin="P16_1",
    miso_pin="P16_2",
):
    if firstbit is None:
        firstbit = SPI.MSB
    return SPI(
        baudrate=baudrate,
        polarity=polarity,
        phase=phase,
        bits=bits,
        firstbit=firstbit,
        sck=Pin(sck_pin),
        mosi=Pin(mosi_pin),
        miso=Pin(miso_pin),
    )


def loopback_test(
    tx=b"\x9f\xf0\x10\x22",
    baudrate=1_000_000,
    polarity=0,
    phase=0,
    sck_pin="P16_0",
    mosi_pin="P16_1",
    miso_pin="P16_2",
):
    """Run MOSI->MISO loopback test.

    Hardware:
      Short MOSI and MISO together.
      Do not connect external slave for this test.
    """
    spi = _new_spi(
        baudrate=baudrate,
        polarity=polarity,
        phase=phase,
        sck_pin=sck_pin,
        mosi_pin=mosi_pin,
        miso_pin=miso_pin,
    )

    try:
        rx = bytearray(len(tx))
        spi.write_readinto(tx, rx)
        ok = bytes(rx) == bytes(tx)
        print("[loopback] tx:", tx)
        print("[loopback] rx:", rx)
        print("[loopback] status:", "PASS" if ok else "FAIL")
        return ok, rx
    finally:
        spi.deinit()


def flash_jedec_test(
    baudrate=1_000_000,
    polarity=0,
    phase=0,
    sck_pin="P16_0",
    mosi_pin="P16_1",
    miso_pin="P16_2",
    cs_pin="P16_3",
):
    """Read JEDEC ID (0x9F) from an SPI flash-like slave.

    Returns tuple (id_bytes, raw_rx).
    id_bytes is rx[1:4] because rx[0] is during command phase.
    """
    spi = _new_spi(
        baudrate=baudrate,
        polarity=polarity,
        phase=phase,
        sck_pin=sck_pin,
        mosi_pin=mosi_pin,
        miso_pin=miso_pin,
    )
    cs = Pin(cs_pin, Pin.OUT, value=1)

    try:
        tx = b"\x9f\x00\x00\x00"
        rx = bytearray(4)

        cs(1)
        sleep_us(2)
        cs(0)
        sleep_us(2)
        spi.write_readinto(tx, rx)
        sleep_us(2)
        cs(1)

        jedec = bytes(rx[1:4])
        print("[flash] tx:", tx)
        print("[flash] rx:", rx)
        print("[flash] jedec:", jedec)

        if jedec == b"\x00\x00\x00":
            print("[flash] WARN: no slave response (all zeros)")
        if bytes(rx) == tx:
            print("[flash] WARN: full echo detected (possible MOSI/MISO short or loopback)")

        return jedec, rx
    finally:
        spi.deinit()


def smoke_tests():
    """Convenience runner: loopback first, then flash read."""
    print("=== SPI loopback test ===")
    loopback_test()
    print("=== SPI flash JEDEC test ===")
    flash_jedec_test()
