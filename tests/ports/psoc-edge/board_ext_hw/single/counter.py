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

# Negative tests: boundary and input validation (ValueError paths).
expect_value_error("invalid_id_-1:", lambda: Counter(-1, src=Pin(PIN_IN)))
expect_value_error("invalid_id_32:", lambda: Counter(32, src=Pin(PIN_IN)))
expect_value_error("invalid_edge:", lambda: Counter(3, src=Pin(PIN_IN), edge=3))
expect_value_error("invalid_direction:", lambda: Counter(4, src=Pin(PIN_IN), direction=3))
expect_value_error("invalid_src_pin:", lambda: Counter(5, src=Pin(PIN_OUT)))

# Test duplicate counter ID. The second instantiation should raise a ValueError.
c_dup = Counter(6, src=Pin(PIN_IN))
try:
    expect_value_error("duplicate_counter_id:", lambda: Counter(6, src=Pin(PIN_IN)))
finally:
    c_dup.deinit()
    pin_out(0)
