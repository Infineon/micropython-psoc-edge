# MicroPython CI On-Target Testing Extension

This extension provides a comprehensive test runner for MicroPython that supports various test types ranging from single-device tests to multi-device hardware-in-the-loop (HIL) testing. The `run_test_plan.py` tool is the main entry point for executing test suites either through structured test plans or direct command-line execution.

## Table of Contents

1. [Test Types](#test-types)
2. [Creating Test Plans](#creating-test-plans)
3. [Using the CLI Tool](#using-the-cli-tool)

## Test Types

The test runner supports the following test types:

### 1. Single Device Tests (`single`)
Tests that run on a *single device*. These are the most common tests and include basic functionality, module tests, and board-specific tests.

This test type is using the [`run-test.py`](https://github.com/micropython/micropython/blob/master/tests/run-tests.py) utility. 

**Example Use Cases:**
- Test basic Python language features
- Modules which do not require interacting with other devices

### 2. Single Device Tests with Post-Test Delay (`single_post_delay`)
Same as single device, tests but with a configurable delay between test executions. Useful for tests that need time for hardware to reset or stabilize between runs.

This test also uses the [`run-test.py`](https://github.com/micropython/micropython/blob/master/tests/run-tests.py) utility, but no group of tests will be passed to it at once, as in simple single tests. 

**Example Use Cases:**
- Tests that modify hardware state
- Power management tests
- Tests requiring hardware reset between runs

### 3. Multi-Device Tests (`multi`)
Tests that require multiple devices of the same type to communicate with each other. Both devices run the same test script and coordinate their actions.

This type of test is enabled by the [`run-multitests.py`](https://github.com/micropython/micropython/blob/master/tests/run-multitests.py) utility. The tests is written in a single script, and the utility synchronizes and manages the execution across multiple devices.

**Example Use Cases:**
- Network communication tests
- Bluetooth pairing tests

### 4. Multi-Device Tests with Stub (`multi_stub`)
Tests where one device (DUT - Device Under Test) runs the actual test while another device (stub) runs a supporting script to provide specific responses or behaviors.

The stub device script is run on the device using the [`mpremote`](https://github.com/micropython/micropython/tree/master/tools/mpremote) tool before starting the main test on the DUT (which is managed by the `run-test.py` utility).

Any synchronization between the DUT and the stub must be handled within the test scripts themselves.

**Example Use Cases:**
- Protocol testing where one device acts as a server/client
- Hardware interface testing with controlled responses
- Complex communication scenarios requiring specific timing

### 5. Custom Tests (`custom`)
Custom test scripts that can accept additional command-line arguments and implement specialized testing logic.

These scripts will be executed in the host environment, with CPython. 

**Example Use Cases:**
- File system stress tests
- Performance benchmarks
- Custom hardware validation scripts

## Creating Test Plans

Test plans are YAML files that define collections of tests to be executed. They provide a structured way to organize and configure multiple test scenarios.

### Basic Test Plan Structure

```yaml
- require:  # Optional: libraries to ensure on target devices before running tests
    - unittest
    - mqtt

- name: test-name
  type: single  # Optional: single, single_post_delay, multi, multi_stub, custom
  test:
    script: 
      - path/to/test1.py
      - path/to/test2.py
    exclude:  # Optional: tests to exclude
      - path/to/exclude.py
    device:
      - board: BOARD_NAME
        version: "1.0.0"  # Optional: specific hardware version
    post_test_delay_ms: 1000  # Optional: delay between tests
    args:  # Optional: only used in custom tests
      - arg1
      - arg2
  stub:  # Optional: only used in multi_stub tests
    script: path/to/stub.py
    device:
      - board: BOARD_NAME
```

### Example Test Plan

```yaml
# Single device test
- name: basic-functionality
  test:
    script: 
      - basics/
      - micropython/
    exclude:
      - basics/bytes_compare3.py  # Exclude problematic test
    device:
      - board: CY8CPROTO-062-4343W
      - board: CY8CPROTO-063-BLE

# Custom test with arguments
- name: filesystem-stress-test
  type: custom
  test:
    script: ports/psoc6/mp_custom/fs.py
    args:
      - stress
      - flash
    device:
      - board: CY8CPROTO-062-4343W

# Multi-device test
- name: network-communication
  type: multi
  test:
    script: ports/psoc6/multi/network_ping.py
    device:
      - board: CY8CPROTO-062-4343W
        version: "0.6.0.b"

# Multi-device test with stub
- name: bluetooth-pairing
  type: multi_stub
  test:
    script: ports/psoc6/multi/bluetooth_client.py
    device:
      - board: CY8CPROTO-062-4343W
  stub:
    script: ports/psoc6/multi/bluetooth_server.py
    device:
      - board: CY8CPROTO-063-BLE
```

Before tests start, the dependency manager can perform a global device preflight
based on optional dependency keys in the test-plan YAML.

Supported keys are: require, requires, deps, and dependencies.

For each required library and each participating device, it first checks
"import <library>" and installs the library via mpremote mip install only if
the import check fails. This check/install sequence runs at most once per
library per device for a given execution.

If no dependency key is provided in YAML, dependency installation is skipped.

### HIL Device Configuration

Hardware-in-the-loop testing requires a device configuration file that maps board types to available physical devices:

```yaml
- board_type: CY8CPROTO-062-4343W
  board_list:
    - sn: 072002F302098400  # Device serial number
      hw_ext: 0.6.0.a       # Hardware extension version
    - sn: 1C14031D03201400
      hw_ext: 0.6.0.b

- board_type: CY8CPROTO-063-BLE
  board_list:
    - sn: 100D0F1400052400
      hw_ext: 0.5.0.b
```

## Using the CLI Tool

The `run_test_plan.py` script can be used in two modes: test plan mode and direct mode.

### Test Plan Mode

Execute a structured test plan with HIL device management:

```bash
python run_test_plan.py --test-plan test-plan.yml --hil-devs devices.yml --board BOARD_NAME
```

**Parameters:**
- `--test-plan`: Path to the YAML test plan file
- `--hil-devs`: Path to the HIL device configuration file
- `--board`: Target board name (must match device configuration)
- `--max-retries`: Maximum retries for failed tests (default: 0)
- `--mpy-root-dir`: Path to MicroPython root directory (auto-detected if not specified)

**Example:**
```bash
# Run all tests in the plan for CY8CPROTO-062-4343W
python run_test_plan.py --test-plan tests/ports/psoc6/test-plan.yml \
                        --hil-devs tests/ports/psoc6/ifx-mpy-hil-devs.yml \
                        --board CY8CPROTO-062-4343W \
                        --max-retries 2

# Run specific test suites from the plan
python run_test_plan.py basic-functionality filesystem-stress-test \
                        --test-plan tests/ports/psoc6/test-plan.yml \
                        --hil-devs tests/ports/psoc6/ifx-mpy-hil-devs.yml \
                        --board CY8CPROTO-062-4343W
```

### Direct Mode

Execute tests directly by specifying device ports:

```bash
python run_test_plan.py TEST_SUITE_NAME --dut-port /dev/ttyACM0 --stub-port /dev/ttyACM1
```

**Parameters:**
- `--dut-port`: Serial port for the device under test (default: /dev/ttyACM0)
- `--stub-port`: Serial port for the stub device (required for multi-device tests)
- `--max-retries`: Maximum retries for failed tests
- `--mpy-root-dir`: Path to MicroPython root directory

**Example:**
```bash
# Run single device test
python run_test_plan.py my-single-test --dut-port /dev/ttyACM0

# Run multi-device test
python run_test_plan.py my-multi-test --dut-port /dev/ttyACM0 --stub-port /dev/ttyACM1

# Run with retries and custom MicroPython directory
python run_test_plan.py my-test --dut-port /dev/ttyACM0 \
                        --max-retries 3 \
                        --mpy-root-dir /path/to/micropython
```

### Command Usage Summary

```bash
# Test plan mode (recommended for CI/CD)
python run_test_plan.py [TEST_SUITE_NAMES...] \
                        --test-plan PLAN_FILE \
                        --hil-devs DEVICES_FILE \
                        --board BOARD_NAME \
                        [--max-retries N] \
                        [--mpy-root-dir PATH]

# Direct mode (for development/debugging)
python run_test_plan.py TEST_SUITE_NAME \
                        --dut-port PORT \
                        [--stub-port PORT] \
                        [--max-retries N] \
                        [--mpy-root-dir PATH]
```

