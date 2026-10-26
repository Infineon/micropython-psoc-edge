# SPI multi-device test
# Tests SPI master and target communication between two boards.
# Both instances also test the SPI/SPITarget constructor (id/pins) and init() behaviour.
#
# KIT_PSE84_AI board Hardware connections (SCB10):
#   Board 0 (Target)    Board 1 (Master)
#   P16_0 (SCK)    <->  P16_0 (SCK)
#   P16_1 (MOSI)   <->  P16_1 (MOSI)
#   P16_2 (MISO)   <->  P16_2 (MISO)
#   P16_3 (SSEL)   <->  P16_3 (CS out)
#   GND            <->  GND
#
import os
import time


def _get_spi_config_for_board():
    uname_fn = getattr(os, "uname", None)
    if uname_fn is None:
        # Some firmware builds do not expose os.uname(); default to KIT mapping.
        return (10, "P16_0", "P16_1", "P16_2", "P16_3")

    board = uname_fn().machine
    board_norm = board.upper().replace("-", "_")

    if "KIT_PSE84_AI" in board_norm or "PSE84" in board_norm:
        return (10, "P16_0", "P16_1", "P16_2", "P16_3")

    print("SKIP: Unsupported board for SPI multi-device test:", board)
    raise SystemExit


SPI_ID, SCK_PIN, MOSI_PIN, MISO_PIN, SSEL_PIN = _get_spi_config_for_board()


def _check(fn):
    try:
        fn()
        print("no error")
    except Exception as e:
        print(type(e).__name__, e)


def _spi_api_test():
    # SPI constructor (id/pins) and init() behaviour. No transfers are done here.
    from machine import SPI, I2C

    print("***** API: constructor and init() *****\n")

    # The id alone is enough: pins are resolved from the id.
    spi = SPI(SPI_ID)
    print(spi)

    # Constructing the same id again returns the same object.
    print(SPI(SPI_ID) is spi)

    # init() reconfigures the object. baudrate is the first positional argument.
    spi.init(2_000_000, polarity=1, phase=1, firstbit=SPI.LSB)
    print(spi)

    # init() without arguments resets everything to defaults.
    spi.init()
    print(spi)
    spi.deinit()

    # Matching id and pins.
    spi = SPI(SPI_ID, sck=SCK_PIN, mosi=MOSI_PIN, miso=MISO_PIN)
    print(spi)
    spi.deinit()

    # Pins only: the id is resolved from the pins.
    spi = SPI(sck=SCK_PIN, mosi=MOSI_PIN, miso=MISO_PIN)
    print(spi)
    spi.deinit()

    # Invalid id and pin combinations.
    _check(lambda: SPI("10"))
    _check(lambda: SPI(-1))
    _check(lambda: SPI(12))
    _check(lambda: SPI())
    _check(lambda: SPI(SPI_ID, sck="P17_0"))
    _check(lambda: SPI(6))

    # Invalid parameters.
    _check(lambda: SPI(SPI_ID, baudrate=0))
    _check(lambda: SPI(SPI_ID, polarity=2))
    _check(lambda: SPI(SPI_ID, phase=2))
    _check(lambda: SPI(SPI_ID, bits=16))
    _check(lambda: SPI(SPI_ID, firstbit=5))

    # A failed construction releases the SCB, so the id can be used again.
    spi = SPI(SPI_ID)
    print(spi)

    # A failed init() keeps the object and the id: init() can be retried.
    _check(lambda: spi.init(bits=16))
    _check(lambda: spi.init(sck="P17_0"))
    spi.init(baudrate=500_000)
    print(spi)
    spi.deinit()

    # The SCB is shared with other peripherals.
    i2c = I2C(SPI_ID)
    _check(lambda: SPI(SPI_ID))
    i2c.deinit()
    print()


def _spi_target_api_test():
    # SPITarget constructor (id/pins) and init() behaviour. No transfers are done here.
    # Runs before the target's first multitest.next(), so the master is not started yet.
    from machine import SPITarget, SPI, I2C

    print("***** API: constructor and init() *****\n")

    # The id alone is enough: pins are resolved from the id.
    target = SPITarget(SPI_ID)
    print(target)

    # Constructing the same id again returns the same object.
    print(SPITarget(SPI_ID) is target)

    # init() reconfigures the object.
    target.init(polarity=1, phase=1, firstbit=SPITarget.LSB)
    print(target)

    # init() without arguments resets everything to defaults.
    target.init()
    print(target)
    target.deinit()

    # Matching id and pins.
    target = SPITarget(SPI_ID, sck=SCK_PIN, mosi=MOSI_PIN, miso=MISO_PIN, ssel=SSEL_PIN)
    print(target)
    target.deinit()

    # Pins only: the id is resolved from the pins.
    target = SPITarget(sck=SCK_PIN, mosi=MOSI_PIN, miso=MISO_PIN, ssel=SSEL_PIN)
    print(target)
    target.deinit()

    # Invalid id and pin combinations.
    _check(lambda: SPITarget("10"))
    _check(lambda: SPITarget(-1))
    _check(lambda: SPITarget(12))
    _check(lambda: SPITarget())
    _check(lambda: SPITarget(SPI_ID, sck="P17_0"))
    _check(lambda: SPITarget(6))

    # Invalid parameters.
    _check(lambda: SPITarget(SPI_ID, polarity=2))
    _check(lambda: SPITarget(SPI_ID, phase=2))
    _check(lambda: SPITarget(SPI_ID, bits=16))
    _check(lambda: SPITarget(SPI_ID, firstbit=5))

    # A failed construction releases the SCB, so the id can be used again.
    target = SPITarget(SPI_ID)
    print(target)

    # A failed init() keeps the object and the id: init() can be retried.
    _check(lambda: target.init(bits=16))
    _check(lambda: target.init(sck="P17_0"))
    target.init(polarity=1)
    print(target)

    # The SCB is shared with other peripherals.
    _check(lambda: SPI(SPI_ID))
    _check(lambda: I2C(SPI_ID))
    target.deinit()
    print()


