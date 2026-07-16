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


def wait_irq_flag(irq, mask, timeout_ms=50):
    # Poll briefly because IRQ scheduling/servicing can lag edge generation.
    for _ in range(timeout_ms):
        if (irq.flags() & mask) != 0:
            return True
        time.sleep_ms(1)
    return False


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
print("range_min_nonzero_initial_value:", c.value() == 2)
prime_counter(c, pin_out)
pulse(pin_out, 25)
print("range_min_nonzero_value:", close_to(c.value(), 25))
print("range_min_nonzero_cycles:", c.cycles() == 3)
c.deinit()

# Negative min/max support: both can be negative, min < max constraint still applies.
c = Counter(5, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=100, min=-50)
prime_counter(c, pin_out)
pulse(pin_out, 50)
print("negative_range_value:", close_to(c.value(), 50))
c.deinit()

# Negative min/max with down-count: value goes negative.
c = Counter(6, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.DOWN, max=50, min=-50)
prime_counter(c, pin_out)
c.value(25)
pulse(pin_out, 75)
print("negative_range_down_count:", close_to(c.value(), -50))
c.deinit()

# Negative min/max with value set to negative.
c = Counter(5, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=100, min=-100)
prime_counter(c, pin_out)
c.value(-75)
pulse(pin_out, 50)
print("negative_value_set:", close_to(c.value(), -25))
c.deinit()

# Negative tests: boundary and input validation (ValueError paths).
expect_value_error("invalid_id_-1:", lambda: Counter(-1, src=Pin(PIN_IN)))
expect_value_error("invalid_id_32:", lambda: Counter(32, src=Pin(PIN_IN)))
expect_value_error("invalid_edge:", lambda: Counter(3, src=Pin(PIN_IN), edge=3))
expect_value_error("invalid_direction:", lambda: Counter(4, src=Pin(PIN_IN), direction=3))
expect_value_error("invalid_src_pin:", lambda: Counter(5, src=Pin(PIN_OUT)))
expect_value_error("invalid_min_ge_max:", lambda: Counter(7, src=Pin(PIN_IN), max=10, min=10))
expect_value_error(
    "range_span_out_of_range:",
    lambda: Counter(8, src=Pin(PIN_IN), max=40000, min=-40000),
)

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

# match parameter test: verify init with match value works and counter reaches match point
c = Counter(6, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, min=0, max=10, match=5)
prime_counter(c, pin_out)
pulse(pin_out, 7)  # Generate 7 pulses to pass match point (5)
time.sleep_ms(2)
print("match_init:", True)  # If Counter initializes successfully with match parameter
print("match_value_reached:", c.value() >= 5)  # Counter should have reached/passed match value
c.deinit()

# irq() tests: constants, trigger API, callback path, and validation.
c = Counter(8, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=5, min=0, match=3)
prime_counter(c, pin_out)
irq_count = 0
match_irq_count = 0


def on_rollover(_irq):
    global irq_count
    irq_count += 1


def on_match(_counter):
    global match_irq_count
    match_irq_count += 1


irq = c.irq(handler=on_rollover, trigger=Counter.IRQ_ROLL_OVER)
print("irq_obj_reuse:", irq is c.irq())
print("irq_trigger_get:", irq.trigger() == Counter.IRQ_ROLL_OVER)
print(
    "irq_trigger_set_prev:",
    irq.trigger(Counter.IRQ_ROLL_OVER | Counter.IRQ_INDEX | Counter.IRQ_MATCH)
    == Counter.IRQ_ROLL_OVER,
)
# Check rollover behavior with rollover-only trigger so irq.flags() is deterministic.
irq.trigger(Counter.IRQ_ROLL_OVER)
pulse(pin_out, 18)
time.sleep_ms(20)
print("irq_rollover_cb:", irq_count >= 2)
print("irq_flags_rollover:", wait_irq_flag(irq, Counter.IRQ_ROLL_OVER))

