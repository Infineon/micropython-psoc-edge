"""ADC functional test.

Setup description:
- Connect P15_0 to GND.
- Connect P15_1 to the midpoint of a resistor divider made with two equal
  resistors between VCC_1.8 and GND.
- Connect P15_2 to VCC_1.8.

Expected voltages:
- P15_0: 0 V
- P15_1: about 0.9 V
- P15_2: 1.8 V

Do not use 3.3 V directly on these ADC pins in this setup.
"""

from machine import ADC, ADCBlock

adc_pin_gnd = "P15_0"
adc_pin_mid = "P15_1"
adc_pin_max = "P15_2"
adc_mid_chan = 1
adc_wrong_pin_name = "P13_7"

tolerance_uv_gnd = 20000
tolerance_uv_signal = 150000
tolerance_u16_gnd = 512
tolerance_u16_signal = 6000


def in_range(actual, expected, tolerance):
    return (expected - tolerance) <= actual <= (expected + tolerance)


try:
    ADC(adc_wrong_pin_name)
    print("invalid ADC pin: ", False)
except ValueError:
    print("invalid ADC pin: ", True)

try:
    ADC(adc_pin_mid, sample_ns=0)
    print("sample_ns validation: ", False)
except ValueError:
    print("sample_ns validation: ", True)

block = ADCBlock(0, bits=12)
adc = block.connect(adc_mid_chan)
print("ADCBlock.connect(channel): ", adc is not None)
adc.deinit()
block.deinit()

block = ADCBlock(0, bits=12)
adc = block.connect(adc_pin_mid)
print("ADCBlock.connect(pin): ", adc is not None)
adc.deinit()
block.deinit()

block = ADCBlock(0, bits=12)
try:
    block.connect(256)
    print("invalid ADC channel: ", False)
except ValueError:
    print("invalid ADC channel: ", True)
block.deinit()

block = ADCBlock(0, bits=12)
adc_mid = block.connect(adc_mid_chan, adc_pin_mid)
adc_gnd = ADC(adc_pin_gnd, sample_ns=1000)
adc_max = ADC(adc_pin_max, sample_ns=1000)

try:
    ADC(adc_pin_mid)
    print("duplicate channel open: ", False)
except ValueError:
    print("duplicate channel open: ", True)

try:
    adc_max.init(sample_ns=2000)
    print("conflicting sample_ns: ", False)
except ValueError:
    print("conflicting sample_ns: ", True)

print("ADC read_uv gnd: ", in_range(adc_gnd.read_uv(), 0, tolerance_uv_gnd))
print("ADC read_u16 gnd: ", in_range(adc_gnd.read_u16(), 0, tolerance_u16_gnd))

print("ADC read_uv mid: ", in_range(adc_mid.read_uv(), 900000, tolerance_uv_signal))
print("ADC read_u16 mid: ", in_range(adc_mid.read_u16(), 32768, tolerance_u16_signal))

print("ADC read_uv max: ", in_range(adc_max.read_uv(), 1800000, tolerance_uv_signal))
print("ADC read_u16 max: ", in_range(adc_max.read_u16(), 65535, tolerance_u16_signal))

adc_mid.deinit()
adc_gnd.deinit()
adc_max.deinit()
