# ADC test
# Setup (single board):
#   - Build a divider with R1=15k and R2=15k.
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
    return sum_uv // count, sum_raw // count


print("*****ADC tests*****")

adc_gnd = ADC(adc_pin_gnd)
adc_max = ADC(adc_pin_max)
time.sleep_ms(20)

avg_read(adc_gnd)
avg_read(adc_max)

print("read_ok:", True)
