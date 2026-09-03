from machine import Pin
import time

# Two pins are connected together.
# An output pin triggers the interrupt.
# An input pin is configured to receive the interrupt
pin_trigger = Pin("P16_0", mode=Pin.OUT, value=True)
pin_irq = Pin("P16_1", mode=Pin.IN)


def irq_handler(pin):
    print("irq")


# Configure the IRQ handler to trigger on falling edge
# The handler should be called twice
irq = pin_irq.irq(irq_handler, trigger=Pin.IRQ_FALLING)
pin_trigger(0)
pin_trigger(1)
pin_trigger(0)
# Wait for the interrupts to be processed
time.sleep_ms(50)
print("irq flag falling:", irq.flags() == Pin.IRQ_FALLING)


# Now it is configured to trigger on rising edge.
# The handler should be called twice
irq.trigger(Pin.IRQ_RISING)
pin_trigger(0)
pin_trigger(1)
time.sleep_ms(50)
pin_trigger(0)
pin_trigger(1)
time.sleep_ms(50)
print("irq flag rising:", irq.flags() == Pin.IRQ_RISING)


# Test hard IRQ
# Use a pre-allocated flag — print() from hard IRQ context calls the UART
# driver which uses a FreeRTOS semaphore (invalid in ISR context).
_hard_irq_seen = [False]


def irq_handler_hard(pin):
    _hard_irq_seen[0] = True


irq = pin_irq.irq(irq_handler_hard, trigger=Pin.IRQ_FALLING, hard=True)
pin_trigger(0)
time.sleep_ms(10)
if _hard_irq_seen[0]:
    print("irq hard")


# Trigger set to None, should not trigger any interrupt.
pin_irq.irq(None)
pin_trigger(1)
pin_trigger(0)
pin_trigger(1)
pin_trigger(0)


# Invalid trigger
try:
    irq.trigger(0x35)
except ValueError as e:
    print("error:", e)
