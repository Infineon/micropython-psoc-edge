# ADC test
# Setup (single board):
#   - Build a divider with R1=10k and R2=10k.
#   - Connect R1 top to board VDD rail (1.8V or 3.3V).
#   - Connect R2 bottom to GND.
#   - Connect divider midpoint to adc_pin_mid.
#   - Connect adc_pin_max directly to the same VDD rail.
#   - Connect adc_pin_gnd directly to GND.
#
# Expected behavior:
#   - adc_pin_gnd reads near 0V.
#   - adc_pin_mid reads about half of adc_pin_max.
#   - adc_pin_max reads near full-scale.

from machine import ADC, ADCBlock
import time


adc_pin_gnd = "P15_3"
adc_pin_mid = "P15_1"
adc_pin_max = "P15_2"
adc_mid_chan = 1
adc_wrong_pin_name = "P13_7"


def avg_read(adc_obj, count=8):
    sum_uv = 0
    sum_raw = 0
    for _ in range(count):
        sum_uv += adc_obj.read_uv()
        sum_raw += adc_obj.read_u16()
        time.sleep_ms(5)
    return sum_uv // count, sum_raw // count


print("*****ADC tests*****")

# Invalid pin check.
invalid_pin_rejected = False
try:
    ADC(adc_wrong_pin_name)
except ValueError:
    invalid_pin_rejected = True
print("invalid_pin_rejected:", invalid_pin_rejected)

# ADCBlock.connect variants.
blk = ADCBlock(0, bits=12)
adc_by_ch = blk.connect(adc_mid_chan)
adc_by_pin = blk.connect(adc_pin_mid)
adc_by_both = blk.connect(adc_mid_chan, adc_pin_mid)
print("connect_channel_ok:", adc_by_ch is not None)
print("connect_pin_ok:", adc_by_pin is not None)
print("connect_channel_pin_ok:", adc_by_both is not None)

# Voltage checks.
adc_gnd = ADC(adc_pin_gnd)
adc_mid = ADC(adc_pin_mid)
adc_max = ADC(adc_pin_max)
time.sleep_ms(20)

g_uv, g_raw = avg_read(adc_gnd)
m_uv, m_raw = avg_read(adc_mid)
x_uv, x_raw = avg_read(adc_max)

# Keep tolerances broad enough for board-to-board variation.
gnd_uv_ok = g_uv <= 200000
gnd_raw_ok = g_raw <= 5000

if x_uv > 0:
    mid_ratio = m_uv / x_uv
else:
    mid_ratio = 0.0
mid_uv_ratio_ok = 0.40 <= mid_ratio <= 0.60

# Midpoint should also be close to half-scale in read_u16.
mid_raw_half_ok = abs(m_raw - 32768) <= 8000

# MAX should be near top of range when tied to VDD.
max_raw_high_ok = x_raw >= 58000

# Ordering sanity check.
monotonic_ok = g_uv < m_uv < x_uv

print("gnd_uv_ok:", gnd_uv_ok)
print("gnd_raw_ok:", gnd_raw_ok)
print("mid_uv_ratio_ok:", mid_uv_ratio_ok)
print("mid_raw_half_ok:", mid_raw_half_ok)
print("max_raw_high_ok:", max_raw_high_ok)
print("monotonic_ok:", monotonic_ok)

# Cleanup.
adc_by_ch.deinit()
adc_by_pin.deinit()
adc_by_both.deinit()
adc_gnd.deinit()
adc_mid.deinit()
adc_max.deinit()
print("cleanup_ok:", True)
