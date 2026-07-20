from operator import add
import os
import re
import sys
from collections import defaultdict, namedtuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../../tools"))
import boardgen

SUPPORTED_AF = {
    "I2C": ["SDA", "SCL"],
    "UART": ["TX", "RX", "CTS", "RTS"],
    "SPI": ["CLK", "MOSI", "MISO", "SELECT0", "SELECT1"],
    "PDM": ["CLK", "DATA"],
    "TCPWM": ["LINE"],
    "PERI_TR_IO": ["INPUT", "OUTPUT"],
    # TODO: Other active functionalities that we need to figure out:
    # - TDM
    # - SMIF
    # - CANFD
    # - PERIx_TR_IO
    # - SDHC
    # - ETH
    # - GFXSS
    # - DEBUG
    # - I3C
    # - M33SYSCPUSS
    # - SRSS_EXT_CLK
}

# Contains the result of parsing an cell from af.csv.
PinAf = namedtuple(
    "PinAf",
    [
        "af_idx",  # int, 0-15
        "af_fn",  # e.g. "I2C"
        "af_unit",  # int, e.g. 1 (for I2C1) or None (for OTG_HS_ULPI_CK)
        "af_signal",  # e.g. "SDA"
        "af_supported",  # bool, see table above
        "af_name",  # e.g. "ACT_x"
        "af_ptr",  # SCBx or <peripheral_instance>
    ],
)


