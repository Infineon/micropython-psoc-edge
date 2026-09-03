# Pin Connections for HIL Test Setup Single Board

| Test Name         | Board 1 Pin   | Board 1 Pin   | Description                    |
|-------------------|---------------|---------------|--------------------------------|
| Pin               | P16_0         | P16_1         | Output to Input                |
|                   |               |               |                                |
| PWM               | P16_1         | P16_0         | PWM Output to IO Input         |
|                   |               |               |                                |
| UART              | P17_0         | P17_1         | UART RX to TX                  |
|                   | P16_5         | P16_6         | UART CTS to RTS                |
|                   |               |               |                                |
| Counter           | P16_7         | P11_1         | GPIO pulse output to Counter input |
|                   |               |               |                                |
| ADC               | P15_3         | GND           | ADC input to GND               |
|                   | P15_2         | VDD           | ADC input to VDD               |
|                   | P15_1         | Divider midpoint | ADC input to divider midpoint  |

ADC divider note: use R1=15k from VDD to P15_1, and R2=15k from P15_1 to GND.

# Pin Connections for HIL Test Setup Between 2 Boards

| Test Name         | Board 1 Pin   | Board 2 Pin   | Description                    |
|-------------------|---------------|---------------|--------------------------------|
| Bitstream         | P16_0         | P16_0         | Bitstream TX to RX             |
|                   | GND           | GND           | Common Ground                  |
|                   |               |               |                                |
| I2C               | P17_0         | P17_0         | I2C SCL to SCL (with pullup)   |
|                   | P17_1         | P17_1         | I2C SDA to SDA (with pullup)   |
|                   | GND           | GND           | Common Ground                  |
|                   |               |               |                                |
| SPI               | P16_0         | P16_0         | SPI SCK to SCK                 |
|                   | P16_1         | P16_1         | SPI MOSI to MOSI               |
|                   | P16_2         | P16_2         | SPI MISO to MISO               |
|                   | P16_3         | P16_3         | SPI SSEL to CS out             |
|                   | GND           | GND           | Common Ground                  |
|                   |               |               |                                |
| time_pulse_us     | P16_0         | P16_0         | Pulse out to pulse in          |
|                   | GND           | GND           | Common Ground                  |
