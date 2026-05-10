# ESP32 T-Display ADS1256

Separate Zephyr app for an ESP32-D0WD-Q6 based TTGO T-Display V1.1 using:

- onboard `ST7789V` 240x135 TFT
- external `ADS1256` over SPI for the plotted signal
- the same serial capture format used by `tools/adc_serial_logger.py`

## Board Target

This app builds against the local board target:

```bash
eeg_signal_reader/esp32/procpu
```

The board definition lives under [`boards/UABC-FIAD/eeg_signal_reader`](./boards/UABC-FIAD/eeg_signal_reader).

## Wiring

### Onboard TFT

Already mapped by the board DTS:

- `SCLK` -> `GPIO18`
- `MOSI` -> `GPIO19`
- `CS` -> `GPIO5`
- `DC` -> `GPIO16`
- `RST` -> `GPIO23`
- `BL` -> `GPIO4`

### ADS1256

- `SCLK` -> `GPIO25`
- `DIN / MOSI` -> `GPIO26`
- `DOUT / MISO` -> `GPIO39`
- `CS` -> `GPIO27`
- `DRDY` -> `GPIO36`
- `RESET` -> `GPIO33`
- `SYNC / PDWN` -> `GPIO32`

Default sampling configuration:

- single-ended `AIN4`
- `PGA=1`
- `10 SPS`

These defaults live in [`eeg_signal_reader_procpu.dts`](./boards/UABC-FIAD/eeg_signal_reader/eeg_signal_reader_procpu.dts) under the `ads1256` node.

### Battery Sense

The board battery gauge is read with the ESP32 internal ADC, not the ADS1256:

- `GPIO34` -> battery sense input
- `GPIO14` -> battery divider enable

Battery scaling in DTS assumes the TTGO T-Display V1.1 onboard `100k/100k` divider, so the firmware reconstructs cell voltage from the divided ADC reading and shows the result on the LVGL status bar.

The percentage shown on screen is a 1-cell Li-Po voltage estimate. Your `400 mAh` pack changes runtime, not the voltage-divider math.

## Build

From this folder:

```bash
source ~/zephyrproject/.venv/bin/activate
west build -b eeg_signal_reader/esp32/procpu
```

If your shell does not expose `esptool`, activating the Zephyr virtualenv first is enough in the environment used here.

## Flash

After a successful build:

```bash
source ~/zephyrproject/.venv/bin/activate
west flash
```

If you have more than one serial device connected, add the ESP32 port explicitly:

```bash
source ~/zephyrproject/.venv/bin/activate
west flash --esp-device /dev/ttyUSB0
```

## Output

The firmware emits:

- `ADC_RAW_INFO`
- `ADC_RAW_HEADER`
- `ADC_RAW`

over the normal ESP32 console UART, so the existing host logger still works.

The same CSV stream is also exposed over BLE using Zephyr's Nordic UART Service (NUS):

- device name: `UABC EEG ADS1256`
- transport: BLE notifications
- payload: the same `ADC_RAW_INFO`, `ADC_RAW_HEADER`, and `ADC_RAW` lines

## BLE Pairing

- hold the `GPIO35` button for about `800 ms`
- the display enters a `Pairing` screen with a `30 s` countdown
- if no device pairs before the timeout, BLE returns to `OFF`
- after successful pairing, the normal dashboard returns and the BLE badge shows `ON` in green
- stored bonds are loaded on boot, so the ESP32 remembers the last paired BLE host and advertises for reconnection automatically

## PyQt Plotter

There is also a PyQt6 live plotter at [`../tools/adc_pyqt_plotter.py`](../tools/adc_pyqt_plotter.py).

Install dependencies:

```bash
python3 -m pip install PyQt6 pyqtgraph pyserial bleak
```

Plot over BLE:

```bash
python3 ../tools/adc_pyqt_plotter.py --transport ble
```

The PyQt UI now includes BLE discovery controls:

- set `Transport` to `BLE`
- press `Scan BLE`
- filter the results by name
- select a device from the list and press `Connect`

For first-time pairing, open the ESP32 pairing window first by holding the `GPIO35` button.
The plotter remembers the last successful BLE device or serial port and preselects it on the next launch.
Y autoscaling is enabled by default so EEG-scale signals are visible immediately. Use `--fixed-y-range` if you want the full ADC range from `ADC_RAW_INFO`.

Plot over serial:

```bash
python3 ../tools/adc_pyqt_plotter.py --transport serial --port /dev/ttyUSB0
```

## Firmware Architecture

The firmware is organized as layered C++ code:

- `src/main.cpp` only starts `app::Controller`.
- `src/app/` owns the high-level state machine, app events, and app-visible state.
- `src/services/` wraps acquisition, BLE streaming, and signal processing.
- `src/drivers/` contains the ADS1256 Zephyr SPI/GPIO wrapper.
- `src/ui/` is the only layer that calls LVGL APIs. UI event helpers publish app events instead of controlling hardware directly.

Board-specific hardware configuration remains in Devicetree under `boards/UABC-FIAD/eeg_signal_reader/`.
The ADS1256 driver reads SPI, CS, DRDY, RESET, SYNC/PDWN, channel, gain, and data-rate configuration from the `ads1256` DTS node.

The current Zephyr module provides LVGL `9.5.0`. XML UI source placeholders are under `src/ui/xml/`; generated code should go under `src/ui/generated/` and be compiled into the firmware, not parsed at runtime.

## Future ESP32-S3 Board

To migrate the same application code to ESP32-S3, add a new board under `boards/` with equivalent Devicetree nodes and aliases:

- `chosen { zephyr,display = &...; }`
- `display_bl`
- `battery_input`
- `aliases { sw0 = &...; }`
- `ads1256` with the same `headband,ads1256` binding

No application, service, driver, or UI source should need pin-number edits if the new board DTS provides those nodes.