class PSE84Pin(boardgen.Pin):
    def __init__(self, cpu_pin_name):
        super().__init__(cpu_pin_name)

        # The pins are name like PX_Y, where X is the port number and Y is the pin number.
        # Each of the numbers can be multiple digits.
        # The port are the characters after 'P' and before '_'
        self._port = int(cpu_pin_name[1 : cpu_pin_name.index("_")])
        # The pin number is the characters after '_'
        self._pin = int(cpu_pin_name[cpu_pin_name.index("_") + 1 :])

        # List of PinAF instances
        self._afs = []
        # Optional Counter source routing metadata parsed from PERI0_TR_IO_INPUTx AFs.
        self._counter_src = None
        # ADC mapping: (block_id, channel_id) or None
        self._adc_map = None

    def definition(self):
        return f"PIN({self._port}, {self._pin}, pin_{self.name()}_af)"

    def add_af_scb(self, af_idx, af_name, af):
        # The SCB alternate function follow the
        # convention:
        # SCB<unit>_<function>_<signal_type>

        # The peripheral pointer is the first prefix including
        # number, e.g. SCB0
        af_ptr = af.split("_")[0]
        # The unit is the number after "SCB" and before "_".
        # For example for SCB0 -> 0.
        af_unit = af.split("_")[0][3:]  # After 'SCB'
        # The second token is the function.
        # For example I2C, SPI, UART
        af_fn = af.split("_")[1]
        # The last token is the signal signal type for the
        # given function, e.g. SDA, SCL, MOSI
        af_signal = af.split("_")[2]

        af_supported = af_fn in SUPPORTED_AF and af_signal in SUPPORTED_AF[af_fn]

        pin_af = PinAf(af_idx, af_fn, af_unit, af_signal, af_supported, af_name, af_ptr)
        self._afs.append(pin_af)

    def add_af_pdm(self, af_idx, af_name, af):
        # The PDM alternate function follow
        # the convention:
        # PDM_PDM_<signal><channel>

        af_ptr = (
            af.split("_")[0] + "0"
        )  # The peripheral is PDM0 (this index is missing in .csv definition)
        af_fn = af.split("_")[1]

        # Split the signal+channel part into alphabetic (signal) and numeric (unit) parts
        signal_channel = af.split("_")[2]

        # Find where the numeric part starts
        match = re.match(r"^([A-Za-z]+)(\d+)$", signal_channel)

        if match:
            af_signal = match.group(1)  # Alphabetic part (e.g., "CLK", "DATA")
            af_unit = match.group(2)  # Numeric part (e.g., "3", "15", "123")
        else:
            # Fallback for unexpected format
            af_signal = signal_channel
            af_unit = None

        af_supported = af_fn in SUPPORTED_AF and af_signal in SUPPORTED_AF[af_fn]

        pin_af = PinAf(af_idx, af_fn, af_unit, af_signal, af_supported, af_name, af_ptr)
        self._afs.append(pin_af)

    def add_af_tcpwm(self, af_idx, af_name, af):
        # The TCPWM alternate functions can appear directly as TCPWM0_LINE<n>
        # or as a suffix after another function, e.g. SMIFx_..._TCPWM0_LINE<n>.
        match = re.search(r"(TCPWM\d+)_(LINE(?:_COMPL)?)(\d+)$", af)
        if not match:
            return

        af_ptr = match.group(1)
        af_fn = "TCPWM"
        af_signal = match.group(2)
        af_unit = match.group(3)

        af_supported = af_fn in SUPPORTED_AF and af_signal in SUPPORTED_AF[af_fn]

        pin_af = PinAf(af_idx, af_fn, af_unit, af_signal, af_supported, af_name, af_ptr)
        self._afs.append(pin_af)

    def add_af_peri_tr_io(self, af_idx, af_name, af):
        # Map PERIx_TR_IO_INPUT/OUTPUTy to regular pin AF objects so consumers can
        # validate capability through the shared AF metadata path.
        # Matches patterns like: PERI0_TR_IO_INPUT4, PERI1_TR_IO_OUTPUT1
        match = re.match(r"^(PERI\d+)_TR_IO_(INPUT|OUTPUT)(\d+)$", af)
        if not match:
            return

        af_ptr = match.group(1)  # e.g., "PERI0", "PERI1"
        af_signal = match.group(2)  # e.g., "INPUT", "OUTPUT"
        af_unit = match.group(3)  # e.g., "4", "1"
        af_fn = "PERI_TR_IO"
        af_supported = af_fn in SUPPORTED_AF and af_signal in SUPPORTED_AF[af_fn]

        pin_af = PinAf(af_idx, af_fn, af_unit, af_signal, af_supported, af_name, af_ptr)
        self._afs.append(pin_af)

    def add_af(self, af_idx, af_name, af):
        # Handle ADC entries (usually from a special "ADC" column)
        if af_name == "ADC":
            # Delegate to generator to track ADC
            self._adc_map = self._generator.parse_adc_label(af)
            return

        # The AF index matches the column index for the ACTx functions 0-15
        # while for DSx functions the columns 16-19 are mapped to DS2-DS5 respectively.
        af_act_max_idx = 15
        af_ds_num = 4

        if af_idx > af_act_max_idx + af_ds_num:
            return

        if af_idx <= af_act_max_idx:
            if af_name != "ACT_{:d}".format(af_idx):
                raise boardgen.PinGeneratorError(
                    "Invalid AF column name '{:s}' for AF index {:d}.".format(af_name, af_idx)
                )

        if af_idx > af_act_max_idx:
            format_idx = (
                af_idx - af_act_max_idx + 1
            )  # The starting index is 2, so we need to subtract
            # the max ACTx index and add 1 to get the correct DS index.
            if af_name != "DS_{:d}".format(format_idx):
                raise boardgen.PinGeneratorError(
                    "Invalid AF column name '{:s}' for AF index {:d}.".format(af_name, af_idx)
                )
        # TODO: Review implementation.
        # The names have the following patterns:
        #
        # <peripheral/block#>_<function/protocol>_<signal/line/channel#>
        #
        # As this is not very consistent we will handle each case
        # separately and as we improve our understanding we will update
        # this code accordingly.
        # Currently we are using the definition in the GPIO_PSE84_BGA_220
        # file which has manually added to the boards/pse84x_af.csv.
        # But ideally this could be also automatically generated if more
        # PSE devices need to be supported in the future.

        # If the prefix match the format SBCx
        if af.startswith("SCB"):
            self.add_af_scb(af_idx, af_name, af)
        elif af.startswith("PDM"):
            self.add_af_pdm(af_idx, af_name, af)
        elif "TCPWM" in af:
            self.add_af_tcpwm(af_idx, af_name, af)
        elif af.startswith("PERI"):
            self.add_af_peri_tr_io(af_idx, af_name, af)
        else:
            # TODO: Extend the parsing to other peripherals.
            pass

    # This will be called at the start of the output (after the prefix). Use
    # it to emit the af objects (via the AF() macro).
    def print_source(self, out_source):
        print(file=out_source)
        print("const machine_pin_af_obj_t pin_{:s}_af[] = {{".format(self.name()), file=out_source)
        for af in self._afs:
            if af.af_supported:
                print("    ", end="", file=out_source)
            else:
                print("    // ", end="", file=out_source)
            # AF(af_name_idx, af_fn, af_unit, af_signal, af_ptr)
            print(
                "AF({:s}, {:s}, {:s}, {:s}, {:s}),  // {:s}_{:s}{:s}_{:s}".format(
                    af.af_name,
                    af.af_fn,
                    af.af_unit or 0,
                    af.af_signal or "NONE",
                    af.af_ptr,
                    af.af_ptr,
                    af.af_fn,
                    af.af_unit,
                    af.af_signal,
                ),
                file=out_source,
            )
        print("};", file=out_source)


