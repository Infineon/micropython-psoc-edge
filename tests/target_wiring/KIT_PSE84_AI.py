# Target wiring for KIT_PSE84_AI.

# UART(5) is on P17_1/P17_0.
uart_loopback_args = ()
uart_loopback_kwargs = {"tx": "P17_1", "rx": "P17_0"}

i2c_loopback_controller_kwargs = {"scl": "P17_2", "sda": "P17_3"}
i2c_loopback_target_args = (5, ) # on pins P17_0/P17_1
i2c_loopback_target_kwargs = {}

