### Prerequisites

To facilitate the installation of the MicroPython PSOC™ Edge port, the `mp-ifx-flash.py` Python script is
provided. It is compatible with Windows, Linux, and macOS.

Before downloading and running the script, it is recommended to create a new folder to keep all the
related files together. For example:

```bash
mkdir mp-install
cd mp-install
```

You can easily download the script from the terminal using the following command:

```bash
curl -s -L https://raw.githubusercontent.com/micropython/micropython/master/ports/psoc-edge/tools/mp-ifx-flash.py > mp-ifx-flash.py
```

Ensure you have a recent version of [Python 3.x](https://www.python.org/downloads/) and the
[pip](https://pip.pypa.io/en/stable/installation/) package installer installed. Then install the following packages:

```bash
pip install requests
```

### Flashing

Flashing can be done using the `from-package` command. Specify the board using the `--board` flag, and the path to the downloaded `.zip` package using the `--zip-package` flag, as shown below:

```bash
python mp-ifx-flash.py from-package --board KIT_PSE84_AI --zip-package pathtodir/psoc-edge-package.zip
```

#### Interactive mode

Running the script without any command starts the interactive mode, which asks for the board, the firmware
source and, optionally, the serial number of the board debugger:

```bash
python mp-ifx-flash.py
```

The firmware source can be an official MicroPython release or preview build downloaded from
[micropython.org](https://micropython.org/download/KIT_PSE84_AI/), a release of the Infineon fork, a local
`.zip` package or local files. The interactive mode needs a terminal.

#### Individual files

The `from-files` command flashes a `.hex` file together with the `.cfg` and `.FLM` files that openocd needs.
They are all included in the `.zip` package:

```bash
python mp-ifx-flash.py from-files --board KIT_PSE84_AI --hex-file firmware.hex --cfg-file qspi_config.cfg --flm-file PSE84_SMIF.FLM
```

#### Multiple connected devices

If you have multiple PSOC™ Edge boards connected to your computer, you can identify them by their
serial number and flash each one individually. To do so, use the `-n` flag to provide the serial number of
the target board as shown below:

```bash
python mp-ifx-flash.py from-package --board KIT_PSE84_AI --zip-package pathtodir/psoc-edge-package.zip -n 181F0D5A01212300
```

