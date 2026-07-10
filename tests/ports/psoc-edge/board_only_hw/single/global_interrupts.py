from machine import Timer
import machine
import time

fired = False


def _cb(_):
    global fired
    fired = True


irq_state = machine.disable_irq()
tim = Timer(0, period=20, mode=Timer.ONE_SHOT, callback=_cb)

# Keep IRQs masked long enough for the one-shot period to elapse.
for _ in range(500000):
    pass

# Callback should still be blocked while IRQs are disabled.
print("while_disabled:", fired)

machine.enable_irq(irq_state)
time.sleep_ms(200)

# Pending callback should run after restoring IRQs.
print("after_enable:", fired)

tim.deinit()