class PSE84PinGenerator(boardgen.PinGenerator):
    def __init__(self):
        super().__init__(pin_type=PSE84Pin, enable_af=True)
        self._unhidden_ports = []
        self._port_max_index = 0

        self._unhidden_scb = []
        self._scb_max_index = 0

        self._unhidden_i2c = []
        self._unhidden_spi = []
        self._unhidden_uart = []

        # TCPWM LINE tracking
        self._pwm_pin_count = 0  # unique exposed pins with a TCPWM LINE AF
        self._tcpwm_counter_max = 0  # highest counter number seen across all LINE AFs
        self._tcpwm_counters = []  # sorted unique TCPWM counter IDs seen in LINE AFs

        # Trigger routing
        self._unhidden_peri0_tr_io_input = []
        self._unhidden_peri0_tr_io_output = []
        self._peri0_tr_io_input_max_index = 0
        self._peri0_tr_io_output_max_index = 0

        self._unhidden_peri1_tr_io_input = []
        self._unhidden_peri1_tr_io_output = []
        self._peri1_tr_io_input_max_index = 0
        self._peri1_tr_io_output_max_index = 0

        # ADC tracking: dict of (block, channel) -> pin object
        self._adc_pin_map = {}

    @staticmethod
    def parse_adc_label(label):
        """Parse ADC label like 'ADCBLOCK0_CH3' -> (0, 3) or None if invalid."""
        label = label.strip()
        if not label:
            return None
        match = re.match(r"^ADCBLOCK(\d+)_CH(\d+)$", label)
        if match:
            return (int(match.group(1)), int(match.group(2)))
        return None

    def add_adc(self):
        """Collect ADC mappings from all available pins."""
        for pin in self.available_pins():
            if pin._adc_map is None:
                continue
            block_id, channel_id = pin._adc_map
            key = (block_id, channel_id)
            pin_loc = (pin._port, pin._pin)

            # Check for conflicts
            if key in self._adc_pin_map:
                old_pin = self._adc_pin_map[key]
                old_loc = (old_pin._port, old_pin._pin)
                if old_loc != pin_loc:
                    raise boardgen.PinGeneratorError(
                        f"ADC conflict: block {block_id} ch {channel_id} assigned to "
                        f"multiple pins ({old_loc} and {pin_loc})"
                    )

            self._adc_pin_map[key] = pin

    def _adc_dims(self):
        if not self._adc_pin_map:
            return (1, 1)

        max_block = max(block_id for block_id, _ in self._adc_pin_map.keys())
        max_channel = max(channel_id for _, channel_id in self._adc_pin_map.keys())
        return (max_block + 1, max_channel + 1)

    def print_adc_tables_source(self, out_source):
        max_blocks, max_channels = self._adc_dims()

        print(file=out_source)
        print("// ADC block/channel lookup table generated from board pin map.", file=out_source)
        print(
            "const machine_pin_obj_t *const "
            f"machine_adc_block_pins[{max_blocks}][{max_channels}] = {{",
            file=out_source,
        )

        if self._adc_pin_map:
            by_block = defaultdict(dict)
            for (block_id, channel_id), pin in self._adc_pin_map.items():
                by_block[block_id][channel_id] = pin

            for block_id in sorted(by_block.keys()):
                print(f"    [{block_id}] = {{", file=out_source)
                for channel_id in sorted(by_block[block_id].keys()):
                    pin = by_block[block_id][channel_id]
                    print(f"        [{channel_id}] = &pin_{pin.name()}_obj,", file=out_source)
                print("    },", file=out_source)
        else:
            print("    [0] = {", file=out_source)
            print("        [0] = NULL,", file=out_source)
            print("    },", file=out_source)

        print("};", file=out_source)

    def print_source(self, out_source):
        super().print_source(out_source)
        self.print_adc_tables_source(out_source)

    # Collect all unhidden ports from the available
    # pins.
    # This function can be only used after parse_board_csv()
    # has run, as the available_pin generator is constructed
    # there.
    def add_ports(self):
        for pin in self.available_pins():
            if pin._port > self._port_max_index:
                self._port_max_index = pin._port
            if pin._port not in self._unhidden_ports and not pin._hidden:
                self._unhidden_ports.append(pin._port)

    # Collect all unhidden SCB units from the available pins.
    # And the each of the potential available I2C, SPI and UART units
    # It will not verify that all signals are available for a given unit.
    # Therefore, the actual amount of available units for each protocol
    # may be lower than the one collected here.
    def add_scbs(self):
        for pin in self.available_pins():
            if not pin._hidden:
                for af in pin._afs:
                    if af.af_ptr.startswith("SCB"):
                        if af.af_unit not in self._unhidden_scb:
                            self._unhidden_scb.append(af.af_unit)
                            if int(af.af_unit) > self._scb_max_index:
                                self._scb_max_index = int(af.af_unit)

                        if af.af_fn == "I2C":
                            if af.af_unit not in self._unhidden_i2c:
                                self._unhidden_i2c.append(af.af_unit)
                        elif af.af_fn == "SPI":
                            if af.af_unit not in self._unhidden_spi:
                                self._unhidden_spi.append(af.af_unit)
                        elif af.af_fn == "UART":
                            if af.af_unit not in self._unhidden_uart:
                                self._unhidden_uart.append(af.af_unit)

        self._unhidden_scb.sort(key=int)
        self._unhidden_i2c.sort(key=int)
        self._unhidden_spi.sort(key=int)
        self._unhidden_uart.sort(key=int)

    # Collect TCPWM LINE statistics across all available pins:
    # - count of unique TCPWM counter IDs visible through at least one LINE AF
    #   (this equals the maximum number of concurrent PWM instances)
    # - highest counter number seen (used to compute compact array size)
    def add_tcpwm(self):
        seen_counters = set()
        for pin in self.available_pins():
            for af in pin._afs:
                if af.af_fn == "TCPWM" and af.af_signal == "LINE" and af.af_supported:
                    counter_num = int(af.af_unit)
                    seen_counters.add(counter_num)
                    if counter_num > self._tcpwm_counter_max:
                        self._tcpwm_counter_max = counter_num
        self._tcpwm_counters = sorted(seen_counters)
        self._pwm_pin_count = len(seen_counters)

    def add_peri_tr_ios(self):
        for pin in self.available_pins(exclude_hidden=True):
            for af in pin._afs:
                if af.af_fn == "PERI_TR_IO":
                    if af.af_ptr == "PERI0":
                        if af.af_signal == "INPUT":
                            if af.af_unit not in self._unhidden_peri0_tr_io_input:
                                self._unhidden_peri0_tr_io_input.append(af.af_unit)
                                if int(af.af_unit) > self._peri0_tr_io_input_max_index:
                                    self._peri0_tr_io_input_max_index = int(af.af_unit)
                        elif af.af_signal == "OUTPUT":
                            if af.af_unit not in self._unhidden_peri0_tr_io_output:
                                self._unhidden_peri0_tr_io_output.append(af.af_unit)
                                if int(af.af_unit) > self._peri0_tr_io_output_max_index:
                                    self._peri0_tr_io_output_max_index = int(af.af_unit)
                    elif af.af_ptr == "PERI1":
                        if af.af_signal == "INPUT":
                            if af.af_unit not in self._unhidden_peri1_tr_io_input:
                                self._unhidden_peri1_tr_io_input.append(af.af_unit)
                                if int(af.af_unit) > self._peri1_tr_io_input_max_index:
                                    self._peri1_tr_io_input_max_index = int(af.af_unit)
                        elif af.af_signal == "OUTPUT":
                            if af.af_unit not in self._unhidden_peri1_tr_io_output:
                                self._unhidden_peri1_tr_io_output.append(af.af_unit)
                                if int(af.af_unit) > self._peri1_tr_io_output_max_index:
                                    self._peri1_tr_io_output_max_index = int(af.af_unit)

        self._unhidden_peri0_tr_io_input.sort(key=int)
        self._unhidden_peri0_tr_io_output.sort(key=int)
        self._unhidden_peri1_tr_io_input.sort(key=int)
        self._unhidden_peri1_tr_io_output.sort(key=int)

    # Override the parse_board_csv to add
    # the unhidden ports after parsing the board CSV.
    def parse_board_csv(self, filename):
        super().parse_board_csv(filename)
        self.add_ports()
        self.add_scbs()
        self.add_tcpwm()
        self.add_peri_tr_ios()
        self.add_adc()

    # Override the default implementation just to change the default arguments
    # (extra header row, skip first column).
    def parse_af_csv(self, filename):
        return super().parse_af_csv(filename, header_rows=1, pin_col=1, af_col=2)

    def print_port_defines(self, out_header):
        print(file=out_header)
        print(
            f"#define MICROPY_PY_MACHINE_PIN_PORT_NUM_ENTRIES ({self._port_max_index + 1})",
            file=out_header,
        )

        print(file=out_header)
        print("// The MICROPY_PY_MACHINE_PIN_FOR_ALL_PORTS(DO) macro will", file=out_header)
        print("// apply the DO macro to all user available ports.", file=out_header)
        print("// The DO macro takes the port as argument: DO(port).", file=out_header)
        print("#define MICROPY_PY_MACHINE_PIN_FOR_ALL_PORTS(DO) \\", file=out_header)

        lines = [f"DO({port})" for port in self._unhidden_ports]
        macro_body = " \\\n".join(lines)
        print(macro_body, file=out_header)

        print(file=out_header)

    def print_scb_defines(self, out_header):
        print(file=out_header)
        print(
            f"#define MICROPY_PY_MACHINE_SCB_NUM_ENTRIES ({self._scb_max_index + 1})",
            file=out_header,
        )

        print(
            f"#define MICROPY_PY_MACHINE_I2C_NUM_ENTRIES ({len(self._unhidden_i2c)})",
            file=out_header,
        )

        print(
            f"#define MICROPY_PY_MACHINE_SPI_NUM_ENTRIES ({len(self._unhidden_spi)})",
            file=out_header,
        )

        print(
            f"#define MICROPY_PY_MACHINE_UART_NUM_ENTRIES ({len(self._unhidden_uart)})",
            file=out_header,
        )

        print(file=out_header)
        print("// The MICROPY_PY_MACHINE_FOR_ALL_SCB(DO) macro will", file=out_header)
        print("// apply the DO macro to all user available SCB units.", file=out_header)
        print("// The DO macro takes the SCB unit as argument: DO(unit).", file=out_header)
        print("#define MICROPY_PY_MACHINE_FOR_ALL_SCB(DO) \\", file=out_header)

        lines = [f"DO({scb})" for scb in self._unhidden_scb]
        macro_body = " \\\n".join(lines)
        print(macro_body, file=out_header)

        print(file=out_header)

    # Overwrite print_header to extend the print header base class
    # function with the pin port defines
    def print_header(self, out_header):
        super().print_header(out_header)
        self.print_port_defines(out_header)

    def print_tcpwm_defines(self, out_header):
        # TCPWM counter ownership array must be large enough to index the highest
        # counter number seen. Counters 0-7 map to indices 0-7, counters 256+ map
        # to indices 8+, so the compact array size is (max_counter - 256 + 8 + 1)
        # for high counters, or simply (max_counter + 1) for low ones.
        max_c = self._tcpwm_counter_max
        if max_c >= 256:
            num_counters = (max_c - 256 + 8) + 1
        else:
            num_counters = max_c + 1

        print(file=out_header)
        print(
            "// Number of entries in the compact TCPWM0 counter ownership array.",
            file=out_header,
        )
        print(
            f"#define MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS ({num_counters}U)",
            file=out_header,
        )
        print(
            "// Number of exposed GPIO pins that support at least one TCPWM LINE AF.",
            file=out_header,
        )
        print(
            f"#define MICROPY_PY_MACHINE_PWM_MAX_OBJS ({self._pwm_pin_count})",
            file=out_header,
        )
        print(file=out_header)

    def print_tcpwm_hw_map(self, out_header):
        # Unified TCPWM hardware map combining timer-id, counter, IRQ and PCLK.
        # Both the timer map and the counter map cover the same 32 counters, so
        # a single source-of-truth macro is sufficient.
        # Each row: X(timer_id, tcpwm_counter_num, irq, pclk_dst)
        hw_entries = [
            # Group 0 (32-bit): Timer IDs 0-7 -> counters 0-7.
            (0, "0U", "tcpwm_0_interrupts_0_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN0"),
            (1, "1U", "tcpwm_0_interrupts_1_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN1"),
            (2, "2U", "tcpwm_0_interrupts_2_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN2"),
            (3, "3U", "tcpwm_0_interrupts_3_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN3"),
            (4, "4U", "tcpwm_0_interrupts_4_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN4"),
            (5, "5U", "tcpwm_0_interrupts_5_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN5"),
            (6, "6U", "tcpwm_0_interrupts_6_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN6"),
            (7, "7U", "tcpwm_0_interrupts_7_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN7"),
            # Group 1 (16-bit): Timer IDs 8-31 -> counters 256-279.
            (8, "256U", "tcpwm_0_interrupts_256_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN256"),
            (9, "257U", "tcpwm_0_interrupts_257_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN257"),
            (10, "258U", "tcpwm_0_interrupts_258_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN258"),
            (11, "259U", "tcpwm_0_interrupts_259_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN259"),
            (12, "260U", "tcpwm_0_interrupts_260_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN260"),
            (13, "261U", "tcpwm_0_interrupts_261_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN261"),
            (14, "262U", "tcpwm_0_interrupts_262_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN262"),
            (15, "263U", "tcpwm_0_interrupts_263_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN263"),
            (16, "264U", "tcpwm_0_interrupts_264_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN264"),
            (17, "265U", "tcpwm_0_interrupts_265_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN265"),
            (18, "266U", "tcpwm_0_interrupts_266_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN266"),
            (19, "267U", "tcpwm_0_interrupts_267_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN267"),
            (20, "268U", "tcpwm_0_interrupts_268_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN268"),
            (21, "269U", "tcpwm_0_interrupts_269_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN269"),
            (22, "270U", "tcpwm_0_interrupts_270_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN270"),
            (23, "271U", "tcpwm_0_interrupts_271_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN271"),
            (24, "272U", "tcpwm_0_interrupts_272_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN272"),
            (25, "273U", "tcpwm_0_interrupts_273_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN273"),
            (26, "274U", "tcpwm_0_interrupts_274_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN274"),
            (27, "275U", "tcpwm_0_interrupts_275_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN275"),
            (28, "276U", "tcpwm_0_interrupts_276_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN276"),
            (29, "277U", "tcpwm_0_interrupts_277_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN277"),
            (30, "278U", "tcpwm_0_interrupts_278_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN278"),
            (31, "279U", "tcpwm_0_interrupts_279_IRQn", "PCLK_TCPWM0_CLOCK_COUNTER_EN279"),
        ]

        print(file=out_header)
        print(
            "// Unified TCPWM map: timer_id, counter_num, IRQ, PCLK destination, and one-to-one trigger input route.",
            file=out_header,
        )
        print(
            "// Each row: X(timer_id, tcpwm_counter_num, irq, pclk_dst, trig_out_enum)",
            file=out_header,
        )
        print("#define MICROPY_PY_MACHINE_TCPWM_MAP(X) \\", file=out_header)
        for i, (tid, counter, irq, pclk) in enumerate(hw_entries):
            counter_token = counter[:-1] if counter.endswith("U") else counter
            trig = f"PERI_0_TRIG_OUT_MUX_3_TCPWM0_ONE_CNT_TR_IN{counter_token}"
            suffix = " \\" if i < len(hw_entries) - 1 else ""
            print(f"    X({tid:2d}, {counter}, {irq}, {pclk}, {trig}){suffix}", file=out_header)
        print(file=out_header)

    def print_peri_tr_io_defines(self, out_header):
        print(file=out_header)
        print(
            f"#define MICROPY_PY_MACHINE_PERI0_TR_IO_INPUT_NUM_ENTRIES ({self._peri0_tr_io_input_max_index + 1})",
            file=out_header,
        )
        print(
            f"#define MICROPY_PY_MACHINE_PERI0_TR_IO_OUTPUT_NUM_ENTRIES ({self._peri0_tr_io_output_max_index + 1})",
            file=out_header,
        )
        print(
            f"#define MICROPY_PY_MACHINE_PERI1_TR_IO_INPUT_NUM_ENTRIES ({self._peri1_tr_io_input_max_index + 1})",
            file=out_header,
        )
        print(
            f"#define MICROPY_PY_MACHINE_PERI1_TR_IO_OUTPUT_NUM_ENTRIES ({self._peri1_tr_io_output_max_index + 1})",
            file=out_header,
        )
        print(file=out_header)

        print("#define MICROPY_PY_MACHINE_FOR_ALL_PERI0_TR_IO_INPUTS(DO) \\", file=out_header)
        lines = [f"DO({in_idx})" for in_idx in self._unhidden_peri0_tr_io_input]
        macro_body = " \\\n".join(lines)
        print(macro_body, file=out_header)
        print(file=out_header)

        print("#define MICROPY_PY_MACHINE_FOR_ALL_PERI0_TR_IO_OUTPUTS(DO) \\", file=out_header)
        lines = [f"DO({out_idx})" for out_idx in self._unhidden_peri0_tr_io_output]
        macro_body = " \\\n".join(lines)
        print(macro_body, file=out_header)
        print(file=out_header)

        print("#define MICROPY_PY_MACHINE_FOR_ALL_PERI1_TR_IO_INPUTS(DO) \\", file=out_header)
        lines = [f"DO({in_idx})" for in_idx in self._unhidden_peri1_tr_io_input]
        macro_body = " \\\n".join(lines)
        print(macro_body, file=out_header)
        print(file=out_header)

        print("#define MICROPY_PY_MACHINE_FOR_ALL_PERI1_TR_IO_OUTPUTS(DO) \\", file=out_header)
        lines = [f"DO({out_idx})" for out_idx in self._unhidden_peri1_tr_io_output]
        macro_body = " \\\n".join(lines)
        print(macro_body, file=out_header)
        print(file=out_header)

        print(file=out_header)

    def print_adc_defines(self, out_header):
        """Generate ADC lookup table declarations and dimensions."""
        max_blocks, max_channels = self._adc_dims()

        print("// ADC block/channel lookup dimensions and extern table.", file=out_header)
        print("typedef struct _machine_pin_obj_t machine_pin_obj_t;", file=out_header)
        print(f"#define MICROPY_HW_ADC_MAX_BLOCKS ({max_blocks})", file=out_header)
        print(f"#define MICROPY_HW_ADC_MAX_CHANNELS ({max_channels})", file=out_header)
        print(
            "extern const machine_pin_obj_t *const "
            "machine_adc_block_pins[MICROPY_HW_ADC_MAX_BLOCKS][MICROPY_HW_ADC_MAX_CHANNELS];",
            file=out_header,
        )
        print(file=out_header)

    def print_af_header(self, out_af_header):
        self.print_scb_defines(out_af_header)
        self.print_tcpwm_defines(out_af_header)
        self.print_tcpwm_hw_map(out_af_header)
        self.print_peri_tr_io_defines(out_af_header)
        self.print_adc_defines(out_af_header)

    # Add additional header file for AF defines and constants
    def extra_args(self, parser):
        parser.add_argument("--output-af-header")

    # Called in main() after everything else is done to write additional files.
    def generate_extra_files(self):
        if self.args.output_af_header:
            with open(self.args.output_af_header, "w") as out_af_header:
                self.print_af_header(out_af_header)


if __name__ == "__main__":
    PSE84PinGenerator().main()
