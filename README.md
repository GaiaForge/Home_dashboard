# Home Dashboard

ESP32-S3 touchscreen wall dashboard, built with LVGL, plus a standalone
autonomous irrigation/pump controller. 

## What it does

- **Indoor** — local BME280 (temperature / humidity / pressure)
- **Weather API** — live Open-Meteo forecast + frost warning, manual location
- **Field Hubs** — live telemetry and alerts from remote field sensor nodes
- **Irrigation** — schedule / threshold / manual watering zones, actuated by
  a standalone valve controller
- **Pump** — mains power-strip control (Tasmota) for a cistern pump, with
  live power monitoring

## Hardware

- Panel: Waveshare ESP32-S3-Touch-LCD-5B (1024×600 RGB touchscreen)
- Valve controller: standalone ESP32 + relay board
- Pump: Nous A5T (Tasmota) smart power strip

## Repo layout

- `waveshare-lcd5-lvgl/` — panel firmware (LVGL 9.5, PlatformIO/Arduino)
- `valve-controller/` — standalone irrigation controller firmware
  (PlatformIO/Arduino)

## Setup

Each subproject needs its own `src/secrets.h` (gitignored, never committed).
Copy the example and fill in your own values:

```sh
cp waveshare-lcd5-lvgl/src/secrets.h.example waveshare-lcd5-lvgl/src/secrets.h
cp valve-controller/src/secrets.h.example valve-controller/src/secrets.h
```

Then build/flash each with [PlatformIO](https://platformio.org/):

```sh
cd waveshare-lcd5-lvgl && pio run --target upload
cd valve-controller && pio run --target upload
```

## Safety

The valve controller drives real irrigation valves and, optionally, a
mains-voltage pump strip. Before connecting water or mains power, read the
safety comment block at the top of `valve-controller/src/main.cpp`. In short:

- Verify relay polarity with a multimeter — a fresh boot must leave every
  relay de-energized (fail-safe closed).
- Every zone has a hard maximum run-time, enforced by the controller
  independent of the panel or network.
- A pump-driven zone (via the Tasmota strip) does **not** have the same
  reboot fail-safe guarantee as a local relay — see the header comment for
  why and how it's mitigated.

## Architecture

The panel talks directly to the standalone valve controller over WiFi
(HTTP) — irrigation scheduling does not go through the MicroClimate server.
The valve controller runs its own schedule/threshold logic locally from
NVS-stored config and never depends on the panel or network to behave
safely.
