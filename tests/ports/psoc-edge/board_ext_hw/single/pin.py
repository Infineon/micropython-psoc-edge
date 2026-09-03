# This test requires two pins connected together.
# Pin.IN and Pin.OPEN_DRAIN will use the other
# connected pin to validate its functionality.

from machine import Pin

# -- Pin.OUT --

# Set the connected pin as high-z to
# validate disconnected Pin.OUT functionality.
pin_in = Pin.cpu.P16_1
pin_in.init(mode=Pin.IN, pull=None)

# Validate initialization values
pin_out_name = "P16_0"
pin_out = Pin(pin_out_name, mode=Pin.OUT, value=True)

print("pin out initial value 1: ", pin_out.value() == 1)

pin_out.init(mode=Pin.OUT, value=False)
print("pin out initial value 0: ", pin_out.value() == 0)

# Validate basic API functions
pin_out(1)
print("pin out callable 1: ", pin_out() == 1)

pin_out(0)
print("pin out callable 0: ", pin_out() == 0)

pin_out.high()
print("pin out value high: ", pin_out.value() == 1)

pin_out.low()
print("pin out value low: ", pin_out.value() == 0)

pin_out.toggle()
print("pin out value toggled (0->1): ", pin_out.value() == 1)

pin_out.value(False)
print("pin out value 0: ", pin_out.value() == False)

pin_out.value(True)
print("pin out value 1: ", pin_out.value() == True)

# Validate mode + pull configuration
# Pin.OUT does not support pull resistors.
try:
    pin_out = Pin(pin_out_name, mode=Pin.OUT, pull=Pin.PULL_UP)
except ValueError as e:
    print(e)

try:
    pin_out = Pin(pin_out_name, mode=Pin.OUT, pull=Pin.PULL_DOWN)
except ValueError as e:
    print(e)

# -- Pin.IN --

# Set the out pin as input to validate disconnected
# Pin.IN functionality.
pin_out.init(mode=Pin.IN, pull=None)

# Validate floating values
pin_in = Pin.cpu.P16_1

pin_in.init(mode=Pin.IN, pull=None, value=0)
print("pin in with pull none initially high-z: ", pin_in.value() == 0 or pin_in.value() == 1)

pin_in.init(mode=Pin.IN, pull=Pin.PULL_UP)
print("pin in with pull up initially 1: ", pin_in.value() == 1)

pin_in.init(pull=Pin.PULL_DOWN)
print("pin in with pull down initially 0: ", pin_in.value() == 0)


# Validate connected values
pin_in.init(pull=None)

pin_out.init(mode=Pin.OUT, value=1)
print("pin in value 1: ", pin_in.value() == 1)

pin_out.value(0)
print("pin in value 0: ", pin_in() == 0)

pin_in.init(pull=Pin.PULL_UP)
pin_in(1)
print("pin in writes 1 but strong out drives 0: ", pin_in() == 0)
print("pin in pull is now pull up after writing 1: ", pin_in.pull() == Pin.PULL_UP)

pin_out(True)
pin_in.value(0)
print("pin in writes 0 but strong out drives 0: ", pin_in() == 1)
print("pin in pull is now pull down after writing 0: ", pin_in.pull() == Pin.PULL_DOWN)

# -- Pin.OPEN_DRAIN --

# Set the out pin as input to validate disconnected
# Pin.OPEN_DRAIN functionality.
pin_out.init(Pin.IN, None)

pin_od = pin_in

# Validate floating values

pin_od = Pin(pin_od, mode=Pin.OPEN_DRAIN)
print("pin od with pull none initially high-z: ", pin_od.value() == 0 or pin_od.value() == 1)

pin_od.init(mode=Pin.OPEN_DRAIN, pull=Pin.PULL_UP, value=1)
print("pin od with pull up initially 1: ", pin_od.value() == 1)

pin_od.init(mode=Pin.OPEN_DRAIN, pull=Pin.PULL_UP, value=0)
print("pin od with pull up initially 0: ", pin_od.value() == 0)

# Validate basic functionality

# OD pin reads
pin_out.init(Pin.OUT, value=0)
print("pin od with pull up but strong out drives 0: ", pin_od.value() == 0)

pin_out(1)
print("pin od with pull up but strong out drives 1: ", pin_od.value() == 1)

# OD drives
pin_in = pin_out
pin_in.init(Pin.IN, None)

pin_od(1)
print("pin od with pull up writes 1: ", pin_in() == 1)

pin_od(0)
print("pin od with pull up writes 0: ", pin_in() == 0)

# No pin sets a (resistor) path to VDD.
pin_in.init(Pin.IN, None)
pin_od.init(mode=Pin.OPEN_DRAIN, pull=None, value=0)

pin_od(1)
print("pin od with pull none writes 1: ", pin_in() == 1 or pin_in() == 0)

pin_od(0)
print("pin od with pull none writes 0: ", pin_in() == 0)


# Pin.OPEN_DRAIN does not support pull down resistors.
try:
    pin_od = Pin(pin_od, mode=Pin.OPEN_DRAIN, pull=Pin.PULL_DOWN)
except ValueError as e:
    print(e)


# -- Validate config setters/getters --

pin_out.mode(Pin.OPEN_DRAIN)
pin_out.pull(Pin.PULL_UP)
pin_out.drive(Pin.DRIVE_4)
print("pin out set mode: ", pin_out.mode() == Pin.OPEN_DRAIN)
print("pin out set pull: ", pin_out.pull() == Pin.PULL_UP)
print("pin out set drive: ", pin_out.drive() == Pin.DRIVE_4)
