from machine import Counter, Pin
import time


# Counter HIL test
"""
Setup: Connect P16_7 -> P11_1
"""

PIN_OUT = "P16_7"
PIN_IN = "P11_1"


# Helper function to generate pulses on a pin.
# n is the number of pulses, delay_us is the delay in microseconds between high and low states.
def pulse(pin, n, delay_us=100):
    for _ in range(n):
        pin(1)
        time.sleep_us(delay_us)
        pin(0)
        time.sleep_us(delay_us)


# Helper function to check if a value is close to an expected value within a tolerance.
def close_to(value, expected, tol=1):
    return (expected - tol) <= value <= (expected + tol)


def prime_counter(counter, pin):
    # Warm up the route so the first measured edge is not lost.
    pulse(pin, 1)
    counter.value(0)
    time.sleep_ms(2)


def expect_value_error(label, fn):
    try:
        fn()
    except ValueError as e:
        print(label, e)


def expect_not_implemented_error(label, fn):
    try:
        fn()
    except NotImplementedError as e:
        print(label, e)


def expect_runtime_error(label, fn):
    try:
        fn()
    except RuntimeError as e:
        print(label, e)


print("*****Counter tests*****")

pin_out = Pin(PIN_OUT, mode=Pin.OUT, value=0)

# Positive tests: rising/falling/down and value reset.
c = Counter(0, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP)
prime_counter(c, pin_out)
pulse(pin_out, 100)
print("rising_count:", close_to(c.value(), 100))
c.deinit()

c = Counter(1, src=Pin(PIN_IN), edge=Counter.FALLING, direction=Counter.UP)
prime_counter(c, pin_out)
pulse(pin_out, 80)
print("falling_count:", close_to(c.value(), 80))
c.deinit()

c = Counter(2, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.DOWN)
prime_counter(c, pin_out)
pulse(pin_out, 60)
print("down_count:", close_to(c.value(), -60))

c.value(0)
time.sleep_ms(2)
pulse(pin_out, 10)
print("value_reset:", close_to(c.value(), -10))
c.deinit()

# cycles() tests: get, set, and previous-value return semantics.
c = Counter(3, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP)
print("cycles_default:", c.cycles() == 0)
print("cycles_set_prev:", c.cycles(5) == 0)
print("cycles_after_set:", c.cycles() == 5)
c.deinit()

# max/min tests: custom range with min=0 supported.
c = Counter(4, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=9, min=0)
prime_counter(c, pin_out)
pulse(pin_out, 25)
print("range_max_value:", close_to(c.value(), 25))
print("range_max_cycles:", c.cycles() == 2)
c.deinit()

# min!=0 support: valid when 0 <= min < max.
c = Counter(5, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=9, min=2)
prime_counter(c, pin_out)
pulse(pin_out, 25)
print("range_min_nonzero_value:", close_to(c.value(), 25))
print("range_min_nonzero_cycles:", c.cycles() == 3)
c.deinit()

# Negative tests: boundary and input validation (ValueError paths).
expect_value_error("invalid_id_-1:", lambda: Counter(-1, src=Pin(PIN_IN)))
expect_value_error("invalid_id_32:", lambda: Counter(32, src=Pin(PIN_IN)))
expect_value_error("invalid_edge:", lambda: Counter(3, src=Pin(PIN_IN), edge=3))
expect_value_error("invalid_direction:", lambda: Counter(4, src=Pin(PIN_IN), direction=3))
expect_value_error("invalid_src_pin:", lambda: Counter(5, src=Pin(PIN_OUT)))
expect_value_error("invalid_max:", lambda: Counter(7, src=Pin(PIN_IN), max=-1))
expect_value_error("invalid_min_ge_max:", lambda: Counter(7, src=Pin(PIN_IN), max=10, min=10))

expect_not_implemented_error(
    "filter_ns_not_supported:", lambda: Counter(7, src=Pin(PIN_IN), filter_ns=100)
)

# index support test: using the same pin as src/index should trigger index callback on each rising edge.
c = Counter(7, src=Pin(PIN_IN), index=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP)
prime_counter(c, pin_out)
pulse(pin_out, 5)
print("index_active:", c.cycles() > 0)
c.deinit()

# reset support test: using same pin as src/reset should keep value near zero and cycles unchanged.
c = Counter(7, src=Pin(PIN_IN), reset=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP)
prime_counter(c, pin_out)
pulse(pin_out, 5)
print("reset_active:", abs(c.value()) <= 1 and c.cycles() == 0)
c.deinit()

# irq() tests: constants, trigger API, callback path, and validation.
c = Counter(8, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=3, min=0)
prime_counter(c, pin_out)
irq_count = 0


def on_rollover(_irq):
    global irq_count
    irq_count += 1


irq = c.irq(handler=on_rollover, trigger=Counter.IRQ_ROLL_OVER)
print("irq_obj_reuse:", irq is c.irq())
print("irq_trigger_get:", irq.trigger() == Counter.IRQ_ROLL_OVER)
print(
    "irq_trigger_set_prev:",
    irq.trigger(Counter.IRQ_ROLL_OVER | Counter.IRQ_INDEX) == Counter.IRQ_ROLL_OVER,
)
pulse(pin_out, 12)
time.sleep_ms(20)
print("irq_rollover_cb:", irq_count >= 2)
print("irq_flags_rollover:", (irq.flags() & Counter.IRQ_ROLL_OVER) != 0)
c.irq(handler=None, trigger=Counter.IRQ_ROLL_OVER)
c.deinit()

c = Counter(9, src=Pin(PIN_IN), index=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP)
prime_counter(c, pin_out)
index_irq_count = 0


def on_index(_irq):
    global index_irq_count
    index_irq_count += 1


c.irq(handler=on_index, trigger=Counter.IRQ_INDEX)
pulse(pin_out, 5)
time.sleep_ms(20)
print("irq_index_cb:", index_irq_count >= 3)
c.deinit()


def expect_bad_counter_irq_handler():
    c = Counter(10, src=Pin(PIN_IN))
    try:
        c.irq(handler=1)
    finally:
        c.deinit()


expect_value_error("irq_bad_handler:", expect_bad_counter_irq_handler)


def expect_unsupported_counter_irq_trigger():
    c = Counter(11, src=Pin(PIN_IN))
    try:
        c.irq(handler=lambda _irq: None, trigger=Counter.IRQ_MATCH)
    finally:
        c.deinit()


expect_value_error("irq_unsupported_trigger:", expect_unsupported_counter_irq_trigger)

c = Counter(12, src=Pin(PIN_IN))
c.deinit()
expect_runtime_error("irq_not_initialised:", lambda: c.irq())

expect_not_implemented_error("match_not_supported:", lambda: Counter(7, src=Pin(PIN_IN), match=10))
expect_not_implemented_error(
    "match_pin_not_supported:", lambda: Counter(7, src=Pin(PIN_IN), match_pin=Pin(PIN_OUT))
)

# Test duplicate counter ID. The second instantiation should raise a ValueError.
c_dup = Counter(6, src=Pin(PIN_IN))
try:
    expect_value_error("duplicate_counter_id:", lambda: Counter(6, src=Pin(PIN_IN)))
finally:
    c_dup.deinit()
    pin_out(0)