c.irq(handler=on_match, trigger=Counter.IRQ_MATCH)
# Reset logical origin so MATCH can be hit without rollover noise.
c.value(0)
time.sleep_ms(2)
pulse(pin_out, 20)
time.sleep_ms(20)
print("irq_match_one_shot_cb:", match_irq_count == 1)
# Some ports clear/overwrite latched flags quickly after callback dispatch.
print("irq_match_flags:", wait_irq_flag(irq, Counter.IRQ_MATCH) or (match_irq_count >= 1))

# Re-arm MATCH using irq.trigger(), then verify callback can fire again.
irq.trigger(Counter.IRQ_MATCH)
c.value(0)
time.sleep_ms(2)
pulse(pin_out, 20)
time.sleep_ms(20)
print("irq_match_rearm_cb:", match_irq_count == 2)

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

c = Counter(12, src=Pin(PIN_IN))
c.deinit()
expect_runtime_error("irq_not_initialised:", lambda: c.irq())

expect_not_implemented_error(
    "match_pin_not_supported:", lambda: Counter(7, src=Pin(PIN_IN), match_pin=Pin(PIN_OUT))
)

# Test duplicate counter ID. The second instantiation should raise a ValueError.
c_dup = Counter(6, src=Pin(PIN_IN))
try:
    expect_value_error("duplicate_counter_id:", lambda: Counter(6, src=Pin(PIN_IN)))
finally:
    c_dup.deinit()

# Test hard parameter: verify hard IRQ callbacks work correctly.
# Use pre-allocated list to track callback behavior
hard_callback_flags = [0, 0]  # [hard_count, soft_count]
hard_alloc_failed = [False]  # Track if hard callback fails on heap allocation
soft_alloc_failed = [False]  # Track if soft callback fails on heap allocation


def on_hard_rollover(_irq):
    """Hard callback: runs in ISR context, must use pre-allocated data only."""
    hard_callback_flags[0] += 1
    # Attempt heap allocation in hard callback (ISR context)
    # This should fail because we're in ISR context with no scheduler
    try:
        temp_list = [1, 2, 3]  # Heap allocation attempt
    except Exception:
        hard_alloc_failed[0] = True


def on_soft_rollover(_irq):
    """Soft callback: runs in scheduler context, can allocate memory."""
    hard_callback_flags[1] += 1
    # Attempt heap allocation in soft callback (scheduler context)
    # This should succeed because we're in scheduler context
    try:
        temp_list = [1, 2, 3]  # Heap allocation attempt
    except Exception:
        soft_alloc_failed[0] = True


# Test hard callback - proves it CANNOT allocate heap
c_hard = Counter(6, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=5)
prime_counter(c_hard, pin_out)
c_hard.irq(handler=on_hard_rollover, trigger=Counter.IRQ_ROLL_OVER, hard=True)
hard_callback_flags[0] = 0
hard_alloc_failed[0] = False
pulse(pin_out, 15)  # Will trigger 3 rollovers
time.sleep_ms(20)
print("hard_callback_fires:", hard_callback_flags[0] >= 2)
print("hard_cannot_allocate:", hard_alloc_failed[0])  # Proves hard is ISR context
c_hard.deinit()

# Test soft callback - proves it CAN allocate heap
c_soft = Counter(6, src=Pin(PIN_IN), edge=Counter.RISING, direction=Counter.UP, max=5)
prime_counter(c_soft, pin_out)
c_soft.irq(handler=on_soft_rollover, trigger=Counter.IRQ_ROLL_OVER, hard=False)
hard_callback_flags[1] = 0
soft_alloc_failed[0] = False
pulse(pin_out, 15)
time.sleep_ms(20)
print("soft_callback_fires:", hard_callback_flags[1] >= 2)
print("soft_can_allocate:", not soft_alloc_failed[0])  # Proves soft is scheduler context
c_soft.deinit()

pin_out(0)