def instance0():
    # SPI Target (Slave)
    from machine import SPITarget

    print("\n***** SPI Target Test *****\n")

    _spi_target_api_test()

    target = SPITarget(
        SPI_ID,
        polarity=1,
        phase=1,
        bits=8,
        firstbit=SPITarget.LSB,
        sck=SCK_PIN,
        mosi=MOSI_PIN,
        miso=MISO_PIN,
        ssel=SSEL_PIN,
    )

    # Reconfigure with init() to the mode used by the master, before any transfer.
    target.init(
        polarity=0,
        phase=0,
        bits=8,
        firstbit=SPITarget.MSB,
        sck=SCK_PIN,
        mosi=MOSI_PIN,
        miso=MISO_PIN,
        ssel=SSEL_PIN,
    )
    print(target)

    # Signal to master that target is ready
    multitest.next()

    # Test 1: Read from master
    print("***** Test 1: Read *****\n")
    rx = bytearray(4)
    n = target.readinto(rx)
    print("Received:", rx[:n])
    read_pass = rx[:n] == b"\x01\x02\x03\x04"
    print("Status:", "PASS" if read_pass else "FAIL")

    # Signal master that read is done
    multitest.next()

    # Wait for master to be ready for write test
    multitest.next()

    # Test 2: Write to master
    print("\n***** Test 2: Write *****\n")
    tx = b"\xaa\xbb\xcc\xdd"
    n = target.write(tx)
    print("Wrote:", n, "bytes")
    write_pass = n == 4
    print("Status:", "PASS" if write_pass else "FAIL")

    # Signal master that write is done
    multitest.next()

    # Wait for master to be ready for write_readinto test
    multitest.next()

    # Test 3: Write and read simultaneously
    print("\n***** Test 3: Write+Read *****\n")
    tx = b"\x11\x22\x33\x44"
    rx = bytearray(4)
    n = target.write_readinto(tx, rx)
    print("Sent:", tx)
    print("Received:", rx[:n])
    readinto_pass = rx[:n] == b"\xde\xad\xbe\xef"
    print("Status:", "PASS" if readinto_pass else "FAIL")

    multitest.next()

    target.deinit()

    # Summary
    print("\n***** Target Summary *****")
    all_pass = read_pass and write_pass and readinto_pass
    print("Overall:", "PASS" if all_pass else "FAIL")


def instance1():
    # SPI Master (Controller)
    from machine import SPI, Pin

    print("\n***** SPI Master Test *****\n")

    # Keep the target deselected while the API is tested.
    cs = Pin(SSEL_PIN, Pin.OUT, value=1)

    _spi_api_test()

    spi = SPI(
        SPI_ID,
        baudrate=1_000_000,
        polarity=0,
        phase=0,
        bits=8,
        firstbit=SPI.MSB,
        sck=SCK_PIN,
        mosi=MOSI_PIN,
        miso=MISO_PIN,
    )
    print(spi)

    # Wait for target to be ready
    print("Waiting for target...")
    multitest.next()
    print("Target ready\n")

    # Test 1: Write to target
    print("***** Test 1: Write *****\n")
    tx = b"\x01\x02\x03\x04"
    cs(0)
    time.sleep_us(10)
    spi.write(tx)
    time.sleep_us(10)
    cs(1)
    print("Sent:", tx)
    print("Status: PASS")

    # Wait for target to finish reading
    multitest.next()

    # Test 2: Read from target
    print("\n***** Test 2: Read *****\n")
    # Signal target to start writing
    multitest.next()
    rx = bytearray(4)
    cs(0)
    time.sleep_us(10)
    spi.readinto(rx)
    time.sleep_us(10)
    cs(1)
    print("Received:", rx)
    read_pass = rx == b"\xaa\xbb\xcc\xdd"
    print("Status:", "PASS" if read_pass else "FAIL")

    # Wait for target to finish write
    multitest.next()

    # Test 3: Simultaneous write and read
    print("\n***** Test 3: Transfer *****\n")

    # Reconfigure the bus with init(): the transfer must still work afterwards.
    spi.init(1_000_000)
    print(spi)

    # Signal target to start write_readinto
    multitest.next()

    tx = b"\xde\xad\xbe\xef"
    rx = bytearray(4)
    cs(0)
    time.sleep_us(10)
    spi.write_readinto(tx, rx)
    time.sleep_us(10)
    cs(1)
    print("Sent:", tx)
    print("Received:", rx)
    transfer_pass = rx == b"\x11\x22\x33\x44"
    print("Status:", "PASS" if transfer_pass else "FAIL")

    multitest.next()

    spi.deinit()

    # Summary
    print("\n***** Master Summary *****")
    all_pass = read_pass and transfer_pass
    print("Overall:", "PASS" if all_pass else "FAIL")
